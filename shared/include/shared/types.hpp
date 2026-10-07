#pragma once

#include <cstdint>
#include <string>
#include <cmath>

namespace Frontier {

using PlayerId = uint16_t;
using EntityId = uint32_t;
using ModelHash = uint32_t;

constexpr PlayerId INVALID_PLAYER_ID = 0xFFFF;
constexpr EntityId INVALID_ENTITY_ID = 0xFFFFFFFF;
constexpr uint16_t DEFAULT_SERVER_PORT = 4674;
constexpr uint32_t MAX_PLAYERS_LIMIT = 64;

struct Vector3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    Vector3() = default;
    constexpr Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    float length() const {
        return std::sqrt(x * x + y * y + z * z);
    }

    float distance_to(const Vector3& other) const {
        float dx = x - other.x;
        float dy = y - other.y;
        float dz = z - other.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    Vector3 normalized() const {
        float l = length();
        if (l < 0.00001f) return Vector3(0, 0, 0);
        return Vector3(x / l, y / l, z / l);
    }

    Vector3 operator+(const Vector3& o) const { return Vector3(x + o.x, y + o.y, z + o.z); }
    Vector3 operator-(const Vector3& o) const { return Vector3(x - o.x, y - o.y, z - o.z); }
    Vector3 operator*(float s) const { return Vector3(x * s, y * s, z * s); }
};

struct Quaternion {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};

    Quaternion() = default;
    constexpr Quaternion(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
};

enum class EntityType : uint8_t {
    PlayerPed = 0,
    NpcPed = 1,
    Horse = 2,
    Vehicle = 3, // Diligencias, carretas, trenes
    Object = 4,
    Pickup = 5
};

} // namespace Frontier
