#include "shared/bitstream.hpp"

namespace Frontier {

BitStream::BitStream(size_t reserveCapacity) {
    m_buffer.reserve(reserveCapacity);
}

BitStream::BitStream(const uint8_t* data, size_t size) {
    if (data && size > 0) {
        m_buffer.assign(data, data + size);
    }
}

void BitStream::writeString(const std::string& str) {
    uint16_t len = static_cast<uint16_t>(str.size());
    write<uint16_t>(len);
    if (len > 0) {
        writeBytes(str.data(), len);
    }
}

void BitStream::writeBytes(const void* data, size_t size) {
    if (data && size > 0) {
        const auto* ptr = static_cast<const uint8_t*>(data);
        m_buffer.insert(m_buffer.end(), ptr, ptr + size);
    }
}

void BitStream::writeVector3(const Vector3& vec) {
    write<float>(vec.x);
    write<float>(vec.y);
    write<float>(vec.z);
}

std::string BitStream::readString() {
    uint16_t len = read<uint16_t>();
    if (len == 0) return {};
    if (m_readOffset + len > m_buffer.size()) {
        throw std::runtime_error("BitStream underflow while reading string");
    }
    std::string s(reinterpret_cast<const char*>(m_buffer.data() + m_readOffset), len);
    m_readOffset += len;
    return s;
}

void BitStream::readBytes(void* dest, size_t size) {
    if (size == 0) return;
    if (m_readOffset + size > m_buffer.size()) {
        throw std::runtime_error("BitStream underflow while reading bytes");
    }
    std::memcpy(dest, m_buffer.data() + m_readOffset, size);
    m_readOffset += size;
}

Vector3 BitStream::readVector3() {
    float x = read<float>();
    float y = read<float>();
    float z = read<float>();
    return Vector3(x, y, z);
}

} // namespace Frontier
