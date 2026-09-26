# TAS native file-restore dependencies

2026-09-26. Static analysis of `tools/bbcf_disasm_ascii.txt`, addresses below are
static VAs with image base 0x400000, NOT runtime pointers. No game execution,
compilation or changes to native loading were performed for this investigation.

## Important correction: two payload ranges are not the entire restore recipe

Existing same-slot success remains valid: its out-of-line dependencies stayed alive.
An independent archive must preserve/rebuild those dependencies too. Do not infer RNG
failure or invalidate previous fixed-wave playback evidence from this finding.

### 0x48 descriptor layout, evidence-backed fields

| Offset | Role established by instructions | Evidence |
| --- | --- | --- |
| +00 | frame | 0x786780 writes caller's frame to selected record |
| +04 | first buffer base, native lookup key | 0x786490 searches 10 entries, stride 0x48 |
| +08 | native first-range byte length; Mod later overwrites with total length | 0x7863B0 subtracts +04 from +18; `save_into_slot` assigns callback size |
| +0C | first-stream write operation count | 0x786CA0 / 0x786CD0 increment after appending |
| +10 | first-stream read operation count | 0x7869A0 / 0x7869D0 increment after reading |
| +14 | first-stream read cursor | reset to +04 by 0x786AF0; advanced by read helpers |
| +18 | first-stream write/end cursor | append helpers advance this; not itself allocation capacity |
| +1C | second buffer base | allocation code 0x7864C0 initializes first slot to allocation +0xD0000 |
| +20 | second-stream span including alignment | 0x7863B0 subtracts +1C from +30 |
| +24 | second-stream reservation count | 0x786A60 increments when reserving aligned copy storage |
| +28 | completed restore-copy count | 0x7868F0 increments after consuming a copy job |
| +2C | second-stream consumed cursor | 0x7868F0 adds copy size plus padding; reset to +1C by 0x786AF0 |
| +30 | second-stream allocation/end cursor | 0x786A60 advances by padding + size |
| +34 | copy-queue pending count | queue pop/enqueue/reset below |
| +38 | copy-queue read index | pop increments with mask 0x3F; restore rewinds |
| +3C | copy-queue write index | enqueue increments with mask 0x3F |
| +40 | out-of-line 0x400-byte queue allocation | 0x785FB0 allocates; 0x786130 frees |
| +44 | queue spin-lock word, bit 0 | lock bts/btr in queue methods |

Do not change the Mod's +08 bookkeeping based only on the native first-length meaning.
Current packed capture derives first length from +18 - +04, intentionally independent
of that overloaded field. Native 0x786470 returns +08 + +20; note this distinction
when comparing native callback sizes with descriptors after the Mod updates them.

### Copy queue contents and why they matter

0x785FB0 allocates 0x400 bytes per record: 64 entries, 16 bytes each.
0x786C30 calls 0x786A60 for aligned second-buffer storage, enqueues:

```
u32 savedSource;     // pointer into second payload range
u32 liveDestination; // original target address, outside the snapshot allocation
u32 size;
u32 alignmentPadding;
```

It then copies liveDestination -> savedSource. Queue enqueue is 0x786860.
Queue pop 0x7867C0 copies one 16-byte entry, increments read index masked with 0x3F,
and decrements pending count. 0x7868F0 passes the popped values to 0x7861E0 as
`copy(liveDestination, savedSource, size)` and advances +2C by size + padding.

0x786AF0 selects the slot, resets stream read cursors/counters, then calls 0x786BB0
and rewinds the queue read index to zero while restoring pending work count. Thus an
empty queue at capture time does NOT imply that its backing entries are unnecessary:
completed jobs are reused on restore. Capture all 0x400 bytes, not just pending entries.
Unused entries may be stale; never execute them merely because they were archived.
The archive records raw material only, not a certified job list.

V2 now captures this allocation read-only immediately after each base save. It rejects
unavailable pointers, locked/invalid queue metadata or changing bytes/descriptors. Two
reads and serial checks detect observed changes; they are not a cross-thread barrier.
No native lock is acquired and no queue cursor is reset by capture.

## Other state selected with the same slot

0x7857E0 looks up the source address with 0x786490, then passes that index and reset=1
to five managers before continuing through 0x785520:

| Context member | Selection routine | Static observations |
| --- | --- | --- |
| +0x170 | 0x786AF0 | main descriptor/streams/copy queue |
| +0x174 | 0x784A70 | 0x18-stride slot entries; restores manager +F8 from slot +8, saves previous value at +FC |
| +0x178 | 0x787040 | 0x708-stride slot history; reads frame at +14, count at +18; updates per-entry flags |
| +0x17C | 0x7846F0 | 0x358-stride slot history; reads frame at +10, count at +14; updates flags at entry +0C |
| +0x180 | 0x784FB0 | 0x30C-stride slot history; reads frame at +10, count at +14; updates flags at entry +4 |

