#pragma once

#include "player_manager.hpp"
#include "entity_manager.hpp"
#include "resource_manager.hpp"
#include "lua_environment.hpp"
#include "network_manager.hpp"
#include "http_server.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <chrono>

namespace Frontier::Server {

struct ServerConfig {
    std::string host{"0.0.0.0"};
    uint16_t port{DEFAULT_SERVER_PORT};
    uint16_t httpPort{4675};
    std::string serverName{"FrontierMP Dedicated Server - Wild West 1899"};
    uint32_t maxPlayers{32};
    uint32_t tickRate{30}; // 30 ticks por segundo
    bool isZombieDlc{false}; // Activar assets de Undead Nightmare
    std::vector<std::string> autoStartResources;
};

class Server {
public:
    Server();
    ~Server();

    bool initialize(const std::string& configFilePath = "config.toml");
    void run();
    void stop();

    PlayerManager& getPlayerManager() { return m_playerManager; }
    EntityManager& getEntityManager() { return m_entityManager; }
    ResourceManager& getResourceManager() { return m_resourceManager; }
    LuaEnvironment& getLuaEnvironment() { return m_luaEnvironment; }
    NetworkManager& getNetworkManager() { return m_networkManager; }
    HttpServer& getHttpServer() { return m_httpServer; }
    const ServerConfig& getConfig() const { return m_config; }

    // Handlers de red
    void handlePlayerHandshake(PlayerId id, const std::string& playerName, ModelHash model);
    void handleClientWelcome(
        PlayerId id,
        ModelHash model,
        const std::string& playerName,
        const Vector3& position,
        const Vector3& rotation);
    void handlePlayerSync(PlayerId id, const Protocol::PlayerSyncPacket& syncData);
    void handlePlayerDisconnect(PlayerId id, const std::string& reason);
    void handleClientEvent(PlayerId id, const std::string& eventName, const ScriptEventArgs& args);
    void handleChatMessage(PlayerId senderId, const std::string& message);

private:
    bool loadConfig(const std::string& path);
    void tick();

    ServerConfig m_config;
    std::atomic<bool> m_running{false};

    PlayerManager m_playerManager;
    EntityManager m_entityManager;
    ResourceManager m_resourceManager;
    LuaEnvironment m_luaEnvironment;
    NetworkManager m_networkManager;
    HttpServer m_httpServer;

    std::chrono::steady_clock::time_point m_lastTickTime;
};

} // namespace Frontier::Server
