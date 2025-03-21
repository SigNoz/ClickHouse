#include <IO/S3Common.h>

#include <Common/Exception.h>
#include <Poco/Util/AbstractConfiguration.h>
#include "config.h"

#if USE_AWS_S3

#    include <Common/quoteString.h>

#    include <IO/WriteBufferFromString.h>
#    include <Storages/StorageS3Settings.h>

#    include <aws/core/Version.h>
#    include <aws/core/auth/AWSCredentialsProvider.h>
#    include <aws/core/auth/AWSCredentialsProviderChain.h>
#    include <aws/core/auth/STSCredentialsProvider.h>
#    include <aws/core/client/DefaultRetryStrategy.h>
#    include <aws/core/client/SpecifiedRetryableErrorsRetryStrategy.h>
#    include <aws/core/platform/Environment.h>
#    include <aws/core/platform/OSVersionInfo.h>
#    include <aws/core/utils/json/JsonSerializer.h>
#    include <aws/core/utils/logging/LogMacros.h>
#    include <aws/core/utils/logging/LogSystemInterface.h>
#    include <aws/core/utils/HashingUtils.h>
#    include <aws/core/utils/UUID.h>
#    include <aws/core/http/HttpClientFactory.h>
#    include <aws/s3/S3Client.h>
#    include <aws/s3/model/HeadObjectRequest.h>

#    include <IO/S3/PocoHTTPClientFactory.h>
#    include <IO/S3/PocoHTTPClient.h>
#    include <Poco/URI.h>
#    include <re2/re2.h>
#    include <boost/algorithm/string/case_conv.hpp>
#    include <Common/logger_useful.h>

#    include <fstream>

namespace ProfileEvents
{
    extern const Event S3HeadObject;
    extern const Event DiskS3HeadObject;
}

namespace DB
{

bool S3Exception::isRetryableError() const
{
    /// Looks like these list is quite conservative, add more codes if you wish
    static const std::unordered_set<Aws::S3::S3Errors> unretryable_errors = {
        Aws::S3::S3Errors::NO_SUCH_KEY,
        Aws::S3::S3Errors::ACCESS_DENIED,
        Aws::S3::S3Errors::INVALID_ACCESS_KEY_ID,
        Aws::S3::S3Errors::INVALID_SIGNATURE,
        Aws::S3::S3Errors::NO_SUCH_UPLOAD,
        Aws::S3::S3Errors::NO_SUCH_BUCKET,
    };

    return !unretryable_errors.contains(code);
}

}

namespace
{

const char * S3_LOGGER_TAG_NAMES[][2] = {
    {"AWSClient", "AWSClient"},
    {"AWSAuthV4Signer", "AWSClient (AWSAuthV4Signer)"},
};

const std::pair<DB::LogsLevel, Poco::Message::Priority> & convertLogLevel(Aws::Utils::Logging::LogLevel log_level)
{
    /// We map levels to our own logger 1 to 1 except WARN+ levels. In most cases we failover such errors with retries
    /// and don't want to see them as Errors in our logs.
    static const std::unordered_map<Aws::Utils::Logging::LogLevel, std::pair<DB::LogsLevel, Poco::Message::Priority>> mapping =
    {
        {Aws::Utils::Logging::LogLevel::Off, {DB::LogsLevel::none, Poco::Message::PRIO_INFORMATION}},
        {Aws::Utils::Logging::LogLevel::Fatal, {DB::LogsLevel::information, Poco::Message::PRIO_INFORMATION}},
        {Aws::Utils::Logging::LogLevel::Error, {DB::LogsLevel::information, Poco::Message::PRIO_INFORMATION}},
        {Aws::Utils::Logging::LogLevel::Warn, {DB::LogsLevel::information, Poco::Message::PRIO_INFORMATION}},
        {Aws::Utils::Logging::LogLevel::Info, {DB::LogsLevel::information, Poco::Message::PRIO_INFORMATION}},
        {Aws::Utils::Logging::LogLevel::Debug, {DB::LogsLevel::debug, Poco::Message::PRIO_TEST}},
        {Aws::Utils::Logging::LogLevel::Trace, {DB::LogsLevel::trace, Poco::Message::PRIO_TEST}},
    };
    return mapping.at(log_level);
}

class AWSLogger final : public Aws::Utils::Logging::LogSystemInterface
{
public:
    explicit AWSLogger(bool enable_s3_requests_logging_)
        :enable_s3_requests_logging(enable_s3_requests_logging_)
    {
        for (auto [tag, name] : S3_LOGGER_TAG_NAMES)
            tag_loggers[tag] = &Poco::Logger::get(name);

        default_logger = tag_loggers[S3_LOGGER_TAG_NAMES[0][0]];
    }

    ~AWSLogger() final = default;

    Aws::Utils::Logging::LogLevel GetLogLevel() const final
    {
        if (enable_s3_requests_logging)
            return Aws::Utils::Logging::LogLevel::Trace;
        else
            return Aws::Utils::Logging::LogLevel::Info;
    }

    void Log(Aws::Utils::Logging::LogLevel log_level, const char * tag, const char * format_str, ...) final // NOLINT
    {
        callLogImpl(log_level, tag, format_str); /// FIXME. Variadic arguments?
    }

    void LogStream(Aws::Utils::Logging::LogLevel log_level, const char * tag, const Aws::OStringStream & message_stream) final
    {
        callLogImpl(log_level, tag, message_stream.str().c_str());
    }

