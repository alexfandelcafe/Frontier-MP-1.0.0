#pragma once

#include "shared/types.hpp"
#include "shared/protocol.hpp"
#include "shared/bitstream.hpp"
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>

namespace Frontier::Server {

class Server;

class NetworkManager {
public:
    explicit NetworkManager(Server& server);
    ~NetworkManager();

    bool start(const std::string& bindHost, uint16_t port, uint32_t maxClients);
    void stop();
    void update(uint32_t timeoutMs = 0);

    bool sendPacket(PlayerId target, Protocol::Channel channel, const BitStream& stream, bool reliable);
    void broadcast(Protocol::Channel channel, const BitStream& stream, bool reliable, PlayerId ignore = INVALID_PLAYER_ID);

    bool isRunning() const { return m_running; }
    uint16_t getPort() const { return m_port; }

private:
    void processPacket(
        PlayerId senderId,
        void* peer,
        Protocol::Channel channel,
        const uint8_t* data,
        size_t size);
    PlayerId ensurePlayerForPeer(void* peer, const std::string& name = {});


    Server& m_server;
    std::string m_host;
    uint16_t m_port{DEFAULT_SERVER_PORT};
    uint32_t m_maxClients{MAX_PLAYERS_LIMIT};
    bool m_running{false};
    void* m_enetHost{nullptr}; // ENetHost*
    struct PeerState {
        void* peer{nullptr}; // ENetPeer*
        PlayerId playerId{INVALID_PLAYER_ID};
    };
    std::unordered_map<void*, std::unique_ptr<PeerState>> m_peers;
};

} // namespace Frontier::Server
