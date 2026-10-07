#include "server/network_manager.hpp"
#include "server/server.hpp"
#include <iostream>

namespace Frontier::Server {

NetworkManager::NetworkManager(Server& server)
    : m_server(server) {}

NetworkManager::~NetworkManager() {
    stop();
}

bool NetworkManager::start(const std::string& bindHost, uint16_t port, uint32_t maxClients) {
    m_host = bindHost;
    m_port = port;
    m_maxClients = maxClients;
    m_running = true;

    std::cout << "[NetworkManager] UDP Listener active on " << bindHost << ":" << port 
              << " (Max players: " << maxClients << ")" << std::endl;
    return true;
}

void NetworkManager::stop() {
    if (!m_running) return;
    m_running = false;
    std::cout << "[NetworkManager] Network stopped." << std::endl;
}

void NetworkManager::update(uint32_t timeoutMs) {
    if (!m_running) return;
    // En una integración completa con enet.dll / enet.lib, aquí se llama a enet_host_service.
}

bool NetworkManager::sendPacket(PlayerId target, Protocol::Channel channel, const BitStream& stream, bool reliable) {
    auto player = m_server.getPlayerManager().getPlayer(target);
    if (!player || !player->peer) return false;

    // Aquí se enviaría el buffer binario a través de ENetPeer
    return true;
}

void NetworkManager::broadcast(Protocol::Channel channel, const BitStream& stream, bool reliable, PlayerId ignore) {
    for (const auto& [id, player] : m_server.getPlayerManager().getAllPlayers()) {
        if (id != ignore) {
            sendPacket(id, channel, stream, reliable);
        }
    }
}

void NetworkManager::processPacket(PlayerId senderId, Protocol::Channel channel, const uint8_t* data, size_t size) {
    if (size < sizeof(uint16_t)) return;

    BitStream bs(data, size);
    auto packetId = static_cast<Protocol::PacketId>(bs.read<uint16_t>());

    switch (packetId) {
        case Protocol::PacketId::HandshakeRequest: {
            std::string playerName = bs.readString();
            ModelHash model = bs.read<ModelHash>();
            m_server.handlePlayerHandshake(senderId, playerName, model);
            break;
        }
        case Protocol::PacketId::PlayerSyncData: {
            Protocol::PlayerSyncPacket sync = bs.read<Protocol::PlayerSyncPacket>();
            m_server.handlePlayerSync(senderId, sync);
            break;
        }
        case Protocol::PacketId::TriggerServerEvent: {
            std::string eventName = bs.readString();
            uint16_t argCount = bs.read<uint16_t>();
            ScriptEventArgs args;
            for (uint16_t i = 0; i < argCount; ++i) {
                args.push_back(bs.readString());
            }
            m_server.handleClientEvent(senderId, eventName, args);
            break;
        }
        case Protocol::PacketId::ChatMessage: {
            std::string msg = bs.readString();
            m_server.handleChatMessage(senderId, msg);
            break;
        }
        default:
            break;
    }
}

} // namespace Frontier::Server