The last three routines can visit OTHER slots based on the previous/current frame delta.
Capturing one descriptor or one target slot is therefore not yet a complete reconstruction
recipe. Do not name these managers as gameplay/audio/etc. without tracing their users.
These findings prove loader dependencies, not that every byte needs wholesale restoration.

## V3 auxiliary capture and offline preflight (source only)

Further static paths traced:
- +174: 0x784910 allocates 0x200000 per slot (10 x 0x18 descriptors).
  0x784AA0 appends bytes and advances slot +14; 0x784880 finalizes length at +4
  and checksum at +0C; 0x784960 stores frame at +8. Header selectors are +F4/F8/FC.
  V3 captures prefix [0,0x108), and ONLY the selected slot's used stream, not all 20 MiB.
- +178: 0x786E60 clears [0x14,0x4664), plus +4664. V3 prefix size 0x4668.
  0x786EA0 searches current-slot 0x38-byte records, compares a 0x20-byte key and
  two values, marks reuse and returns stored values. 0x786FD0 appends records.
- +17C: 0x7845B0 clears 10 x 0x358 bytes starting at +10. V3 prefix size 0x2180.
  0x784510 invokes callbacks at manager +2180/+2184 for active entries with types 1/2;
  0x784570 processes and clears the next slot. Callback function pointers are NOT included/copied.
- +180: 0x784E40 clears 10 x 0x30C bytes starting at +10. V3 prefix size 0x1E88.
  0x784D10 consumes the secondary list through callback owner +1EB0; 0x784D70 uses
  callback owner +1E98 and can append cleanup entries; 0x784DF0 processes/clears history.
  V3 captures history, NOT callback object ownership or those objects' pointees.

Thus the histories are not arbitrary discardable caches. Copying their whole prefixes
back would also overwrite selectors/pointers/shared slots; V3 ONLY reads and archives.
The prefix bounds are evidenced layouts, not a claim that all external dependencies
are captured. Main slot number is used only after confirming context +170 equals the
captured manager; the remaining native selectors need not equal it after callbacks advance.

`CopyAuxiliaryState` reads via the current live context, bounds each range, rechecks all
prefixes/payloads and the context/manager/descriptor identities. The queue is rechecked
across the combined capture. Failures disable archive export but leave native play intact.
No locks are taken, no worker/event handles are restored, no new Hook is added.
Double reads are observations, not a replacement for native synchronization.

`TasRestorePlan::Analyze` is a pure, transactional offline preflight:
- validates descriptor/frame/ranges and non-wrapped queue layout, requiring consumed +
  pending = reservation count; pending jobs alone are NOT a save failure (0x786C30 also
  copies synchronously), and consumed-prefix rewind is modeled without mutating anything;
- verifies each job's 16-byte alignment, exact stream coverage and destination arithmetic;
- produces packed/native source offsets, live destination requirements and six proven
  descriptor pointer-field offsets; never guesses relocations from pointer-looking words;
- always leaves nativeLoadAllowed=false. Does not interpret first-stream embedded
  addresses without its call schema, or treat matching live addresses as matching lifetimes.

Read/validate UI reports preflight issues separately from file corruption. A structurally
valid archive can still have an unsupported restore layout. Test source now covers both,
using sampled body corruption/truncation to avoid a quadratic test over the enlarged file.
Neither tests nor MSVC builds were run (user requested no compilation).

## Mixed first stream and callback inventory (static follow-up)

The first stream is NOT a sequence of uniformly tagged address headers:

| Save / load | Bytes | Descriptor operation count |
| --- | --- | --- |
| 0x786CA0 / 0x7869A0 | raw bytes; size and destination supplied by caller | +1 |
| 0x786CD0 / 0x7869D0 | u32 destination, u32 size, then size bytes | +2 |

No alignment/padding is added by these helpers. Zero-byte transfers still increment
operation counts. Examples of both families: 0x408910..0x40892A writes addressed 0x11C
records (load loop 0x4086D3); 0x409E33..0x409E5E writes raw 4-byte records and then a
caller-sized buffer (load 0x409753..0x40977E); 0x40B5C4/0x40B5D9 writes a raw 0x60-byte
object and then a count-dependent array (load 0x40B1B4/0x40B1C9). Raw object bytes can
also contain pointers. Parsing addressed headers alone does not enumerate all references.

`AnalyzeFirstStream` now supports an EXTERNALLY supplied, independently verified call
schema. It checks exact byte/operation coverage, header agreement, overflow, truncation,
and snapshot-storage overlap. It does not discover schema by guessing from payload bytes.
**No complete game traversal schema is available or connected to the UI yet.** File
validation reports this unresolved condition, not a successfully validated first stream.

Callback distinction corrected/refined:
- +17C +2180/+2184 are direct cdecl function pointers, not callback-owner objects.
  Constructor call 0x4E5B94..0x4E5BA0 supplies 0x485C30 and 0x485E10. Wrappers forward
  two arguments to global object 0xC97C68 through vtable +0/+4 (initial vtable 0x8A86A0).
  The dump misdecodes the boundary immediately before 0x485C30; its wrapper body and
  constructor reference are usable evidence, not safe instruction bytes for a hook.
