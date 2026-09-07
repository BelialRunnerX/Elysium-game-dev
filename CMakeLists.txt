cmake_minimum_required(VERSION 3.24)
project(ElysiumPrototype VERSION 0.20.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ELYSIUM_BUILD_CLIENT "Build the raylib + EnTT playable client" ON)
option(ELYSIUM_BUILD_TESTS "Build dependency-free headless architecture tests" ON)

find_package(Threads REQUIRED)

add_library(elysium_core STATIC
    src/core/JobSystem.cpp
)
target_include_directories(elysium_core PUBLIC src)
target_link_libraries(elysium_core PUBLIC Threads::Threads)

add_library(elysium_world STATIC
    src/world/Block.cpp
    src/world/BaseInfrastructure.cpp
    src/world/CubeSphere.cpp
    src/world/MicroBrick.cpp
    src/world/PlanetSurface.cpp
    src/world/PlanetEnvironment.cpp
    src/world/SurfaceInfrastructure.cpp
    src/world/SurfaceSuspicion.cpp
    src/world/ShipTransit.cpp
    src/world/SurfaceIndustry.cpp
    src/world/InfrastructureJournal.cpp
    src/world/SurfaceChunkCache.cpp
    src/world/SurfaceWorldRead.cpp
    src/world/SurfaceNavigation.cpp
    src/world/SurfaceSiege.cpp
    src/world/SurfaceChunkPersistence.cpp
    src/world/PlanetSurfaceMesher.cpp
    src/world/VoxelMesher.cpp
    src/world/World.cpp
)
target_link_libraries(elysium_world PUBLIC elysium_core)
target_include_directories(elysium_world PUBLIC src)

# Backend-neutral renderer orchestration is headless-buildable. It consumes
# CpuMeshData and an IGraphicsBackend; no raylib/OpenGL types leak here.
add_library(elysium_render_core STATIC
    src/render/WorldRenderer.cpp
    src/render/PlanetSurfaceRenderer.cpp
)
target_link_libraries(elysium_render_core PUBLIC elysium_world elysium_core)
target_include_directories(elysium_render_core PUBLIC src)

if(MSVC)
    foreach(tgt IN ITEMS elysium_core elysium_world elysium_render_core)
        target_compile_options(${tgt} PRIVATE /W4 /permissive-)
    endforeach()
else()
    foreach(tgt IN ITEMS elysium_core elysium_world elysium_render_core)
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()

if(ELYSIUM_BUILD_TESTS)
    enable_testing()
    add_executable(elysium_headless_tests tests/headless_tests.cpp)
    target_link_libraries(elysium_headless_tests PRIVATE elysium_render_core elysium_world elysium_core)
    add_test(NAME elysium_headless COMMAND elysium_headless_tests)
    add_test(
        NAME include_boundary
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_include_boundaries.sh")
    set_tests_properties(include_boundary PROPERTIES LABELS "architecture;headless")
endif()

if(ELYSIUM_BUILD_CLIENT)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.5
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        EnTT
        GIT_REPOSITORY https://github.com/skypjack/entt.git
        GIT_TAG v3.13.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(raylib EnTT)

    add_library(elysium_ecs STATIC src/ecs/EcsWorld.cpp)
    target_link_libraries(elysium_ecs PUBLIC elysium_core PRIVATE EnTT::EnTT)
    target_include_directories(elysium_ecs PUBLIC src)

    add_library(elysium_render_raylib STATIC src/render/RaylibGraphicsBackend.cpp)
    target_link_libraries(elysium_render_raylib PUBLIC elysium_render_core raylib)
    target_include_directories(elysium_render_raylib PUBLIC src)

    add_library(elysium_game STATIC src/game/Game.cpp)
    target_link_libraries(elysium_game PUBLIC elysium_core elysium_world elysium_ecs elysium_render_core elysium_render_raylib raylib)
    target_include_directories(elysium_game PUBLIC src)

    add_executable(elysium src/main.cpp)
    target_link_libraries(elysium PRIVATE elysium_game)

    if(MSVC)
        foreach(tgt IN ITEMS elysium_ecs elysium_render_raylib elysium_game)
            target_compile_options(${tgt} PRIVATE /W4 /permissive-)
        endforeach()
    else()
        foreach(tgt IN ITEMS elysium_ecs elysium_render_raylib elysium_game)
            target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
        endforeach()
    endif()
endif()
