#include "core/player_factory.hpp"
#include "core/native_invoker.hpp"
#include "core/native_hashes.hpp"
#include <iostream>
#include <thread>
#include <chrono>

namespace Frontier::Core {

bool PlayerFactory::initialize() {
    std::cout << "[PlayerFactory] Initialized actor lifecycle manager." << std::endl;
    return true;
}

uintptr_t PlayerFactory::getPlayerLayout() {
    return NativeInvoker::invoke<uintptr_t>(Natives::FIND_NAMED_LAYOUT, "PlayerLayout");
}

bool PlayerFactory::requestAndStreamModel(ModelHash modelHash, uint32_t timeoutMs) {
    NativeInvoker::invoke<void>(Natives::STREAMING_REQUEST_ACTOR, modelHash);

    auto start = std::chrono::steady_clock::now();
    while (!NativeInvoker::invoke<bool>(Natives::STREAMING_IS_ACTOR_LOADED, modelHash)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > timeoutMs) {
            std::cerr << "[PlayerFactory] Timed out waiting for actor model 0x" << std::hex << modelHash << std::dec << std::endl;
            return false;
        }
    }
    return true;
}

uintptr_t PlayerFactory::spawnLocalPlayer(const Vector3& position, float heading, ModelHash modelHash) {
    uintptr_t layout = 0;
    for (int retry = 0; retry < 25; ++retry) {
        layout = getPlayerLayout();
        if (layout) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!layout) {
        std::cerr << "[PlayerFactory] Failed to find 'PlayerLayout' (world layout not ready yet)." << std::endl;
        return 0;
    }

    if (!requestAndStreamModel(modelHash)) {
        return 0;
    }

    std::cout << "[PlayerFactory] Spawning local player actor at (" 
              << position.x << ", " << position.y << ", " << position.z << ")..." << std::endl;

    // Ejecución de la nativa clave que resuelve la transición
    uintptr_t actor = NativeInvoker::invoke<uintptr_t>(
        Natives::CREATE_PLAYER_ACTOR_IN_LAYOUT,
        layout,
        modelHash,
        position.x,
        position.y,
        position.z,
        heading
    );

    if (actor) {
        enablePlayerControl(true);
        std::cout << "[PlayerFactory] Local player actor spawned successfully (Ptr: 0x" 
                  << std::hex << actor << std::dec << ")." << std::endl;
    }

    return actor;
}

uintptr_t PlayerFactory::spawnRemotePlayer(ModelHash modelHash, const Vector3& position, float heading) {
    uintptr_t layout = getPlayerLayout();
    if (!layout) return 0;

    if (!requestAndStreamModel(modelHash)) {
        return 0;
    }

    uintptr_t actor = NativeInvoker::invoke<uintptr_t>(
        Natives::CREATE_ACTOR_IN_LAYOUT,
        layout,
        modelHash,
        position.x,
        position.y,
        position.z,
        heading
    );

    return actor;
}

void PlayerFactory::destroyActor(uintptr_t actorPtr) {
    if (!actorPtr) return;
    NativeInvoker::invoke<void>(Natives::KILL_ACTOR, actorPtr);
}

uintptr_t PlayerFactory::getLocalPlayerActor() {
    return NativeInvoker::invoke<uintptr_t>(Natives::GET_PLAYER_ACTOR, -1);
}

void PlayerFactory::enablePlayerControl(bool enable) {
    NativeInvoker::invoke<void>(Natives::SET_PLAYER_CONTROL, -1, enable);
}

} // namespace Frontier::Core
