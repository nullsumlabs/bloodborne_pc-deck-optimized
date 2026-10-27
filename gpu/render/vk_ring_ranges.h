// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the vertex and index ranges of a draw that the GPU command thread (stage A) copied into
// the constant ring, so that the draw recording thread (stage B) binds them without a buffer-cache
// lookup (OPTIMIZATION_GOALS.md §3, "move the buffer side of a draw from stage B to stage A").
//
// Stage A already reads the vertex streams' V# tables there — a later constant RAM dump would
// replace them (issue #44) — and it merges the streams into as few ranges as possible. Those
// ranges are small and read-only, the shape the constant ring already takes for a draw's constants
// (`CollectRingBindings`, raster_pipe.cpp): stage A copies them into the ring too and stage B binds
// the ring's buffer and offset. What leaves stage B per draw is `ObtainBuffer` for each range and
// the index buffer, the in-place read notes and the barrier bookkeeping that goes with them.
//
// The descriptor writes, the texture bindings and the barriers stay on stage B: moving those takes
// stage A owning the buffer cache itself (the doc's step 3), not a moved call.
//
// BB_PIPE_BUFFERS=0: the ranges stay with stage B (`ObtainBuffer`), as before this change. `=1`
// forces them back on for a run with the variable inherited as 0. The draw pipeline has to be in
// use for either (stage A only exists then).
#pragma once

#include <cstdio>
#include <cstdlib>

#include "common/types.h"

namespace Vulkan {

/// A guest range [begin, end) copied by stage A; its bytes are at `offset` in stage A's constant
/// ring, which holds them until the submission that records this draw has run.
struct RingRange {
    VAddr begin;
    VAddr end;
    u64 offset;
};

/// The switch's value for a BB_PIPE_BUFFERS setting: unset or empty — on (the default), a value
/// starting with '0' — off.
inline bool RingDrawInputsValue(const char* env) {
    return !(env != nullptr && env[0] == '0');
}

/// Whether stage A copies a draw's vertex and index ranges into the ring (the default) rather than
/// leaving them to stage B. Read once; prints the startup line when the variable is set.
inline bool RingDrawInputsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PIPE_BUFFERS");
        const bool on = RingDrawInputsValue(env);
        if (env != nullptr && env[0] != '\0') {
            std::printf("GPU: draw inputs (vertex and index ranges) copied into the constant ring on "
                        "the GPU thread %s (BB_PIPE_BUFFERS=%s)\n",
                        on ? "on" : "off", env);
        }
        return on;
    }();
    return enabled;
}

/// The copy of [begin, end) among stage A's ranges, or null when it did not make one. A copy covers
/// the range it is asked for — stage B may merge the same streams differently — and `begin` of the
/// answer is where the copy starts, which is what offsets into it are relative to.
inline const RingRange* RingRangeFor(const RingRange* ranges, u32 count, VAddr begin, VAddr end) {
    if (ranges == nullptr) {
        return nullptr;
    }
    for (u32 i = 0; i < count; ++i) {
        const auto& range = ranges[i];
        if (range.begin <= begin && range.end >= end) {
            return &range;
        }
    }
    return nullptr;
}

} // namespace Vulkan
