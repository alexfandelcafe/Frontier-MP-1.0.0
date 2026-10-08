cmake_minimum_required(VERSION 3.20)
project(FrontierMP VERSION 1.0.0 LANGUAGES C CXX)

include(FetchContent)

# Frontier uses the real ENet transport for both client and dedicated server.
# ENet 1.3.18 is the stable release used by the current public SDK ecosystem.
FetchContent_Declare(
    frontier_enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(frontier_enet)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_SERVER "Build dedicated server" ON)
option(BUILD_CLIENT "Build client hook and launcher" ON)
option(BUILD_SHARED "Build shared library" ON)

# Global include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/shared/include
)

# Subdirectories
add_subdirectory(shared)

if(BUILD_SERVER)
    add_subdirectory(server)
endif()

if(BUILD_CLIENT)
    add_subdirectory(client)
endif()
