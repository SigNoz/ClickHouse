# NOTE: should be macro to export RUST_CXXFLAGS/RUST_CFLAGS for subfolders
macro(configure_rustc)
    # NOTE: this can also be done by overriding rustc, but it not trivial with rustup.
    set(RUST_CFLAGS "${CMAKE_C_FLAGS}")

    set(CXX_INCLUDE_DIR "${ClickHouse_SOURCE_DIR}/contrib/llvm-project/libcxx/include")
    set(RUST_CXXFLAGS "${CMAKE_CXX_FLAGS} -isystem ${CXX_INCLUDE_DIR} -nostdinc++")

    if (CMAKE_OSX_SYSROOT)
        set(RUST_CXXFLAGS "${RUST_CXXFLAGS} -isysroot ${CMAKE_OSX_SYSROOT}")
        set(RUST_CFLAGS "${RUST_CFLAGS} -isysroot ${CMAKE_OSX_SYSROOT}")
    elseif(CMAKE_SYSROOT)
        set(RUST_CXXFLAGS "${RUST_CXXFLAGS} --sysroot ${CMAKE_SYSROOT}")
        set(RUST_CFLAGS "${RUST_CFLAGS} --sysroot ${CMAKE_SYSROOT}")
    endif()

    message(STATUS "RUST_CFLAGS: ${RUST_CFLAGS}")
    message(STATUS "RUST_CXXFLAGS: ${RUST_CXXFLAGS}")

    # NOTE: requires RW access for the source dir
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/.cargo/config.toml.in" "${CMAKE_CURRENT_SOURCE_DIR}/.cargo/config.toml" @ONLY)
endmacro()
configure_rustc()

function(clickhouse_import_crate)
    # This is a workaround for Corrosion case sensitive build type matching in
    # _generator_add_cargo_targets(), that leads to different paths in
    # IMPORTED_LOCATION and real path of the library.
    #
    # It uses CMAKE_CONFIGURATION_TYPES and $<CONFIG>, so here we preserve the
    # case of ${CMAKE_BUILD_TYPE} in ${CMAKE_CONFIGURATION_TYPES}.
    if ("${CMAKE_BUILD_TYPE_UC}" STREQUAL "DEBUG")
        set(CMAKE_CONFIGURATION_TYPES "${CMAKE_BUILD_TYPE};release")
    else()
        set(CMAKE_CONFIGURATION_TYPES "${CMAKE_BUILD_TYPE};debug")
    endif()
    # NOTE: we may use LTO for rust too

    corrosion_import_crate(NO_STD ${ARGN})
endfunction()

# Add crate from the build directory.
#
# Our crates has configuration files:
# - config for cargo (see config.toml.in)
# - and possibly config for build (build.rs.in)
#
# And to avoid overlaps different builds for one source directory, crate will
# be copied from source directory to the binary directory.
file(COPY ".cargo" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
function(add_rust_subdirectory src)
    set(dst "${CMAKE_CURRENT_BINARY_DIR}/${src}")
    message(STATUS "Copy ${src} to ${dst}")
    file(COPY "${src}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
    add_subdirectory("${dst}" "${dst}")
endfunction()

add_rust_subdirectory (BLAKE3)
add_rust_subdirectory (skim)
