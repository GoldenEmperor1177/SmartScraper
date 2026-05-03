cmake_minimum_required(VERSION 3.20)
project(smartscraper VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

find_path(HPDF_INCLUDE_DIR hpdf.h)
find_library(HPDF_LIBRARY NAMES hpdf)
if(NOT HPDF_INCLUDE_DIR OR NOT HPDF_LIBRARY)
    message(FATAL_ERROR "libhpdf not found. Install: sudo apt install libhpdf-dev")
endif()

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

set(SOURCES
    src/config.cpp
    src/dns.cpp
    src/http_client.cpp
    src/reportmaker/budget.cpp
    src/reportmaker/tools.cpp
    src/reportmaker/agent.cpp
    src/server.cpp
    src/pdf_writer.cpp
    src/main.cpp
)

add_executable(rp ${SOURCES})

target_include_directories(rp PRIVATE include ${HPDF_INCLUDE_DIR})

target_link_libraries(rp PRIVATE
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    ${HPDF_LIBRARY}
)

target_compile_options(rp PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS rp DESTINATION /usr/local/bin)
