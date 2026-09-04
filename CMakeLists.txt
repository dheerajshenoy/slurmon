cmake_minimum_required(VERSION 3.12)

project(slurmon VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v7.0.3  # Replace with a version, tag, or commit hash
)

FetchContent_MakeAvailable(ftxui)

include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/slurmon.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE
    PRIVATE ftxui::screen
    PRIVATE ftxui::dom
    PRIVATE ftxui::component
)

target_precompile_headers(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include/pch.hpp)

