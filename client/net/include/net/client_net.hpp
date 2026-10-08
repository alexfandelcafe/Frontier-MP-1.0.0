#pragma once

#include "shared/types.hpp"
#include "shared/protocol.hpp"
#include "shared/bitstream.hpp"
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <filesystem>
#include <chrono>

struct _ENetHost;
struct _ENetPeer;

using ENetHost = _ENetHost;
using ENetPeer = _ENetPeer;

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
    ~ClientNetwork();

    void processPacket(Protocol::Channel channel, const uint8_t* data, size_t size);
    void beginServerDataLoading();
    void finishWorldLoadIfReady();

    bool m_connected{false};
    PlayerId m_localPlayerId{INVALID_PLAYER_ID};
    std::string m_playerName;
    std::string m_serverHost;
    uint16_t m_serverPort{0};
    uint16_t m_httpPort{4674};

    ENetHost* m_enetHost{nullptr};
    ENetPeer* m_enetPeer{nullptr};

    std::atomic<bool> m_resourceLoading{false};
    // resourcesReady means the equivalent of DownloadResources +
    // ResourcesManager::LoadAllResources + DoesAllResourcesAreLoaded.
    std::atomic<bool> m_resourcesReady{false};
    std::atomic<bool> m_resourceFailed{false};
    std::atomic<bool> m_loadResourcesStarted{false};
    std::atomic<bool> m_loadingScreenVisible{false};
    std::atomic<bool> m_clientWelcomeSent{false};
    std::atomic<bool> m_worldTransitionRequested{false};
    std::atomic<bool> m_localPlayerObserved{false};

    std::thread m_resourceThread;
    std::filesystem::path m_cacheRoot;
    std::chrono::steady_clock::time_point m_clientWelcomeDeadline{};

    Vector3 m_spawnPosition{-180.0f, 60.0f, 1950.0f};
    float m_spawnHeading{0.0f};
    ModelHash m_spawnModel{837};

    std::unordered_map<PlayerId, RemotePlayer> m_remotePlayers;
};

} // namespace Frontier::Net
