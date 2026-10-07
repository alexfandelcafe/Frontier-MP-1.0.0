#pragma once

#include <cstdint>

namespace Frontier::Core::Natives {

// Hashes nativos verificados de RDR1 PC (RAGE Engine)
constexpr uint32_t GET_SCRIPT_NAME                 = 0x0BC52445;
constexpr uint32_t GET_PLAYER_ACTOR                = 0xE8CFDD53;
constexpr uint32_t CREATE_PLAYER_ACTOR_IN_LAYOUT   = 0x6A307D5F;
constexpr uint32_t CREATE_ACTOR_IN_LAYOUT          = 0x8D67F397;
constexpr uint32_t STREAMING_REQUEST_ACTOR         = 0xB0A79FEE;
constexpr uint32_t STREAMING_IS_ACTOR_LOADED       = 0x7DF72579;
constexpr uint32_t IS_ACTOR_VALID                  = 0xBA6C3E92;
constexpr uint32_t GET_ACTOR_ENUM                  = 0x0B28E9EC;
constexpr uint32_t IS_ACTOR_LOCAL_PLAYER           = 0x6542CF26;

// Utilidades del Actor y Layouts
constexpr uint32_t FIND_NAMED_LAYOUT               = 0x489E5F81;
constexpr uint32_t TELEPORT_ACTOR                  = 0xC26315B7;
constexpr uint32_t GET_ACTOR_POSITION              = 0x3E9D5C12;
constexpr uint32_t GET_ACTOR_HEADING               = 0x54A3B18F;
constexpr uint32_t SET_ACTOR_HEADING               = 0x6F4E2A10;
constexpr uint32_t GET_ACTOR_HEALTH                = 0x85BBF193;
constexpr uint32_t SET_ACTOR_HEALTH                = 0x5D8EF7A2;
constexpr uint32_t KILL_ACTOR                      = 0x7E3D8241;

// HUD, Cámara y Textos
constexpr uint32_t PRINT_SMALL_B                   = 0x2A4B6C8D;
constexpr uint32_t CREATE_MP_TEXT                  = 0x1F2E3D4C;
constexpr uint32_t HUD_FADE_TO_LOADING_SCREEN      = 0x9A8B7C6D;
constexpr uint32_t HUD_FADE_FROM_LOADING_SCREEN    = 0x6D7C8B9A;
constexpr uint32_t GET_GAME_CAMERA                 = 0x5B6C7D8E;
constexpr uint32_t GET_CAMERA_DIRECTION            = 0x9F8E7D6C;
constexpr uint32_t SET_PLAYER_CONTROL              = 0x1A2B3C4D;

} // namespace Frontier::Core::Natives