    void callLogImpl(Aws::Utils::Logging::LogLevel log_level, const char * tag, const char * message)
    {
        const auto & [level, prio] = convertLogLevel(log_level);
        if (tag_loggers.contains(tag))
        {
            LOG_IMPL(tag_loggers[tag], level, prio, "{}", message);
        }
        else
        {
            LOG_IMPL(default_logger, level, prio, "{}: {}", tag, message);
        }
    }

    void Flush() final {}

private:
    Poco::Logger * default_logger;
    bool enable_s3_requests_logging;
    std::unordered_map<String, Poco::Logger *> tag_loggers;
};

class AWSEC2MetadataClient : public Aws::Internal::AWSHttpResourceClient
{
    static constexpr char EC2_SECURITY_CREDENTIALS_RESOURCE[] = "/latest/meta-data/iam/security-credentials";
    static constexpr char EC2_IMDS_TOKEN_RESOURCE[] = "/latest/api/token";
    static constexpr char EC2_IMDS_TOKEN_HEADER[] = "x-aws-ec2-metadata-token";
    static constexpr char EC2_IMDS_TOKEN_TTL_DEFAULT_VALUE[] = "21600";
    static constexpr char EC2_IMDS_TOKEN_TTL_HEADER[] = "x-aws-ec2-metadata-token-ttl-seconds";

public:
    /// See EC2MetadataClient.

    explicit AWSEC2MetadataClient(const Aws::Client::ClientConfiguration & client_configuration, const char * endpoint_)
        : Aws::Internal::AWSHttpResourceClient(client_configuration)
        , endpoint(endpoint_)
        , logger(&Poco::Logger::get("AWSEC2InstanceProfileConfigLoader"))
    {
    }

    AWSEC2MetadataClient& operator =(const AWSEC2MetadataClient & rhs) = delete;
    AWSEC2MetadataClient(const AWSEC2MetadataClient & rhs) = delete;
    AWSEC2MetadataClient& operator =(const AWSEC2MetadataClient && rhs) = delete;
    AWSEC2MetadataClient(const AWSEC2MetadataClient && rhs) = delete;

    ~AWSEC2MetadataClient() override = default;

    using Aws::Internal::AWSHttpResourceClient::GetResource;

    virtual Aws::String GetResource(const char * resource_path) const
    {
        return GetResource(endpoint.c_str(), resource_path, nullptr/*authToken*/);
    }

    virtual Aws::String getDefaultCredentials() const
    {
        String credentials_string;
        {
            std::lock_guard locker(token_mutex);

            LOG_TRACE(logger, "Getting default credentials for ec2 instance from {}", endpoint);
            auto result = GetResourceWithAWSWebServiceResult(endpoint.c_str(), EC2_SECURITY_CREDENTIALS_RESOURCE, nullptr);
            credentials_string = result.GetPayload();
            if (result.GetResponseCode() == Aws::Http::HttpResponseCode::UNAUTHORIZED)
            {
                return {};
            }
        }

        String trimmed_credentials_string = Aws::Utils::StringUtils::Trim(credentials_string.c_str());
        if (trimmed_credentials_string.empty())
            return {};

        std::vector<String> security_credentials = Aws::Utils::StringUtils::Split(trimmed_credentials_string, '\n');

        LOG_DEBUG(logger, "Calling EC2MetadataService resource, {} returned credential string {}.",
                EC2_SECURITY_CREDENTIALS_RESOURCE, trimmed_credentials_string);

        if (security_credentials.empty())
        {
            LOG_WARNING(logger, "Initial call to EC2MetadataService to get credentials failed.");
            return {};
        }

        Aws::StringStream ss;
        ss << EC2_SECURITY_CREDENTIALS_RESOURCE << "/" << security_credentials[0];
        LOG_DEBUG(logger, "Calling EC2MetadataService resource {}.", ss.str());
        return GetResource(ss.str().c_str());
    }

    static Aws::String awsComputeUserAgentString()
    {
        Aws::StringStream ss;
        ss << "aws-sdk-cpp/" << Aws::Version::GetVersionString() << " " << Aws::OSVersionInfo::ComputeOSVersionString()
                << " " << Aws::Version::GetCompilerVersionString();
        return ss.str();
    }

