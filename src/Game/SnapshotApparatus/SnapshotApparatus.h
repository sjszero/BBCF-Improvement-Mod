#pragma once
#include "Game/GhidraDefs.h"
#include "Game/CharData.h"
#include "Game/SnapshotApparatus/SnapshotSlotPool.h"
#include "Game/TasNativeArchive.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>
#define SNAPSHOT_PREALLOC_SIZE  1

class Snapshot {
public:
	char padding[0xa10000]; 
	//btw chardata resides on buf + 0x623E10 for P1 and for p2 buf + 0x623E10 + 0x24978 for training mode THIS IS NOT TRUE, SEEMS TO CHANGE
	
};

//!!!!!!!!!!!!Uncomment this later(and all the functions related to it) when I go back to working on rewind!!!! leaving out for possible crash reasons
//static Snapshot snapshot_replay_pre_allocated[SNAPSHOT_PREALLOC_SIZE]; //keeping this with only one element for now while its not used for any implementation to save space
//!!!!!!!!!!!!!!!!


class SnapshotApparatus {


public:
	unsigned int snapshot_count;
	CharData* p1_ptr;//p1 CharData*
	CharData* p2_ptr; //p2 CharData*
	int last_saved_snapshot_size;
	//p1 and p2 ptrs are used for now to determine when I need to remake the snapshot
	GGPOSessionCallbacks*  callbacks_ptr;
	//Snapshot* p_snapshot_reseve;
	//Snapshot** pp_snapshot_reseve;
	SnapshotApparatus();
	~SnapshotApparatus();

	// Claims `count` slots of the game's ring for this apparatus alone. Unreserved apparatuses
	// retain a rolling cursor over unclaimed slots only. Returns false when no range is available;
	// indexed writes are then limited to the shared, unreserved part of the ring.
	bool ReserveSlots(const char* owner, int count);

	// Physical ring slot for a logical index inside this apparatus's range. Callers only ever
	// deal in logical indices, so nothing outside needs to know where the range sits.
	int slot_for(unsigned int logicalIndex) const;
	// Logical index this apparatus last saved into, or -1 if it has saved nothing. Pass it
	// straight back to load_snapshot_index to reload that exact state.
	int last_saved_slot() const;
	int get_last_saved_physical_slot() const { return m_lastSavedPhysicalSlot; }

	bool save_snapshot(Snapshot** pbuf);
	// save_snapshot, but into a chosen logical slot instead of the next one in sequence.
	bool save_snapshot_index(int logicalIndex);
	bool save_snapshot_prealloc();
	bool load_snapshot(Snapshot* buf);
	bool load_snapshot_sized(const void* buf, size_t buf_size);
	// Disabled: arbitrary byte addresses are not registered native states.
	// Returns false without patching code or invoking game callbacks.
	bool RestoreExternalBytes(const void* buf, size_t buf_size);
	bool load_snapshot_prealloc(int index);
	bool load_snapshot_index(int index);
	bool check_if_valid(CharData* p1, CharData* p2);
	void clear_count();
	bool clear_framecounts();
	int get_nearest_prealloc_frame(int current_frame, std::map<int, Snapshot*> frame_snap_map);
	int get_last_saved_snapshot_size() const;

	// --- raw slot bytes ---------------------------------------------------------------
	// Read-only copies for byte/digest inspection, not independently loadable states.
	// Diagnostic read-only capture. The native record owns two separated payload ranges;
	// `bytes` packs those ranges without the gap, and is NOT a loadable native address.
	struct SlotBytes {
		std::vector<unsigned char> bytes;
		std::array<unsigned char, 0x48> descriptor{}; // captured provenance; not a writeback recipe
		size_t firstOffset = 0;
		size_t firstSize = 0;
		size_t secondOffset = 0;
		size_t secondSize = 0;
		uintptr_t sourceManager = 0;
		uintptr_t sourceBuffer = 0;
		int sourcePhysicalSlot = -1;
		uint64_t sourceEpoch = 0;
		uint64_t sourceSaveSerial = 0;
		unsigned int frame = 0;
		uint32_t digest = 0; // FNV-1a of both packed ranges
		bool valid = false;

		bool SameSourceAndLayout(const SlotBytes& other) const {
			return valid && other.valid && sourceManager == other.sourceManager &&
				sourceBuffer == other.sourceBuffer && sourcePhysicalSlot == other.sourcePhysicalSlot &&
				sourceEpoch == other.sourceEpoch && sourceSaveSerial == other.sourceSaveSerial &&
				frame == other.frame && firstOffset == other.firstOffset && firstSize == other.firstSize &&
				secondOffset == other.secondOffset && secondSize == other.secondSize;
		}
	};

	// Packs the two validated native data ranges from a reserved logical slot into `out`.
	// Returns false with an invalid output if the native layout, length or pointers drift.
	// This read-only copy is NOT an independently loadable state.
	bool CopyLogicalSlot(int logicalSlot, SlotBytes* out) const;
	// Archive dependency outside the two payload ranges: 64 native 16-byte copy jobs.
	// Read only, no lock acquisition or cursor rewinding. Does not certify restore safety.
	bool CopyRestoreQueue(int logicalSlot, const SlotBytes& captured,
		std::array<unsigned char, 0x400>* out) const;
	// Selected +174 stream plus bounded ten-slot history prefixes of +174/+178/+17C/+180.
	bool CopyAuxiliaryState(int logicalSlot, const SlotBytes& captured,
		TasNativeArchive::Dependencies* out) const;
	// Inspect/hash or compare in place without allocating another payload vector.
	// Inspect returns valid metadata with EMPTY bytes; it is not a captured SlotBytes payload.
	// Call on the existing serialized game path; serial checks are not thread synchronization.
	bool InspectLogicalSlot(int logicalSlot, SlotBytes* metadata) const;
	bool MatchesLogicalSlot(int logicalSlot, const SlotBytes& state) const;
	// Diagnostic only: same saved generation, same native slot and unchanged bytes required.
	// Rewrites the two payload ranges then loads via the registered native slot.
	bool RestoreCopyToOriginalSlot(int logicalSlot, const SlotBytes& state);

	// Disabled compatibility API: always refuses private-address loading.
	// Byte equality is not sufficient to reconstruct a registered native state.
	bool RestoreFromBytes(const SlotBytes& state);

	// Number of logical slots this apparatus can address, for bounds checks by callers.
	int GetLogicalSlotCount() const { return slot_count; }
private:
	bool save_into_slot(int target_slot, Snapshot** pbuf_mine);
	bool ReadLogicalSlot(int logicalSlot, SlotBytes* out, bool copyPayload) const;
	void QuarantineSlot(int physicalSlot);
	// Shared body of load_snapshot_sized and RestoreExternalBytes. `preserveSlotBookkeeping`
	// selects whether the selected slot's descriptor is updated by an external load.
	bool LoadSnapshotSizedInternal(const void* buf, size_t buf_size, bool preserveSlotBookkeeping);


	int slot_base = 0;
	int slot_count = SnapshotSlotPool::kSlotCount;
	bool slots_reserved = false;
	uint64_t m_reservationEpoch = 0;
	int m_lastSavedPhysicalSlot = -1;
	// Quarantine survives reservation changes in this apparatus; only a successful save clears it.
	struct SlotWriteState { uint64_t serial = 0; bool captureAllowed = false; bool quarantined = false; };
	SlotWriteState m_slotWrites[SnapshotSlotPool::kSlotCount]{};
};