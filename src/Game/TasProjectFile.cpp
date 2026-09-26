#include "TasProjectFile.h"

#include <cstring>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace TasProjectFile {
namespace {
constexpr uint32_t kMaxFrames = 1000000; // file allocation bound, not an editor limit
constexpr uint32_t kMaxSections = 4096;
constexpr uint32_t kMaxText = 4096;
const char kMagic[] = "BBCF_TAS_PROJECT";

void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// Stream the payload; do not build another 20 MiB encoded buffer.
class Codec {
public:
    explicit Codec(std::istream& in) : input(&in) {}
    explicit Codec(std::ostream& out) : output(&out) {}
    bool Reading() const { return input != nullptr; }
    void Bytes(void* data, size_t size) {
        if (Reading()) {
            input->read(static_cast<char*>(data), static_cast<std::streamsize>(size));
            Require(static_cast<bool>(*input), "Truncated or unreadable TAS project.");
        } else {
            output->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            Require(static_cast<bool>(*output), "Failed writing TAS project.");
        }
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    }
    void U32(uint32_t& value) {
        unsigned char bytes[4];
        for (unsigned int i = 0; i < 4; ++i) bytes[i] = static_cast<unsigned char>(value >> (i * 8));
        Bytes(bytes, sizeof(bytes));
        if (Reading()) {
            value = 0;
            for (unsigned int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[i]) << (i * 8);
        }
    }
    void U64(uint64_t& value) {
        uint32_t lo = static_cast<uint32_t>(value), hi = static_cast<uint32_t>(value >> 32);
        U32(lo); U32(hi);
        if (Reading()) value = lo | (static_cast<uint64_t>(hi) << 32);
    }
    void Text(std::string& text, uint32_t limit = kMaxText) {
        uint32_t size = static_cast<uint32_t>(text.size());
        Require(Reading() || text.size() <= limit, "TAS project text exceeds limit.");
        U32(size);
        Require(size <= limit, "TAS project text exceeds limit.");
        if (Reading()) text.resize(size);
        if (size) Bytes(&text[0], size);
    }
    void Checkpoint() {
        const uint32_t expected = hash;
        uint32_t stored = expected;
        U32(stored);
        Require(stored == expected, "TAS project checksum mismatch.");
        hash = 2166136261u;
    }
private:
    std::istream* input = nullptr;
    std::ostream* output = nullptr;
    uint32_t hash = 2166136261u;
};

uint32_t DescriptorWord(const Base& b, size_t offset) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(b.descriptor[offset + i]) << (8 * i);
    return value;
}

void ValidateBase(const Base& b) {
    Require(TasNativeArchive::Valid(b.dependencies, b.sourcePhysicalSlot), "Invalid or missing TAS auxiliary capture.");
    Require(b.restoreQueueCaptured && DescriptorWord(b, 0x34) <= 64 &&
        DescriptorWord(b, 0x38) < 64 && DescriptorWord(b, 0x3C) < 64 &&
        DescriptorWord(b, 0x40) != 0 && DescriptorWord(b, 0x40) <= UINT32_MAX - 0x400 &&
        (DescriptorWord(b, 0x44) & 1) == 0,
        "Missing or invalid TAS restore queue capture.");
    Require(b.firstSize > 0 && b.secondSize > 0 &&
        b.firstSize <= b.secondOffset && b.secondOffset <= kSnapshotCapacity &&
        b.secondSize <= kSnapshotCapacity - b.secondOffset &&
        b.payload.size() == static_cast<size_t>(b.firstSize) + b.secondSize,
        "Invalid TAS base range layout.");
    Require(b.sourceManager && b.sourceManager <= UINT32_MAX &&
        b.sourceBuffer && b.sourceBuffer <= UINT32_MAX - kSnapshotCapacity &&
        b.sourcePhysicalSlot < 10 && b.sourceEpoch && b.sourceSaveSerial,
        "Invalid TAS base provenance.");
    Require(!b.details.empty() && b.details.size() <= kMaxText, "Missing TAS base capture details.");
    Require(b.runtimeEvidence.size() <= kMaxRuntimeEvidence, "TAS runtime evidence exceeds limit.");
}

