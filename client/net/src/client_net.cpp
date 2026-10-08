#include "net/client_net.hpp"
#include "net/resource_client.hpp"
#include "core/player_factory.hpp"
#include "core/engine_hooks.hpp"
#include "ui/cef_manager.hpp"
#include "ui/d3d11_renderer.hpp"

#include <enet/enet.h>

#include <chrono>
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
    if (m_resourceThread.joinable()) {
        m_resourceThread.join();
    }
}

bool ClientNetwork::connect(
    const std::string& host,
    uint16_t port,
    const std::string& playerName)
{
    // A duplicate UI click/key event must not tear down an already valid
    // ENet connection. ServerData(0x04) is processed asynchronously by update().
    if (m_connected) {
        std::cout
            << "[ClientNetwork] Connect ignored: already connected to "
            << m_serverHost << ":" << m_serverPort << "."
            << std::endl;
        return true;
    }

    if (m_enetPeer || m_enetHost) {
        disconnect();
    }

    if (!ensureEnetInitialized()) {
        std::cerr << "[ClientNetwork] ENet initialization failed." << std::endl;
        return false;
    }

    m_playerName = playerName;
    m_serverHost = host;
    m_serverPort = port;
    m_httpPort = port; // Original RDRMP HTTPClient reuses ENet host/port.
    m_localPlayerId = INVALID_PLAYER_ID;
    m_resourcesReady.store(false, std::memory_order_release);
    m_resourceFailed.store(false, std::memory_order_release);
    m_resourceLoading.store(false, std::memory_order_release);
    m_loadResourcesStarted.store(false, std::memory_order_release);
    m_clientWelcomeSent.store(false, std::memory_order_release);
    m_worldTransitionRequested.store(false, std::memory_order_release);
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
        std::cerr << "[ClientNetwork] No se pudo crear el ENetHost cliente."
                  << std::endl;
        return false;
    }

    ENetAddress address{};
    if (enet_address_set_host(&address, host.c_str()) != 0) {
        std::cerr << "[ClientNetwork] No se pudo resolver el host: "
                  << host << std::endl;
        enet_host_destroy(m_enetHost);
        m_enetHost = nullptr;
        return false;
    }

    address.port = port;

    m_enetPeer = enet_host_connect(
        m_enetHost,
        &address,
        kEnetChannels,
        0);

    if (!m_enetPeer) {
        std::cerr << "[ClientNetwork] ENet no pudo crear el peer."
                  << std::endl;
        enet_host_destroy(m_enetHost);
        m_enetHost = nullptr;
        return false;
    }

    std::cout << "[ClientNetwork] ENet connection attempt started to "
              << host << ":" << port
              << " as '" << playerName << "'..." << std::endl;

    // Esperar únicamente el establecimiento de transporte. El ServerData
    // posterior es el evento que realmente comienza el Loading World.
    ENetEvent event{};
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kConnectTimeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        const int service = enet_host_service(
            m_enetHost,
            &event,
            50);

        if (service < 0) {
            break;
        }

        if (service == 0) {
            continue;
        }

        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            m_connected = true;

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
            return true;
        }

        if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            m_enetPeer = nullptr;
            break;
        }
    }

    std::cerr
        << "[ClientNetwork] ENet connection timeout/refused for "
        << host << ":" << port << std::endl;

    if (ResourceClient::httpGetStatus(host, m_httpPort)) {
        std::cerr
            << "[ClientNetwork] HTTP control server responde en "
            << host << ":" << m_httpPort
            << ", pero ENet UDP no acepta conexiones en "
            << host << ":" << port
            << ". El problema está en el listener UDP/server build."
            << std::endl;
    } else {
        std::cerr
            << "[ClientNetwork] HTTP control server tampoco responde en "
            << host << ":" << m_httpPort
            << ". Verifica que frontier_server.exe esté ejecutándose."
            << std::endl;
    }

    if (m_enetHost) {
        enet_host_destroy(m_enetHost);
        m_enetHost = nullptr;
    }
    m_enetPeer = nullptr;
    m_connected = false;
    return false;
}

void ClientNetwork::disconnect() {
    m_connected = false;

    if (m_enetPeer) {
        enet_peer_disconnect(m_enetPeer, 0);
        enet_host_flush(m_enetHost);
        m_enetPeer = nullptr;
    }

    if (m_enetHost) {
        enet_host_destroy(m_enetHost);
        m_enetHost = nullptr;
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
    UI::D3D11Renderer::get().setLoadingScreenVisible(false, "");
    m_localPlayerObserved.store(false, std::memory_order_release);
    m_localPlayerId = INVALID_PLAYER_ID;
    m_remotePlayers.clear();
}

void ClientNetwork::update() {
    if (!m_enetHost) return;

    ENetEvent event{};
    while (enet_host_service(m_enetHost, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_RECEIVE:
                if (event.packet && event.packet->dataLength > 0) {
                    processPacket(
                        static_cast<Protocol::Channel>(event.channelID),
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
                    << std::endl;
                m_connected = false;
                m_enetPeer = nullptr;
                break;

            default:
                break;
        }
    }

    if (m_resourcesReady.load(std::memory_order_acquire)) {
        finishWorldLoadIfReady();
    }
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

    if (!m_clientWelcomeSent.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() <
            m_clientWelcomeDeadline) {
            return;
        }

        bool expected = false;
        if (!m_clientWelcomeSent.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }

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

        std::cout
            << "[ClientNetwork] ClientWelcome packet 0 enviado después "
               "de LoadAllResources + 1000 ms."
            << std::endl;

        UI::D3D11Renderer::get().setLoadingScreenVisible(
            true,
            "Finalizing multiplayer session...");
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