    virtual Aws::String getDefaultCredentialsSecurely() const
    {
        String user_agent_string = awsComputeUserAgentString();
        String new_token;

        {
            std::lock_guard locker(token_mutex);

            Aws::StringStream ss;
            ss << endpoint << EC2_IMDS_TOKEN_RESOURCE;
            std::shared_ptr<Aws::Http::HttpRequest> token_request(Aws::Http::CreateHttpRequest(ss.str(), Aws::Http::HttpMethod::HTTP_PUT,
                                                                        Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
            token_request->SetHeaderValue(EC2_IMDS_TOKEN_TTL_HEADER, EC2_IMDS_TOKEN_TTL_DEFAULT_VALUE);
            token_request->SetUserAgent(user_agent_string);
            LOG_TRACE(logger, "Calling EC2MetadataService to get token.");
            auto result = GetResourceWithAWSWebServiceResult(token_request);
            const String & token_string = result.GetPayload();
            new_token = Aws::Utils::StringUtils::Trim(token_string.c_str());

            if (result.GetResponseCode() == Aws::Http::HttpResponseCode::BAD_REQUEST)
            {
                return {};
            }
            else if (result.GetResponseCode() != Aws::Http::HttpResponseCode::OK || new_token.empty())
            {
                LOG_TRACE(logger, "Calling EC2MetadataService to get token failed, falling back to less secure way.");
                return getDefaultCredentials();
            }
            token = new_token;
        }

        String url = endpoint + EC2_SECURITY_CREDENTIALS_RESOURCE;
        std::shared_ptr<Aws::Http::HttpRequest> profile_request(Aws::Http::CreateHttpRequest(url,
                Aws::Http::HttpMethod::HTTP_GET,
                Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
        profile_request->SetHeaderValue(EC2_IMDS_TOKEN_HEADER, new_token);
        profile_request->SetUserAgent(user_agent_string);
        String profile_string = GetResourceWithAWSWebServiceResult(profile_request).GetPayload();

        String trimmed_profile_string = Aws::Utils::StringUtils::Trim(profile_string.c_str());
        std::vector<String> security_credentials = Aws::Utils::StringUtils::Split(trimmed_profile_string, '\n');

        LOG_DEBUG(logger, "Calling EC2MetadataService resource, {} with token returned profile string {}.",
                EC2_SECURITY_CREDENTIALS_RESOURCE, trimmed_profile_string);

        if (security_credentials.empty())
        {
            LOG_WARNING(logger, "Calling EC2Metadataservice to get profiles failed.");
            return {};
        }

        Aws::StringStream ss;
        ss << endpoint << EC2_SECURITY_CREDENTIALS_RESOURCE << "/" << security_credentials[0];
        std::shared_ptr<Aws::Http::HttpRequest> credentials_request(Aws::Http::CreateHttpRequest(ss.str(),
                Aws::Http::HttpMethod::HTTP_GET,
                Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
        credentials_request->SetHeaderValue(EC2_IMDS_TOKEN_HEADER, new_token);
        credentials_request->SetUserAgent(user_agent_string);
        LOG_DEBUG(logger, "Calling EC2MetadataService resource {} with token.", ss.str());
        return GetResourceWithAWSWebServiceResult(credentials_request).GetPayload();
    }

    virtual Aws::String getCurrentRegion() const
    {
        return Aws::Region::AWS_GLOBAL;
    }

private:
    const Aws::String endpoint;
    mutable std::recursive_mutex token_mutex;
    mutable Aws::String token;
    Poco::Logger * logger;
};

std::shared_ptr<AWSEC2MetadataClient> InitEC2MetadataClient(const Aws::Client::ClientConfiguration & client_configuration)
{
    Aws::String ec2_metadata_service_endpoint = Aws::Environment::GetEnv("AWS_EC2_METADATA_SERVICE_ENDPOINT");
    auto * logger = &Poco::Logger::get("AWSEC2InstanceProfileConfigLoader");
    if (ec2_metadata_service_endpoint.empty())
    {
        Aws::String ec2_metadata_service_endpoint_mode = Aws::Environment::GetEnv("AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE");
        if (ec2_metadata_service_endpoint_mode.length() == 0)
        {
            ec2_metadata_service_endpoint = "http://169.254.169.254"; //default to IPv4 default endpoint
        }
        else
        {
            if (ec2_metadata_service_endpoint_mode.length() == 4)
            {
                if (Aws::Utils::StringUtils::CaselessCompare(ec2_metadata_service_endpoint_mode.c_str(), "ipv4"))
                {
                    ec2_metadata_service_endpoint = "http://169.254.169.254"; //default to IPv4 default endpoint
                }
                else if (Aws::Utils::StringUtils::CaselessCompare(ec2_metadata_service_endpoint_mode.c_str(), "ipv6"))
                {
                    ec2_metadata_service_endpoint = "http://[fd00:ec2::254]";
                }
                else
                {
                    LOG_ERROR(logger, "AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE can only be set to ipv4 or ipv6, received: {}", ec2_metadata_service_endpoint_mode);
                }
            }
            else
            {
                LOG_ERROR(logger, "AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE can only be set to ipv4 or ipv6, received: {}", ec2_metadata_service_endpoint_mode);
            }
        }
    }
    LOG_INFO(logger, "Using IMDS endpoint: {}", ec2_metadata_service_endpoint);
    return std::make_shared<AWSEC2MetadataClient>(client_configuration, ec2_metadata_service_endpoint.c_str());
}

class AWSEC2InstanceProfileConfigLoader : public Aws::Config::AWSProfileConfigLoader
{
public:
    explicit AWSEC2InstanceProfileConfigLoader(const std::shared_ptr<AWSEC2MetadataClient> & client_, bool use_secure_pull_)
        : client(client_)
        , use_secure_pull(use_secure_pull_)
        , logger(&Poco::Logger::get("AWSEC2InstanceProfileConfigLoader"))
    {
    }

    ~AWSEC2InstanceProfileConfigLoader() override = default;

protected:
    bool LoadInternal() override
    {
        auto credentials_str = use_secure_pull ? client->getDefaultCredentialsSecurely() : client->getDefaultCredentials();

        /// See EC2InstanceProfileConfigLoader.
        if (credentials_str.empty())
            return false;

        Aws::Utils::Json::JsonValue credentials_doc(credentials_str);
        if (!credentials_doc.WasParseSuccessful())
        {
            LOG_ERROR(logger, "Failed to parse output from EC2MetadataService.");
            return false;
        }
        String access_key, secret_key, token;

        auto credentials_view = credentials_doc.View();
        access_key = credentials_view.GetString("AccessKeyId");
        LOG_TRACE(logger, "Successfully pulled credentials from EC2MetadataService with access key.");

        secret_key = credentials_view.GetString("SecretAccessKey");
        token = credentials_view.GetString("Token");

        auto region = client->getCurrentRegion();

        Aws::Config::Profile profile;
        profile.SetCredentials(Aws::Auth::AWSCredentials(access_key, secret_key, token));
        profile.SetRegion(region);
        profile.SetName(Aws::Config::INSTANCE_PROFILE_KEY);

        m_profiles[Aws::Config::INSTANCE_PROFILE_KEY] = profile;

        return true;
    }

private:
    std::shared_ptr<AWSEC2MetadataClient> client;
    bool use_secure_pull;
    Poco::Logger * logger;
};

class AWSInstanceProfileCredentialsProvider : public Aws::Auth::AWSCredentialsProvider
{
public:
    /// See InstanceProfileCredentialsProvider.

    explicit AWSInstanceProfileCredentialsProvider(const std::shared_ptr<AWSEC2InstanceProfileConfigLoader> & config_loader)
        : ec2_metadata_config_loader(config_loader)
        , load_frequency_ms(Aws::Auth::REFRESH_THRESHOLD)
        , logger(&Poco::Logger::get("AWSInstanceProfileCredentialsProvider"))
    {
        LOG_INFO(logger, "Creating Instance with injected EC2MetadataClient and refresh rate.");
    }

    Aws::Auth::AWSCredentials GetAWSCredentials() override
    {
        refreshIfExpired();
        Aws::Utils::Threading::ReaderLockGuard guard(m_reloadLock);
        auto profile_it = ec2_metadata_config_loader->GetProfiles().find(Aws::Config::INSTANCE_PROFILE_KEY);

        if (profile_it != ec2_metadata_config_loader->GetProfiles().end())
        {
            return profile_it->second.GetCredentials();
        }

        return Aws::Auth::AWSCredentials();
    }

protected:
    void Reload() override
    {
        LOG_INFO(logger, "Credentials have expired attempting to repull from EC2 Metadata Service.");
        ec2_metadata_config_loader->Load();
        AWSCredentialsProvider::Reload();
    }

private:
    void refreshIfExpired()
    {
        LOG_DEBUG(logger, "Checking if latest credential pull has expired.");
        Aws::Utils::Threading::ReaderLockGuard guard(m_reloadLock);
        if (!IsTimeToRefresh(load_frequency_ms))
        {
            return;
        }

        guard.UpgradeToWriterLock();
        if (!IsTimeToRefresh(load_frequency_ms)) // double-checked lock to avoid refreshing twice
        {
            return;
        }
        Reload();
    }

    std::shared_ptr<AWSEC2InstanceProfileConfigLoader> ec2_metadata_config_loader;
    Int64 load_frequency_ms;
    Poco::Logger * logger;
};

class AwsAuthSTSAssumeRoleWebIdentityCredentialsProvider : public Aws::Auth::AWSCredentialsProvider
{
    /// See STSAssumeRoleWebIdentityCredentialsProvider.

public:
    explicit AwsAuthSTSAssumeRoleWebIdentityCredentialsProvider(DB::S3::PocoHTTPClientConfiguration & aws_client_configuration)
        : logger(&Poco::Logger::get("AwsAuthSTSAssumeRoleWebIdentityCredentialsProvider"))
    {
        // check environment variables
        String tmp_region = Aws::Environment::GetEnv("AWS_DEFAULT_REGION");
        role_arn = Aws::Environment::GetEnv("AWS_ROLE_ARN");
        token_file = Aws::Environment::GetEnv("AWS_WEB_IDENTITY_TOKEN_FILE");
        session_name = Aws::Environment::GetEnv("AWS_ROLE_SESSION_NAME");

        // check profile_config if either m_roleArn or m_tokenFile is not loaded from environment variable
        // region source is not enforced, but we need it to construct sts endpoint, if we can't find from environment, we should check if it's set in config file.
        if (role_arn.empty() || token_file.empty() || tmp_region.empty())
        {
            auto profile = Aws::Config::GetCachedConfigProfile(Aws::Auth::GetConfigProfileName());
            if (tmp_region.empty())
            {
                tmp_region = profile.GetRegion();
            }
            // If either of these two were not found from environment, use whatever found for all three in config file
            if (role_arn.empty() || token_file.empty())
            {
                role_arn = profile.GetRoleArn();
                token_file = profile.GetValue("web_identity_token_file");
                session_name = profile.GetValue("role_session_name");
            }
        }

        if (token_file.empty())
        {
            LOG_WARNING(logger, "Token file must be specified to use STS AssumeRole web identity creds provider.");
            return; // No need to do further constructing
        }
        else
        {
            LOG_DEBUG(logger, "Resolved token_file from profile_config or environment variable to be {}", token_file);
        }

        if (role_arn.empty())
        {
            LOG_WARNING(logger, "RoleArn must be specified to use STS AssumeRole web identity creds provider.");
            return; // No need to do further constructing
        }
        else
        {
            LOG_DEBUG(logger, "Resolved role_arn from profile_config or environment variable to be {}", role_arn);
        }

        if (tmp_region.empty())
        {
            tmp_region = Aws::Region::US_EAST_1;
        }
        else
        {
            LOG_DEBUG(logger, "Resolved region from profile_config or environment variable to be {}", tmp_region);
        }

        if (session_name.empty())
        {
            session_name = Aws::Utils::UUID::RandomUUID();
        }
        else
        {
            LOG_DEBUG(logger, "Resolved session_name from profile_config or environment variable to be {}", session_name);
        }

        aws_client_configuration.scheme = Aws::Http::Scheme::HTTPS;
        aws_client_configuration.region = tmp_region;

        std::vector<String> retryable_errors;
        retryable_errors.push_back("IDPCommunicationError");
        retryable_errors.push_back("InvalidIdentityToken");

        aws_client_configuration.retryStrategy = std::make_shared<Aws::Client::SpecifiedRetryableErrorsRetryStrategy>(
            retryable_errors, /* maxRetries = */3);

        client = std::make_unique<Aws::Internal::STSCredentialsClient>(aws_client_configuration);
        initialized = true;
        LOG_INFO(logger, "Creating STS AssumeRole with web identity creds provider.");
    }

    Aws::Auth::AWSCredentials GetAWSCredentials() override
    {
        // A valid client means required information like role arn and token file were constructed correctly.
        // We can use this provider to load creds, otherwise, we can just return empty creds.
        if (!initialized)
        {
            return Aws::Auth::AWSCredentials();
        }
        refreshIfExpired();
        Aws::Utils::Threading::ReaderLockGuard guard(m_reloadLock);
        return credentials;
    }

protected:
    void Reload() override
    {
        LOG_INFO(logger, "Credentials have expired, attempting to renew from STS.");

        std::ifstream token_stream(token_file.data());
        if (token_stream)
        {
            String token_string((std::istreambuf_iterator<char>(token_stream)), std::istreambuf_iterator<char>());
            token = token_string;
        }
        else
        {
            LOG_INFO(logger, "Can't open token file: {}", token_file);
            return;
        }
        Aws::Internal::STSCredentialsClient::STSAssumeRoleWithWebIdentityRequest request{session_name, role_arn, token};

        auto result = client->GetAssumeRoleWithWebIdentityCredentials(request);
        LOG_TRACE(logger, "Successfully retrieved credentials.");
        credentials = result.creds;
    }

private:
    void refreshIfExpired()
    {
        Aws::Utils::Threading::ReaderLockGuard guard(m_reloadLock);
        if (!credentials.IsExpiredOrEmpty())
        {
            return;
        }

        guard.UpgradeToWriterLock();
        if (!credentials.IsExpiredOrEmpty()) // double-checked lock to avoid refreshing twice
        {
            return;
        }

        Reload();
    }

    std::unique_ptr<Aws::Internal::STSCredentialsClient> client;
    Aws::Auth::AWSCredentials credentials;
    Aws::String role_arn;
    Aws::String token_file;
    Aws::String session_name;
    Aws::String token;
    bool initialized = false;
    Poco::Logger * logger;
};

class S3CredentialsProviderChain : public Aws::Auth::AWSCredentialsProviderChain
{
public:
    S3CredentialsProviderChain(const DB::S3::PocoHTTPClientConfiguration & configuration, const Aws::Auth::AWSCredentials & credentials, bool use_environment_credentials, bool use_insecure_imds_request)
    {
        auto * logger = &Poco::Logger::get("S3CredentialsProviderChain");

        if (use_environment_credentials)
        {
            static const char AWS_ECS_CONTAINER_CREDENTIALS_RELATIVE_URI[] = "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI";
            static const char AWS_ECS_CONTAINER_CREDENTIALS_FULL_URI[] = "AWS_CONTAINER_CREDENTIALS_FULL_URI";
            static const char AWS_ECS_CONTAINER_AUTHORIZATION_TOKEN[] = "AWS_CONTAINER_AUTHORIZATION_TOKEN";
            static const char AWS_EC2_METADATA_DISABLED[] = "AWS_EC2_METADATA_DISABLED";

            /// The only difference from DefaultAWSCredentialsProviderChain::DefaultAWSCredentialsProviderChain()
            /// is that this chain uses custom ClientConfiguration. Also we removed process provider because it's useless in our case.
            ///
            /// AWS API tries credentials providers one by one. Some of providers (like ProfileConfigFileAWSCredentialsProvider) can be
            /// quite verbose even if nobody configured them. So we use our provider first and only after it use default providers.
            {
                DB::S3::PocoHTTPClientConfiguration aws_client_configuration = DB::S3::ClientFactory::instance().createClientConfiguration(
                    configuration.region,
                    configuration.remote_host_filter,
                    configuration.s3_max_redirects,
                    configuration.enable_s3_requests_logging,
                    configuration.for_disk_s3,
                    configuration.get_request_throttler,
                    configuration.put_request_throttler);
                AddProvider(std::make_shared<AwsAuthSTSAssumeRoleWebIdentityCredentialsProvider>(aws_client_configuration));
            }

            AddProvider(std::make_shared<Aws::Auth::EnvironmentAWSCredentialsProvider>());


            /// ECS TaskRole Credentials only available when ENVIRONMENT VARIABLE is set.
            const auto relative_uri = Aws::Environment::GetEnv(AWS_ECS_CONTAINER_CREDENTIALS_RELATIVE_URI);
            LOG_DEBUG(logger, "The environment variable value {} is {}", AWS_ECS_CONTAINER_CREDENTIALS_RELATIVE_URI,
                    relative_uri);

            const auto absolute_uri = Aws::Environment::GetEnv(AWS_ECS_CONTAINER_CREDENTIALS_FULL_URI);
            LOG_DEBUG(logger, "The environment variable value {} is {}", AWS_ECS_CONTAINER_CREDENTIALS_FULL_URI,
                    absolute_uri);

            const auto ec2_metadata_disabled = Aws::Environment::GetEnv(AWS_EC2_METADATA_DISABLED);
            LOG_DEBUG(logger, "The environment variable value {} is {}", AWS_EC2_METADATA_DISABLED,
                    ec2_metadata_disabled);

            if (!relative_uri.empty())
            {
                AddProvider(std::make_shared<Aws::Auth::TaskRoleCredentialsProvider>(relative_uri.c_str()));
                LOG_INFO(logger, "Added ECS metadata service credentials provider with relative path: [{}] to the provider chain.",
                        relative_uri);
            }
            else if (!absolute_uri.empty())
            {
                const auto token = Aws::Environment::GetEnv(AWS_ECS_CONTAINER_AUTHORIZATION_TOKEN);
                AddProvider(std::make_shared<Aws::Auth::TaskRoleCredentialsProvider>(absolute_uri.c_str(), token.c_str()));

                /// DO NOT log the value of the authorization token for security purposes.
                LOG_INFO(logger, "Added ECS credentials provider with URI: [{}] to the provider chain with a{} authorization token.",
                        absolute_uri, token.empty() ? "n empty" : " non-empty");
            }
            else if (Aws::Utils::StringUtils::ToLower(ec2_metadata_disabled.c_str()) != "true")
            {
                DB::S3::PocoHTTPClientConfiguration aws_client_configuration = DB::S3::ClientFactory::instance().createClientConfiguration(
                    configuration.region,
                    configuration.remote_host_filter,
                    configuration.s3_max_redirects,
                    configuration.enable_s3_requests_logging,
                    configuration.for_disk_s3,
                    configuration.get_request_throttler,
                    configuration.put_request_throttler);

                /// See MakeDefaultHttpResourceClientConfiguration().
                /// This is part of EC2 metadata client, but unfortunately it can't be accessed from outside
                /// of contrib/aws/aws-cpp-sdk-core/source/internal/AWSHttpResourceClient.cpp
                aws_client_configuration.maxConnections = 2;
                aws_client_configuration.scheme = Aws::Http::Scheme::HTTP;

                /// Explicitly set the proxy settings to empty/zero to avoid relying on defaults that could potentially change
                /// in the future.
                aws_client_configuration.proxyHost = "";
                aws_client_configuration.proxyUserName = "";
                aws_client_configuration.proxyPassword = "";
                aws_client_configuration.proxyPort = 0;

                /// EC2MetadataService throttles by delaying the response so the service client should set a large read timeout.
                /// EC2MetadataService delay is in order of seconds so it only make sense to retry after a couple of seconds.
                aws_client_configuration.connectTimeoutMs = 1000;
                aws_client_configuration.requestTimeoutMs = 1000;

                aws_client_configuration.retryStrategy = std::make_shared<Aws::Client::DefaultRetryStrategy>(1, 1000);

                auto ec2_metadata_client = InitEC2MetadataClient(aws_client_configuration);
                auto config_loader = std::make_shared<AWSEC2InstanceProfileConfigLoader>(ec2_metadata_client, !use_insecure_imds_request);

                AddProvider(std::make_shared<AWSInstanceProfileCredentialsProvider>(config_loader));
                LOG_INFO(logger, "Added EC2 metadata service credentials provider to the provider chain.");
            }
        }

        AddProvider(std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(credentials));
        /// Quite verbose provider (argues if file with credentials doesn't exist) so iut's the last one
        /// in chain.
        AddProvider(std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>());
    }
};

}


namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int S3_ERROR;
}

namespace S3
{
    ClientFactory::ClientFactory()
    {
        aws_options = Aws::SDKOptions{};
        Aws::InitAPI(aws_options);
        Aws::Utils::Logging::InitializeAWSLogging(std::make_shared<AWSLogger>(false));
        Aws::Http::SetHttpClientFactory(std::make_shared<PocoHTTPClientFactory>());
    }

    ClientFactory::~ClientFactory()
    {
        Aws::Utils::Logging::ShutdownAWSLogging();
        Aws::ShutdownAPI(aws_options);
    }

    ClientFactory & ClientFactory::instance()
    {
        static ClientFactory ret;
        return ret;
    }

    std::unique_ptr<Aws::S3::S3Client> ClientFactory::create( // NOLINT
        const PocoHTTPClientConfiguration & cfg_,
        bool is_virtual_hosted_style,
        const String & access_key_id,
        const String & secret_access_key,
        const String & server_side_encryption_customer_key_base64,
        HeaderCollection headers,
        bool use_environment_credentials,
        bool use_insecure_imds_request)
    {
        PocoHTTPClientConfiguration client_configuration = cfg_;
        client_configuration.updateSchemeAndRegion();

        if (!server_side_encryption_customer_key_base64.empty())
        {
            /// See S3Client::GeneratePresignedUrlWithSSEC().

            headers.push_back({Aws::S3::SSEHeaders::SERVER_SIDE_ENCRYPTION_CUSTOMER_ALGORITHM,
                Aws::S3::Model::ServerSideEncryptionMapper::GetNameForServerSideEncryption(Aws::S3::Model::ServerSideEncryption::AES256)});

            headers.push_back({Aws::S3::SSEHeaders::SERVER_SIDE_ENCRYPTION_CUSTOMER_KEY,
                server_side_encryption_customer_key_base64});

            Aws::Utils::ByteBuffer buffer = Aws::Utils::HashingUtils::Base64Decode(server_side_encryption_customer_key_base64);
            String str_buffer(reinterpret_cast<char *>(buffer.GetUnderlyingData()), buffer.GetLength());
            headers.push_back({Aws::S3::SSEHeaders::SERVER_SIDE_ENCRYPTION_CUSTOMER_KEY_MD5,
                Aws::Utils::HashingUtils::Base64Encode(Aws::Utils::HashingUtils::CalculateMD5(str_buffer))});
        }

        client_configuration.extra_headers = std::move(headers);

        Aws::Auth::AWSCredentials credentials(access_key_id, secret_access_key);
        auto credentials_provider = std::make_shared<S3CredentialsProviderChain>(
                client_configuration,
                std::move(credentials),
                use_environment_credentials,
                use_insecure_imds_request);

        return std::make_unique<Aws::S3::S3Client>(
            std::move(credentials_provider),
            std::move(client_configuration), // Client configuration.
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
            is_virtual_hosted_style || client_configuration.endpointOverride.empty() /// Use virtual addressing if endpoint is not specified.
        );
    }

    PocoHTTPClientConfiguration ClientFactory::createClientConfiguration( // NOLINT
        const String & force_region,
        const RemoteHostFilter & remote_host_filter,
        unsigned int s3_max_redirects,
        bool enable_s3_requests_logging,
        bool for_disk_s3,
        const ThrottlerPtr & get_request_throttler,
        const ThrottlerPtr & put_request_throttler)
    {
        return PocoHTTPClientConfiguration(
            force_region,
            remote_host_filter,
            s3_max_redirects,
            enable_s3_requests_logging,
            for_disk_s3,
            get_request_throttler,
            put_request_throttler);
    }

    URI::URI(const std::string & uri_)
    {
        /// Case when bucket name represented in domain name of S3 URL.
        /// E.g. (https://bucket-name.s3.Region.amazonaws.com/key)
        /// https://docs.aws.amazon.com/AmazonS3/latest/dev/VirtualHosting.html#virtual-hosted-style-access
        static const RE2 virtual_hosted_style_pattern(R"((.+)\.(s3|cos|obs|oss)([.\-][a-z0-9\-.:]+))");

        /// Case when bucket name and key represented in path of S3 URL.
        /// E.g. (https://s3.Region.amazonaws.com/bucket-name/key)
        /// https://docs.aws.amazon.com/AmazonS3/latest/dev/VirtualHosting.html#path-style-access
        static const RE2 path_style_pattern("^/([^/]*)/(.*)");

        static constexpr auto S3 = "S3";
        static constexpr auto COSN = "COSN";
        static constexpr auto COS = "COS";
        static constexpr auto OBS = "OBS";
        static constexpr auto OSS = "OSS";

        uri = Poco::URI(uri_);

        storage_name = S3;

        if (uri.getHost().empty())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Host is empty in S3 URI.");

        /// Extract object version ID from query string.
        bool has_version_id = false;
        for (const auto & [query_key, query_value] : uri.getQueryParameters())
            if (query_key == "versionId")
            {
                version_id = query_value;
                has_version_id = true;
            }

        /// Poco::URI will ignore '?' when parsing the path, but if there is a vestionId in the http parameter,
        /// '?' can not be used as a wildcard, otherwise it will be ambiguous.
        /// If no "vertionId" in the http parameter, '?' can be used as a wildcard.
        /// It is necessary to encode '?' to avoid deletion during parsing path.
        if (!has_version_id && uri_.find('?') != String::npos)
        {
            String uri_with_question_mark_encode;
            Poco::URI::encode(uri_, "?", uri_with_question_mark_encode);
            uri = Poco::URI(uri_with_question_mark_encode);
        }

        String name;
        String endpoint_authority_from_uri;

        if (re2::RE2::FullMatch(uri.getAuthority(), virtual_hosted_style_pattern, &bucket, &name, &endpoint_authority_from_uri))
        {
            is_virtual_hosted_style = true;
            endpoint = uri.getScheme() + "://" + name + endpoint_authority_from_uri;
            validateBucket(bucket, uri);

            if (!uri.getPath().empty())
            {
                /// Remove leading '/' from path to extract key.
                key = uri.getPath().substr(1);
            }

            boost::to_upper(name);
            if (name != S3 && name != COS && name != OBS && name != OSS)
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Object storage system name is unrecognized in virtual hosted style S3 URI: {}", quoteString(name));

            if (name == S3)
                storage_name = name;
            else if (name == OBS)
                storage_name = OBS;
            else if (name == OSS)
                storage_name = OSS;
            else
                storage_name = COSN;
        }
        else if (re2::RE2::PartialMatch(uri.getPath(), path_style_pattern, &bucket, &key))
        {
            is_virtual_hosted_style = false;
            endpoint = uri.getScheme() + "://" + uri.getAuthority();
            validateBucket(bucket, uri);
        }
        else
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Bucket or key name are invalid in S3 URI.");
    }

    void URI::validateBucket(const String & bucket, const Poco::URI & uri)
    {
        /// S3 specification requires at least 3 and at most 63 characters in bucket name.
        /// https://docs.aws.amazon.com/awscloudtrail/latest/userguide/cloudtrail-s3-bucket-naming-requirements.html
        if (bucket.length() < 3 || bucket.length() > 63)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Bucket name length is out of bounds in virtual hosted style S3 URI:     {}{}",
                            quoteString(bucket), !uri.empty() ? " (" + uri.toString() + ")" : "");
    }

    bool isNotFoundError(Aws::S3::S3Errors error)
    {
        return error == Aws::S3::S3Errors::RESOURCE_NOT_FOUND || error == Aws::S3::S3Errors::NO_SUCH_KEY;
    }

    Aws::S3::Model::HeadObjectOutcome headObject(const Aws::S3::S3Client & client, const String & bucket, const String & key, const String & version_id, bool for_disk_s3)
    {
        ProfileEvents::increment(ProfileEvents::S3HeadObject);
        if (for_disk_s3)
            ProfileEvents::increment(ProfileEvents::DiskS3HeadObject);

        Aws::S3::Model::HeadObjectRequest req;
        req.SetBucket(bucket);
        req.SetKey(key);

        if (!version_id.empty())
            req.SetVersionId(version_id);

        return client.HeadObject(req);
    }

    S3::ObjectInfo getObjectInfo(const Aws::S3::S3Client & client, const String & bucket, const String & key, const String & version_id, bool throw_on_error, bool for_disk_s3)
    {
        auto outcome = headObject(client, bucket, key, version_id, for_disk_s3);

        if (outcome.IsSuccess())
        {
            auto read_result = outcome.GetResultWithOwnership();
            return {.size = static_cast<size_t>(read_result.GetContentLength()), .last_modification_time = read_result.GetLastModified().Millis() / 1000};
        }
        else if (throw_on_error)
        {
            const auto & error = outcome.GetError();
            throw DB::Exception(ErrorCodes::S3_ERROR,
                "Failed to HEAD object: {}. HTTP response code: {}",
                error.GetMessage(), static_cast<size_t>(error.GetResponseCode()));
        }
        return {};
    }

    size_t getObjectSize(const Aws::S3::S3Client & client, const String & bucket, const String & key, const String & version_id, bool throw_on_error, bool for_disk_s3)
    {
        return getObjectInfo(client, bucket, key, version_id, throw_on_error, for_disk_s3).size;
    }

    bool objectExists(const Aws::S3::S3Client & client, const String & bucket, const String & key, const String & version_id, bool for_disk_s3)
    {
        auto outcome = headObject(client, bucket, key, version_id, for_disk_s3);

        if (outcome.IsSuccess())
            return true;

        const auto & error = outcome.GetError();
        if (isNotFoundError(error.GetErrorType()))
            return false;

        throw S3Exception(error.GetErrorType(),
            "Failed to check existence of key {} in bucket {}: {}",
            key, bucket, error.GetMessage());
    }
}

}

#endif

namespace DB
{

namespace ErrorCodes
{
    extern const int INVALID_CONFIG_PARAMETER;
}

namespace S3
{

AuthSettings AuthSettings::loadFromConfig(const std::string & config_elem, const Poco::Util::AbstractConfiguration & config)
{
    auto access_key_id = config.getString(config_elem + ".access_key_id", "");
    auto secret_access_key = config.getString(config_elem + ".secret_access_key", "");
    auto region = config.getString(config_elem + ".region", "");
    auto server_side_encryption_customer_key_base64 = config.getString(config_elem + ".server_side_encryption_customer_key_base64", "");

    std::optional<bool> use_environment_credentials;
    if (config.has(config_elem + ".use_environment_credentials"))
        use_environment_credentials = config.getBool(config_elem + ".use_environment_credentials");

    std::optional<bool> use_insecure_imds_request;
    if (config.has(config_elem + ".use_insecure_imds_request"))
        use_insecure_imds_request = config.getBool(config_elem + ".use_insecure_imds_request");

    HeaderCollection headers;
    Poco::Util::AbstractConfiguration::Keys subconfig_keys;
    config.keys(config_elem, subconfig_keys);
    for (const std::string & subkey : subconfig_keys)
    {
        if (subkey.starts_with("header"))
        {
            auto header_str = config.getString(config_elem + "." + subkey);
            auto delimiter = header_str.find(':');
            if (delimiter == std::string::npos)
                throw Exception("Malformed s3 header value", ErrorCodes::INVALID_CONFIG_PARAMETER);
            headers.emplace_back(HttpHeader{header_str.substr(0, delimiter), header_str.substr(delimiter + 1, String::npos)});
        }
    }

    return AuthSettings
    {
        std::move(access_key_id), std::move(secret_access_key),
        std::move(region),
        std::move(server_side_encryption_customer_key_base64),
        std::move(headers),
        use_environment_credentials,
        use_insecure_imds_request
    };
}


void AuthSettings::updateFrom(const AuthSettings & from)
{
    /// Update with check for emptyness only parameters which
    /// can be passed not only from config, but via ast.

    if (!from.access_key_id.empty())
        access_key_id = from.access_key_id;
    if (!from.secret_access_key.empty())
        secret_access_key = from.secret_access_key;

    headers = from.headers;
    region = from.region;
    server_side_encryption_customer_key_base64 = from.server_side_encryption_customer_key_base64;
    use_environment_credentials = from.use_environment_credentials;
    use_insecure_imds_request = from.use_insecure_imds_request;
}

}
}
