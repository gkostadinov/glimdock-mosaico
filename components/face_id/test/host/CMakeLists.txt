# Host tests of face ID's matching, agreement, liveness and template logic
# (synthetic data only). See test_face_match.cpp for the commands.
cmake_minimum_required(VERSION 3.16)
project(face_id_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
add_executable(test_face_match test_face_match.cpp "${SRC}/face_match.cpp")
target_include_directories(test_face_match PRIVATE "${SRC}")
target_compile_options(test_face_match PRIVATE -Wall -Wextra -Werror)
enable_testing()
add_test(NAME face_match COMMAND test_face_match)
