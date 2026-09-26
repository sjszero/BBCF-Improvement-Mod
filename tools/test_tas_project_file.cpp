// Standalone codec test; compile together with src/Game/TasProjectFile.cpp.
// No Windows/game dependencies. Does not test native restoration.
#include "../src/Game/TasProjectFile.h"
#include "../src/Game/TasRestorePlan.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace TasProjectFile;
void Check(bool value) { if (!value) throw std::runtime_error("test failed"); }

Base MakeBase(uint32_t frame, uint32_t slot) {
    Base b;
    b.frame = frame; b.firstSize = 3; b.secondOffset = 16; b.secondSize = 2;
    b.sourceManager = 0x10000; b.sourceBuffer = 0x1000000 + slot * kSnapshotCapacity;
    b.sourcePhysicalSlot = slot; b.sourceEpoch = 1; b.sourceSaveSerial = 2;
    b.payload = { 0, 1, 255, 3, 4 };
    b.details = "P1 x=-100 y=0\nP2 x=100 y=0\n";
    b.runtimeEvidence = "synthetic-evidence-only\n";
    for (size_t i = 0; i < b.descriptor.size(); ++i) b.descriptor[i] = static_cast<unsigned char>(i);
    // Queue descriptor: one completed job, read/write index 1, a synthetic allocation, unlocked.
    const auto word = [&](size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) b.descriptor[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    const uint32_t start = static_cast<uint32_t>(b.sourceBuffer);
    word(0, frame); word(4, start); word(8, 5); word(0x14, start);
    word(0x18, start + 3); word(0x1C, start + 16); word(0x20, 2);
    word(0x24, 1); word(0x28, 1); word(0x2C, start + 18); word(0x30, start + 18);
    word(0x34, 0); word(0x38, 1); word(0x3C, 1); word(0x40, 0x20000); word(0x44, 0);
    const uint32_t job[] = { start + 16, 0x90000, 2, 0 };
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) b.restoreQueue[i * 4 + j] = static_cast<unsigned char>(job[i] >> (8 * j));
    b.restoreQueueCaptured = true;
    b.dependencies.sourceContext = 0x30000;
    b.dependencies.captured = true;
    for (uint32_t i = 0; i < TasNativeArchive::kAuxCount; ++i) {
        auto& a = b.dependencies.auxiliary[i];
        a.contextOffset = TasNativeArchive::kContextOffsets[i];
        a.sourceAddress = 0x40000 + i * 0x10000;
        a.prefix.resize(TasNativeArchive::kPrefixSizes[i], 0);
        if (i == 0) {
            a.sourcePayload = 0x80000;
            a.payload = { 0xAB, 0xCD };
            const auto set = [&](size_t offset, uint32_t v) {
                for (unsigned j = 0; j < 4; ++j) a.prefix[offset + j] = static_cast<unsigned char>(v >> (8 * j));
            };
            set(slot * 0x18, a.sourcePayload);
            set(slot * 0x18 + 4, 2);
            set(slot * 0x18 + 0x14, a.sourcePayload + 2);
        }
    }
    return b;
}

