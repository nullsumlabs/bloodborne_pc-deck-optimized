// The policy deciding before which GPU labels the game's occlusion results are written
// (gpu/render/vk_occlusion_labels.h). No Vulkan device: it is the decision, not the queries.
#include <cstdio>
#include <cstdlib>

#include "render/vk_occlusion_labels.h"

namespace {
int failures = 0;

void check(const char* what, bool ok) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
} // namespace

int main() {
    using Vulkan::LabelPolls;

    unsetenv("BB_OCCLUSION_LABEL_ORDER");
    check("unset: every label (the default)",
          LabelPolls::ModeFromEnv() == LabelPolls::Mode::Every);
    setenv("BB_OCCLUSION_LABEL_ORDER", "0", 1);
    check("0: never", LabelPolls::ModeFromEnv() == LabelPolls::Mode::Never);
    setenv("BB_OCCLUSION_LABEL_ORDER", "1", 1);
    check("1: every label", LabelPolls::ModeFromEnv() == LabelPolls::Mode::Every);
    setenv("BB_OCCLUSION_LABEL_ORDER", "2", 1);
    check("2: only the pages the game reads (experimental)",
          LabelPolls::ModeFromEnv() == LabelPolls::Mode::Polled);
    unsetenv("BB_OCCLUSION_LABEL_ORDER");

    LabelPolls every{LabelPolls::Mode::Every};
    bool watch = true;
    check("every: ordered, nothing watched", every.ShouldWrite(0x1000, watch) && !watch);

    LabelPolls never{LabelPolls::Mode::Never};
    watch = true;
    check("never: skipped, nothing watched", !never.ShouldWrite(0x1000, watch) && !watch);

    LabelPolls polls{LabelPolls::Mode::Polled};
    constexpr VAddr label = 0x2000'0100;
    watch = false;
    check("polled: a new page is not ordered", !polls.ShouldWrite(label, watch));
    check("polled: ... and its page is watched", watch && polls.Watching(label));
    watch = false;
    check("polled: one watch per page", !polls.ShouldWrite(label + 0x80, watch) && !watch);
    check("polled: no page read yet", polls.PolledPages() == 0u && polls.WatchedPages() == 1u);
    polls.Touched(label + 0x40);
    check("polled: an access teaches the page",
          polls.PolledPages() == 1u && !polls.Watching(label));
    watch = false;
    check("polled: its labels are ordered from then on",
          polls.ShouldWrite(label + 0x200, watch) && !watch);
    watch = false;
    check("polled: another page still is not", !polls.ShouldWrite(label + 0x1000, watch) && watch);

    LabelPolls many{LabelPolls::Mode::Polled};
    VAddr page = 0x10'0000;
    for (size_t i = 0; i < 256; ++i, page += 0x1000) {
        bool armed = false;
        many.ShouldWrite(page, armed);
    }
    check("polled: 256 pages watched", many.WatchedPages() == 256u);
    watch = false;
    check("polled: past the cap an unknown page is ordered", many.ShouldWrite(page, watch) && !watch);

    // A fault that arrives after another thread's handler lifted the trap (vk_occlusion.cpp,
    // OnAccess): the game's own access, retried when the page allows it again.
    using Vulkan::StaleLabelTrapFault;
    check("stale trap, write, page writable again: retried", StaleLabelTrapFault(true, true, 3));
    check("stale trap, read, page readable again: retried", StaleLabelTrapFault(false, true, 1));
    check("stale trap, write, page read-only: a guest fault", !StaleLabelTrapFault(true, true, 1));
    check("stale trap, read, page without read: a guest fault", !StaleLabelTrapFault(false, true, 0));
    check("stale trap, address unmapped: a guest fault", !StaleLabelTrapFault(true, false, 3));

    std::printf("%s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
