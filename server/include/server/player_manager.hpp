#pragma once

#include "shared/types.hpp"
#include "shared/protocol.hpp"
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

namespace Frontier::Server {

struct Player {
    PlayerId id{INVALID_PLAYER_ID};
    std::string name;
    void* peer{nullptr}; // Pointer to ENetPeer
    uint32_t ping{0};
    bool isSpawned{false};

    // Estado físico y de sincronización
    Vector3 position{0, 0, 0};
    Vector3 velocity{0, 0, 0};
    float heading{0.0f};
    float pitch{0.0f};
    uint16_t health{100};
    uint32_t currentWeaponHash{0};
    ModelHash modelHash{0};
    EntityId mountEntityId{INVALID_ENTITY_ID};
    uint8_t flags{0};

    // Tiempos
    uint64_t lastSyncTime{0};
};

class PlayerManager {
public:
    PlayerManager() = default;

    PlayerId allocateId();
    std::shared_ptr<Player> addPlayer(void* peer, const std::string& name);
    void removePlayer(PlayerId id);

    std::shared_ptr<Player> getPlayer(PlayerId id) const;
    std::shared_ptr<Player> getPlayerByPeer(void* peer) const;
    const std::unordered_map<PlayerId, std::shared_ptr<Player>>& getAllPlayers() const { return m_players; }

    size_t getPlayerCount() const { return m_players.size(); }

private:
    std::unordered_map<PlayerId, std::shared_ptr<Player>> m_players;
    std::unordered_map<void*, PlayerId> m_peerToId;
    PlayerId m_nextId{0};
};

} // namespace Frontier::Server
