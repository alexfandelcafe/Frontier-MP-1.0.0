#include "server/server.hpp"
#include <iostream>
#include <fstream>
#include <thread>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Frontier::Server {

namespace fs = std::filesystem;

static fs::path getExecutableDirectory() {
#ifdef _WIN32
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    return fs::path(path).parent_path();
#else
    return fs::current_path();
#endif
}

static fs::path resolveServerPath(const std::string& relativePath) {
    fs::path base(relativePath);
    fs::path exeDir = getExecutableDirectory();
    std::vector<fs::path> candidates = {
        fs::current_path() / base,
        exeDir / base,
        exeDir.parent_path() / base,
        exeDir.parent_path() / "server" / base,
        exeDir.parent_path().parent_path() / "server" / base,
        exeDir.parent_path().parent_path() / base
    };

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            return candidate;
        }
    }
    return base;
}

Server::Server()
    : m_resourceManager(*this),
      m_luaEnvironment(*this),
      m_networkManager(*this) {}

Server::~Server() {
    stop();
}

bool Server::initialize(const std::string& configFilePath) {
    std::cout << "==========================================================" << std::endl;
    std::cout << "           FrontierMP Dedicated Server - RDR1 PC          " << std::endl;
    std::cout << "             FiveM Architecture for Red Dead 1            " << std::endl;
    std::cout << "==========================================================" << std::endl;

    fs::path resolvedConfig = resolveServerPath(configFilePath);
    std::cout << "[Server] Loading configuration from: " << resolvedConfig.string() << std::endl;
    loadConfig(resolvedConfig.string());

    // 1. Iniciar subsistema de scripting
    if (!m_luaEnvironment.initialize()) {
        std::cerr << "[Server] Failed to initialize Lua environment" << std::endl;
        return false;
    }

    // 2. Descubrir recursos estilo FiveM
    fs::path resolvedResources = resolveServerPath("resources");
    std::cout << "[Server] Resources directory: " << resolvedResources.string() << std::endl;
    m_resourceManager.discoverResources(resolvedResources.string());

    // 3. Iniciar recursos predeterminados
    for (const auto& resName : m_config.autoStartResources) {
        m_resourceManager.startResource(resName);
    }

    // 4. Iniciar servidor HTTP para descarga de assets/scripts
    if (!m_httpServer.start(
            m_config.host,
            m_config.httpPort,
            resolvedResources.string())) {
        std::cerr
            << "[Server] Failed to start HTTP resource server on port "
            << m_config.httpPort << std::endl;
        return false;
    }

    // 5. Iniciar listener UDP de red
    if (!m_networkManager.start(m_config.host, m_config.port, m_config.maxPlayers)) {
        std::cerr << "[Server] Failed to start network listener" << std::endl;
        return false;
    }

    m_running = true;
    m_lastTickTime = std::chrono::steady_clock::now();
    return true;
}

bool Server::loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cout << "[Server] Config file '" << path << "' not found, using default settings." << std::endl;
        m_config.autoStartResources = {"chat", "freeroam", "nametags"};
        return false;
    }

    std::string line;
    std::string currentArray;

    while (std::getline(file, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') continue;

        if (line.find("port =") != std::string::npos) {
            size_t eq = line.find('=');
            m_config.port = static_cast<uint16_t>(std::stoi(line.substr(eq + 1)));
        } else if (line.find("http_port =") != std::string::npos) {
            size_t eq = line.find('=');
            m_config.httpPort = static_cast<uint16_t>(std::stoi(line.substr(eq + 1)));
        } else if (line.find("max_players =") != std::string::npos) {
            size_t eq = line.find('=');
            m_config.maxPlayers = static_cast<uint32_t>(std::stoi(line.substr(eq + 1)));
        } else if (line.find("server_name =") != std::string::npos) {
            size_t q1 = line.find('\"');
            size_t q2 = line.rfind('\"');
            if (q1 != std::string::npos && q2 > q1) {
                m_config.serverName = line.substr(q1 + 1, q2 - q1 - 1);
            }
        } else if (line.find("resources =") != std::string::npos) {
            currentArray = "resources";
        }

        if (currentArray == "resources") {
            size_t q1 = line.find('\"');
            size_t q2 = line.rfind('\"');
            if (q1 != std::string::npos && q2 > q1 && q1 != q2) {
                m_config.autoStartResources.push_back(line.substr(q1 + 1, q2 - q1 - 1));
            }
            if (line.find(']') != std::string::npos) currentArray.clear();
        }
    }
    return true;
}

void Server::run() {
    std::cout << "[Server] Server started successfully. Ready for incoming connections." << std::endl;

    const auto tickInterval = std::chrono::milliseconds(1000 / m_config.tickRate);

    while (m_running) {
        auto start = std::chrono::steady_clock::now();

        m_networkManager.update(1);
        tick();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        if (elapsed < tickInterval) {
            std::this_thread::sleep_for(tickInterval - elapsed);
        }
    }
}

void Server::stop() {
    if (!m_running) return;
    std::cout << "[Server] Shutting down server..." << std::endl;
    m_running = false;
    m_networkManager.stop();
    m_httpServer.stop();
}