void TestFirstStreamAndHistory() {
    using namespace TasRestorePlan;
    const auto put = [](unsigned char* p, uint32_t v) {
        for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
    };
    Base b = MakeBase(100, 1);
    b.firstSize = 14;
    b.payload.assign(16, 0); // raw 3, header 8, data 2, raw 1, second stream 2
    b.payload[0] = 0xFF; b.payload[1] = 0xFF; b.payload[2] = 0xFF;
    put(b.payload.data() + 3, 0x90000); put(b.payload.data() + 7, 2);
    put(b.descriptor.data() + 0x0C, 4); put(b.descriptor.data() + 0x10, 0);
    std::vector<FirstCall> schema = {
        { FirstCallKind::Raw, 3, 0x91000 },
        { FirstCallKind::Addressed, 2, 0x90000 },
        { FirstCallKind::Raw, 1, 0x92000 }
    };
    FirstStreamPlan first;
    std::string error;
    Check(AnalyzeFirstStream(b, schema, first, error));
    Check(!first.nativeLoadAllowed && first.nativeOperationCount == 4 && first.transfers.size() == 3);
    Check(first.transfers[0].headerOffset == UINT32_MAX);
    Check(first.transfers[1].headerOffset == 3 && first.transfers[1].packedOffset == 11);
    const auto reject = [&](const Base& bad, const std::vector<FirstCall>& calls) {
        Check(!AnalyzeFirstStream(bad, calls, first, error));
        Check(first.transfers.size() == 3 && first.transfers[1].packedOffset == 11);
    };
    reject(b, {}); // no auto-detection of headers
    auto calls = schema;
    calls[1].expectedDestination++;
    reject(b, calls);
    calls = schema; calls[0].size = UINT32_MAX;
    reject(b, calls);
    calls = schema; calls[0].expectedDestination = static_cast<uint32_t>(b.sourceBuffer);
    reject(b, calls);
    calls = schema; calls[0].expectedDestination = UINT32_MAX - 1;
    reject(b, calls);
    calls = schema; calls[0].kind = static_cast<FirstCallKind>(99);
    reject(b, calls);
    Base bad = b; bad.firstSize = 10; reject(bad, schema); // truncated header
    bad = b; bad.firstSize = 15; reject(bad, schema); // unexplained byte
    bad = b; put(bad.descriptor.data() + 0x0C, 3); reject(bad, schema);
    bad = b; put(bad.descriptor.data() + 0x10, 5); reject(bad, schema);
    // Zero-byte raw calls still consume one native operation, not a stream byte.
    calls = schema; calls.push_back({ FirstCallKind::Raw, 0, 0 });
    put(b.descriptor.data() + 0x0C, 5);
    Check(AnalyzeFirstStream(b, calls, first, error));
    Check(first.nativeOperationCount == 5 && first.transfers.back().size == 0);

    b = MakeBase(100, 1);
    auto* h17c = b.dependencies.auxiliary[2].prefix.data() + 0x10 + 9 * 0x358;
    put(h17c + 4, 2);
    put(h17c + 8, 0x123); put(h17c + 12, 0x456); put(h17c + 16, 1);
    h17c[20] = 1;
    put(h17c + 24, 0x789); put(h17c + 28, 0xABC); put(h17c + 32, 2);
    auto* h180 = b.dependencies.auxiliary[3].prefix.data() + 0x10 + 8 * 0x30C;
    put(h180 + 4, 1); put(h180 + 8, 0xDEF); h180[12] = 1;
    put(h180 + 0x208, 1); put(h180 + 0x20C, 0xFED);
    Plan plan;
    Check(Analyze(b, plan, error));
    Check(!plan.nativeLoadAllowed && !plan.firstStreamSchemaAvailable && plan.historyUses.size() == 4);
    // Slot 8 is enumerated before slot 9; primary then secondary lists.
    Check(plan.historyUses[0].slot == 8 && plan.historyUses[0].callbackMemberOffset == 0x1E98);
    Check(plan.historyUses[1].argument0 == 0xFED && plan.historyUses[1].callbackMemberOffset == 0x1EB0);
    Check(plan.historyUses[2].argument1 == 0x456 && plan.historyUses[2].callbackMemberOffset == 0x2180);
    Check(plan.historyUses[3].argument0 == 0x789 && !plan.historyUses[3].active);
}

