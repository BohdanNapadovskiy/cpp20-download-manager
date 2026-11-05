include(FetchContent)

FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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
