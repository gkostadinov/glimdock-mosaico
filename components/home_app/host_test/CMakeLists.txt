# SPDX-License-Identifier: Apache-2.0
# Home's hub client against a running demo hub (see hub_live_test.cpp). A host
# build of its own: ESP-IDF never builds this directory.
cmake_minimum_required(VERSION 3.20)
project(home_hub_live_test CXX)
set(CMAKE_CXX_STANDARD 17)

set(HOME_APP "${CMAKE_CURRENT_LIST_DIR}/..")
set(ARDUINOJSON "${HOME_APP}/../../managed_components/bblanchon__arduinojson/src" CACHE PATH "ArduinoJson's src")
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(hub-live-test hub_live_test.cpp "${HOME_APP}/src/home_hub.cpp")
target_include_directories(hub-live-test PRIVATE "${HOME_APP}/src" "${ARDUINOJSON}")
target_link_libraries(hub-live-test PRIVATE CURL::libcurl Threads::Threads)
