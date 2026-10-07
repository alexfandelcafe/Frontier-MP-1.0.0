#include "net/client_net.hpp"
#include "core/player_factory.hpp"
#include "ui/cef_manager.hpp"
#include <iostream>

namespace Frontier::Net {

ClientNetwork& ClientNetwork::get() {
    static ClientNetwork instance;
    return instance;
}

bool ClientNetwork::connect(const std::string& host, uint16_t port, const std::string& playerName) {
    m_playerName = playerName;
    std::cout << "[ClientNetwork] Connecting to " << host << ":" << port << " as '" << playerName << "'..." << std::endl;

    // Simular handshake
    m_connected = true;
    m_localPlayerId = 0;

    // Enviar HandshakeRequest
    BitStream bs;
    bs.write<uint16_t>(static_cast<uint16_t>(Protocol::PacketId::HandshakeRequest));
    bs.writeString(playerName);
    bs.write<ModelHash>(837); // John Marston por defecto
    sendPacket(Protocol::ChannelReliable, bs, true);

    std::cout << "[ClientNetwork] Connection established. Assigned PlayerID: " << m_localPlayerId << std::endl;
    return true;
}

void ClientNetwork::disconnect() {
    if (!m_connected) return;
    std::cout << "[ClientNetwork] Disconnecting from server..." << std::endl;
    m_connected = false;
    m_remotePlayers.clear();
}

void ClientNetwork::update() {
    if (!m_connected) return;
    // Bucle de enet_host_service para el cliente
}

void ClientNetwork::sendPacket(Protocol::Channel channel, const BitStream& bs, bool reliable) {
    // Envío UDP por ENet
}

void ClientNetwork::sendPlayerSync(const Protocol::PlayerSyncPacket& packet) {
    BitStream bs;
    bs.write<uint16_t>(static_cast<uint16_t>(Protocol::PacketId::PlayerSyncData));
    bs.write<Protocol::PlayerSyncPacket>(packet);
    sendPacket(Protocol::ChannelSync, bs, false);
}

void ClientNetwork::sendChatMessage(const std::string& message) {
    BitStream bs;
    bs.write<uint16_t>(static_cast<uint16_t>(Protocol::PacketId::ChatMessage));
    bs.writeString(message);
    sendPacket(Protocol::ChannelReliable, bs, true);
}

void ClientNetwork::triggerServerEvent(const std::string& eventName, const std::vector<std::string>& args) {
    BitStream bs;
    bs.write<uint16_t>(static_cast<uint16_t>(Protocol::PacketId::TriggerServerEvent));
    bs.writeString(eventName);
    bs.write<uint16_t>(static_cast<uint16_t>(args.size()));
    for (const auto& a : args) {
        bs.writeString(a);
    }
    sendPacket(Protocol::ChannelEvents, bs, true);
}

void ClientNetwork::processPacket(Protocol::Channel channel, const uint8_t* data, size_t size) {
    if (size < sizeof(uint16_t)) return;

    BitStream bs(data, size);
    auto packetId = static_cast<Protocol::PacketId>(bs.read<uint16_t>());

    switch (packetId) {
        case Protocol::PacketId::HandshakeResponse: {
            m_localPlayerId = bs.read<PlayerId>();
            std::string serverName = bs.readString();
            std::cout << "[ClientNetwork] Joined server: " << serverName << std::endl;

            // Ocultar menú principal y spawnear actor
            UI::CefManager::get().setMainMenuVisible(false);
            Core::PlayerFactory::spawnLocalPlayer(Vector3(-180.0f, 60.0f, 1950.0f), 0.0f, 837);
            break;
        }
        case Protocol::PacketId::PlayerSyncData: {
            Protocol::PlayerSyncPacket sync = bs.read<Protocol::PlayerSyncPacket>();
            if (sync.playerId != m_localPlayerId) {
                auto& remote = m_remotePlayers[sync.playerId];
                remote.targetPosition = sync.position;
                remote.targetHeading = sync.heading;
                remote.health = sync.health;
                remote.weaponHash = sync.currentWeaponHash;
            }
            break;
        }
        case Protocol::PacketId::ChatMessage: {
            std::string msg = bs.readString();
            UI::CefManager::get().sendChatMessageToUi(msg);
            break;
        }
        case Protocol::PacketId::PlayerJoined: {
            PlayerId id = bs.read<PlayerId>();
            std::string name = bs.readString();
            std::cout << "[ClientNetwork] Remote player joined: " << name << " (" << id << ")" << std::endl;
            break;
        }
        case Protocol::PacketId::PlayerLeft: {
            PlayerId id = bs.read<PlayerId>();
            auto it = m_remotePlayers.find(id);
            if (it != m_remotePlayers.end()) {
                if (it->second.actorPtr) {
                    Core::PlayerFactory::destroyActor(it->second.actorPtr);
                }
                m_remotePlayers.erase(it);
            }
            break;
        }
        default:
            break;
    }
}

} // namespace Frontier::Net
