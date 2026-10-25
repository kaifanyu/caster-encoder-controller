#pragma once
#include "telemetry.h"

namespace caster {
// Initialize/recover before starting acquisition. Starts HTTP + UDP discovery.
void recorder_init(const char* boot_id, int reset_reason, Diagnostics* diagnostics);
// Called by the sole acquisition owner immediately before encoder_read().
// True begins a fresh velocity / CENTER unwrap segment, never resets PCNT count.
bool recorder_new_segment();
// Nonblocking queue insertion; overflow terminates the run visibly.
void recorder_capture(const Sample& sample);
// Live sample UDP is suppressed from prepare through finalization. Metadata and
// control Wi-Fi remain available. Queued samples must not leak during recording.
bool recorder_suppress_live();
}  // namespace caster
