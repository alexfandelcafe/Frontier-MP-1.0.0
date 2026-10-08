#include "net/client_net.hpp"
#include "net/resource_client.hpp"
#include "core/player_factory.hpp"
#include "core/engine_hooks.hpp"
#include "ui/cef_manager.hpp"
#include "ui/d3d11_renderer.hpp"

#include <enet/enet.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace Frontier::Net {

namespace {

constexpr uint32_t kEnetChannels = 4;
constexpr uint32_t kConnectTimeoutMs = 5000;
constexpr const char* kProtocolVersion = "Alpha vpre-0.0.5";

bool ensureEnetInitialized() {
    static const bool initialized = (enet_initialize() == 0);
    return initialized;
}

} // namespace

ClientNetwork& ClientNetwork::get() {
    static ClientNetwork instance;
    return instance;
}

ClientNetwork::~ClientNetwork() {
    disconnect();
    if (m_networkThread.joinable()) {
        m_networkThread.join();
    }
    if (m_resourceThread.joinable()) {
        m_resourceThread.join();
    }
}

bool ClientNetwork::connect(
    const std::string& host,
    uint16_t port,
    const std::string& playerName)
{
    if (m_connected.load(std::memory_order_acquire) ||
        m_connecting.load(std::memory_order_acquire) ||
        m_connectRequested.load(std::memory_order_acquire)) {

        std::cout
            << "[ClientNetwork] Connect ignored: "
               "ya existe una conexión o intento pendiente."
            << std::endl;
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(m_connectionRequestMutex);
        m_requestedHost = host;
        m_requestedPort = port;
        m_requestedPlayerName = playerName;
    }

    m_connectRequested.store(true, std::memory_order_release);
    startNetworkThread();

    std::cout
        << "[ClientNetwork] ENet connection request queued for "
        << host << ":" << port
        << " as '" << playerName
        << "'; dedicated ENet thread will service transport."
        << std::endl;

    return true;
}
void ClientNetwork::startPendingConnection() {
    if (!m_connectRequested.exchange(
            false,
            std::memory_order_acq_rel)) {
        return;
    }

    std::string host;
    std::string playerName;
    uint16_t port = 0;

    {
        std::lock_guard<std::mutex> lock(m_connectionRequestMutex);
        host = m_requestedHost;
        port = m_requestedPort;
        playerName = m_requestedPlayerName;
    }

    if (host.empty() || port == 0 || playerName.empty()) {
        std::cerr
            << "[ClientNetwork] Invalid pending connection request."
            << std::endl;
        return;
    }

    if (!ensureEnetInitialized()) {
        std::cerr
            << "[ClientNetwork] ENet initialization failed."
            << std::endl;
        return;
    }

    m_playerName = playerName;
    m_serverHost = host;
    m_serverPort = port;
    m_httpPort = port;

    m_localPlayerId = INVALID_PLAYER_ID;
    m_resourcesReady.store(false, std::memory_order_release);
    m_resourceFailed.store(false, std::memory_order_release);
    m_resourceLoading.store(false, std::memory_order_release);
    m_loadResourcesStarted.store(false, std::memory_order_release);
    m_clientWelcomeSent.store(false, std::memory_order_release);
    m_worldTransitionRequested.store(false, std::memory_order_release);
    m_localPlayerObserved.store(false, std::memory_order_release);
    m_clientWelcomeDeadline = {};

    char modulePath[MAX_PATH] = {};
    HMODULE frontierModule = GetModuleHandleA("frontier_core.dll");
    if (frontierModule &&
        GetModuleFileNameA(frontierModule, modulePath, MAX_PATH)) {
        m_cacheRoot =
            fs::path(modulePath).parent_path() / "cache";
    } else {
        m_cacheRoot = fs::current_path() / "cache";
    }

    m_enetHost = enet_host_create(
        nullptr,
        1,
        kEnetChannels,
        0,
        0);

    if (!m_enetHost) {
        std::cerr
            << "[ClientNetwork] No se pudo crear el ENetHost cliente."
            << std::endl;
        return;
    }

    ENetAddress address{};
    if (enet_address_set_host(&address, host.c_str()) != 0) {
        std::cerr
            << "[ClientNetwork] No se pudo resolver el host: "
            << host
            << std::endl;
        enet_host_destroy(m_enetHost);
        m_enetHost = nullptr;
        return;
    }

    address.port = port;

    m_enetPeer = enet_host_connect(
        m_enetHost,
        &address,
        kEnetChannels,
        0);

    if (!m_enetPeer) {
        std::cerr
            << "[ClientNetwork] ENet no pudo crear el peer."
            << std::endl;
        enet_host_destroy(m_enetHost);
        m_enetHost = nullptr;
        return;
    }

    m_connecting.store(true, std::memory_order_release);
    m_connectDeadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kConnectTimeoutMs);

    std::cout
        << "[ClientNetwork] ENet connection attempt started to "
        << host << ":" << port
        << " as '" << playerName << "'..."
        << std::endl;
}
void ClientNetwork::disconnect() {
    m_connectRequested.store(false, std::memory_order_release);
    m_connecting.store(false, std::memory_order_release);
    m_connected.store(false, std::memory_order_release);
    m_clientWelcomeQueued.store(false, std::memory_order_release);
    m_networkRunning.store(false, std::memory_order_release);

    if (m_networkThread.get_id() != std::this_thread::get_id() &&
        m_networkThread.joinable()) {
        m_networkThread.join();
    }

    if (m_resourceThread.joinable()) {
        m_resourceThread.join();
    }

    m_resourceLoading.store(false, std::memory_order_release);
    m_resourcesReady.store(false, std::memory_order_release);
    m_resourceFailed.store(false, std::memory_order_release);
    m_loadResourcesStarted.store(false, std::memory_order_release);
    m_clientWelcomeSent.store(false, std::memory_order_release);
    m_worldTransitionRequested.store(false, std::memory_order_release);
    m_clientWelcomeDeadline = {};
    m_connectDeadline = {};
    UI::D3D11Renderer::get().setLoadingScreenVisible(false, "");
    m_localPlayerObserved.store(false, std::memory_order_release);
    m_localPlayerId = INVALID_PLAYER_ID;
    m_remotePlayers.clear();
}