void ValidatePair(const BasePair& pair) {
    Require(pair.ready && !pair.compatibility.empty() && pair.compatibility.size() <= kMaxText,
        "No captured TAS base pair.");
    ValidateBase(pair.a); ValidateBase(pair.b);
    Require(pair.b.frame > pair.a.frame && pair.b.frame - pair.a.frame == kLeadInFrames,
        "TAS base pair is not 60 frames apart.");
    Require(pair.a.sourceManager == pair.b.sourceManager && pair.a.sourceEpoch == pair.b.sourceEpoch &&
        (pair.a.sourceBuffer + kSnapshotCapacity <= pair.b.sourceBuffer ||
            pair.b.sourceBuffer + kSnapshotCapacity <= pair.a.sourceBuffer) &&
        pair.a.sourcePhysicalSlot != pair.b.sourcePhysicalSlot,
        "TAS base pair provenance mismatch.");
    Require(pair.a.dependencies.sourceContext == pair.b.dependencies.sourceContext, "TAS base context changed during capture.");
    for (uint32_t i = 0; i < TasNativeArchive::kAuxCount; ++i)
        Require(pair.a.dependencies.auxiliary[i].sourceAddress == pair.b.dependencies.auxiliary[i].sourceAddress,
            "TAS auxiliary manager changed during base capture.");
}

void AuxiliaryFields(Codec& c, TasNativeArchive::Dependencies& deps) {
    c.U32(deps.sourceContext);
    for (uint32_t i = 0; i < TasNativeArchive::kAuxCount; ++i) {
        auto& a = deps.auxiliary[i];
        c.U32(a.contextOffset); c.U32(a.sourceAddress);
        uint32_t size = static_cast<uint32_t>(a.prefix.size());
        c.U32(size);
        Require(a.contextOffset == TasNativeArchive::kContextOffsets[i] && size == TasNativeArchive::kPrefixSizes[i],
            "Unsupported TAS auxiliary layout.");
        if (c.Reading()) a.prefix.resize(size);
        c.Bytes(a.prefix.data(), size);
        c.U32(a.sourcePayload);
        size = static_cast<uint32_t>(a.payload.size()); c.U32(size);
        Require(size <= (i == 0 ? TasNativeArchive::kAuxPayloadCapacity : 0u), "Invalid TAS auxiliary payload size.");
        if (c.Reading()) a.payload.resize(size);
        if (size) c.Bytes(a.payload.data(), size);
    }
    if (c.Reading()) deps.captured = true;
}

void BaseFields(Codec& c, Base& b) {
    c.U32(b.frame); c.U32(b.firstSize); c.U32(b.secondOffset); c.U32(b.secondSize);
    c.U64(b.sourceManager); c.U64(b.sourceBuffer); c.U32(b.sourcePhysicalSlot);
    c.U64(b.sourceEpoch); c.U64(b.sourceSaveSerial);
    c.Bytes(b.descriptor.data(), b.descriptor.size());
    c.Bytes(b.restoreQueue.data(), b.restoreQueue.size());
    if (c.Reading()) b.restoreQueueCaptured = true;
    AuxiliaryFields(c, b.dependencies);
    c.Text(b.runtimeEvidence, kMaxRuntimeEvidence);
    c.Text(b.details);
    uint32_t size = static_cast<uint32_t>(b.payload.size());
    c.U32(size);
    Require(size <= kSnapshotCapacity && size > 0 &&
        b.firstSize <= size && b.secondSize == size - b.firstSize,
        "Invalid TAS base payload length.");
    Require(b.firstSize > 0 && b.secondSize > 0 && b.firstSize <= b.secondOffset &&
        b.secondOffset <= kSnapshotCapacity && b.secondSize <= kSnapshotCapacity - b.secondOffset,
        "Invalid TAS base range layout.");
    if (c.Reading()) b.payload.resize(size);
    c.Bytes(b.payload.data(), size);
    c.Checkpoint();
    ValidateBase(b);
}

bool ValidInput(uint16_t input) {
    return (input & 0xF) >= 1 && (input & 0xF) <= 9 && (input & ~0x1FFu) == 0;
}

