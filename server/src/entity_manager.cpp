#include "server/entity_manager.hpp"

namespace Frontier::Server {

std::shared_ptr<NetworkEntity> EntityManager::createEntity(EntityType type, ModelHash model, const Vector3& pos, float heading, PlayerId owner) {
    auto entity = std::make_shared<NetworkEntity>();
    entity->id = m_nextEntityId++;
    entity->type = type;
    entity->model = model;
    entity->position = pos;
    entity->heading = heading;
    entity->owner = owner;

    m_entities[entity->id] = entity;
    return entity;
}

void EntityManager::destroyEntity(EntityId id) {
    m_entities.erase(id);
}

std::shared_ptr<NetworkEntity> EntityManager::getEntity(EntityId id) const {
    auto it = m_entities.find(id);
    if (it != m_entities.end()) {
        return it->second;
    }
    return nullptr;
}

} // namespace Frontier::Server
