#pragma once
#include <cstdint>
#include <atomic>
#include "encoders.h"
namespace caster {
struct Sample {
    uint64_t sequence;
    Reading reading;
};
struct Diagnostics {
    std::atomic<uint32_t> acquisition_overruns{0};
    std::atomic<uint32_t> queue_drops{0};
    std::atomic<uint32_t> invalid_spi_responses{0};
    std::atomic<uint32_t> index_queue_drops{0};
    std::atomic<uint32_t> radio_failures{0};
};
// Heap-allocated JSON text returned by these functions; release with free().
char* serialize_samples(const char* boot_id, const Sample* samples, int count,
                        const Diagnostics& diagnostics);
char* serialize_metadata(const char* boot_id, const EncoderConfig& config, int reset_reason);
void transport_init();
bool transport_connected();
void transport_reconnect();
bool transport_send(const char* data);
}  // namespace caster