void ValidateMovie(const std::vector<TasFrameInput>& movie, const std::vector<TasSection>& sections, size_t cursor) {
    Require(!movie.empty() && movie.size() <= kMaxFrames && cursor <= movie.size(), "Invalid TAS project frame count/cursor.");
    Require(sections.size() <= kMaxSections, "Too many TAS project sections.");
    for (const auto& frame : movie) Require(ValidInput(frame.p1) && ValidInput(frame.p2), "Invalid TAS project input.");
    size_t previous = 0;
    bool first = true;
    for (const auto& section : sections) {
        Require(section.frame <= movie.size() && (first || section.frame > previous) &&
            !section.name.empty() && section.name.size() <= kMaxText, "Invalid TAS project section.");
        first = false; previous = section.frame;
    }
}

void Header(Codec& c, std::string& compatibility) {
    char magic[sizeof(kMagic)];
    std::memcpy(magic, kMagic, sizeof(magic));
    c.Bytes(magic, sizeof(magic));
    Require(std::memcmp(magic, kMagic, sizeof(magic)) == 0, "Not a TAS project file.");
    uint32_t version = kVersion, flags = kArchiveOnly, leadIn = kLeadInFrames;
    c.U32(version); c.U32(flags); c.U32(leadIn);
    Require(version == kVersion && flags == kArchiveOnly && leadIn == kLeadInFrames,
        "Unsupported TAS project version or capabilities.");
    c.Text(compatibility);
    c.Checkpoint();
}
} // namespace

bool Write(std::ostream& stream, const BasePair& base,
    const std::vector<TasFrameInput>& movie, const std::vector<TasSection>& sections,
    size_t cursor, std::string& error) {
    try {
        ValidatePair(base); ValidateMovie(movie, sections, cursor);
        Codec c(stream);
        // Codec's write path never mutates fields; const_cast avoids copying large payloads.
        Header(c, const_cast<std::string&>(base.compatibility));
        BaseFields(c, const_cast<Base&>(base.a));
        BaseFields(c, const_cast<Base&>(base.b));
        uint32_t count = static_cast<uint32_t>(movie.size()), savedCursor = static_cast<uint32_t>(cursor);
        c.U32(count); c.U32(savedCursor);
        for (const auto& frame : movie) {
            uint32_t packed = frame.p1 | (static_cast<uint32_t>(frame.p2) << 16);
            c.U32(packed);
        }
        count = static_cast<uint32_t>(sections.size()); c.U32(count);
        for (const auto& section : sections) {
            uint32_t frame = static_cast<uint32_t>(section.frame); c.U32(frame);
            c.Text(const_cast<std::string&>(section.name));
        }
        c.Checkpoint();
        stream.flush();
        Require(static_cast<bool>(stream), "Failed flushing TAS project.");
        error.clear(); return true;
    } catch (const std::exception& ex) { error = ex.what(); return false; }
}

bool Read(std::istream& stream, Project& out, std::string& error) {
    try {
        Project candidate;
        Codec c(stream);
        Header(c, candidate.base.compatibility);
        BaseFields(c, candidate.base.a); BaseFields(c, candidate.base.b);
        candidate.base.ready = true;
        ValidatePair(candidate.base);
        uint32_t count = 0;
        c.U32(count); c.U32(candidate.cursor);
        Require(count > 0 && count <= kMaxFrames && candidate.cursor <= count, "Invalid TAS project frame count/cursor.");
        candidate.movie.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t packed = 0; c.U32(packed);
            TasFrameInput frame;
            frame.p1 = static_cast<uint16_t>(packed); frame.p2 = static_cast<uint16_t>(packed >> 16);
            Require(ValidInput(frame.p1) && ValidInput(frame.p2), "Invalid TAS project input.");
            candidate.movie.push_back(frame);
        }
        c.U32(count);
        Require(count <= kMaxSections, "Too many TAS project sections.");
        candidate.sections.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t frame = 0; c.U32(frame);
            TasSection section; section.frame = frame; c.Text(section.name);
            candidate.sections.push_back(std::move(section));
        }
        c.Checkpoint();
        ValidateMovie(candidate.movie, candidate.sections, candidate.cursor);
        Require(stream.peek() == std::char_traits<char>::eof() && !stream.bad(), "Trailing data in TAS project.");
        out = std::move(candidate);
        error.clear(); return true;
    } catch (const std::exception& ex) { error = ex.what(); return false; }
}
} // namespace TasProjectFile