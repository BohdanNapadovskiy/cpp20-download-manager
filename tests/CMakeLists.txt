include(FetchContent)

FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        URL_HASH SHA256=40d4ec942217dcc84a9ebe2a68584ada7d4a33a8ee958755763278ea1c5e18ff
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Header-only HTTP server for the in-process test server. Plain HTTP only: no TLS, no compression.
FetchContent_Declare(
        httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.65.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        URL_HASH SHA256=ce6caa3bb1341ee7f04dc41ad81c06b74176217258f956bbf39c86cfbcb5d459
)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)  # needs APIs MinGW lacks
FetchContent_MakeAvailable(httplib)

add_executable(dm_tests
        curl_environment.cpp
        sandbox_test.cpp
        transfer_result_test.cpp
        download_once_test.cpp
)
target_link_libraries(dm_tests PRIVATE dm_warnings)
target_link_libraries(dm_tests PRIVATE dm_core GTest::gtest_main httplib::httplib)
# MinGW defaults to an older Windows API level; httplib requires Windows 10.
target_compile_definitions(dm_tests PRIVATE _WIN32_WINNT=0x0A00)


include(GoogleTest)
gtest_discover_tests(dm_tests)
