#include "server/lua_environment.hpp"
#include "server/server.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

namespace Frontier::Server {

LuaEnvironment::LuaEnvironment(Server& server)
    : m_server(server) {}

LuaEnvironment::~LuaEnvironment() {
    reset();
}

bool LuaEnvironment::initialize() {
    registerBindings();
    std::cout << "[LuaEnvironment] Scripting engine initialized." << std::endl;
    return true;
}

void LuaEnvironment::registerBindings() {
    // Registramos los handlers del sistema de eventos base
    registerEvent("chat:command", [this](PlayerId src, const ScriptEventArgs& args) {
        if (!args.empty()) {
            std::string cmd = args[0];
            std::cout << "[ChatCommand] Player " << src << " executed: /" << cmd << std::endl;
        }
    });

    registerEvent("player:spawn_request", [this](PlayerId src, const ScriptEventArgs& args) {
        auto player = m_server.getPlayerManager().getPlayer(src);
        if (player) {
            player->isSpawned = true;
            // Spawn en Valentine por defecto
            player->position = Vector3(-180.0f, 60.0f, 1950.0f);
            player->health = 100;
            std::cout << "[LuaEnvironment] Player " << player->name << " spawned at Valentine." << std::endl;

            // Notificar al cliente que puede activar el actor del jugador
            BitStream bs;
            bs.write<uint16_t>(static_cast<uint16_t>(Protocol::PacketId::EntityCreate));
            bs.write<uint8_t>(static_cast<uint8_t>(EntityType::PlayerPed));
            bs.write<PlayerId>(src);
            bs.writeVector3(player->position);
            bs.write<float>(player->heading);
            m_server.getNetworkManager().sendPacket(src, Protocol::ChannelReliable, bs, true);
        }
    });
}

bool LuaEnvironment::executeScript(const std::string& resourceName, const std::string& scriptPath) {
    std::ifstream file(scriptPath);
    if (!file.is_open()) {
        std::cerr << "[LuaEnvironment] Could not open script file: " << scriptPath << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string scriptContent = buffer.str();

    std::cout << "[LuaEnvironment] [" << resourceName << "] Executed script: " << scriptPath 
              << " (" << scriptContent.size() << " bytes)" << std::endl;

    return true;
}

void LuaEnvironment::reset() {
    m_eventHandlers.clear();
}

void LuaEnvironment::registerEvent(const std::string& eventName, EventHandler handler) {
    m_eventHandlers[eventName].push_back(handler);
}

void LuaEnvironment::triggerEvent(const std::string& eventName, PlayerId source, const ScriptEventArgs& args) {
    auto it = m_eventHandlers.find(eventName);
    if (it != m_eventHandlers.end()) {
        for (const auto& handler : it->second) {
            try {
                handler(source, args);
            } catch (const std::exception& e) {
                std::cerr << "[LuaEnvironment] Error in event handler for '" << eventName << "': " << e.what() << std::endl;
            }
        }
    }
}

void LuaEnvironment::onPlayerJoin(PlayerId id, const std::string& name) {
    triggerEvent("core:on_player_joined", id, {std::to_string(id), name});
}

void LuaEnvironment::onPlayerLeave(PlayerId id, const std::string& name) {
    triggerEvent("core:on_player_left", id, {std::to_string(id), name});
}

void LuaEnvironment::onPlayerChat(PlayerId id, const std::string& message) {
    triggerEvent("chat:message", id, {std::to_string(id), message});
}

} // namespace Frontier::Server
