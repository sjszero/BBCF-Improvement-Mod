#pragma once
#include <array>
#include <cstdint>
#include <vector>

// Portable archive data, not native structs. No stored address may be dereferenced by a reader.
namespace TasNativeArchive {
constexpr uint32_t kAuxCount = 4;
// Exact prefixes containing the ten slot histories and their selectors, not callback owners.
// Static evidence: 0x784910/0x784A70, 0x786E60, 0x7845B0, 0x784E40.
constexpr uint32_t kContextOffsets[kAuxCount] = { 0x174, 0x178, 0x17C, 0x180 };
constexpr uint32_t kPrefixSizes[kAuxCount] = { 0x108, 0x4668, 0x2180, 0x1E88 };
constexpr uint32_t kAuxPayloadCapacity = 0x200000;

struct Auxiliary {
    uint32_t contextOffset = 0;
    uint32_t sourceAddress = 0;
    std::vector<unsigned char> prefix;
    // Only +174 owns an out-of-line byte stream captured here: selected slot, not all 10.
    uint32_t sourcePayload = 0;
    std::vector<unsigned char> payload;
};
struct Dependencies {
    uint32_t sourceContext = 0;
    std::array<Auxiliary, kAuxCount> auxiliary;
    bool captured = false;
};
inline uint32_t Word(const unsigned char* bytes) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= static_cast<uint32_t>(bytes[i]) << (8 * i);
    return v;
}
// Structural evidence only. Does not validate pointed-to allocations, callbacks or object lifetime.
inline bool Valid(const Dependencies& deps, uint32_t slot) {
    if (!deps.captured || !deps.sourceContext || deps.sourceContext > UINT32_MAX - 0x184 || slot >= 10) return false;
    for (uint32_t i = 0; i < kAuxCount; ++i) {
        const auto& a = deps.auxiliary[i];
        if (a.contextOffset != kContextOffsets[i] || !a.sourceAddress ||
            a.sourceAddress > UINT32_MAX - kPrefixSizes[i] || a.prefix.size() != kPrefixSizes[i]) return false;
        if (i == 0) {
            const auto* record = a.prefix.data() + slot * 0x18;
            const uint32_t start = Word(record), length = Word(record + 4), end = Word(record + 0x14);
            if (Word(a.prefix.data() + 0xF4) >= 10 || !start ||
                start > UINT32_MAX - kAuxPayloadCapacity || length > kAuxPayloadCapacity ||
                end < start || end - start != length || a.sourcePayload != start || a.payload.size() != length) return false;
        } else {
            if (Word(a.prefix.data()) >= 10 || a.sourcePayload || !a.payload.empty()) return false;
            const uint32_t stride = i == 1 ? 0x708 : (i == 2 ? 0x358 : 0x30C);
            const uint32_t begin = i == 1 ? 0x14 : 0x10;
            for (uint32_t history = 0; history < 10; ++history) {
                const auto* record = a.prefix.data() + begin + history * stride;
                const uint32_t count = Word(record + 4);
                // Bounds derived from inline layout, not guessed event semantics.
                const uint32_t capacity = i == 1 ? 32 : (i == 2 ? 53 : 64);
                if (count > capacity || (i == 3 && Word(record + 0x208) > 64)) return false;
            }
        }
    }
    return true;
}
} // namespace TasNativeArchive