void Server::tick() {
    // Sincronización continua de entidades
}

void Server::handlePlayerHandshake(
    PlayerId id,
    const std::string& playerName,
    ModelHash model)
{
    auto player = m_playerManager.getPlayer(id);
    if (!player) return;

    player->name = playerName;
    player->modelHash = model;
    player->position = Vector3(-180.0f, 60.0f, 1950.0f);
    player->heading = 0.0f;
    player->isSpawned = false;

    std::cout
        << "[Server] Player connected: "
        << playerName << " (ID: " << id << ")"
        << std::endl;

    // Este es el equivalente Frontier al packet 4 de ServerData del cliente
    // original: el ID del paquete es 0x04 y el campo de versión debe coincidir
    // exactamente con el build de cliente que estamos reproduciendo.
    BitStream serverData;
    serverData.write<uint16_t>(
        static_cast<uint16_t>(Protocol::PacketId::ServerData));
    serverData.write<PlayerId>(id);
    serverData.writeString(m_config.serverName);
    serverData.writeString("Alpha vpre-0.0.5");
    serverData.write<uint16_t>(m_config.httpPort);
    serverData.write<ModelHash>(model);
    serverData.writeVector3(player->position);
    serverData.write<float>(player->heading);

    if (!m_networkManager.sendPacket(
            id,
            Protocol::ChannelReliable,
            serverData,
            true)) {

        std::cerr
            << "[Server] failed_server_sending_server_data: "
               "no se pudo enviar packet 4 al jugador "
            << id << std::endl;
        return;
    }

    // Los recursos se descubren por HTTP durante DownloadResources("cache\\").
    // No necesitamos enviar una lista de archivos por ENet.
    m_luaEnvironment.onPlayerJoin(id, playerName);

    BitStream joinNotify;
    joinNotify.write<uint16_t>(
        static_cast<uint16_t>(Protocol::PacketId::PlayerJoined));
    joinNotify.write<PlayerId>(id);
    joinNotify.writeString(playerName);

    m_networkManager.broadcast(
        Protocol::ChannelReliable,
        joinNotify,
        true,
        id);
}

void Server::handleClientWelcome(
    PlayerId id,
    ModelHash model,
    const std::string& playerName,
    const Vector3& position,
    const Vector3& rotation)
{
    auto player = m_playerManager.getPlayer(id);
    if (!player) return;

    player->modelHash = model;
    player->name = playerName;
    player->position = position;
    player->heading = rotation.z;
    player->isSpawned = true;

    std::cout
        << "[Server] ClientWelcome recibido de "
        << playerName << " (ID: " << id << "). "
           "Jugador marcado como spawned."
        << std::endl;

    m_luaEnvironment.triggerEvent(
        "player:ready",
        id,
        {playerName});
}

void Server::handlePlayerSync(PlayerId id, const Protocol::PlayerSyncPacket& syncData) {
    auto player = m_playerManager.getPlayer(id);
    if (!player) return;

    player->position = syncData.position;
    player->velocity = syncData.velocity;
    player->heading = syncData.heading;
    player->pitch = syncData.pitch;
    player->health = syncData.health;
    player->currentWeaponHash = syncData.currentWeaponHash;
    player->flags = syncData.flags;
    player->mountEntityId = syncData.mountEntityId;

    // Reenviar a los demás jugadores
    BitStream bs;
    bs.write<uint16_t>(static_cast<uint16_t>(Protocol::PacketId::PlayerSyncData));
    bs.write<Protocol::PlayerSyncPacket>(syncData);
    m_networkManager.broadcast(Protocol::ChannelSync, bs, false, id);
}

void Server::handlePlayerDisconnect(PlayerId id, const std::string& reason) {
    auto player = m_playerManager.getPlayer(id);
    if (!player) return;

    std::string name = player->name;
    m_playerManager.removePlayer(id);
    std::cout << "[Server] Player disconnected: " << name << " (" << reason << ")" << std::endl;

    m_luaEnvironment.onPlayerLeave(id, name);

    BitStream bs;
    bs.write<uint16_t>(static_cast<uint16_t>(Protocol::PacketId::PlayerLeft));
    bs.write<PlayerId>(id);
    m_networkManager.broadcast(Protocol::ChannelReliable, bs, true);
}

void Server::handleClientEvent(PlayerId id, const std::string& eventName, const ScriptEventArgs& args) {
    m_luaEnvironment.triggerEvent(eventName, id, args);
}

void Server::handleChatMessage(PlayerId senderId, const std::string& message) {
    auto player = m_playerManager.getPlayer(senderId);
    if (!player) return;

    if (!message.empty() && message[0] == '/') {
        // Es un comando
        m_luaEnvironment.triggerEvent("chat:command", senderId, {message.substr(1)});
        return;
    }

    std::cout << "[Chat] " << player->name << ": " << message << std::endl;

    BitStream bs;
    bs.write<uint16_t>(static_cast<uint16_t>(Protocol::PacketId::ChatMessage));
    bs.writeString(player->name + ": " + message);
    m_networkManager.broadcast(Protocol::ChannelReliable, bs, true);
}

} // namespace Frontier::Server
