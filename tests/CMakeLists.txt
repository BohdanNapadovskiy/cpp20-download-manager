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

add_executable(dm_tests
        sandbox_test.cpp
)
target_link_libraries(dm_tests PRIVATE dm_warnings)
target_link_libraries(dm_tests PRIVATE dm_core GTest::gtest_main)


include(GoogleTest)
gtest_discover_tests(dm_tests)