- +17C slot record is frame/count + 16-byte entries (arg0,arg1,type,active). 0x784670
  may reactivate a matching entry during replay; 0x784510 invokes active type 1/2 entries.
- +180 uses function-wrapper objects +1E98/+1EB0 through vtable +8, not the same ABI as
  +17C. Primary 8-byte entries contain value/active; secondary 4-byte entries are passed
  unconditionally. Values are not assumed to be pointers, and callback semantics/freeing
  effects are not proven merely by these dispatch paths.

`Plan::historyUses` inventories these callback arguments across ALL ten slots, including
inactive +17C/+180 primary entries. This is an ownership-resolution work list, NOT an
executable callback list or permission to restore histories. Matching handler addresses
would still not establish that their referenced resources/handles survived slot overwrite.
The full +178 history and +174 stream remain archived but are not decoded by this inventory.

New test source covers mixed unaligned streams, missing/wrong schema, operation counts,
zero-byte calls, truncation/overflow/overlap, transactional rejection and cross-slot callback
inventory. Tests remain uncompiled/unexecuted. No native write path or new hook added.

## Runtime-dependent dispatch boundary and V4 evidence

Further static tracing identifies the concrete missing input:
- 0x785B30 invokes context +2C/+30 registered objects through vtable +0.
- 0x785CB0 (save) / 0x785520 (load) invoke +B0/+B4 objects through vtable +0/+4.
- 0x785920 registers 12-byte `{object,partitionIndex,partitionCount}` jobs in the
  context +00 template queue; 0x7852B0 copies it to +14 working queue. Dispatch is
  0x785BC0/0x785430, with three workers (0x785D40) and the initiating thread.
  Both coordinators wait for three completion events before returning. Unlocked queue
  metadata alone does not prove that all popped jobs have finished.
- +174 append/finalization is conditional on context +184 (0x785BA0 / 0x785B62).
  A captured selected descriptor alone cannot prove this stream was updated by that save.
- +178 cached outputs are consumed at 0x4BF9FA..0x4BFA3D; misses call a runtime object's
  vtable +14 at 0x4BFAEE and append via 0x4BFB48. Resource/handle semantics still need
  the actual dynamic target; clearing this history is not a justified restore strategy.

We need a real target-session object/dispatch inventory to continue along the relevant
methods rather than assume a universal first-stream schema. This is NOT a claim that no
further static analysis is possible: collected RVAs guide the next static investigation.

V4 adds `runtimeEvidence` text to each base, bounded to 256 KiB, under the base checksum.
`TasRuntimeEvidence.h` observes live context registries, queued dispatch objects, bounded
object prefixes, handler methods/RVAs, descriptors and memory mappings. It requires explicit
`BBCF_TAS_CAPTURE_EVIDENCE=1`, uses VirtualQuery/RPM, and never executes a native handler,
locks a game mutex, changes a hook, or dereferences file-provided addresses. Evidence is
also logged before auxiliary capture so a refused archive does not hide the failing setup.
PE/PID/creation-time observations are provenance, not a complete identity/lifetime gate.

First collection procedure and transaction design constraints are documented in
`TAS_RUNTIME_EVIDENCE_COLLECTION.md`. C++ and scripts remain uncompiled/unexecuted.
V4 rejects V1/V2/V3. No independent native restore is enabled.

## Implementation boundary and next actions

Implemented: V4 persists payloads, descriptor, copy queue, bounded auxiliary history
prefixes, selected +174 stream and optional runtime evidence text. All checksummed with
their base. Offline copy-job planning is connected to file validation. No native writes;
V1/V2/V3 rejected explicitly.

Still required before an old file can replace overwritten slots:
1. Trace auxiliary managers' save/populate/consume paths and identify the minimal old
   state and live ownership that must survive. Do not copy whole manager heaps blindly.
2. Capture exact executable identity and process/match lifetime identity. Matching pointer
   values or an equal frame number do not prove the same allocation/lifetime.
3. Build a restore plan using a currently registered native allocation. Rebase only proven
   snapshot-internal source pointers; validate/reconstruct live destination identities.
   First-stream 0x786CD0/0x7869D0 also writes/reads `(live address, size)` headers, so the
   second-stream queue is not the only payload containing absolute destination addresses.
4. Resolve queue completion/worker synchronization and ownership, not just unlocked bits.
5. Restore only once the combined recipe is understood; publish the imported movie/base
   after successful restoration. Keep separate failure quarantine, no fallback to current state.

Acceptance remains export X -> overwrite with Y -> read X from disk -> restore X ->
same original input/characters/combo/random wave. Match re-entry and process restart are
separate scopes. No need to locate a new RNG seed unless a proven file restore diverges.