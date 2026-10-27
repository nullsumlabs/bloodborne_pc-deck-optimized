// The vertex/index ranges the GPU command thread copies into the constant ring for the draw
// recording thread to bind (gpu/render/vk_ring_ranges.h). No Vulkan device and no draw: the switch
// and the lookup that decides whether a range of the draw has a copy.
#include <cstdio>
#include <cstdlib>

#include "render/vk_ring_ranges.h"

namespace {
int failures = 0;

void check(const char* what, bool ok) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
} // namespace

int main() {
    using namespace Vulkan;

    // BB_PIPE_BUFFERS: on unless set to something starting with 0 (the default is the change).
    unsetenv("BB_PIPE_BUFFERS");
    check("unset: on", RingDrawInputsValue(nullptr));
    check("empty: on", RingDrawInputsValue(""));
    check("1: on", RingDrawInputsValue("1"));
    check("0: off", !RingDrawInputsValue("0"));
    check("0 with more: off", !RingDrawInputsValue("0 "));

    // Stage A's copies of a draw: two vertex ranges and the index buffer.
    const RingRange ranges[] = {
        {0x1000'0000, 0x1000'0400, 0x0},  // 1 KiB of vertex streams, ring offset 0
        {0x1000'2000, 0x1000'3000, 0x400}, // streams that touch, merged into one range
        {0x2000'0000, 0x2000'0FF0, 0x1400}, // 4080 bytes of indices
    };
    constexpr u32 count = 3;

    check("no packet: no copy", RingRangeFor(nullptr, count, 0x1000'0000, 0x1000'0400) == nullptr);
    check("no ranges: no copy", RingRangeFor(ranges, 0, 0x1000'0000, 0x1000'0400) == nullptr);

    const RingRange* exact = RingRangeFor(ranges, count, 0x1000'0000, 0x1000'0400);
    check("a range stage A copied: its copy", exact == &ranges[0] && exact->offset == 0x0);
    const RingRange* index = RingRangeFor(ranges, count, 0x2000'0000, 0x2000'0FF0);
    check("the index buffer: its copy", index == &ranges[2] && index->offset == 0x1400);

    // Stage B merges the same streams itself; the copy covers the range it asks for, and offsets
    // into it are relative to where the copy starts.
    const RingRange* part = RingRangeFor(ranges, count, 0x1000'2100, 0x1000'2200);
    check("a range inside a copy: the copy", part == &ranges[1] && part->begin == 0x1000'2000);
    check("a range straddling two copies: none",
          RingRangeFor(ranges, count, 0x1000'0F00, 0x1000'2100) == nullptr);
    check("a range past the end of a copy: none",
          RingRangeFor(ranges, count, 0x1000'0300, 0x1000'0500) == nullptr);
    check("a range elsewhere: none",
          RingRangeFor(ranges, count, 0x3000'0000, 0x3000'0010) == nullptr);

    std::printf("%s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
