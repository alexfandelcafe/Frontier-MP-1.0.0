#include "core/player_factory.hpp"
#include "core/native_invoker.hpp"
#include "core/native_hashes.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>


namespace Frontier::Core {

namespace {
std::atomic<bool> s_spawnPending{false};
std::atomic<bool> s_modelRequested{false};
Vector3 s_pendingPosition{};
float s_pendingHeading{0.0f};
ModelHash s_pendingModel{0};
std::atomic<uintptr_t> s_localActor{0};
uint32_t s_spawnPollCounter{0};
}

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

void PlayerFactory::requestLocalPlayerSpawn(const Vector3& position, float heading, ModelHash modelHash) {
    s_localActor.store(0, std::memory_order_release);
    s_pendingPosition = position;
    s_pendingHeading = heading;
    s_pendingModel = modelHash;
    s_modelRequested.store(false, std::memory_order_release);
    s_spawnPending.store(true, std::memory_order_release);
    s_spawnPollCounter = 0;
    std::cout << "[PlayerFactory] Spawn local solicitado; esperando transición de mundo y 'PlayerLayout'." << std::endl;
}

void PlayerFactory::processPendingSpawn() {
    if (!s_spawnPending.load(std::memory_order_acquire)) return;
    if (!NativeInvoker::isReady()) return;

    ++s_spawnPollCounter;
    uintptr_t layout = getPlayerLayout();
    if (!layout) {
        if ((s_spawnPollCounter % 120) == 0) {
            std::cout << "[PlayerFactory] Esperando 'PlayerLayout'..." << std::endl;
        }
        return;
    }

    if (!s_modelRequested.load(std::memory_order_acquire)) {
        NativeInvoker::invoke<void>(Natives::STREAMING_REQUEST_ACTOR, s_pendingModel);
        s_modelRequested.store(true, std::memory_order_release);
        std::cout << "[PlayerFactory] PlayerLayout listo; solicitando modelo 0x"
                  << std::hex << s_pendingModel << std::dec << "." << std::endl;
    }

    if (!NativeInvoker::invoke<bool>(Natives::STREAMING_IS_ACTOR_LOADED, s_pendingModel)) {
        return;
    }

    uintptr_t actor =
        s_localActor.load(std::memory_order_acquire);

    if (!actor) {
        std::cout
            << "[PlayerFactory] Mundo gameplay listo; creando jugador local..."
            << std::endl;

        actor = NativeInvoker::invoke<uintptr_t>(
            Natives::CREATE_PLAYER_ACTOR_IN_LAYOUT,
            layout,
            s_pendingModel,
            s_pendingPosition.x,
            s_pendingPosition.y,
            s_pendingPosition.z,
            s_pendingHeading
        );

        if (!actor) {
            std::cerr
                << "[PlayerFactory] CREATE_PLAYER_ACTOR_IN_LAYOUT no devolvió actor."
                << std::endl;
            return;
        }

        s_localActor.store(
            actor,
            std::memory_order_release);

        enablePlayerControl(true);

        std::cout
            << "[PlayerFactory] Local player actor creado (Ptr: 0x"
            << std::hex << actor << std::dec
            << "). Esperando GET_PLAYER_ACTOR..."
            << std::endl;
    }

    // Match the original InitSpawn completion condition:
    // GET_LOCAL_SLOT() -> GET_PLAYER_ACTOR(slot).
    const int localSlot =
        NativeInvoker::invoke<int>(
            Natives::GET_LOCAL_SLOT);

    if (localSlot < 0) {
        return;
    }

    const uintptr_t playerActor =
        NativeInvoker::invoke<uintptr_t>(
            Natives::GET_PLAYER_ACTOR,
            localSlot);

    if (!playerActor) {
        if ((s_spawnPollCounter % 120) == 0) {
            std::cout
                << "[PlayerFactory] Esperando GET_PLAYER_ACTOR..."
                << std::endl;
        }
        return;
    }

    s_localActor.store(
        playerActor,
        std::memory_order_release);

    s_spawnPending.store(
        false,
        std::memory_order_release);
    s_modelRequested.store(
        false,
        std::memory_order_release);

    std::cout
        << "[PlayerFactory] GET_PLAYER_ACTOR confirmó actor local: 0x"
        << std::hex << playerActor
        << std::dec << "."
        << std::endl;
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
    return s_localActor.load(
        std::memory_order_acquire);
}

bool PlayerFactory::isLocalPlayerReady() {
    return s_localActor.load(
        std::memory_order_acquire) != 0 &&
           !s_spawnPending.load(std::memory_order_acquire);
}

void PlayerFactory::enablePlayerControl(bool enable) {
    NativeInvoker::invoke<void>(Natives::SET_PLAYER_CONTROL, -1, enable);
}

} // namespace Frontier::Core