void ClientNetwork::update() {
    // ENet I/O is exclusively owned by networkLoop(). This function only
    // advances game-thread state such as the multiplayer loading gate.
    if (m_resourcesReady.load(std::memory_order_acquire)) {
        finishWorldLoadIfReady();
    }
}

void ClientNetwork::startNetworkThread() {
    bool expected = false;
    if (!m_networkRunning.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    m_networkThread = std::thread(
        &ClientNetwork::networkLoop,
        this);
}

void ClientNetwork::networkLoop() {
    startPendingConnection();

    if (!m_enetHost) {
        m_networkRunning.store(false, std::memory_order_release);
        return;
    }

    while (m_networkRunning.load(std::memory_order_acquire)) {
        ENetEvent event{};
        while (enet_host_service(m_enetHost, &event, 0) > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    m_connecting.store(false, std::memory_order_release);
                    m_connected.store(true, std::memory_order_release);

                    BitStream bs;
                    bs.write<uint16_t>(
                        static_cast<uint16_t>(
                            Protocol::PacketId::HandshakeRequest));
                    bs.writeString(m_playerName);
                    bs.write<ModelHash>(m_spawnModel);

                    sendPacket(
                        Protocol::ChannelReliable,
                        bs,
                        true);

                    std::cout
                        << "[ClientNetwork] ENet transport connected. "
                           "HandshakeRequest sent; waiting for ServerData packet 4."
                        << std::endl;
                    break;
                }

                case ENET_EVENT_TYPE_RECEIVE:
                    if (event.packet &&
                        event.packet->dataLength > 0) {

                        uint16_t packetId = 0;
                        if (event.packet->dataLength >= sizeof(uint16_t)) {
                            std::memcpy(
                                &packetId,
                                event.packet->data,
                                sizeof(packetId));
                        }

                        std::cout
                            << "[ClientNetwork] ENet RX packet id=0x"
                            << std::hex
                            << packetId
                            << std::dec
                            << " channel="
                            << static_cast<uint32_t>(event.channelID)
                            << " bytes="
                            << event.packet->dataLength
                            << std::endl;

                        processPacket(
                            static_cast<Protocol::Channel>(
                                event.channelID),
                            event.packet->data,
                            event.packet->dataLength);
                    }

                    if (event.packet) {
                        enet_packet_destroy(event.packet);
                    }
                    break;

                case ENET_EVENT_TYPE_DISCONNECT:
                    std::cerr
                        << "[ClientNetwork] ENet disconnected from server."
                        << " data="
                        << event.data
                        << std::endl;
                    m_connecting.store(false, std::memory_order_release);
                    m_connected.store(false, std::memory_order_release);
                    m_enetPeer = nullptr;
                    break;

                default:
                    break;
            }
        }

        if (m_clientWelcomeQueued.exchange(
                false,
                std::memory_order_acq_rel) &&
            m_connected.load(std::memory_order_acquire) &&
            m_enetPeer) {

            BitStream bs;
            bs.write<uint16_t>(
                static_cast<uint16_t>(
                    Protocol::PacketId::ClientWelcome));
            bs.write<ModelHash>(m_spawnModel);
            bs.writeString(m_playerName);
            bs.writeVector3(m_spawnPosition);
            bs.write<float>(0.0f);
            bs.write<float>(0.0f);
            bs.write<float>(m_spawnHeading);

            sendPacket(
                Protocol::ChannelReliable,
                bs,
                true);

            m_clientWelcomeSent.store(
                true,
                std::memory_order_release);

            std::cout
                << "[ClientNetwork] ClientWelcome packet 0 enviado por el hilo ENet."
                << std::endl;
        }

        if (m_connecting.load(std::memory_order_acquire) &&
            std::chrono::steady_clock::now() >= m_connectDeadline) {

            std::cerr
                << "[ClientNetwork] ENet connection timeout/refused for "
                << m_serverHost
                << ":"
                << m_serverPort
                << std::endl;

            m_connecting.store(false, std::memory_order_release);
            m_connected.store(false, std::memory_order_release);

            if (m_enetHost) {
                enet_host_destroy(m_enetHost);
                m_enetHost = nullptr;
                m_enetPeer = nullptr;
            }
            break;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(5));
    }

    if (m_enetHost) {
        enet_host_flush(m_enetHost);
        enet_host_destroy(m_enetHost);
        m_enetHost = nullptr;
        m_enetPeer = nullptr;
    }

    m_connecting.store(false, std::memory_order_release);
    m_connected.store(false, std::memory_order_release);
    m_networkRunning.store(false, std::memory_order_release);
}

