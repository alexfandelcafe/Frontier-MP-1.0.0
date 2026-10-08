#include "server/network_manager.hpp"
#include "server/server.hpp"

#include <enet/enet.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace Frontier::Server {

namespace {

constexpr size_t kMaxIncomingPacket = 64 * 1024;
constexpr const char* kProtocolVersion = "Alpha vpre-0.0.5";

bool ensureEnetInitialized() {
    static const bool initialized = (enet_initialize() == 0);
    return initialized;
}

} // namespace

NetworkManager::NetworkManager(Server& server)
    : m_server(server) {}

NetworkManager::~NetworkManager() {
    stop();
}

bool NetworkManager::start(
    const std::string& bindHost,
    uint16_t port,
    uint32_t maxClients)
{
    if (m_running) {
        return true;
    }

    if (!ensureEnetInitialized()) {
        std::cerr
            << "[NetworkManager] ENet initialization failed."
            << std::endl;
        return false;
    }

    m_host = bindHost;
    m_port = port;
    m_maxClients = maxClients;

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    // ENetHost recibe cuatro canales: reliable handshake/eventos, sync, etc.
    ENetHost* host = enet_host_create(
        &address,
        maxClients,
        Protocol::ChannelCount,
        0,
        0);

    if (!host) {
        std::cerr
            << "[NetworkManager] No se pudo crear ENetHost en "
            << bindHost << ":" << port << std::endl;
        return false;
    }

    // If a concrete address was supplied, log it while still binding INADDR_ANY
    // so local and remote clients work identically.
    m_enetHost = host;
    m_running = true;

    std::cout
        << "[NetworkManager] ENet UDP Listener READY"
        << " | bind=" << bindHost
        << " | port=" << port
        << " | maxPlayers=" << maxClients
        << " | protocolVersion=Alpha vpre-0.0.5"
        << std::endl;

    return true;
}

void NetworkManager::stop() {
    if (!m_running && !m_enetHost) {
        return;
    }

    m_running = false;

    for (auto& [peer, state] : m_peers) {
        if (peer) {
            enet_peer_disconnect(
                reinterpret_cast<ENetPeer*>(peer),
                0);
        }
    }

    if (m_enetHost) {
        ENetHost* host =
            reinterpret_cast<ENetHost*>(m_enetHost);

        enet_host_flush(host);
        enet_host_destroy(host);
        m_enetHost = nullptr;
    }

    m_peers.clear();

    std::cout
        << "[NetworkManager] Network stopped."
        << std::endl;
}

PlayerId NetworkManager::ensurePlayerForPeer(
    void* peer,
    const std::string& name)
{
    if (!peer) {
        return INVALID_PLAYER_ID;
    }

    auto existing =
        m_server.getPlayerManager().getPlayerByPeer(peer);

    if (existing) {
        return existing->id;
    }

    auto player =
        m_server.getPlayerManager().addPlayer(
            peer,
            name);

    if (!player) {
        return INVALID_PLAYER_ID;
    }

    auto state =
        std::make_unique<PeerState>();

    state->peer = peer;
    state->playerId = player->id;
    m_peers[peer] = std::move(state);

    return player->id;
}

void NetworkManager::update(uint32_t timeoutMs) {
    if (!m_running || !m_enetHost) {
        return;
    }

    ENetHost* host =
        reinterpret_cast<ENetHost*>(m_enetHost);

    ENetEvent event{};

    for (;;) {
        const int result =
            enet_host_service(host, &event, timeoutMs);

        timeoutMs = 0;

        if (result < 0) {
            std::cerr
                << "[NetworkManager] enet_host_service failed."
                << std::endl;
            return;
        }

        if (result == 0) {
            return;
        }

        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                std::cout
                    << "[NetworkManager] ENet peer connected."
                    << std::endl;
                break;
            }

            case ENET_EVENT_TYPE_RECEIVE: {
                if (!event.packet ||
                    event.packet->dataLength == 0 ||
                    event.packet->dataLength > kMaxIncomingPacket) {

                    if (event.packet) {
                        enet_packet_destroy(event.packet);
                    }
                    break;
                }

                const Protocol::Channel channel =
                    static_cast<Protocol::Channel>(
                        event.channelID);

                // HandshakeRequest creates the Player object because the
                // ENet connect event itself contains no player identity.
                PlayerId senderId =
                    INVALID_PLAYER_ID;

                auto existing =
                    m_server.getPlayerManager().getPlayerByPeer(
                        event.peer);

                if (existing) {
                    senderId = existing->id;
                }

                processPacket(
                    senderId,
                    reinterpret_cast<void*>(event.peer),
                    channel,
                    event.packet->data,
                    event.packet->dataLength);

                enet_packet_destroy(event.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT: {
                void* peer =
                    reinterpret_cast<void*>(event.peer);

                auto player =
                    m_server.getPlayerManager().getPlayerByPeer(
                        peer);

                if (player) {
                    m_server.handlePlayerDisconnect(
                        player->id,
                        "enet_disconnect");
                }

                m_peers.erase(peer);

                std::cout
                    << "[NetworkManager] ENet peer disconnected."
                    << std::endl;
                break;
            }

            default:
                break;
        }
    }
}

