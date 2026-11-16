cmake_minimum_required(VERSION 3.16)
project(myshell C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(Sources.cmake)

add_executable(shell ${SHELL_SOURCES})
target_include_directories(shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(shell PRIVATE -Wall -Wextra)
