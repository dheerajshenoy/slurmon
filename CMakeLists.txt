cmake_minimum_required(VERSION 3.12)

project(slurmon VERSION 0.1 LANGUAGES C)

find_package(Curses REQUIRED)

include_directories(${PROJECT_NAME} PRIVATE include)

add_executable(${PROJECT_NAME}
    src/main.c)