bool NetworkManager::sendPacket(
    PlayerId target,
    Protocol::Channel channel,
    const BitStream& stream,
    bool reliable)
{
    if (!m_enetHost) {
        return false;
    }

    auto player =
        m_server.getPlayerManager().getPlayer(target);

    if (!player || !player->peer) {
        return false;
    }

    const enet_uint32 flags =
        reliable
            ? ENET_PACKET_FLAG_RELIABLE
            : 0;

    ENetPacket* packet =
        enet_packet_create(
            stream.data(),
            stream.size(),
            flags);

    if (!packet) {
        return false;
    }

    ENetPeer* peer =
        reinterpret_cast<ENetPeer*>(player->peer);

    if (enet_peer_send(
            peer,
            static_cast<enet_uint8>(channel),
            packet) != 0) {

        enet_packet_destroy(packet);
        return false;
    }

    enet_host_flush(
        reinterpret_cast<ENetHost*>(m_enetHost));

    return true;
}

void NetworkManager::broadcast(
    Protocol::Channel channel,
    const BitStream& stream,
    bool reliable,
    PlayerId ignore)
{
    for (const auto& [id, player] :
         m_server.getPlayerManager().getAllPlayers()) {

        if (id != ignore) {
            sendPacket(
                id,
                channel,
                stream,
                reliable);
        }
    }
}

void NetworkManager::processPacket(
    PlayerId senderId,
    void* peer,
    Protocol::Channel channel,
    const uint8_t* data,
    size_t size)
{
    (void)channel;

    if (size < sizeof(uint16_t) || !data) {
        return;
    }

    try {
        BitStream bs(data, size);

        const auto packetId =
            static_cast<Protocol::PacketId>(
                bs.read<uint16_t>());

        std::cout
            << "[NetworkManager] RX packet id=0x"
            << std::hex
            << static_cast<uint16_t>(packetId)
            << std::dec
            << " channel="
            << static_cast<uint32_t>(channel)
            << " bytes="
            << size
            << std::endl;

        switch (packetId) {
            case Protocol::PacketId::HandshakeRequest: {
                // senderId is INVALID for the first packet from a new peer.
                const std::string playerName =
                    bs.readString();

                const ModelHash model =
                    bs.read<ModelHash>();

                if (senderId == INVALID_PLAYER_ID) {
                    senderId = ensurePlayerForPeer(
                        peer,
                        playerName);
                }

                if (senderId == INVALID_PLAYER_ID) {
                    std::cerr
                        << "[NetworkManager] No se pudo asociar HandshakeRequest "
                           "con un peer ENet."
                        << std::endl;
                    break;
                }

                std::cout
                    << "[NetworkManager] HandshakeRequest OK: player='"
                    << playerName
                    << "' id="
                    << senderId
                    << " model=0x"
                    << std::hex
                    << model
                    << std::dec
                    << std::endl;

                m_server.handlePlayerHandshake(
                    senderId,
                    playerName,
                    model);
                break;
            }

            case Protocol::PacketId::ClientWelcome: {
                const ModelHash model =
                    bs.read<ModelHash>();
                const std::string name =
                    bs.readString();
                const Vector3 position =
                    bs.readVector3();
                Vector3 rotation{};
                rotation.x = bs.read<float>();
                rotation.y = bs.read<float>();
                rotation.z = bs.read<float>();

                if (senderId != INVALID_PLAYER_ID) {
                    m_server.handleClientWelcome(
                        senderId,
                        model,
                        name,
                        position,
                        rotation);
                }
                break;
            }

            case Protocol::PacketId::PlayerSyncData: {
                if (senderId == INVALID_PLAYER_ID) {
                    break;
                }

                const auto sync =
                    bs.read<Protocol::PlayerSyncPacket>();

                m_server.handlePlayerSync(
                    senderId,
                    sync);
                break;
            }

            case Protocol::PacketId::TriggerServerEvent: {
                if (senderId == INVALID_PLAYER_ID) {
                    break;
                }

                const std::string eventName =
                    bs.readString();

                const uint16_t argCount =
                    bs.read<uint16_t>();

                ScriptEventArgs args;
                for (uint16_t i = 0;
                     i < argCount;
                     ++i) {

                    args.push_back(
                        bs.readString());
                }

                m_server.handleClientEvent(
                    senderId,
                    eventName,
                    args);
                break;
            }

            case Protocol::PacketId::ChatMessage: {
                if (senderId == INVALID_PLAYER_ID) {
                    break;
                }

                const std::string msg =
                    bs.readString();

                m_server.handleChatMessage(
                    senderId,
                    msg);
                break;
            }

            default:
                break;
        }
    } catch (const std::exception& ex) {
        std::cerr
            << "[NetworkManager] Packet parse error: "
            << ex.what()
            << std::endl;
    }
}

} // namespace Frontier::Server
