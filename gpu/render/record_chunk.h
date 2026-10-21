// SPDX-License-Identifier: GPL-2.0-or-later
// bbport render: blocks of deferred Vulkan commands for the recording threads (RecordChunk),
// and the command buffer segments of one submission (SubmissionBatch).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "bbport_toggles.h"
#include "common/types.h"
#include "vulkan/vk_common.h"

namespace Vulkan {

/// The command buffer segments of one submission and their chunk counters (guarded by the
/// scheduler's recorder_mutex). With BB_ASYNC_SUBMIT several are in flight: a submission goes
/// out from a recording thread once its segments are recorded.
struct SubmissionBatch {
    static constexpr u32 MaxSegments = 64;
    std::array<vk::CommandBuffer, MaxSegments> segments{}; ///< null until begun
    std::array<u32, MaxSegments> handed{}, recorded{};
    void Reset() {
        segments.fill({});
        handed.fill(0);
        recorded.fill(0);
    }
};

/// A block of deferred Vulkan commands. Commands are closures placed in
/// fixed storage (no allocation per command) and run in order on the recording thread.
class RecordChunk {
public:
    static constexpr size_t Capacity = 128 * 1024;

    /// Returns false (and leaves `func` untouched) when the chunk has no room.
    template <typename Func>
    bool Push(Func&& func) {
        using Command = TypedCommand<std::decay_t<Func>>;
        static_assert(sizeof(Command) <= Capacity, "recorded command is too large");
        const size_t offset = (used + alignof(Command) - 1) & ~(alignof(Command) - 1);
        if (offset + sizeof(Command) > Capacity) {
            return false;
        }
        auto* command = new (storage + offset) Command(std::forward<Func>(func));
        if (last) {
            last->next = command;
        } else {
            first = command;
        }
        last = command;
        used = offset + sizeof(Command);
        PrefetchAhead();
        return true;
    }

    /// Raw storage in the chunk for a command's variable-length data. A payload the chunk cannot
    /// hold at all goes to a block of its own (AllocateOversized) instead of stopping the game;
    /// null only when the host is out of memory or the size is beyond what a draw carries.
    void* Allocate(size_t bytes, size_t align) {
        const size_t offset = (used + align - 1) & ~(align - 1);
        if (offset + bytes <= Capacity) {
            used = offset + bytes;
            PrefetchAhead();
            return storage + offset;
        }
        return AllocateOversized(bytes, align);
    }

    void Execute(vk::CommandBuffer cmdbuf) {
        for (CommandBase* command = first; command;) {
            CommandBase* const next = command->next;
            command->Execute(cmdbuf);
            command->~CommandBase();
            command = next;
        }
        first = last = nullptr;
        used = 0;
        // The spill blocks are empty again (their memory is kept for the next pass).
        for (SpillBlock& block : spill_blocks) {
            block.used = 0;
        }
    }

    [[nodiscard]] bool Empty() const noexcept {
        return first == nullptr;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return used;
    }

    u32 segment = 0; ///< the command buffer segment the chunk is recorded into (Scheduler)
    SubmissionBatch* batch = nullptr; ///< the submission it belongs to (Scheduler)
    /// Ordered chunks: the command chunks of `segment` (and all of the segments before it)
    /// that must be recorded before this chunk runs.
    u32 after = 0;

private:
    /// The recording thread last read these cache lines: the first write to each has to take
    /// it back from that core. Requesting ownership a few lines ahead overlaps those
    /// transfers instead of stalling on each (RecordPrefetch).
    void PrefetchAhead() const {
        if (!BbToggle::Disabled(BbToggle::RecordPrefetch)) {
            __builtin_prefetch(storage + used + 384, 1, 3);
            __builtin_prefetch(storage + used + 448, 1, 3);
        }
    }

    struct CommandBase {
        virtual ~CommandBase() = default;
        virtual void Execute(vk::CommandBuffer cmdbuf) = 0;
        CommandBase* next{};
    };
    template <typename Func>
    struct TypedCommand final : CommandBase {
        explicit TypedCommand(Func&& func_) : func{std::move(func_)} {}
        explicit TypedCommand(const Func& func_) : func{func_} {}
        void Execute(vk::CommandBuffer cmdbuf) override {
            func(cmdbuf);
        }
        Func func;
    };

    /// A block of its own for a payload the chunk cannot hold. Kept by the chunk and reused: a
    /// draw whose record data exceeds Capacity allocates its block when the draw first shows up,
    /// not on every frame it appears in. Without one the payload had nowhere to go and the assert
    /// in ReserveRecordData stopped the game ("oversized command data").
    struct SpillBlock {
        std::unique_ptr<std::byte[]> bytes;
        size_t size = 0;
        size_t used = 0; ///< filled from the front each pass (Execute resets it)
    };

    /// Places an oversized payload in the block reserved last, or in a new one when it does not
    /// fit. A payload beyond what a draw could carry (or a failed allocation) returns null; the
    /// caller reports that instead of writing through it.
    void* AllocateOversized(size_t bytes, size_t align) {
        static constexpr size_t Ceiling = 16 * 1024 * 1024;
        static constexpr size_t MinBlock = 128 * 1024;
        if (bytes > Ceiling) {
            return nullptr;
        }
        if (!spill_blocks.empty()) {
            SpillBlock& block = spill_blocks.back();
            std::byte* const base = block.bytes.get();
            const auto address = reinterpret_cast<uintptr_t>(base) + block.used;
            const auto aligned = (address + align - 1) & ~uintptr_t(align - 1);
            const size_t offset = size_t(aligned - reinterpret_cast<uintptr_t>(base));
            if (offset + bytes <= block.size) {
                block.used = offset + bytes;
                return base + offset;
            }
        }
        const size_t size = std::max(bytes + align, MinBlock);
        std::unique_ptr<std::byte[]> block{new (std::nothrow) std::byte[size]};
        if (!block) {
            return nullptr;
        }
        std::byte* const base = block.get();
        const auto address = reinterpret_cast<uintptr_t>(base);
        const auto aligned = (address + align - 1) & ~uintptr_t(align - 1);
        const size_t offset = size_t(aligned - address);
        spill_blocks.push_back(SpillBlock{std::move(block), size, offset + bytes});
        return base + offset;
    }

    alignas(64) std::byte storage[Capacity];
    size_t used = 0;
    CommandBase* first{};
    CommandBase* last{};
    std::vector<SpillBlock> spill_blocks; ///< oversized payloads (AllocateOversized)
};

} // namespace Vulkan
