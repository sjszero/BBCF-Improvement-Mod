#pragma once
#include "TasProjectFile.h"
#include <exception>
#include <string>
#include <utility>
#include <vector>

// Pure file preflight. Never dereferences file addresses; never grants permission to load.
namespace TasRestorePlan {
struct CopyJob {
    uint32_t packedOffset = 0;
    uint32_t nativeOffset = 0;
    uint32_t liveDestination = 0;
    uint32_t size = 0;
    uint32_t padding = 0;
};
// First stream has no tags: CA0/9A0 transfer raw caller-sized bytes; CD0/9D0
// transfer an 8-byte (destination,size) header followed by data. Never infer a
// call schema by scanning pointer-looking bytes. The caller must supply a schema
// derived from a separately verified native save/load traversal, not from the file.
enum class FirstCallKind { Raw, Addressed };
struct FirstCall {
    FirstCallKind kind = FirstCallKind::Raw;
    uint32_t size = 0;
    uint32_t expectedDestination = 0; // required even for addressed records
};
struct FirstTransfer {
    uint32_t packedOffset = 0;
    uint32_t size = 0;
    uint32_t liveDestination = 0;
    uint32_t headerOffset = UINT32_MAX; // raw data has no address header
};
struct FirstStreamPlan {
    std::vector<FirstTransfer> transfers;
    uint32_t nativeOperationCount = 0;
    bool nativeLoadAllowed = false; // schema agreement is not proof of live ownership
};
inline bool AnalyzeFirstStream(const TasProjectFile::Base& b,
    const std::vector<FirstCall>& schema, FirstStreamPlan& out, std::string& error) try {
    const auto fail = [&](const char* text) { error = text; return false; };
    if (!b.firstSize || b.firstSize > TasProjectFile::kSnapshotCapacity ||
        b.firstSize > b.payload.size() || !b.sourceBuffer ||
        b.sourceBuffer > UINT32_MAX - TasProjectFile::kSnapshotCapacity)
        return fail("Invalid first-stream range.");
    const uint32_t writes = TasNativeArchive::Word(b.descriptor.data() + 0x0C);
    const uint32_t reads = TasNativeArchive::Word(b.descriptor.data() + 0x10);
    if (schema.empty() || schema.size() > writes || reads > writes)
        return fail("Missing first-stream call schema or inconsistent operation counts.");
    FirstStreamPlan candidate;
    uint32_t cursor = 0;
    uint64_t operations = 0;
    for (const auto& call : schema) {
        FirstTransfer transfer;
        transfer.size = call.size;
        transfer.liveDestination = call.expectedDestination;
        if (call.kind == FirstCallKind::Addressed) {
            if (b.firstSize - cursor < 8) return fail("Truncated first-stream address header.");
            transfer.headerOffset = cursor;
            if (TasNativeArchive::Word(b.payload.data() + cursor) != call.expectedDestination ||
                TasNativeArchive::Word(b.payload.data() + cursor + 4) != call.size)
                return fail("First-stream header disagrees with verified call schema.");
            cursor += 8;
            operations += 2; // header and data each increment +0C/+10, even for zero bytes
        } else if (call.kind == FirstCallKind::Raw) {
            ++operations;
        } else {
            return fail("Unknown first-stream call kind.");
        }
        if (operations > writes || transfer.size > b.firstSize - cursor)
            return fail("First-stream transfer exceeds recorded operations or payload.");
        if (transfer.size) {
            if (!transfer.liveDestination || transfer.liveDestination > UINT32_MAX - transfer.size)
                return fail("Invalid first-stream destination arithmetic.");
            if (static_cast<uint64_t>(transfer.liveDestination) < b.sourceBuffer + TasProjectFile::kSnapshotCapacity &&
                static_cast<uint64_t>(transfer.liveDestination) + transfer.size > b.sourceBuffer)
                return fail("First-stream destination overlaps snapshot storage.");
        }
        transfer.packedOffset = cursor;
        candidate.transfers.push_back(transfer);
        cursor += transfer.size;
    }
    if (cursor != b.firstSize || operations != writes)
        return fail("First-stream schema does not exactly cover payload and operation count.");
    candidate.nativeOperationCount = static_cast<uint32_t>(operations);
    out = std::move(candidate);
    error.clear();
    return true;
} catch (const std::exception& ex) {
    error = ex.what();
    return false;
}

// Non-executable inventory. Raw history values can be handles/IDs, not necessarily pointers.
struct HistoryUse {
    uint32_t contextOffset = 0;
    uint32_t slot = 0;
    uint32_t entry = 0;
    uint32_t callbackMemberOffset = 0;
    uint32_t argument0 = 0;
    uint32_t argument1 = 0;
    bool active = false; // inactive records are retained: native replay can reactivate them
};
struct Plan {
    std::vector<CopyJob> jobs;
    std::vector<HistoryUse> historyUses;
    bool firstStreamSchemaAvailable = false; // no complete game traversal reconstructed yet
    // Proven descriptor pointers to rebase: base/read/end of both streams only.
    std::array<uint32_t, 6> descriptorPointerOffsets{{ 4, 0x14, 0x18, 0x1C, 0x2C, 0x30 }};
    bool nativeLoadAllowed = false; // always false until runtime ownership reconstruction exists
    std::string blockers;
};
inline bool Analyze(const TasProjectFile::Base& b, Plan& out, std::string& error) try {
    const auto fail = [&](const char* text) { error = text; return false; };
    const auto word = [&](size_t offset) { return TasNativeArchive::Word(b.descriptor.data() + offset); };
    if (!b.restoreQueueCaptured || !TasNativeArchive::Valid(b.dependencies, b.sourcePhysicalSlot))
        return fail("Missing native archive dependencies.");
    if (!b.sourceBuffer || b.sourceBuffer > UINT32_MAX - TasProjectFile::kSnapshotCapacity ||
        !b.firstSize || !b.secondSize || b.firstSize > b.secondOffset ||
        b.secondOffset > TasProjectFile::kSnapshotCapacity ||
        b.secondSize > TasProjectFile::kSnapshotCapacity - b.secondOffset ||
        b.payload.size() != static_cast<size_t>(b.firstSize) + b.secondSize)
        return fail("Invalid base payload layout.");
    const uint32_t start = static_cast<uint32_t>(b.sourceBuffer);
    const uint32_t second = start + b.secondOffset;
    const uint32_t end = second + b.secondSize;
    if (word(0) != b.frame || word(4) != start || word(0x18) != start + b.firstSize ||
        word(0x1C) != second || word(0x20) != b.secondSize || word(0x30) != end ||
        (word(8) != b.firstSize && word(8) != b.payload.size()) ||
        word(0x14) < start || word(0x14) > start + b.firstSize || word(0x2C) < second || word(0x2C) > end)
        return fail("Descriptor disagrees with archived ranges/frame.");
    const uint32_t count = word(0x24);
    // Native rewind stops at index zero, not at an arbitrary ring origin. Wrapped captures
    // need a separately proven replay recipe; do not guess a job count from pending alone.
    // Save enqueues jobs and also performs the copy synchronously (0x786C30).
    // Queue pending count need not be zero: load rewinds the consumed prefix back into it.
    // Accept any non-wrapped partition [consumed][pending] with the same total.
    if (!count || count >= 64 || word(0x38) > count || word(0x34) != count - word(0x38) ||
        word(0x3C) != count || (word(0x44) & 1))
        return fail("Copy queue is wrapped, locked or inconsistent with its reservation count.");
    Plan candidate;
    uint32_t cursor = second;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* entry = b.restoreQueue.data() + i * 16;
        const uint32_t source = TasNativeArchive::Word(entry);
        CopyJob job;
        job.liveDestination = TasNativeArchive::Word(entry + 4);
        job.size = TasNativeArchive::Word(entry + 8);
        job.padding = TasNativeArchive::Word(entry + 12);
        const uint32_t expectedPadding = (0u - cursor) & 15u;
        if (job.padding != expectedPadding || job.padding > end - cursor ||
            source != cursor + job.padding || !job.size || job.size > end - source ||
            !job.liveDestination || job.liveDestination > UINT32_MAX - job.size)
            return fail("Invalid copy job source/destination/size/alignment.");
        // This plan is for copying back to live state, never into its own snapshot allocation.
        if (static_cast<uint64_t>(job.liveDestination) < static_cast<uint64_t>(start) + TasProjectFile::kSnapshotCapacity &&
            static_cast<uint64_t>(job.liveDestination) + job.size > start)
            return fail("Copy destination overlaps its snapshot allocation.");
        job.nativeOffset = source - start;
        job.packedOffset = b.firstSize + source - second;
        candidate.jobs.push_back(job);
        cursor = source + job.size;
    }
    if (cursor != end) return fail("Copy jobs do not cover the archived second stream.");
    // +17C stores (arg0,arg1,type,active) at slot+8, stride 16. Its
    // +2180/+2184 members are direct cdecl function pointers, NOT object owners.
    // +180 instead invokes function-wrapper objects at +1E98/+1EB0 (vtable +8).
    for (uint32_t slot = 0; slot < 10; ++slot) {
        const auto* h17c = b.dependencies.auxiliary[2].prefix.data() + 0x10 + slot * 0x358;
        for (uint32_t i = 0; i < TasNativeArchive::Word(h17c + 4); ++i) {
            const auto* entry = h17c + 8 + i * 16;
            const uint32_t type = TasNativeArchive::Word(entry + 8);
            if (type != 1 && type != 2) continue; // 0x784510 does not call either callback
            HistoryUse use;
            use.contextOffset = 0x17C; use.slot = slot; use.entry = i;
            use.callbackMemberOffset = type == 1 ? 0x2180 : 0x2184;
            use.argument0 = TasNativeArchive::Word(entry);
            use.argument1 = TasNativeArchive::Word(entry + 4);
            use.active = entry[12] != 0;
            candidate.historyUses.push_back(use);
        }
        const auto* h180 = b.dependencies.auxiliary[3].prefix.data() + 0x10 + slot * 0x30C;
        for (uint32_t i = 0; i < TasNativeArchive::Word(h180 + 4); ++i) {
            const auto* entry = h180 + 8 + i * 8;
            HistoryUse use;
            use.contextOffset = 0x180; use.slot = slot; use.entry = i;
            use.callbackMemberOffset = 0x1E98;
            use.argument0 = TasNativeArchive::Word(entry); use.active = entry[4] != 0;
            candidate.historyUses.push_back(use);
        }
        for (uint32_t i = 0; i < TasNativeArchive::Word(h180 + 0x208); ++i) {
            HistoryUse use;
            use.contextOffset = 0x180; use.slot = slot; use.entry = i;
            use.callbackMemberOffset = 0x1EB0;
            use.argument0 = TasNativeArchive::Word(h180 + 0x20C + i * 4); use.active = true;
            candidate.historyUses.push_back(use);
        }
    }
    candidate.blockers = "Native load blocked: missing verified mixed first-stream call schema, live object/callback ownership, "
        "auxiliary cross-slot restore policy, executable/lifetime identity and runtime transaction not implemented.";
    out = std::move(candidate);
    error.clear();
    return true;
} catch (const std::exception& ex) {
    error = ex.what();
    return false;
}
} // namespace TasRestorePlan