# Host tests for the profiles model (test_profiles.cpp): the same sources as
# the board's, over the host's store, clock and portable PBKDF2.
cmake_minimum_required(VERSION 3.16)
project(profiles_test CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(PROFILES "${CMAKE_CURRENT_LIST_DIR}/..")
add_executable(profiles-test test_profiles.cpp "${PROFILES}/src/profiles.cpp" "${PROFILES}/src/profiles_platform.cpp")
target_include_directories(profiles-test PRIVATE "${PROFILES}/include" "${PROFILES}/src")
target_compile_options(profiles-test PRIVATE -Wall -Wextra -Werror -O2)
find_package(Threads REQUIRED)
target_link_libraries(profiles-test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME profiles COMMAND profiles-test)
