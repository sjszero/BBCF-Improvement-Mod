#pragma once

#include "TasNativeArchive.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

struct TasFrameInput {
    uint16_t p1 = 5;
    uint16_t p2 = 5;
};

struct TasSection {
    size_t frame = 0;
    std::string name;
};

// File data only. Nothing in this module dereferences a saved address or calls the game.
namespace TasProjectFile {
constexpr uint32_t kVersion = 4; // V4 adds bounded post-save runtime evidence text
constexpr uint32_t kMaxRuntimeEvidence = 256 * 1024;
constexpr uint32_t kSnapshotCapacity = 0xA10000;
constexpr uint32_t kLeadInFrames = 60;
// Archives capture material; they do NOT certify independent native restoration.
constexpr uint32_t kArchiveOnly = 1;

struct Base {
    uint32_t frame = 0;
    uint32_t firstSize = 0;
    uint32_t secondOffset = 0;
    uint32_t secondSize = 0;
    uint64_t sourceManager = 0;
    uint64_t sourceBuffer = 0;
    uint32_t sourcePhysicalSlot = 0;
    uint64_t sourceEpoch = 0;
    uint64_t sourceSaveSerial = 0;
    std::array<unsigned char, 0x48> descriptor{}; // provenance, never blindly written back
    std::array<unsigned char, 0x400> restoreQueue{}; // 64 x (saved source, live destination, size, padding)
    bool restoreQueueCaptured = false; // mandatory since V2; not proof of full restore coverage
    TasNativeArchive::Dependencies dependencies;
    std::string runtimeEvidence; // diagnostic only; never parsed as permission to load
    std::string details; // sampled at capture, NOT at export
    std::vector<unsigned char> payload; // two ranges packed without the gap
};

struct BasePair {
    Base a, b;
    std::string compatibility;
    bool ready = false;
};

struct Project {
    BasePair base;
    std::vector<TasFrameInput> movie;
    std::vector<TasSection> sections;
    uint32_t cursor = 0; // editor metadata, never a claim that the game is at this frame
};

// Explicit little-endian fields, bounded lengths, per-block FNV-1a corruption checks.
// FNV is not authentication. Native state files must not be treated as trusted pointers.
// Read is transactional: on failure `out` is unchanged. Streams must be binary.
bool Write(std::ostream& stream, const BasePair& base,
    const std::vector<TasFrameInput>& movie, const std::vector<TasSection>& sections,
    size_t cursor, std::string& error);
bool Read(std::istream& stream, Project& out, std::string& error);
}