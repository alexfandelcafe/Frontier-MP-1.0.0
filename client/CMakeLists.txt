add_library(minhook STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/minhook/src/buffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/minhook/src/hook.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/minhook/src/trampoline.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/minhook/src/hde/hde32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/minhook/include
)

add_library(frontier_core SHARED
    core/src/dllmain.cpp
    core/src/engine_hooks.cpp
    core/src/pattern_scanner.cpp
    core/src/native_invoker.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/minhook/include
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
)

add_executable(frontier_launcher
    launcher/src/main.cpp
)

if(WIN32)
    target_link_libraries(frontier_launcher PRIVATE comdlg32 shell32 ole32)
endif()

set_target_properties(frontier_launcher PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Copiar automáticamente la carpeta ui_assets y settings.json junto a la DLL y Launcher
add_custom_command(TARGET frontier_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ui_assets"
        "$<TARGET_FILE_DIR:frontier_core>/ui_assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/settings.json"
        "$<TARGET_FILE_DIR:frontier_launcher>/settings.json"
    COMMENT "Copying ui_assets and settings.json to client output directory..."
)
