#pragma once

#include "types.hpp"
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <stdexcept>

namespace Frontier {

class BitStream {
public:
    BitStream() = default;
    explicit BitStream(size_t reserveCapacity);
    BitStream(const uint8_t* data, size_t size);

    // Serialization (Write)
    template <typename T>
    void write(const T& val) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        const auto* ptr = reinterpret_cast<const uint8_t*>(&val);
        m_buffer.insert(m_buffer.end(), ptr, ptr + sizeof(T));
    }

    void writeString(const std::string& str);
    void writeBytes(const void* data, size_t size);
    void writeVector3(const Vector3& vec);

    // Deserialization (Read)
    template <typename T>
    T read() {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        if (m_readOffset + sizeof(T) > m_buffer.size()) {
            throw std::runtime_error("BitStream underflow: attempt to read past end of buffer");
        }
        T val;
        std::memcpy(&val, m_buffer.data() + m_readOffset, sizeof(T));
        m_readOffset += sizeof(T);
        return val;
    }

    std::string readString();
    void readBytes(void* dest, size_t size);
    Vector3 readVector3();

    // Utility
    const uint8_t* data() const { return m_buffer.data(); }
    size_t size() const { return m_buffer.size(); }
    size_t readOffset() const { return m_readOffset; }
    void resetRead() { m_readOffset = 0; }
    void clear() { m_buffer.clear(); m_readOffset = 0; }

private:
    std::vector<uint8_t> m_buffer;
    size_t m_readOffset{0};
};

} // namespace Frontier
