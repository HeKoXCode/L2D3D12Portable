#pragma once

#include <cstdint>

namespace interlude_modern {

void AppendLog(const wchar_t* message);

void AppendTelemetry(
    std::uint64_t first_frame,
    std::uint64_t last_frame,
    double average_frame_ms,
    double p50_frame_ms,
    double p95_frame_ms,
    double p99_frame_ms,
    double maximum_frame_ms,
    double average_present_ms,
    long present_result);

}  // namespace interlude_modern
