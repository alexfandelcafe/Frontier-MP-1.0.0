#include "server/player_manager.hpp"

namespace Frontier::Server {

PlayerId PlayerManager::allocateId() {
    for (PlayerId id = 0; id < MAX_PLAYERS_LIMIT; ++id) {
        if (m_players.find(id) == m_players.end()) {
            return id;
        }
    }
    return INVALID_PLAYER_ID;
}

std::shared_ptr<Player> PlayerManager::addPlayer(void* peer, const std::string& name) {
    PlayerId id = allocateId();
    if (id == INVALID_PLAYER_ID) return nullptr;

    auto player = std::make_shared<Player>();
    player->id = id;
    player->name = name.empty() ? ("Outlaw_" + std::to_string(id)) : name;
    player->peer = peer;

    m_players[id] = player;
    m_peerToId[peer] = id;
    return player;
}

void PlayerManager::removePlayer(PlayerId id) {
    auto it = m_players.find(id);
    if (it != m_players.end()) {
        m_peerToId.erase(it->second->peer);
        m_players.erase(it);
    }
}

std::shared_ptr<Player> PlayerManager::getPlayer(PlayerId id) const {
    auto it = m_players.find(id);
    if (it != m_players.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<Player> PlayerManager::getPlayerByPeer(void* peer) const {
    auto it = m_peerToId.find(peer);
    if (it != m_peerToId.end()) {
        return getPlayer(it->second);
    }
    return nullptr;
}

} // namespace Frontier::Server
