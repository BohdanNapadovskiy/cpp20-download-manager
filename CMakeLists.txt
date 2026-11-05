cmake_minimum_required(VERSION 3.21)
project(download-manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif ()

add_library(dm_warnings INTERFACE)
target_compile_options(dm_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Wunused -Woverloaded-virtual
        -Wconversion -Wsign-conversion -Wnull-dereference
        -Wdouble-promotion -Wformat=2
        -Werror=return-type
)

set(DM_CORE_SOURCES
        src/core/curl_global.cpp
        src/core/download_manager.cpp
        src/core/http_transfer.cpp
        src/core/resume.cpp
        src/core/retry.cpp
        src/core/state_store.cpp
)

include(FetchContent)

FetchContent_Declare(
        curl
        URL https://curl.se/download/curl-8.23.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        URL_HASH SHA256=85477fe11408471285f38a601e7c6f53470d13be9a5a316919b071ad02e68213
)

set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(curl)

add_library(dm_core STATIC ${DM_CORE_SOURCES})
target_include_directories(dm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dm_core PRIVATE dm_warnings)
target_link_libraries(dm_core PRIVATE CURL::libcurl)

set(DM_CLI_SOURCES
        src/cli/commands.cpp
        src/cli/console.cpp
)
add_library(dm_cli STATIC ${DM_CLI_SOURCES})
target_link_libraries(dm_cli PUBLIC dm_core)
target_link_libraries(dm_cli PRIVATE dm_warnings)

add_executable(download-manager src/main.cpp)
target_link_options(download-manager PRIVATE
        $<$<CONFIG:Release>:-static-libgcc -static-libstdc++>)
target_link_libraries(download-manager PRIVATE dm_cli)
target_link_libraries(download-manager PRIVATE dm_warnings)

if (MINGW)
    get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET download-manager POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINGW_BIN}/libwinpthread-1.dll"
            "$<TARGET_FILE_DIR:download-manager>")
endif ()

enable_testing()
add_subdirectory(tests)