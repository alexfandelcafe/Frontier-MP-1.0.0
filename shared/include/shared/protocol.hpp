#pragma once

#include "types.hpp"
#include <cstdint>

namespace Frontier::Protocol {

// ENet Channels
enum Channel : uint8_t {
    ChannelReliable = 0,    // Handshake, RPC, Eventos fiables, Chat
    ChannelSync = 1,        // Sincronización continua de entidades (pos, vel, rot)
    ChannelEvents = 2,      // Eventos de recursos (Lua Triggers)
    ChannelVoice = 3,       // Audio espacial VOIP (opcional)
    ChannelCount
};

// Protocol Identifiers (Packet IDs)
enum class PacketId : uint16_t {
    // 0x00 - 0x0F: Handshake y Conexión
    HandshakeRequest = 0x01,
    HandshakeResponse = 0x02,
    ClientReady = 0x03,
    PlayerJoined = 0x04,
    PlayerLeft = 0x05,
    Heartbeat = 0x06,
    DisconnectReason = 0x07,

    // 0x10 - 0x1F: Recursos (Estilo FiveM)
    ResourceListRequest = 0x10,
    ResourceListResponse = 0x11,
    ResourceStart = 0x12,
    ResourceStop = 0x13,

    // 0x20 - 0x3F: Sincronización de Entidades y Jugadores
    PlayerSyncData = 0x20,         // Posición, rotación, velocidad, heading
    PlayerHealthSync = 0x21,       // Salud, armadura, estado de muerte
    PlayerWeaponSync = 0x22,       // Arma equipada, munición, disparo
    PlayerAnimationSync = 0x23,    // Tareas de animación activas
    PlayerMountSync = 0x24,        // Estado a caballo / diligencia

    // 0x40 - 0x4F: Entidades del Mundo (NPCs, Caballos, Diligencias)
    EntityCreate = 0x40,
    EntityDestroy = 0x41,
    EntitySyncData = 0x42,

    // 0x50 - 0x5F: Clima, Hora y Mundo
    WorldStateSync = 0x50,

    // 0x60 - 0x6F: Sistema de Eventos (Lua Scripting)
    TriggerServerEvent = 0x60,     // Cliente -> Servidor (tipo FiveM: TriggerServerEvent)
    TriggerClientEvent = 0x61,     // Servidor -> Cliente (tipo FiveM: TriggerClientEvent)
    BroadcastClientEvent = 0x62,   // Servidor -> Todos los clientes

    // 0x70 - 0x7F: Chat & NUI
    ChatMessage = 0x70,
    NuiCallback = 0x71
};

// Estructura de paquete de sincronización de jugador (Tick sync)
#pragma pack(push, 1)
struct PlayerSyncPacket {
    PlayerId playerId;
    Vector3 position;
    Vector3 velocity;
    float heading;
    float pitch;
    uint32_t currentWeaponHash;
    uint16_t health;
    uint8_t flags; // bit 0: isDead, bit 1: isAiming, bit 2: isShooting, bit 3: isMounted
    EntityId mountEntityId; // ID del caballo o asiento de diligencia
};
#pragma pack(pop)

} // namespace Frontier::Protocol
