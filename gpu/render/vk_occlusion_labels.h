// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: whether the game's occlusion results must be written before a GPU label (vk_occlusion.h).
//
// A label ahead of results not written yet would let the game read the label and then stale
// counters, so the results were written before every label — each one ending the render pass and
// resolving the pending queries. Only a label the game actually reads needs that: a page with a
// label on it is watched for its first CPU access (any read or write, once), and from that access
// on the labels there are ordered. Pages nobody touches keep the pass open.
//
// BB_OCCLUSION_LABEL_ORDER=1: before every label (the behaviour before this). =0: never (as
// statistics only). Unset: the pages the game reads.
#pragma once

#include <cstdlib>
#include <unordered_set>

#include "common/types.h"

namespace Vulkan {

class LabelPolls {
public:
    enum class Mode : u8 {
        Polled, ///< only where the game has touched a page a label was written to
        Every,  ///< before every label (BB_OCCLUSION_LABEL_ORDER=1)
        Never,  ///< never (BB_OCCLUSION_LABEL_ORDER=0)
    };

    static Mode ModeFromEnv() {
        const char* env = std::getenv("BB_OCCLUSION_LABEL_ORDER");
        if (env == nullptr) {
            return Mode::Polled;
        }
        return env[0] == '0' ? Mode::Never : Mode::Every;
    }

    explicit LabelPolls(Mode mode_ = Mode::Polled) : mode{mode_} {}

    /// Whether the results recorded so far are written before a label at `address`. `watch` is
    /// set when the caller should watch the page for its first access (once per page, at most
    /// MaxWatched pages).
    bool ShouldWrite(VAddr address, bool& watch) {
        watch = false;
        if (mode == Mode::Never) {
            return false;
        }
        if (mode == Mode::Every) {
            return true;
        }
        const u64 page = Page(address);
        if (polled.count(page) != 0) {
            return true;
        }
        if (watched.size() >= MaxWatched) {
            return true; // cannot be watched any more: ordered unasked
        }
        if (watched.count(page) != 0) {
            return false; // watched already: the first access is still to come
        }
        watched.insert(page);
        watch = true;
        return false;
    }

    /// `address` was accessed by the CPU (any thread, read or write): the game reads labels there.
    void Touched(VAddr address) {
        const u64 page = Page(address);
        if (watched.erase(page) != 0) {
            polled.insert(page);
        }
    }

    bool Watching(VAddr address) const noexcept {
        return watched.count(Page(address)) != 0;
    }
    Mode GetMode() const noexcept {
        return mode;
    }
    size_t PolledPages() const noexcept {
        return polled.size();
    }
    size_t WatchedPages() const noexcept {
        return watched.size();
    }

private:
    /// An access trap cannot tell where in its page a label is.
    static u64 Page(VAddr address) {
        return address & ~u64{4095};
    }
    /// Label memory is a handful of pages; past the cap a page is ordered unasked rather than
    /// watched, so a game with many of them keeps the safe behaviour.
    static constexpr size_t MaxWatched = 256;

    Mode mode;
    std::unordered_set<u64> polled;  ///< pages the game has accessed after a label there
    std::unordered_set<u64> watched; ///< pages watched for their first access
};

/// A fault taken for a watched page's trap, once the trap is gone (vk_occlusion.cpp, OnAccess).
/// The trap makes its page no access, and a label page is also written by the game's own threads:
/// when it is armed, every thread that touches it faults, and the first handler lifts the trap for
/// all of them. A later fault that gets here therefore found no reason of its own — the access is
/// the game's own and only left over from the moment it faulted. `mapped`/`prot` are the page's
/// mapping now (0 unmapped, 1 read, 2 write). True: retry the instruction. False: the page is not
/// mapped, or still forbids that access — a guest fault of the game's own.
inline bool StaleLabelTrapFault(bool write, bool mapped, unsigned prot) {
    if (!mapped) {
        return false;
    }
    return (prot & (write ? 2u : 1u)) != 0;
}

} // namespace Vulkan
