# SPDX-License-Identifier: Apache-2.0
# The games' difficulty, simulated on a computer (no board, no LVGL): fixed
# seeds, 30 steps a second, scripted input and the games' own pilots.
#   cmake -S components/games_app/host_test -B build-host/games-sim -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-host/games-sim && build-host/games-sim/games-sim
cmake_minimum_required(VERSION 3.16)
project(games_sim CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(SRC "${CMAKE_CURRENT_LIST_DIR}/../src")
add_executable(games-sim games_sim.cpp
    "${SRC}/games_common.cpp" "${SRC}/games_3d.cpp" "${SRC}/game_crypt.cpp" "${SRC}/game_starbore.cpp"
    "${SRC}/game_skyglide.cpp" "${SRC}/game_jellystorm.cpp" "${SRC}/game_pinball.cpp" "${SRC}/game_rally.cpp")
target_include_directories(games-sim PRIVATE "${SRC}")
target_compile_options(games-sim PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME games-sim COMMAND games-sim)