int main() {
    try {
        TestFirstStreamAndHistory();
        BasePair base;
        base.ready = true; base.compatibility = "synthetic-test-only";
        base.a = MakeBase(100, 1); base.b = MakeBase(160, 0);
        std::vector<TasFrameInput> movie(3);
        movie[1].p1 = 0x15; movie[2].p2 = 0x109;
        std::vector<TasSection> sections(1);
        sections[0].frame = 1; sections[0].name = "section";
        std::string error;
        std::ostringstream encoded(std::ios::binary | std::ios::out);
        Check(Write(encoded, base, movie, sections, 2, error));
        const std::string bytes = encoded.str();
        Project parsed;
        std::istringstream input(bytes, std::ios::binary | std::ios::in);
        Check(Read(input, parsed, error));
        Check(parsed.base.a.payload == base.a.payload && parsed.base.b.payload == base.b.payload);
        Check(parsed.base.a.descriptor == base.a.descriptor && parsed.base.b.details == base.b.details);
        Check(parsed.base.a.runtimeEvidence == base.a.runtimeEvidence &&
            parsed.base.b.runtimeEvidence == base.b.runtimeEvidence);
        Check(parsed.base.a.restoreQueueCaptured && parsed.base.a.restoreQueue == base.a.restoreQueue);
        Check(parsed.base.b.restoreQueueCaptured && parsed.base.b.restoreQueue == base.b.restoreQueue);
        Check(parsed.cursor == 2 && parsed.movie[1].p1 == 0x15 && parsed.sections[0].name == "section");
        std::ostringstream reencoded(std::ios::binary | std::ios::out);
        Check(Write(reencoded, parsed.base, parsed.movie, parsed.sections, parsed.cursor, error));
        Check(reencoded.str() == bytes); // all serialized fields, not just payload
        TasRestorePlan::Plan plan;
        Check(TasRestorePlan::Analyze(parsed.base.a, plan, error));
        Check(!plan.nativeLoadAllowed && !plan.blockers.empty() && plan.jobs.size() == 1);
        Check(plan.jobs[0].packedOffset == 3 && plan.jobs[0].nativeOffset == 16);
        Base pendingPlan = parsed.base.a;
        pendingPlan.descriptor[0x34] = 1; pendingPlan.descriptor[0x38] = 0;
        Check(TasRestorePlan::Analyze(pendingPlan, plan, error));
        Base invalidPlan = parsed.base.a;
        invalidPlan.restoreQueue[8] = 3; // exceeds second payload
        Check(!TasRestorePlan::Analyze(invalidPlan, plan, error));
        Check(plan.jobs.size() == 1 && plan.jobs[0].size == 2); // failed analysis leaves output unchanged
        invalidPlan = parsed.base.a;
        invalidPlan.descriptor[0x24] = 64; // wrapped queue cannot use index-zero rewind recipe
        Check(!TasRestorePlan::Analyze(invalidPlan, plan, error));
        invalidPlan = parsed.base.a;
        invalidPlan.dependencies.auxiliary[0].payload.clear();
        Check(!TasRestorePlan::Analyze(invalidPlan, plan, error));

        const auto reject = [&](const std::string& bad) {
            Project sentinel;
            sentinel.cursor = 123; sentinel.base.compatibility = "unchanged";
            std::istringstream stream(bad, std::ios::binary | std::ios::in);
            Check(!Read(stream, sentinel, error));
            Check(sentinel.cursor == 123 && sentinel.base.compatibility == "unchanged");
        };
        // V3 contains ~68 KiB of auxiliary prefixes. Bound runtime instead of quadratic
        // full-file scans for every byte; cover each byte in the header and sampled body offsets.
        for (size_t i = 0; i < bytes.size(); i += (i < 256 ? 1 : 97)) {
            reject(bytes.substr(0, i));
            std::string corrupt = bytes;
            corrupt[i] = static_cast<char>(static_cast<unsigned char>(corrupt[i]) ^ 0x80);
            reject(corrupt);
        }
        reject(bytes + "trailing");
        std::string oldVersion = bytes;
        oldVersion[17] = 1; // V1 has no copy queue; reject before interpreting base blocks.
        reject(oldVersion);
        oldVersion[17] = 2;
        reject(oldVersion);
        oldVersion[17] = 3;
        reject(oldVersion);
        std::ostringstream oversizedEvidence;
        base.a.runtimeEvidence.assign(kMaxRuntimeEvidence + 1, 'x');
        Check(!Write(oversizedEvidence, base, movie, sections, 0, error));
        Check(oversizedEvidence.str().empty());
        base.a.runtimeEvidence.clear();
        std::ostringstream disabledEvidence;
        Check(Write(disabledEvidence, base, movie, sections, 0, error));
        Project disabledParsed;
        std::istringstream disabledInput(disabledEvidence.str(), std::ios::binary | std::ios::in);
        Check(Read(disabledInput, disabledParsed, error) && disabledParsed.base.a.runtimeEvidence.empty());
        base.a.runtimeEvidence = "synthetic-evidence-only\n";
        const uint64_t originalBuffer = base.a.sourceBuffer;
        base.a.sourceBuffer = base.b.sourceBuffer + 16;
        std::ostringstream overlappingPair;
        Check(!Write(overlappingPair, base, movie, sections, 0, error) && overlappingPair.str().empty());
        base.a.sourceBuffer = originalBuffer;
        std::ostringstream missingAux;
        base.a.dependencies.captured = false;
        Check(!Write(missingAux, base, movie, sections, 0, error));
        Check(missingAux.str().empty());
        base.a.dependencies.captured = true;
        base.a.dependencies.auxiliary[1].prefix[0x18] = 33; // inline capacity is 32
        Check(!Write(missingAux, base, movie, sections, 0, error));
        base.a.dependencies.auxiliary[1].prefix[0x18] = 0;
        std::ostringstream missingQueue;
        base.a.restoreQueueCaptured = false;
        Check(!Write(missingQueue, base, movie, sections, 0, error));
        Check(missingQueue.str().empty());
        base.a.restoreQueueCaptured = true;
        base.a.descriptor[0x44] = 1;
        Check(!Write(missingQueue, base, movie, sections, 0, error));
        base.a.descriptor[0x44] = 0;
        base.a.descriptor[0x38] = 64;
        Check(!Write(missingQueue, base, movie, sections, 0, error));
        base.a.descriptor[0x38] = 1;
        base.b.frame = 161;
        std::ostringstream invalid;
        Check(!Write(invalid, base, movie, sections, 0, error));
        Check(invalid.str().empty());
        base.b.frame = 160;
        base.a.secondOffset = kSnapshotCapacity;
        Check(!Write(invalid, base, movie, sections, 0, error));
        base.a.secondOffset = 16;
        movie[0].p1 = 0;
        Check(!Write(invalid, base, movie, sections, 0, error));
        std::cout << "PASS: roundtrip, sampled corruption/truncation, trailing data, transactional rejection, invalid pair/layout/input, restore-plan bounds\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}