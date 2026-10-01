project(valkeybloom)

find_program(CARGO cargo REQUIRED)

if (BUILD_BLOOM STREQUAL "module")
    set(BLOOM_CRATE_TYPE cdylib)
    set(BLOOM_LIB "libvalkey_bloom${CMAKE_SHARED_LIBRARY_SUFFIX}")
else ()
    set(BLOOM_CRATE_TYPE staticlib)
    set(BLOOM_LIB "libvalkey_bloom.a")
endif ()
set(BLOOM_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/target/release/${BLOOM_LIB}")

# Cargo tracks the crate's own inputs, so it runs on every build. It must run
# from the source directory to pick up .cargo/config.toml (vendored sources).
add_custom_target(
    valkeybloom_cargo ALL
    COMMAND ${CARGO} rustc --frozen --release --lib --crate-type ${BLOOM_CRATE_TYPE} --target-dir
            "${CMAKE_CURRENT_BINARY_DIR}/target"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    BYPRODUCTS "${BLOOM_LIB_PATH}"
    USES_TERMINAL)

if (BUILD_BLOOM STREQUAL "static")
    add_library(valkeybloom STATIC IMPORTED GLOBAL)
    set_target_properties(valkeybloom PROPERTIES IMPORTED_LOCATION "${BLOOM_LIB_PATH}")
    add_dependencies(valkeybloom valkeybloom_cargo)
else ()
    install(FILES "${BLOOM_LIB_PATH}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif ()
