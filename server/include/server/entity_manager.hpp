#pragma once

#include "shared/types.hpp"
#include <unordered_map>
#include <memory>

namespace Frontier::Server {

struct NetworkEntity {
    EntityId id{INVALID_ENTITY_ID};
    EntityType type{EntityType::Object};
    ModelHash model{0};
    Vector3 position{0, 0, 0};
    Vector3 velocity{0, 0, 0};
    float heading{0.0f};
    PlayerId owner{INVALID_PLAYER_ID}; // El jugador con autoridad de sincronización
};

class EntityManager {
public:
    EntityManager() = default;

    std::shared_ptr<NetworkEntity> createEntity(EntityType type, ModelHash model, const Vector3& pos, float heading, PlayerId owner);
    void destroyEntity(EntityId id);
    std::shared_ptr<NetworkEntity> getEntity(EntityId id) const;

    const std::unordered_map<EntityId, std::shared_ptr<NetworkEntity>>& getAllEntities() const { return m_entities; }

private:
    std::unordered_map<EntityId, std::shared_ptr<NetworkEntity>> m_entities;
    EntityId m_nextEntityId{100};
};

} // namespace Frontier::Server
