cmake_minimum_required(VERSION 3.12)

project(slurmon VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a Release build when the user didn't pick a config.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# Interprocedural (link-time) optimization for Release builds.
include(CheckIPOSupported)
check_ipo_supported(RESULT SLURMON_IPO_SUPPORTED OUTPUT SLURMON_IPO_ERR)

include(FetchContent)

FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v7.0.3  # Replace with a version, tag, or commit hash
)

set(FTXUI_QUIET ON CACHE BOOL "Silence FTXUI's configure banner" FORCE)
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

target_compile_definitions(${PROJECT_NAME} PRIVATE
    SLURMON_VERSION="${PROJECT_VERSION}"
)

# Extra optimization flags on top of the build-type defaults.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:-O3 -DNDEBUG -fno-plt -ffunction-sections -fdata-sections>
        $<$<CONFIG:RelWithDebInfo>:-O3 -fno-plt -ffunction-sections -fdata-sections>
        -Wall -Wextra -Wpedantic -Werror
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:-Wl,--gc-sections -Wl,-s>
        $<$<CONFIG:RelWithDebInfo>:-Wl,--gc-sections>
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/O2 /DNDEBUG>
        /W4 /WX
    )
endif()

if(SLURMON_IPO_SUPPORTED)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

include(GNUInstallDirs)

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES ${CMAKE_SOURCE_DIR}/docs/slurmon.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

install(FILES ${CMAKE_SOURCE_DIR}/example_config.toml
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

