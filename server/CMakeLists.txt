add_executable(frontier_server
    src/main.cpp
    src/server.cpp
    src/player_manager.cpp
    src/entity_manager.cpp
    src/resource_manager.cpp
    src/lua_environment.cpp
    src/network_manager.cpp
    src/http_server.cpp
)

target_include_directories(frontier_server PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)

target_link_libraries(frontier_server PRIVATE
    frontier_shared
)

if(WIN32)
    target_link_libraries(frontier_server PRIVATE ws2_32)
endif()

set_target_properties(frontier_server PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Copiar automáticamente config.toml y la carpeta resources al directorio del ejecutable
add_custom_command(TARGET frontier_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.toml"
        "$<TARGET_FILE_DIR:frontier_server>/config.toml"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:frontier_server>/resources"
    COMMENT "Copying config.toml and resources to server output directory..."
)
