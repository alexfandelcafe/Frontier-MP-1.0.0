set(MINHOOK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/minhook")
set(MINHOOK_SOURCE_PRESENT
    EXISTS "${MINHOOK_ROOT}/src/buffer.c"
    AND EXISTS "${MINHOOK_ROOT}/src/hook.c"
    AND EXISTS "${MINHOOK_ROOT}/src/trampoline.c"
    AND EXISTS "${MINHOOK_ROOT}/src/hde/hde64.c"
)

if(MINHOOK_SOURCE_PRESENT)
    add_library(minhook STATIC
        ${MINHOOK_ROOT}/src/buffer.c
        ${MINHOOK_ROOT}/src/hook.c
        ${MINHOOK_ROOT}/src/trampoline.c
        ${MINHOOK_ROOT}/src/hde/hde32.c
        ${MINHOOK_ROOT}/src/hde/hde64.c
    )
    target_include_directories(minhook PUBLIC
        ${MINHOOK_ROOT}/include
    )
else()
    include(FetchContent)

    FetchContent_Declare(
        frontier_minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG 8af6b4acae5a9388fd742b56fa79ece89d96f823
        GIT_SHALLOW FALSE
        SOURCE_SUBDIR .
    )

    FetchContent_MakeAvailable(frontier_minhook)
endif()

add_library(frontier_core SHARED
    core/src/dllmain.cpp
    core/src/engine_hooks.cpp
    core/src/pattern_scanner.cpp
    core/src/native_invoker.cpp
    core/src/script_bridge.cpp
    core/src/player_factory.cpp
    net/src/client_net.cpp
    ui/src/cef_manager.cpp
    ui/src/d3d11_renderer.cpp
)

target_include_directories(frontier_core PRIVATE
    core/include
    net/include
    ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
    ${MINHOOK_ROOT}/include
)

target_link_libraries(frontier_core PRIVATE
    frontier_shared
    minhook
)

if(WIN32)
    target_link_libraries(frontier_core PRIVATE d3d12 d3d11 dxgi d3dcompiler)
endif()

set_target_properties(frontier_core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    SUFFIX ".asi"
)

add_executable(frontier_launcher
    launcher/src/main.cpp
    launcher/src/dependency_bootstrap.cpp
)

if(WIN32)
    target_link_libraries(frontier_launcher PRIVATE comdlg32 shell32 ole32 winhttp)
endif()

# ScriptHookRDR is kept outside the source repository as a dependency of the game,
# but this project packages the user-provided runtime DLL into the client output.
add_custom_command(TARGET frontier_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/ScriptHookRDR.dll"
        "$<TARGET_FILE_DIR:frontier_launcher>/ScriptHookRDR.dll"
    COMMENT "Copying ScriptHookRDR.dll to client output directory..."
)

set_target_properties(frontier_launcher PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_custom_command(TARGET frontier_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ui_assets"
        "$<TARGET_FILE_DIR:frontier_core>/ui_assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/settings.json"
        "$<TARGET_FILE_DIR:frontier_launcher>/settings.json"
    COMMENT "Copying ui_assets and settings.json to client output directory..."
)
