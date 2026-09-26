#pragma once
#include <Windows.h>
#include "TasNativeArchive.h"
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <initializer_list>

// Read-only observations at the existing post-save boundary. No native calls,
// hooks, locks, memory writes or reads through archive-supplied addresses.
// This is evidence for resolving dispatch, NOT a restore authorization token.
namespace TasRuntimeEvidence {
inline bool Read(uintptr_t address, void* out, size_t size) {
    if (!address || size > UINT32_MAX || address > UINT32_MAX - size) return false;
    uintptr_t cursor = address;
    const uintptr_t end = address + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &m, sizeof(m)) ||
            m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_NOCACHE | PAGE_WRITECOMBINE))) return false;
        const DWORD access = m.Protect & 0xFF;
        if (access != PAGE_READONLY && access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
            access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY) return false;
        const uint64_t next = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m.BaseAddress)) + m.RegionSize;
        if (next <= cursor) return false;
        cursor = static_cast<uintptr_t>(next < end ? next : end);
    }
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        out, size, &got) != FALSE && got == size;
}
inline void Mapping(std::ostream& out, uintptr_t address) {
    MEMORY_BASIC_INFORMATION m{};
    if (!address || !VirtualQuery(reinterpret_cast<const void*>(address), &m, sizeof(m))) {
        out << " map=unavailable"; return;
    }
    out << " allocation=" << reinterpret_cast<uintptr_t>(m.AllocationBase)
        << " region=" << reinterpret_cast<uintptr_t>(m.BaseAddress)
        << " bytes=" << m.RegionSize << " state=" << m.State
        << " protect=" << m.Protect << " type=" << m.Type;
}
inline void Code(std::ostream& out, uint32_t address, uintptr_t module, uint32_t imageSize) {
    out << " target=" << address;
    if (address >= module && static_cast<uint64_t>(address) - module < imageSize)
        out << " rva=" << address - module;
    Mapping(out, address);
    // Only sample code within the identified main image, not arbitrary object pointees.
    std::array<unsigned char, 16> bytes{}, again{};
    if (address >= module && imageSize >= bytes.size() &&
        static_cast<uint64_t>(address) - module <= imageSize - bytes.size() &&
        Read(address, bytes.data(), bytes.size()) && Read(address, again.data(), again.size())) {
        out << " codeStable=" << (bytes == again) << " code=";
        for (auto byte : bytes) out << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
        out << std::setfill(' ');
    }
}
inline bool Object(std::ostream& out, uint32_t object, uintptr_t module, uint32_t imageSize) {
    uint32_t table = 0, tableAfter = 0;
    std::array<uint32_t, 3> methods{}, after{};
    out << "object=" << object;
    Mapping(out, object);
    if (!Read(object, &table, 4) || !Read(table, methods.data(), sizeof(methods))) {
        out << " dispatch=unreadable\n"; return false;
    }
    std::array<unsigned char, 0x60> prefix{}, prefixAfter{};
    const bool prefixRead = Read(object, prefix.data(), prefix.size()) &&
        Read(object, prefixAfter.data(), prefixAfter.size());
    out << " prefixReadable=" << prefixRead;
    if (prefixRead) {
        out << " prefixStable=" << (prefix == prefixAfter) << " prefixWords=";
        for (size_t i = 0; i < prefix.size(); i += 4) out << TasNativeArchive::Word(prefix.data() + i) << ',';
    }
    // Prefix length is a diagnostic read bound, not a proven object allocation size.
    const bool stable = prefixRead && prefix == prefixAfter &&
        Read(object, &tableAfter, 4) && table == tableAfter &&
        Read(table, after.data(), sizeof(after)) && methods == after;
    out << " vtable=" << table << " observedStable=" << stable << '\n';
    for (unsigned i = 0; i < methods.size(); ++i) {
        out << "  method=" << i * 4;
        Code(out, methods[i], module, imageSize); out << '\n';
    }
    return stable;
}
inline std::string Capture(const void* liveModule) {
    std::ostringstream out;
    out << "BBCF_TAS_RUNTIME_EVIDENCE_1\nhexadecimal=true noWrites=true lifetimeProof=false\n" << std::hex;
    FILETIME created{}, ended{}, kernel{}, user{};
    const bool times = GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user) != FALSE;
    const uintptr_t module = reinterpret_cast<uintptr_t>(liveModule);
    out << "pid=" << GetCurrentProcessId() << " tid=" << GetCurrentThreadId()
        << " processCreationKnown=" << times << " processCreation=" << created.dwHighDateTime
        << ':' << created.dwLowDateTime << " module=" << module << '\n';
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS32 nt{};
    if (!Read(module, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0 || static_cast<uint32_t>(dos.e_lfanew) > 0x100000 ||
        module > UINT32_MAX - static_cast<uint32_t>(dos.e_lfanew) ||
        !Read(module + static_cast<uint32_t>(dos.e_lfanew), &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.OptionalHeader.SizeOfImage < 0x142A928 || module > UINT32_MAX - nt.OptionalHeader.SizeOfImage) {
        out << "captureComplete=false reason=unsupported-PE-layout\n"; return out.str();
    }
    const uint32_t imageSize = nt.OptionalHeader.SizeOfImage;
    out << "imageSize=" << imageSize << " peTimestamp=" << nt.FileHeader.TimeDateStamp
        << " peChecksum=" << nt.OptionalHeader.CheckSum
        << " executableSHA256=external-collection-required\n";
    uint32_t context = 0, contextAfter = 0;
    std::array<unsigned char, 0x188> c{}, after{};
    if (!Read(module + 0x142A924, &context, 4) || !Read(context, c.data(), c.size())) {
        out << "captureComplete=false reason=context-unreadable\n"; return out.str();
    }
    const auto word = [&](size_t n) { return TasNativeArchive::Word(c.data() + n); };
    bool complete = times;
    out << "context=" << context << " mode=" << word(0x28) << " auxiliaryEnabled=" << unsigned(c[0x184]) << '\n';
    // These prefixes are diagnostic only: event handles/locks are never restored.
    out << "contextWords=";
    for (size_t i = 0; i < c.size(); i += 4) out << word(i) << ',';
    out << '\n';
    for (uint32_t countOffset : { 0x2Cu, 0xB0u }) {
        const uint32_t count = word(countOffset);
        out << "registry=" << countOffset << " count=" << count << '\n';
        if (count > 32) { complete = false; out << "registry=over-capacity\n"; continue; }
        for (uint32_t i = 0; i < count; ++i) {
            out << " registryEntry=" << i << ' ';
            if (!Object(out, word(countOffset + 4 + i * 4), module, imageSize)) complete = false;
        }
    }
    // Context +00 template queue; +14 working queue. Read each complete bounded backing
    // store, but resolve only active entries. Never enqueue or invoke any entry.
    for (uint32_t q : { 0u, 0x14u }) {
        const uint32_t count = word(q), read = word(q + 4), write = word(q + 8), data = word(q + 12);
        out << "dispatchQueue=" << q << " count=" << count << " read=" << read << " write=" << write
            << " storage=" << data << " lock=" << word(q + 16) << '\n';
        std::array<unsigned char, 64 * 12> jobs{}, verify{};
        if (count > 64 || read >= 64 || write >= 64 || (word(q + 16) & 1) ||
            !Read(data, jobs.data(), jobs.size()) || !Read(data, verify.data(), verify.size()) || jobs != verify) {
            complete = false; out << "queueObservation=unavailable-or-changing\n"; continue;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const auto* job = jobs.data() + ((read + i) & 63) * 12;
            out << " queueEntry=" << i << " part=" << TasNativeArchive::Word(job + 4)
                << " parts=" << TasNativeArchive::Word(job + 8) << ' ';
            if (!Object(out, TasNativeArchive::Word(job), module, imageSize)) complete = false;
        }
    }
    const uint32_t main = word(0x170);
    std::array<unsigned char, 0x2E0> mainBytes{}, mainAfter{};
    if (Read(main, mainBytes.data(), mainBytes.size()) && Read(main, mainAfter.data(), mainAfter.size())) {
        out << "mainManager=" << main << " observedStable=" << (mainBytes == mainAfter) << " words=";
        for (size_t i = 0; i < mainBytes.size(); i += 4) out << TasNativeArchive::Word(mainBytes.data() + i) << ',';
        out << '\n'; complete = complete && mainBytes == mainAfter;
        for (uint32_t slot = 0; slot < 10; ++slot) {
            const uint32_t queue = TasNativeArchive::Word(mainBytes.data() + slot * 0x48 + 0x40);
            std::array<unsigned char, 0x400> jobs{}, jobsAfter{};
            const uint32_t count = TasNativeArchive::Word(mainBytes.data() + slot * 0x48 + 0x24);
            // Only mappings, not target content; retain count==64 as diagnostic evidence.
            if (!count) continue;
            if (count > 64 || !Read(queue, jobs.data(), jobs.size()) ||
                !Read(queue, jobsAfter.data(), jobsAfter.size()) || jobs != jobsAfter) {
                complete = false; out << "copyQueueObservation=unsupported-or-changing slot=" << slot << '\n'; continue;
            }
            for (uint32_t i = 0; i < count; ++i) {
                out << "copyMapping slot=" << slot << " index=" << i
                    << " destination=" << TasNativeArchive::Word(jobs.data() + i * 16 + 4);
                Mapping(out, TasNativeArchive::Word(jobs.data() + i * 16 + 4)); out << '\n';
            }
        }
    } else { complete = false; out << "mainManager=unreadable\n"; }
    for (uint32_t i = 0; i < TasNativeArchive::kAuxCount; ++i) {
        const uint32_t manager = word(TasNativeArchive::kContextOffsets[i]);
        std::vector<unsigned char> prefix(TasNativeArchive::kPrefixSizes[i]), verify(prefix.size());
        out << "auxiliary=" << TasNativeArchive::kContextOffsets[i] << " manager=" << manager;
        if (!Read(manager, prefix.data(), prefix.size()) || !Read(manager, verify.data(), verify.size())) {
            complete = false; out << " unreadable\n"; continue;
        }
        const bool same = prefix == verify;
        complete = complete && same;
        out << " observedStable=" << same << " selector="
            << TasNativeArchive::Word(prefix.data() + (i == 0 ? 0xF4 : 0)) << '\n';
        for (uint32_t slot = 0; slot < 10; ++slot) {
            const uint32_t stride = i == 0 ? 0x18 : (i == 1 ? 0x708 : (i == 2 ? 0x358 : 0x30C));
            const uint32_t begin = i == 0 ? 0 : (i == 1 ? 0x14 : 0x10);
            const auto* record = prefix.data() + begin + slot * stride;
            out << " auxSlot=" << slot << " header=";
            for (uint32_t j = 0; j < (i == 0 ? 0x18u : 8u); j += 4)
                out << TasNativeArchive::Word(record + j) << ',';
            if (i == 3) out << " secondaryCount=" << TasNativeArchive::Word(record + 0x208);
            out << '\n';
        }
    }
    for (uint32_t contextOffset : { 0x17Cu, 0x180u }) {
        const uint32_t manager = word(contextOffset);
        for (uint32_t member : (contextOffset == 0x17C ? std::array<uint32_t, 2>{{0x2180, 0x2184}} :
            std::array<uint32_t, 2>{{0x1E98, 0x1EB0}})) {
            uint32_t handler = 0, again = 0;
            out << "callback manager=" << contextOffset << " member=" << member;
            if (!manager || manager > UINT32_MAX - member || !Read(manager + member, &handler, 4) ||
                !Read(manager + member, &again, 4) || again != handler) {
                complete = false; out << " unreadable-or-changing\n"; continue;
            }
            if (contextOffset == 0x17C) { Code(out, handler, module, imageSize); out << '\n'; }
            else { out << ' '; if (!Object(out, handler, module, imageSize)) complete = false; }
        }
    }
    // The +17C dispatch wrappers use this global object (static VA 0xC97C68).
    out << "callbackGlobal ";
    if (!Object(out, static_cast<uint32_t>(module + 0x897C68), module, imageSize)) complete = false;
    const bool stable = Read(module + 0x142A924, &contextAfter, 4) && context == contextAfter &&
        Read(context, after.data(), after.size()) && c == after;
    out << "contextObservedStable=" << stable << " captureComplete=" << (complete && stable)
        << " synchronized=false nativeLoadAllowed=false\n";
    std::string result = out.str();
    // Keep bounded even if future formatting grows. Truncated evidence is explicitly incomplete.
    if (result.size() > 250000) { result.resize(250000); result += "\ncaptureComplete=false reason=report-truncated\n"; }
    return result;
}
} // namespace TasRuntimeEvidence