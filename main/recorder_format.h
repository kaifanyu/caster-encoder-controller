#pragma once

#include "telemetry.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

// Portable, explicitly little-endian storage format. Never persist C++ structs.
namespace caster::recording {
constexpr size_t kHeaderBytes = 4096;
constexpr size_t kRecordBytes = 100;
constexpr size_t kFooterBytes = 4096;
constexpr size_t kControlBytes = 128;
constexpr size_t kStartOffset = 0;
constexpr size_t kStopOffset = 256;
constexpr size_t kAckOffset = 512;
constexpr size_t kReadyOffset = 768;
constexpr uint32_t kVersion = 1;

inline uint32_t crc32(const void* data, size_t length, uint32_t previous = 0) {
    uint32_t crc = ~previous;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
inline void put(uint8_t* out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}
inline uint64_t get(const uint8_t* in, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= uint64_t(in[i]) << (8 * i);
    return value;
}
inline void put_double(uint8_t* out, double value) {
    static_assert(sizeof(double) == 8, "Log format requires binary64 doubles");
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put(out, bits, 8);
}
inline void encode_record(const Sample& sample, uint8_t* out) {
    const auto& r = sample.reading;
    put(out, sample.sequence, 8);
    put(out + 8, static_cast<uint64_t>(r.timestamp_us), 8);
    put(out + 16, static_cast<uint64_t>(r.raw_count), 8);
    put(out + 24, r.raw_spi_word, 2);
    put(out + 26, static_cast<uint16_t>(r.position14), 2);
    put(out + 28, r.flags, 4);
    put_double(out + 32, r.angle_rad);
    put_double(out + 40, r.absolute_angle_rad);
    put_double(out + 48, r.calibrated_alpha_rad);
    put_double(out + 56, r.velocity_rad_s);
    put_double(out + 64, r.filtered_velocity_rad_s);
    put(out + 72, r.index_event_count, 8);
    put(out + 80, static_cast<uint64_t>(r.index_timestamp_us), 8);
    put(out + 88, static_cast<uint64_t>(r.index_raw_count), 8);
    put(out + 96, crc32(out, 96), 4);
}
inline bool valid_record(const uint8_t* record) {
    return get(record + 96, 4) == crc32(record, 96) &&
           get(record + 8, 8) <= INT64_MAX;
}
inline bool erased(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) if (data[i] != 0xff) return false;
    return true;
}
// Control blocks are written once into a pre-erased sector. Last four bytes CRC.
inline void seal_control(uint8_t* block) {
    put(block + kControlBytes - 4, crc32(block, kControlBytes - 4), 4);
}
inline bool valid_control(const uint8_t* block, const char* magic) {
    return std::memcmp(block, magic, 8) == 0 &&
           get(block + kControlBytes - 4, 4) == crc32(block, kControlBytes - 4);
}
}  // namespace caster::recording
