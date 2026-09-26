# TAS project archive V4 (implementation in progress)

V4 adds optional bounded post-save runtime evidence. V3 introduced auxiliary histories
and the selected +174 stream. V2 introduced the out-of-line 0x400-byte restore-copy queue.
V1/V2/V3 are rejected, not silently treated
as complete. Still archive-only: live ownership and the native write transaction remain
unimplemented. See TAS_NATIVE_RESTORE_DEPENDENCIES.md and TasRestorePlan.h.

Status: source implemented, not compiled or exercised in game. This is an archive codec,
not an independently restorable native state. Do not distribute it as completed project import.

## Goal and scope

The user confirms that identical frame edits already reproduce the same random wave.
Preserve that existing input/base relationship through file export and restoration. RNG
reverse engineering is not a prerequisite; investigate missing dependencies only if file
restoration diverges. Edited-input random-result locking is deferred.

## Capture and UI

`TasManager` owns one `BasePair`, separate from optional fidelity diagnostic copies.
A and B are copied immediately after their existing native saves, on post-increment
boundaries. Character position/resources/action summaries are sampled there, never when
Export is clicked. No new hook or changed lead-in is introduced. The pair uses roughly
17 MiB for current main payload sizes, plus about 68 KiB of auxiliary prefixes and
up to 4 MiB of selected auxiliary streams and 512 KiB of evidence per pair; it is discarded on base replacement/abort/session clear.
Archive capture failure leaves native playback available but disables project export.

- **Save project archive**: writes `.bbtas` via a same-directory `.pending` file.
- **Read / validate archive**: decodes a local candidate and checks structure/checksums.
  Does not import inputs, load a slot, move the cursor, or change current game/base state.
- Legacy `.txt` input export/import remains separate. Optional text details now come from
  the saved B capture, not the live characters at export time.

## Binary encoding

Integers are unsigned little-endian, never host struct dumps or `size_t`.
All checksums use FNV-1a-32 (initial 2166136261, multiplier 16777619). Each checksum
covers the bytes since the previous checksum; the stored checksum itself is excluded.
This detects accidental corruption, not malicious modification or native pointer safety.

1. Header: 17 bytes `BBCF_TAS_PROJECT` including NUL; u32 version=4;
   u32 capability=1 (archive-only); u32 lead-in=60; compatibility text; checksum.
2. Base A block, then base B block, each:
   - u32 frame, firstSize, secondOffset, secondSize (firstOffset is always zero).
   - u64 sourceManager, sourceBuffer; u32 sourcePhysicalSlot.
   - u64 sourceEpoch, sourceSaveSerial.
   - 72 raw descriptor bytes (source provenance only).
   - 1024 raw copy-queue bytes from descriptor +0x40; same base checksum coverage.
   - u32 sourceContext; four auxiliary blocks in fixed order +174/+178/+17C/+180.
     Each: u32 contextOffset, sourceAddress, prefixLength; prefix bytes;
     u32 sourcePayload, payloadLength; payload bytes. Prefix sizes are exactly
     0x108/0x4668/0x2180/0x1E88. Only +174 may carry payload, bounded at 0x200000.
   - runtimeEvidence text (V4 addition, at most 256 KiB; may be empty/disabled/incomplete).
   - details text; u32 payload length; packed two-range payload; checksum.
3. Movie: u32 count, cursor; count u32 words (low 16 P1, high 16 P2).
4. Sections: u32 count; each u32 frame and name text; checksum over movie + sections.
5. Exact EOF; trailing bytes rejected.

Text is u32 byte length followed by bytes. Limits: runtimeEvidence 256 KiB, other text 4096 bytes, each payload
0xA10000 bytes, frames 1,000,000, sections 4096. These are file safety limits, not
new editor timeline limits. Section frames must be strictly increasing. Input direction
is 1..9, buttons limited to ABCD/taunt. A/B must share manager/epoch, have distinct source
buffers/slots, and be exactly 60 frames apart. Reader publishes only a fully valid candidate.

Compatibility text currently records schema/revision and source module address;
executable fingerprint is explicitly **unknown**. Reader validates the archive schema,
NOT game-version compatibility. RNG coverage is explicitly unmeasured; there is no fake
seed or fabricated RNG field. Raw payload may contain addresses; files are not portable
merely because fields use a portable integer encoding.

## Native restoration blocker

Existing `SnapshotApparatus::RestoreCopyToOriginalSlot` still requires unchanged source
identity and bytes. Its conditions have NOT been weakened for these files.

Known descriptor offsets from current capture implementation:
- +00 frame; +04 buffer/first start; +08 sum of both payload lengths.
- +18 first end; +1C second start; +20 second length; +30 second end.

The stream counters/cursors and out-of-line queue at +34..+47 are now mapped by static
analysis; see TAS_NATIVE_RESTORE_DEPENDENCIES.md for instruction evidence. Queue +40
owns a separate allocation; entries hold snapshot source and live destination addresses.
The first stream mixes untagged raw transfers and address/size-headed transfers. The
new schema-driven parser requires an independently verified native call sequence; none
is yet connected for real game archives. Do not scan payload bytes to guess headers.
Offline preflight also inventories +17C/+180 callback arguments across all ten slots;
this is not a live-ownership check. V4 additionally carries diagnostic runtime text,
never interpreted as a restore authorization token.
Four auxiliary managers also use the selected slot and sometimes other ring slots.
Saving these raw values is not permission to dereference or overwrite them. Their live
ownership, synchronization, auxiliary state and proven relocation rules must be resolved.
The native loader requires a registered buffer; private vector addresses are never passed to it.

Next meaningful native acceptance test: export X, overwrite the original slots with Y,
read X from disk, reconstruct legal registered states, restore X and reproduce the original
combo/wave. Re-entering the match and restarting the process are separate later tests.

## Runtime evidence (V4)

`BBCF_TAS_CAPTURE_EVIDENCE=1` opts into `TasRuntimeEvidence::Capture` at the two existing
post-save boundaries. Default off; no hooks or per-frame polling. All live reads use
VirtualQuery + ReadProcessMemory, skip guard/no-access mappings, bound counts and compare
repeated reads. No native function calls, memory writes or file-address dereferences.

Text includes registered objects/vtables/method RVAs, bounded 0x60-byte diagnostic object
prefixes (not a proven allocation size), context dispatch queues, main manager descriptors,
callback handlers/global wrapper target, copy-target mappings, PE metadata, PID and process
creation time. Some individual observations are double-read; none form an atomic snapshot.
`captureComplete=1` only means this bounded observation pass completed, not full dependency
coverage, thread quiescence or live resource ownership. Failures stay explicit in text.

`tools/inspect_tas_archive.py` extracts V4 evidence as JSON after checksum/EOF checks.
It is not the complete C++ preflight validator. `tools/collect_tas_identity.ps1` hashes disk
BBCF.exe/dinput8.dll/archives, with CreateNew output and no game attachment. Disk hashes do
not prove loaded-image identity. Both scripts are unexecuted. Collection instructions:
`docs/TAS_RUNTIME_EVIDENCE_COLLECTION.md`.

## Verification pending

`tools/test_tas_project_file.cpp` links with `src/Game/TasProjectFile.cpp` as a standalone
C++14-or-later program. Tests byte-identical roundtrip, header-byte and sampled body
truncation/corruption, invalid auxiliary metadata, queue/job-plan bounds, trailing data,
invalid input/layout/pair and unchanged output on rejection.

No test execution or MSVC build is claimed. The terminal was initially unavailable;
after it recovered, the user explicitly requested no compilation. No CI push/deployment
was performed. Native A/B regression and fixed-wave file-restoration remain pending.