#pragma once

#include "shared/types.hpp"
#include "shared/protocol.hpp"
#include "shared/bitstream.hpp"
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>

namespace Frontier::Net {

struct RemotePlayer {
    PlayerId id{INVALID_PLAYER_ID};
    std::string name;
    ModelHash model{0};
    uintptr_t actorPtr{0};

    Vector3 targetPosition{0, 0, 0};
    Vector3 currentPosition{0, 0, 0};
    float targetHeading{0.0f};
    float currentHeading{0.0f};
    uint16_t health{100};
    uint32_t weaponHash{0};
    uint8_t flags{0};
};

class ClientNetwork {
public:
    static ClientNetwork& get();

    bool connect(const std::string& host, uint16_t port, const std::string& playerName);
    void disconnect();
    void update();

    void sendPacket(Protocol::Channel channel, const BitStream& bs, bool reliable);
    void sendPlayerSync(const Protocol::PlayerSyncPacket& packet);
    void sendChatMessage(const std::string& message);
    void triggerServerEvent(const std::string& eventName, const std::vector<std::string>& args);

    bool isConnected() const { return m_connected; }
    PlayerId getLocalPlayerId() const { return m_localPlayerId; }

    const std::unordered_map<PlayerId, RemotePlayer>& getRemotePlayers() const { return m_remotePlayers; }

private:
    ClientNetwork() = default;
    ~ClientNetwork() = default;

    void processPacket(Protocol::Channel channel, const uint8_t* data, size_t size);

    bool m_connected{false};
    PlayerId m_localPlayerId{INVALID_PLAYER_ID};
    std::string m_playerName;
    std::unordered_map<PlayerId, RemotePlayer> m_remotePlayers;
};

} // namespace Frontier::Net