void ClientNetwork::sendPacket(
    Protocol::Channel channel,
    const BitStream& bs,
    bool reliable)
{
    if (!m_enetHost || !m_enetPeer) {
        std::cerr
            << "[ClientNetwork] sendPacket: ENet peer not ready."
            << std::endl;
        return;
    }

    const enet_uint32 flags =
        reliable ? ENET_PACKET_FLAG_RELIABLE : 0;

    ENetPacket* packet = enet_packet_create(
        bs.data(),
        bs.size(),
        flags);

    if (!packet) {
        std::cerr
            << "[ClientNetwork] ENet packet allocation failed."
            << std::endl;
        return;
    }

    if (enet_peer_send(
            m_enetPeer,
            static_cast<enet_uint8>(channel),
            packet) != 0) {

        enet_packet_destroy(packet);

        std::cerr
            << "[ClientNetwork] enet_peer_send failed."
            << std::endl;
        return;
    }

    enet_host_flush(m_enetHost);
}

void ClientNetwork::sendPlayerSync(
    const Protocol::PlayerSyncPacket& packet)
{
    BitStream bs;
    bs.write<uint16_t>(
        static_cast<uint16_t>(
            Protocol::PacketId::PlayerSyncData));
    bs.write<Protocol::PlayerSyncPacket>(packet);
    sendPacket(
        Protocol::ChannelSync,
        bs,
        false);
}

void ClientNetwork::sendChatMessage(
    const std::string& message)
{
    BitStream bs;
    bs.write<uint16_t>(
        static_cast<uint16_t>(
            Protocol::PacketId::ChatMessage));
    bs.writeString(message);
    sendPacket(
        Protocol::ChannelReliable,
        bs,
        true);
}

