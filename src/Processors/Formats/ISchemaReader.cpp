#include <Processors/Formats/ISchemaReader.h>
#include <Formats/SchemaInferenceUtils.h>
#include <DataTypes/DataTypeString.h>
#include <Interpreters/parseColumnsListForTableFunction.h>
#include <boost/algorithm/string.hpp>

namespace DB
{

namespace ErrorCodes
{
    extern const int ONLY_NULLS_WHILE_READING_SCHEMA;
    extern const int INCORRECT_DATA;
    extern const int EMPTY_DATA_PASSED;
    extern const int BAD_ARGUMENTS;
}

void checkFinalInferredType(DataTypePtr & type, const String & name, const FormatSettings & settings, const DataTypePtr & default_type, size_t rows_read)
{
    if (!checkIfTypeIsComplete(type))
    {
        if (!default_type)
            throw Exception(
                ErrorCodes::ONLY_NULLS_WHILE_READING_SCHEMA,
                "Cannot determine type for column '{}' by first {} rows of data, most likely this column contains only Nulls or empty "
                "Arrays/Maps. You can specify the type for this column using setting schema_inference_hints. "
                "If your data contains complex JSON objects, try enabling one of the settings allow_experimental_object_type/input_format_json_read_objects_as_strings",
                name,
                rows_read);

        type = default_type;
    }

    if (settings.schema_inference_make_columns_nullable)
        type = makeNullableRecursively(type);
}

IIRowSchemaReader::IIRowSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_, DataTypePtr default_type_)
    : ISchemaReader(in_), default_type(default_type_), hints_str(format_settings_.schema_inference_hints), format_settings(format_settings_)
{
}


void IIRowSchemaReader::setContext(ContextPtr & context)
{
    ColumnsDescription columns;
    if (tryParseColumnsListFromString(hints_str, columns, context))
    {
        for (const auto & [name, type] : columns.getAll())
            hints[name] = type;
    }
}

void IIRowSchemaReader::transformTypesIfNeeded(DataTypePtr & type, DataTypePtr & new_type)
{
    transformInferredTypesIfNeeded(type, new_type, format_settings);
}

IRowSchemaReader::IRowSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_)
    : IIRowSchemaReader(in_, format_settings_), column_names(splitColumnNames(format_settings.column_names_for_schema_inference))
{
}

IRowSchemaReader::IRowSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_, DataTypePtr default_type_)
    : IIRowSchemaReader(in_, format_settings_, default_type_), column_names(splitColumnNames(format_settings.column_names_for_schema_inference))
{
}

IRowSchemaReader::IRowSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_, const DataTypes & default_types_)
    : IRowSchemaReader(in_, format_settings_)
{
    default_types = default_types_;
}

NamesAndTypesList IRowSchemaReader::readSchema()
{
    if (max_rows_to_read == 0)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "Cannot read rows to determine the schema, the maximum number of rows to read is set to 0. "
            "Most likely setting input_format_max_rows_to_read_for_schema_inference is set to 0");

    DataTypes data_types = readRowAndGetDataTypes();

    /// Check that we read at list one column.
    if (data_types.empty())
        throw Exception(ErrorCodes::EMPTY_DATA_PASSED, "Cannot read rows from the data");

    /// If column names weren't set, use default names 'c1', 'c2', ...
    if (column_names.empty())
    {
        column_names.reserve(data_types.size());
        for (size_t i = 0; i != data_types.size(); ++i)
            column_names.push_back("c" + std::to_string(i + 1));
    }
    /// If column names were set, check that the number of names match the number of types.
    else if (column_names.size() != data_types.size())
    {
        throw Exception(
            ErrorCodes::INCORRECT_DATA,
            "The number of column names {} differs with the number of types {}", column_names.size(), data_types.size());
    }
    else
    {
        std::unordered_set<std::string_view> names_set;
        for (const auto & name : column_names)
        {
            if (names_set.contains(name))
                throw Exception(ErrorCodes::INCORRECT_DATA, "Duplicate column name found while schema inference: \"{}\"", name);
            names_set.insert(name);
        }
    }

    for (size_t i = 0; i != column_names.size(); ++i)
    {
        auto hint_it = hints.find(column_names[i]);
        if (hint_it != hints.end())
            data_types[i] = hint_it->second;
    }

    for (rows_read = 1; rows_read < max_rows_to_read; ++rows_read)
    {
        DataTypes new_data_types = readRowAndGetDataTypes();
        if (new_data_types.empty())
            /// We reached eof.
            break;

        if (new_data_types.size() != data_types.size())
            throw Exception(ErrorCodes::INCORRECT_DATA, "Rows have different amount of values");

        for (field_index = 0; field_index != data_types.size(); ++field_index)
        {
            /// Check if we couldn't determine the type of this column in a new row
            /// or the type for this column was taken from hints.
            if (!new_data_types[field_index] || hints.contains(column_names[field_index]))
                continue;

            chooseResultColumnType(*this, data_types[field_index], new_data_types[field_index], getDefaultType(field_index), std::to_string(field_index + 1), rows_read);
        }
    }

    NamesAndTypesList result;
    for (field_index = 0; field_index != data_types.size(); ++field_index)
    {
        /// Don't check/change types from hints.
        if (!hints.contains(column_names[field_index]))
        {
            transformFinalTypeIfNeeded(data_types[field_index]);
            /// Check that we could determine the type of this column.
            checkFinalInferredType(data_types[field_index], column_names[field_index], format_settings, getDefaultType(field_index), rows_read);
        }
        result.emplace_back(column_names[field_index], data_types[field_index]);
    }

    return result;
}

