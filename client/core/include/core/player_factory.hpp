#pragma once

#include "shared/types.hpp"
#include <cstdint>

namespace Frontier::Core {

class PlayerFactory {
public:
    static bool initialize();

    // Ciclo de vida resuelto: Frontend -> Online Transition
    static uintptr_t getPlayerLayout();
    static bool requestAndStreamModel(ModelHash modelHash, uint32_t timeoutMs = 5000);
    static uintptr_t spawnLocalPlayer(const Vector3& position, float heading, ModelHash modelHash);

    // Programa el spawn para ejecutarlo desde el hilo de scripts del juego,
    // evitando bloquear el callback de UI mientras PlayerLayout/cachés aún cargan.
    static void requestLocalPlayerSpawn(const Vector3& position, float heading, ModelHash modelHash);
    static void processPendingSpawn();
    static uintptr_t spawnRemotePlayer(ModelHash modelHash, const Vector3& position, float heading);
    static void destroyActor(uintptr_t actorPtr);

    static uintptr_t getLocalPlayerActor();
    static bool isLocalPlayerReady();
    static void enablePlayerControl(bool enable);
};

} // namespace Frontier::Core