void ClientNetwork::triggerServerEvent(
    const std::string& eventName,
    const std::vector<std::string>& args)
{
    BitStream bs;
    bs.write<uint16_t>(
        static_cast<uint16_t>(
            Protocol::PacketId::TriggerServerEvent));
    bs.writeString(eventName);
    bs.write<uint16_t>(
        static_cast<uint16_t>(args.size()));

    for (const auto& arg : args) {
        bs.writeString(arg);
    }

    sendPacket(
        Protocol::ChannelEvents,
        bs,
        true);
}

void ClientNetwork::beginServerDataLoading() {
    if (m_resourceLoading.exchange(
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    m_resourcesReady.store(false, std::memory_order_release);
    m_resourceFailed.store(false, std::memory_order_release);

    std::cout
        << "[ClientNetwork] ServerData válido. Iniciando worker "
           "HTTP DownloadResources(\"cache\\\\\")..."
        << std::endl;

    if (m_resourceThread.joinable()) {
        m_resourceThread.join();
    }

    const std::string host = m_serverHost;
    const uint16_t httpPort = m_httpPort;
    const fs::path cacheRoot = m_cacheRoot;

    m_resourceThread = std::thread(
        [this, host, httpPort, cacheRoot]() {
            const bool downloaded =
                ResourceClient::downloadAll(
                    host,
                    httpPort,
                    cacheRoot);

            uint32_t resourceCount = 0;
            uint32_t fileCount = 0;
            const bool loaded =
                downloaded &&
                ResourceClient::loadAllResources(
                    cacheRoot,
                    resourceCount,
                    fileCount);

            const bool ok = downloaded && loaded;

            m_loadResourcesStarted.store(
                loaded,
                std::memory_order_release);
            m_resourceFailed.store(
                !ok,
                std::memory_order_release);
            m_resourcesReady.store(
                ok,
                std::memory_order_release);
            m_resourceLoading.store(
                false,
                std::memory_order_release);

            if (ok) {
                std::cout
                    << "[ClientNetwork] DownloadResources + "
                       "LoadAllResources completados; "
                       "DoesAllResourcesAreLoaded() = true."
                    << " recursos=" << resourceCount
                    << " archivos=" << fileCount
                    << std::endl;
            } else {
                std::cerr
                    << "[ClientNetwork] Resource loading failed; "
                       "world loading aborted."
                    << std::endl;
            }
        });
}

void ClientNetwork::finishWorldLoadIfReady() {
    if (!m_resourcesReady.load(std::memory_order_acquire)) {
        return;
    }

    if (!m_worldTransitionRequested.exchange(
            true,
            std::memory_order_acq_rel)) {

        std::cout
            << "[ClientNetwork] LoadOnline/InitSpawn: iniciando "
               "transición de mundo después de "
               "DoesAllResourcesAreLoaded()."
            << std::endl;

        UI::D3D11Renderer::get().setLoadingScreenVisible(
            true,
            "Loading multiplayer world...");

        // These are the verified RDRMP transition commands used by the
        // original LoadOnline path.
        Core::EngineHooks::requestMultiplayerWorldLoad();

        // InitSpawn runs alongside LoadOnline in the original client.
        Core::PlayerFactory::requestLocalPlayerSpawn(
            m_spawnPosition,
            m_spawnHeading,
            m_spawnModel);

        UI::CefManager::get().setMainMenuVisible(false);

        // FUN_180002F50 waits one second after resource completion before
        // sending ClientWelcome.
        m_clientWelcomeDeadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(1000);
    }

    if (!m_clientWelcomeSent.load(std::memory_order_acquire) &&
        !m_clientWelcomeQueued.load(std::memory_order_acquire)) {

        if (std::chrono::steady_clock::now() <
            m_clientWelcomeDeadline) {
            return;
        }

        m_clientWelcomeQueued.store(
            true,
            std::memory_order_release);

        UI::D3D11Renderer::get().setLoadingScreenVisible(
            true,
            "Finalizing multiplayer session...");

        std::cout
            << "[ClientNetwork] ClientWelcome encolado para el hilo ENet "
               "después de LoadAllResources + 1000 ms."
            << std::endl;
    }

    // Final stage from the original InitSpawn fiber:
    // wait until GET_PLAYER_ACTOR reports a valid local actor.
    if (Core::PlayerFactory::isLocalPlayerReady() &&
        !m_localPlayerObserved.exchange(
            true,
            std::memory_order_acq_rel)) {

        std::cout
            << "[ClientNetwork] GET_PLAYER_ACTOR válido; "
               "actor local confirmado. In Multiplayer."
            << std::endl;

        UI::D3D11Renderer::get().setLoadingScreenVisible(
            false,
            "");
    }
}

void ClientNetwork::processPacket(
    Protocol::Channel channel,
    const uint8_t* data,
    size_t size)
{
    (void)channel;

    if (size < sizeof(uint16_t) || !data) {
        return;
    }

    try {
        BitStream bs(data, size);
        const auto packetId =
            static_cast<Protocol::PacketId>(
                bs.read<uint16_t>());

        switch (packetId) {
            case Protocol::PacketId::HandshakeResponse: {
                m_localPlayerId = bs.read<PlayerId>();
                const std::string serverName =
                    bs.readString();

                std::cout
                    << "[ClientNetwork] Legacy HandshakeResponse received from '"
                    << serverName << "'; awaiting ServerData packet 4."
                    << std::endl;
                break;
            }

            case Protocol::PacketId::ServerData: {
                m_localPlayerId = bs.read<PlayerId>();
                const std::string serverName = bs.readString();
                const std::string version = bs.readString();
                // HTTPClient::Connect uses the same host/port as ENet; the
                // original ServerData does not carry a separate HTTP port.
                m_httpPort = m_serverPort;
                m_spawnModel = bs.read<ModelHash>();
                m_spawnPosition = bs.readVector3();
                m_spawnHeading = bs.read<float>();

                std::cout
                    << "[ClientNetwork] ServerData packet 4 recibido. "
                       "PlayerID="
                    << m_localPlayerId
                    << " Server='" << serverName
                    << "' Version='" << version
                    << "' HTTP=" << m_httpPort
                    << std::endl;

                if (version != kProtocolVersion) {
                    std::cerr
                        << "[ClientNetwork] failed_server_invalid_version: "
                           "se esperaba '"
                        << kProtocolVersion
                        << "', recibido '" << version << "'."
                        << std::endl;

                    m_resourceFailed.store(
                        true,
                        std::memory_order_release);

                    // Keep the ENet host alive until the event pump has
                    // returned; destroying it from inside processPacket would
                    // invalidate the surrounding enet_host_service loop.
                    if (m_enetPeer) {
                        enet_peer_disconnect(
                            m_enetPeer,
                            1);
                    }
                    m_connected = false;
                    break;
                }

                beginServerDataLoading();
                break;
            }

            case Protocol::PacketId::PlayerSyncData: {
                const auto sync =
                    bs.read<Protocol::PlayerSyncPacket>();

                if (sync.playerId != m_localPlayerId) {
                    auto& remote =
                        m_remotePlayers[sync.playerId];

                    remote.targetPosition = sync.position;
                    remote.targetHeading = sync.heading;
                    remote.health = sync.health;
                    remote.weaponHash =
                        sync.currentWeaponHash;
                }
                break;
            }

            case Protocol::PacketId::ChatMessage: {
                const std::string msg = bs.readString();
                UI::CefManager::get().sendChatMessageToUi(msg);
                break;
            }

            case Protocol::PacketId::PlayerJoined: {
                const PlayerId id =
                    bs.read<PlayerId>();
                const std::string name =
                    bs.readString();

                std::cout
                    << "[ClientNetwork] Remote player joined: "
                    << name << " (" << id << ")"
                    << std::endl;
                break;
            }

            case Protocol::PacketId::PlayerLeft: {
                const PlayerId id =
                    bs.read<PlayerId>();

                const auto it =
                    m_remotePlayers.find(id);

                if (it != m_remotePlayers.end()) {
                    if (it->second.actorPtr) {
                        Core::PlayerFactory::destroyActor(
                            it->second.actorPtr);
                    }

                    m_remotePlayers.erase(it);
                }
                break;
            }

            default:
                break;
        }
    } catch (const std::exception& ex) {
        std::cerr
            << "[ClientNetwork] Error procesando packet: "
            << ex.what()
            << std::endl;
    }
}

} // namespace Frontier::Net