Strings splitColumnNames(const String & column_names_str)
{
    if (column_names_str.empty())
        return {};

    Strings column_names;
    /// column_names_for_schema_inference is a string in format 'column1,column2,column3,...'
    boost::split(column_names, column_names_str, boost::is_any_of(","));
    for (auto & column_name : column_names)
    {
        std::string col_name_trimmed = boost::trim_copy(column_name);
        if (!col_name_trimmed.empty())
            column_name = col_name_trimmed;
    }
    return column_names;
}

DataTypePtr IRowSchemaReader::getDefaultType(size_t column) const
{
    if (default_type)
        return default_type;
    if (column < default_types.size() && default_types[column])
        return default_types[column];
    return nullptr;
}

IRowWithNamesSchemaReader::IRowWithNamesSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_, DataTypePtr default_type_)
    : IIRowSchemaReader(in_, format_settings_, default_type_)
{
}

NamesAndTypesList IRowWithNamesSchemaReader::readSchema()
{
    if (max_rows_to_read == 0)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "Cannot read rows to determine the schema, the maximum number of rows to read is set to 0. "
            "Most likely setting input_format_max_rows_to_read_for_schema_inference is set to 0");

    bool eof = false;
    auto names_and_types = readRowAndGetNamesAndDataTypes(eof);
    std::unordered_map<String, DataTypePtr> names_to_types;
    std::vector<String> names_order;
    names_to_types.reserve(names_and_types.size());
    names_order.reserve(names_and_types.size());
    for (const auto & [name, type] : names_and_types)
    {
        if (names_to_types.contains(name))
            throw Exception(ErrorCodes::INCORRECT_DATA, "Duplicate column name found while schema inference: \"{}\"", name);

        auto hint_it = hints.find(name);
        if (hint_it != hints.end())
            names_to_types[name] = hint_it->second;
        else
            names_to_types[name] = type;
        names_order.push_back(name);
    }

    for (rows_read = 1; rows_read < max_rows_to_read; ++rows_read)
    {
        auto new_names_and_types = readRowAndGetNamesAndDataTypes(eof);
        if (eof)
            /// We reached eof.
            break;

        std::unordered_set<std::string_view> names_set; /// We should check for duplicate column names in current row
        for (auto & [name, new_type] : new_names_and_types)
        {
            if (names_set.contains(name))
                throw Exception(ErrorCodes::INCORRECT_DATA, "Duplicate column name found while schema inference: \"{}\"", name);
            names_set.insert(name);

            auto it = names_to_types.find(name);
            /// If we didn't see this column before, just add it.
            if (it == names_to_types.end())
            {
                auto hint_it = hints.find(name);
                if (hint_it != hints.end())
                    names_to_types[name] = hint_it->second;
                else
                    names_to_types[name] = new_type;
                names_order.push_back(name);
                continue;
            }

            if (hints.contains(name))
                continue;

            auto & type = it->second;
            chooseResultColumnType(*this, type, new_type, default_type, name, rows_read);
        }
    }

    /// Check that we read at list one column.
    if (names_to_types.empty())
        throw Exception(ErrorCodes::EMPTY_DATA_PASSED, "Cannot read rows from the data");

    NamesAndTypesList result = getStaticNamesAndTypes();
    for (auto & name : names_order)
    {
        auto & type = names_to_types[name];
        /// Don't check/change types from hints.
        if (!hints.contains(name))
        {
            transformFinalTypeIfNeeded(type);
            /// Check that we could determine the type of this column.
            checkFinalInferredType(type, name, format_settings, default_type, rows_read);
        }
        result.emplace_back(name, type);
    }

    return result;
}

}
