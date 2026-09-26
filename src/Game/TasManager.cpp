#include "TasManager.h"
#include "TasRestorePlan.h"
#include "TasRuntimeEvidence.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Game/CharData.h"
#include "Game/GhidraDefs.h"
#include "Game/SnapshotApparatus/SnapshotSlotPool.h"
#include "Game/characters.h"
#include "Hooks/hooks_battle_input.h"
#include "Overlay/Window/FrameHistory/FrameHistoryWindow.h"
#include "Overlay/WindowManager.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
namespace {
// One savestate is 10.06 MiB and the game's ring holds ten, so a state per frame is not on
// the table (a single second of frames would be 600 MB in a 2 GB address space). Keyframes
// every kKeyframeInterval frames make a nearby seek cheap; anything further back replays
// from the base state, which is correct either way, just slower.
const char* FidelitySourceName(FidelityRestoreSource source) {
    switch (source) {
    case FidelityRestoreSource::Slot: return "slot";
    case FidelityRestoreSource::PrivateBytes: return "bytes-disabled";
    case FidelityRestoreSource::CopyToOriginalSlot: return "copy-to-original-slot";
    }
    return "unknown";
}
constexpr unsigned int kKeyframeInterval = 60;
constexpr int kTasBaseSlot = 0;             // editing base B, after the lead-in
constexpr int kTasPlaybackBaseSlot = 1;     // playback base A, before the lead-in
constexpr int kTasFirstKeyframeSlot = 2;
// The keyframe ring is what is left of the reservation: slots 2..5. Deriving it here is what
// keeps slot_for() from wrapping past the reservation - a fifth keyframe would have mapped to
// slot 6, which this manager does not own, and would have overwritten another feature's state
// (and, with the ring starting at 0, the base states themselves).
constexpr int kTasKeyframeSlotCount = 4;
constexpr int kTasReservedSlots = kTasFirstKeyframeSlot + kTasKeyframeSlotCount; // 6
// Deep enough to walk back a bad editing session, small enough to stay trivial in memory:
// a 10,000 frame movie is 40 KB, so this ceiling is a couple of megabytes at worst.
constexpr size_t kUndoDepth = 64;

// TAS Movie length is not artificially capped.
// The hidden lead-in between the two base snapshots. It belongs to the base pair, not to the
// movie, so it owns no movie frame: it exists to put movie frame 0 of a presentation recording
// on the same game state the editor starts editing from. 60 is also the keyframe interval, so
// the two line up by construction.
constexpr unsigned int kPresentationLeadInFrames = 60;
constexpr unsigned int kPresentationLeadOutFrames = 240;
constexpr const char* kTasMovieHeaderV1 = "BBCF_TAS_MOVIE_V1";
constexpr const char* kTasMovieHeaderV2 = "BBCF_TAS_MOVIE_V2";

// Matching the packed values used by hooks_battle_input.h (taunt = 256).
constexpr uint16_t kInputButtonTaunt = 256;

uint16_t ButtonValue(char button) {
    switch (button) {
    case 'A': return 16;
    case 'B': return 32;
    case 'C': return 64;
    case 'D': return 128;
    default: return 0;
    }
}

std::string HumanInput(uint16_t packed) {
    return TasManager::FormatInput(packed);
}

bool ParseHumanInput(const std::string& text, uint16_t* result) {
    if (!result) return false;
    uint16_t value = 0;
    bool sawDirection = false;

    // Same implied neutral as the command parser: a frame written with no direction is 5. The
    // format always writes one, but the file's own header tells a human they can leave it out, so
    // reading it back has to accept what that invites them to type.
    const auto implyNeutralDirection = [&]() {
        if (!sawDirection) {
            value = 5;
            sawDirection = true;
        }
    };

    for (size_t i = 0; i < text.size(); ) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
        if (ch >= '1' && ch <= '9') {
            if (sawDirection) return false;
            value = static_cast<uint16_t>(ch - '0');
            sawDirection = true;
            ++i;
        } else if (ch == 'A' && i + 1 < text.size() &&
                   static_cast<char>(std::toupper(static_cast<unsigned char>(text[i + 1]))) == 'P') {
            // "ap" = taunt button. Checked before the single 'A' branch so 5ap does not
            // get parsed as "5A" followed by an invalid 'p'.
            if (value & kInputButtonTaunt) return false;
            implyNeutralDirection();
            value = static_cast<uint16_t>(value + kInputButtonTaunt);
            i += 2;
        } else if (ch == 'A' || ch == 'B' || ch == 'C' || ch == 'D') {
            if (value & ButtonValue(ch)) return false;
            implyNeutralDirection();
            value = static_cast<uint16_t>(value + ButtonValue(ch));
            ++i;
        } else if (ch != ' ' && ch != '\t') {
            return false;
        } else {
            ++i;
        }
    }
    if (!sawDirection) return false;
    *result = value;
    return true;
}
}

TasManager& TasManager::Instance() {
    static TasManager instance;
    return instance;
}

// --- base-state fidelity diagnostics -------------------------------------------------
//
// Restoring a base state has to bring back far more than the frame number: the characters'
// actions, timers and positions, and every random stream the match has consumed so far.
// The two reasons a restored base can still diverge are (a) the native savestate does not
// carry a given field at all, or (b) it does, but TAS re-enters the match at a different
// point in the frame than the capture did.
//
// To tell those apart without guessing, dump the same compact fingerprint on both sides of
// every base save / base load. Everything logged here is read-only.
namespace {

// FNV-1a, matching FastDigest32Local in SnapshotApparatus so digests are comparable.
uint32_t TasDigest32(const void* data, size_t size) {
    if (!data || size == 0) {
        return 0u;
    }
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint32_t>(bytes[i]);
        h *= 16777619u;
    }
    return h;
}

// FNV-1a over a NUL-terminated fixed-size name field (lastAction/currentAction/...).
uint32_t TasDigestName(const char* name, size_t capacity) {
    if (!name) {
        return 0u;
    }
    size_t len = 0;
    while (len < capacity && name[len] != '\0') {
        ++len;
    }
    return TasDigest32(name, len);
}

void TasLogPlayerState(const char* tag, const char* who, CharData* data) {
    if (!data) {
        LOG(1, "[TAS][STATE] %s %s=<null>\n", tag ? tag : "(null)", who ? who : "?");
        return;
    }
    LOG(1,
        "[TAS][STATE] %s %s ptr=%p charIndex=%d facing=%d pos=(%d,%d) hp=%d/%d "
        "actionTime=%d actionTimeNoHitstop=%d hitstop=%d hitstun=%d blockstun=%d "
        "stateChanged=%d spriteFrame=%d/%d sprite=%llu "
        "actionHash=0x%08X lastActionHash=0x%08X scriptLine=%p\n",
        tag ? tag : "(null)",
        who ? who : "?",
        data,
        data->charIndex,
        data->facingLeft,
        data->position_x,
        data->position_y,
        data->currentHP,
        data->maxHP,
        data->actionTime,
        data->actionTimeNoHitstop,
        data->hitstop,
        data->hitstun,
        data->blockstun,
        data->stateChangedCount,
        data->frameCounterCurrentSprite,
        data->frameLengthCurrentSprite,
        static_cast<unsigned long long>(data->currentSprite),
        TasDigestName(data->currentAction, sizeof(data->currentAction)),
        TasDigestName(data->lastAction, sizeof(data->lastAction)),
        data->nextScriptLineLocationInMemory);
}

// The whole CharData blob hashed as raw bytes. If this digest survives a save and a load
// unchanged, the character state is being carried by the native savestate and the only
// remaining variable is frame phase. If it does not, the state is genuinely incomplete.
void TasLogSnapshotFingerprint(const char* tag) {
    TasManager::BaseDiagnostics diagnostics = TasManager::Instance().GetBaseDiagnostics();
    LOG(1,
        "[TAS][STATE] %s owner=%p count=%u baseFrame=%u playbackBase=%u playbackBaseReady=%d "
        "playhead=%u runState=%d gameFrame=%u\n",
        tag ? tag : "(null)",
        diagnostics.snapshotOwner,
        diagnostics.snapshotCount,
        diagnostics.baseFrame,
        diagnostics.playbackBaseFrame,
        diagnostics.playbackBaseReady ? 1 : 0,
        static_cast<unsigned int>(diagnostics.playhead),
        diagnostics.runState,
        TasManager::Instance().GetCurrentFrame());

    LOG(1,
        "[TAS][STATE] %s owner lastSavedLogical=%d lastSavedPhysical=%d lastSavedSize=%d\n",
        tag ? tag : "(null)",
        diagnostics.lastSavedLogicalSlot,
        diagnostics.lastSavedPhysicalSlot,
        diagnostics.lastSavedSnapshotSize);
    TasLogPlayerState(tag, "P1", g_interfaces.player1.GetData());
    TasLogPlayerState(tag, "P2", g_interfaces.player2.GetData());

    if (g_gameVals.pFrameCount) {
        LOG(1, "[TAS][STATE] %s game frameCount=%u\n",
            tag ? tag : "(null)", *g_gameVals.pFrameCount);
    }
}

// Digest of one of this manager's saved-state slots, plus the raw slot digest alongside it.
//
// This is the direct answer to "does the native savestate carry enough state?". Two slots
// captured either side of a lead-in are supposed to differ; the useful comparison is the same
// slot hashed at capture time and again after a restore. A slot whose digest survives a
// save/restore cycle unchanged is one the game reproduces byte-for-byte, which is the
// precondition for anything built on top of it.
void TasLogSlotDigest(const char* tag, int logicalSlot) {
    size_t hashedBytes = 0;
    const uint32_t digest =
        TasManager::Instance().DigestLogicalSlot(logicalSlot, &hashedBytes);
    LOG(1,
        "[TAS][SLOT] %s logical=%d hashedBytes=%u digest=0x%08X\n",
        tag ? tag : "(null)",
        logicalSlot,
        static_cast<unsigned int>(hashedBytes),
        digest);
}

// Both halves of the base pair, hashed at the moment they became usable, so a later restore
// has fixed values to compare against instead of only the live state it produced.
void TasLogBasePairDigests(const char* tag) {
    TasLogSlotDigest(tag, kTasPlaybackBaseSlot);
    TasLogSlotDigest(tag, kTasBaseSlot);
}

}  // namespace

TasManager::BaseDiagnostics TasManager::GetBaseDiagnostics() const {
    BaseDiagnostics diagnostics;
    diagnostics.snapshotOwner = m_snapshotOwner;
    diagnostics.snapshotCount = m_snapshotOwner ? m_snapshotOwner->snapshot_count : 0u;
    diagnostics.baseFrame = m_baseFrame;
    diagnostics.playbackBaseFrame = m_playbackBaseFrame;
    diagnostics.playbackBaseReady = m_playbackBaseReady;
    diagnostics.playhead = m_playhead;
    diagnostics.runState = static_cast<int>(m_runState);
    if (m_snapshotOwner) {
        diagnostics.lastSavedLogicalSlot = m_snapshotOwner->last_saved_slot();
        diagnostics.lastSavedPhysicalSlot = m_snapshotOwner->get_last_saved_physical_slot();
        diagnostics.lastSavedSnapshotSize = m_snapshotOwner->get_last_saved_snapshot_size();
    }
    return diagnostics;
}

uint32_t TasManager::DigestLogicalSlot(int logicalSlot, size_t* hashedBytes) const {
    if (hashedBytes) {
        *hashedBytes = 0;
    }
    if (!m_snapshotOwner) {
        return 0u;
    }
    SnapshotApparatus::SlotBytes view;
    if (!m_snapshotOwner->InspectLogicalSlot(logicalSlot, &view)) {
        LOG(1, "[TAS][SLOT] logical=%d segmented digest unavailable\n", logicalSlot);
        return 0u;
    }
    const size_t packedSize = view.firstSize + view.secondSize;
    if (hashedBytes) *hashedBytes = packedSize;
    LOG(1, "[TAS][SLOT] logical=%d physical=%d packed=%u frame=%u serial=%llu\n",
        logicalSlot, view.sourcePhysicalSlot, static_cast<unsigned int>(packedSize),
        view.frame, static_cast<unsigned long long>(view.sourceSaveSerial));
    return view.digest;
}

TasManager::~TasManager() {
    ClearInputOverride();
    ClearSnapshot();
}

bool TasManager::IsInTrainingMatch() const {
    return g_gameVals.pGameMode && g_gameVals.pGameState &&
        *g_gameVals.pGameMode == GameMode_Training &&
        *g_gameVals.pGameState == GameState_InMatch &&
        !g_interfaces.player1.IsCharDataNullPtr() &&
        !g_interfaces.player2.IsCharDataNullPtr();
}

void TasManager::SetError(const char* message) {
    m_error = message ? message : "Unknown error.";
    m_status.clear();
}

void TasManager::ClearInputOverride() {
    ClearBattleInputOverride(0);
    ClearBattleInputOverride(1);
    m_hasScheduledInput = false;
    m_scheduledInput = TasFrameInput{};
}

int TasManager::GetKeyframeCount() const {
    int count = 0;
    for (const Keyframe& keyframe : m_keyframes) {
        if (keyframe.valid) {
            ++count;
        }
    }
    return count;
}

void TasManager::InvalidateKeyframesAfter(size_t frame) {
    for (Keyframe& keyframe : m_keyframes) {
        if (keyframe.valid && keyframe.movieFrame > frame) {
            keyframe.valid = false;
            keyframe.sectionCheckpoint = false;
        }
    }
}

void TasManager::InvalidateSectionCheckpointsBefore(size_t frame) {
    for (Keyframe& keyframe : m_keyframes) {
        // A checkpoint at `frame` is the state immediately before that frame executes, so it
        // remains valid when that frame itself is edited. Only checkpoints after it depend on
        // the changed input.
        if (keyframe.valid && keyframe.sectionCheckpoint && keyframe.movieFrame > frame) {
            keyframe.valid = false;
            keyframe.sectionCheckpoint = false;
        }
    }
}

void TasManager::ClearKeyframes() {
    // The keyframe ring is exactly the part of the reservation the two base states do not use.
    m_keyframes.assign(static_cast<size_t>(kTasKeyframeSlotCount), Keyframe{});
    m_nextKeyframeSlot = 0;
    m_pendingKeyframeSlot = -1;
    m_pendingKeyframeFrame = 0;
    m_pendingKeyframeIsSection = false;
}

int TasManager::FindKeyframeFor(size_t targetFrame) const {
    int best = -1;
    size_t bestFrame = 0;
    for (size_t i = 0; i < m_keyframes.size(); ++i) {
        const Keyframe& keyframe = m_keyframes[i];
        if (!keyframe.valid || keyframe.movieFrame > targetFrame) {
            continue;
        }
        if (best == -1 || keyframe.movieFrame > bestFrame) {
            best = static_cast<int>(i);
            bestFrame = keyframe.movieFrame;
        }
    }
    return best;
}

void TasManager::CaptureKeyframeIfDue(bool deferUntilFrameIncrement) {
    if (m_keyframes.empty() || !m_snapshotOwner || m_playhead == 0) {
        return;
    }

    const bool atSection = std::any_of(m_sections.begin(), m_sections.end(),
        [this](const TasSection& section) {
            return section.frame < (std::numeric_limits<size_t>::max)() &&
                section.frame + 1 == m_playhead;
        });
    if (!atSection && m_playhead % kKeyframeInterval != 0) {
        return;
    }

    for (Keyframe& keyframe : m_keyframes) {
        if (keyframe.valid && keyframe.movieFrame == m_playhead) {
            if (atSection) keyframe.sectionCheckpoint = true;
            return;
        }
    }

    size_t slot = m_keyframes.size();
    if (atSection) {
        // Prefer an unused periodic slot, and never evict another section checkpoint.
        for (size_t i = 0; i < m_keyframes.size(); ++i) {
            if (!m_keyframes[i].valid || !m_keyframes[i].sectionCheckpoint) {
                slot = i;
                break;
            }
        }
    } else {
        // A full ring of section checkpoints is intentionally allowed to suppress periodic
        // captures; do not seed slot with a valid index, or the loop can overwrite a checkpoint.
        for (size_t offset = 0; offset < m_keyframes.size(); ++offset) {
            const size_t candidate = (m_nextKeyframeSlot + offset) % m_keyframes.size();
            if (!m_keyframes[candidate].sectionCheckpoint) {
                slot = candidate;
                break;
            }
        }
    }
    if (slot == m_keyframes.size()) {
        return;
    }

    if (!deferUntilFrameIncrement) {
        if (!m_snapshotOwner->save_snapshot_index(kTasFirstKeyframeSlot + static_cast<int>(slot))) {
            LOG(1, "[TAS] keyframe capture failed at frame %u\n", static_cast<unsigned int>(m_playhead));
            return;
        }
        m_keyframes[slot].movieFrame = m_playhead;
        m_keyframes[slot].valid = true;
        m_keyframes[slot].sectionCheckpoint = atSection;
        m_nextKeyframeSlot = slot + 1;
        LOG(1, "[TAS] %s checkpoint slot=%u movieFrame=%u gameFrame=%u\n",
            atSection ? "section" : "periodic", static_cast<unsigned int>(slot),
            static_cast<unsigned int>(m_playhead), GetCurrentFrame());
        return;
    }

    // Playback captures after the game increments pFrameCount. Keep the chosen slot and timeline
    // position pending until the post-increment hook so state and frame metadata share a boundary.
    m_pendingKeyframeSlot = static_cast<int>(slot);
    m_pendingKeyframeFrame = m_playhead;
    m_pendingKeyframeIsSection = atSection;
}

void TasManager::FinalizeKeyframeAfterIncrement() {
    if (m_pendingKeyframeSlot < 0) {
        return;
    }

    const int slot = m_pendingKeyframeSlot;
    const size_t movieFrame = m_pendingKeyframeFrame;
    const bool isSection = m_pendingKeyframeIsSection;
    m_pendingKeyframeSlot = -1;
    m_pendingKeyframeFrame = 0;
    m_pendingKeyframeIsSection = false;

    if (!m_snapshotOwner || !m_snapshotOwner->save_snapshot_index(
        kTasFirstKeyframeSlot + slot)) {
        LOG(1, "[TAS] keyframe capture failed at frame %u\n",
            static_cast<unsigned int>(movieFrame));
        return;
    }

    m_keyframes[slot].movieFrame = movieFrame;
    m_keyframes[slot].valid = true;
    m_keyframes[slot].sectionCheckpoint = isSection;
    m_nextKeyframeSlot = static_cast<size_t>(slot) + 1;
    LOG(1, "[TAS] %s checkpoint slot=%d movieFrame=%u gameFrame=%u\n",
        isSection ? "section" : "periodic", slot,
        static_cast<unsigned int>(movieFrame), GetCurrentFrame());
}

void TasManager::ClearSnapshot() {
    m_projectBase = TasProjectFile::BasePair{};
    FinishFidelityRun("base-invalidated");
    // Dropping the apparatus releases its reserved slots, so every keyframe held in them
    // is gone too. Covers import, exit and any base-state reset.
    ClearKeyframes();
    // Same reason for the verification copies: their whole identity is "this slot, in this
    // match". Once the apparatus is gone there is nothing left for them to be compared against,
    // and a stale copy that still looked ready would let a later run test bytes against a match
    // they never came from.
    ClearFidelityBaseCopies();
    delete m_snapshotBuffer;
    m_snapshotBuffer = nullptr;
    m_snapshotSize = 0;
    delete m_snapshotOwner;
    m_snapshotOwner = nullptr;
    m_baseFrame = 0;
    m_playbackBaseFrame = 0;
    m_playbackSnapshotSize = 0;
    m_playbackBaseReady = false;
    m_baseLeadInRemaining = 0;
    m_capturePlaybackBaseAfterIncrement = false;
    m_finalizeBasePairAfterIncrement = false;
    m_finalizePresentationLeadInAfterIncrement = false;
}

void TasManager::Enter() {
    if (m_active) {
        return;
    }
    if (!IsInTrainingMatch()) {
        SetError(L("TAS mode is available only during a training match.").c_str());
        return;
    }
    // Every TAS input goes through OverrideBattleInputPacked, which only reaches the game
    // via the BattleInputWrite hook. That hook is skipped when controller hooks are
    // disabled, so without it playback would run and silently do nothing.
    if (!IsBattleInputHookInstalled()) {
        SetError(L("TAS mode needs the controller hooks. Enable EnableControllerHooks in settings.ini and restart the game.").c_str());
        return;
    }

    m_active = true;
    m_runState = TasRunState::Idle;
    m_playhead = 0;
    m_runTarget = 0;
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    m_lastScheduledFrame = 0;
    m_movie.clear();
    m_sections.clear();
    m_commandFrames.clear();
    m_commandCursor = 0;
    m_inputsParsed = false;
    m_rerecordCount = 0;
    m_error.clear();
    m_status.clear();

    auto& overrides = ControllerOverrideManager::GetInstance();
    m_p2KeyboardOverrideWasEnabled = overrides.IsMultipleKeyboardOverrideEnabled();
    if (m_p2KeyboardOverrideWasEnabled) {
        overrides.SetMultipleKeyboardOverrideEnabled(false);
    }
    LOG(1, "[TAS] entered movie mode frame=%u\n", GetCurrentFrame());
    // Build identification. A log that cannot say which diagnostic code produced it is what
    // makes "the trace lines are missing" ambiguous between a build problem and a code path
    // that never ran, so the banner is emitted here rather than inferred later.
    LOG(1,
        "[TAS][DIAG] revision=tas-project-archive-4-runtime-evidence buildDate=%s buildTime=%s traceGate=%d\n",
        __DATE__, __TIME__, m_fidelityTraceEnabled ? 1 : 0);

    auto* frameHistory = WindowManager::GetInstance().GetWindowContainer()
        ->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);
    if (frameHistory && !frameHistory->IsOpen()) {
        frameHistory->Open();
        m_frameHistoryOpenedByTas = true;
    }
}

void TasManager::Exit() {
    FinishFidelityRun("exit");
    // Exiting is an explicit cancel: a pending capture must not be able to run its save after
    // the apparatus it depends on has been released.
    m_baseLeadInRemaining = 0;
    m_capturePlaybackBaseAfterIncrement = false;
    m_finalizeBasePairAfterIncrement = false;
    m_finalizePresentationLeadInAfterIncrement = false;
    m_liveRecording = false;
    m_liveRecordingFrames.clear();
    m_liveRecordingOpponentFrames.clear();
    m_liveRecordingHasFrame = false;
    ClearInputOverride();
    if (m_p2KeyboardOverrideWasEnabled) {
        ControllerOverrideManager::GetInstance().SetMultipleKeyboardOverrideEnabled(true);
    }
    m_p2KeyboardOverrideWasEnabled = false;

    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    if (m_frameHistoryOpenedByTas) {
        auto* frameHistory = WindowManager::GetInstance().GetWindowContainer()
            ->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);
        if (frameHistory) {
            frameHistory->Close();
        }
    }

    m_frameHistoryOpenedByTas = false;
    m_playbackUiHidden = false;
    m_frameHistoryWasOpenBeforePlayback = false;
    m_runState = TasRunState::Idle;
    m_active = false;
    m_playhead = 0;
    m_runTarget = 0;
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    m_lastScheduledFrame = 0;
    m_movie.clear();
    m_commandFrames.clear();
    m_commandCursor = 0;
    m_inputsParsed = false;
    m_error.clear();
    m_status.clear();
    ClearSnapshot();
}

void TasManager::ScheduleMovieFrame() {
    if (m_playhead >= m_movie.size()) {
        ClearInputOverride();
        return;
    }
    m_scheduledInput = m_movie[m_playhead];
    m_hasScheduledInput = true;
    OverrideBattleInputPacked(0, m_scheduledInput.p1, 1);
    OverrideBattleInputPacked(1, m_scheduledInput.p2, 1);
}

void TasManager::ScheduleNeutralFrame() {
    m_scheduledInput = TasFrameInput{};
    m_hasScheduledInput = true;
    OverrideBattleInputPacked(0, m_scheduledInput.p1, 1);
    OverrideBattleInputPacked(1, m_scheduledInput.p2, 1);
}

// --- snapshot-copy verification ---------------------------------------------------------
//
// Private-byte restore failed its native address-registration precondition. Keep
// read-only copies and native-slot baselines, but reject private restore requests.
// No file import, buffer substitution, or cross-process state reconstruction.

bool TasManager::CaptureFidelityBaseCopies() {
    if (!m_active || !IsInTrainingMatch()) {
        SetError(L("TAS mode is available only during a training match.").c_str());
        return false;
    }
    if (!m_snapshotOwner || !HasBasePair()) {
        SetError(L("Save a base state first.").c_str());
        return false;
    }
    if (IsPlaybackRunning()) {
        SetError(L("Stop playback before copying the base states.").c_str());
        return false;
    }
    if (m_liveRecording) {
        SetError(L("Stop the live recording before copying the base states.").c_str());
        return false;
    }

    // Copy into locals and adopt them only once both halves are good: a half pair that looked
    // usable would be worse than no copies, because a run against it would compare B's copy
    // against A's slot and call any difference a divergence.
    SnapshotApparatus::SlotBytes copyA;
    SnapshotApparatus::SlotBytes copyB;
    if (!m_snapshotOwner->CopyLogicalSlot(kTasPlaybackBaseSlot, &copyA)) {
        SetError(L("Could not copy playback base A.").c_str());
        return false;
    }
    if (!m_snapshotOwner->CopyLogicalSlot(kTasBaseSlot, &copyB)) {
        SetError(L("Could not copy editing base B.").c_str());
        return false;
    }

    // The copies have to describe the pair the manager believes it has, in the same way the
    // capture validates it: A exactly kPresentationLeadInFrames before B. A pair copied while
    // another feature had rewritten one slot would otherwise be tested as if it were ours.
    if (copyA.sourceManager != copyB.sourceManager || copyA.sourceEpoch != copyB.sourceEpoch ||
        copyA.sourceBuffer == copyB.sourceBuffer || copyA.sourcePhysicalSlot == copyB.sourcePhysicalSlot) {
        SetError(L("The copied A/B states do not belong to distinct slots in one reservation.").c_str());
        return false;
    }
    if (copyB.frame != m_baseFrame || copyA.frame != m_playbackBaseFrame) {
        SetError(L("The copied base states do not match the saved pair.").c_str());
        LOG(0, "[TAS][COPY] frame mismatch copyA=%u expectA=%u copyB=%u expectB=%u\n",
            copyA.frame, m_playbackBaseFrame, copyB.frame, m_baseFrame);
        return false;
    }
    if (copyB.frame <= copyA.frame || copyB.frame - copyA.frame != kPresentationLeadInFrames) {
        SetError(L("The copied base states are not 60 frames apart.").c_str());
        LOG(0, "[TAS][COPY] pair spacing rejected A=%u B=%u expectedLeadIn=%u\n",
            copyA.frame, copyB.frame, kPresentationLeadInFrames);
        return false;
    }
    // Different native captures may have different payload lengths. Report the sizes;
    // a length difference alone does not prove truncation or corruption.
    if (copyA.bytes.size() != copyB.bytes.size()) {
        LOG(1, "[TAS][COPY] payload lengths differ A=%u B=%u; read-only copies retained\n",
            static_cast<unsigned int>(copyA.bytes.size()),
            static_cast<unsigned int>(copyB.bytes.size()));
    }

    // The one check CopyLogicalSlot cannot do for us: read the slot again and compare, so "the
    // copy equals the slot" is established at copy time rather than assumed. A mismatch here
    // means the slot is being written while we read it - another feature owns it, or a keyframe
    // capture landed in between - and no comparison built on these bytes would mean anything.
    const bool reA = m_snapshotOwner->MatchesLogicalSlot(kTasPlaybackBaseSlot, copyA);
    const bool reB = m_snapshotOwner->MatchesLogicalSlot(kTasBaseSlot, copyB);
    if (!reA || !reB) {
        SetError(L("The base state slots changed while they were being copied.").c_str());
        LOG(0, "[TAS][COPY] re-read mismatch okA=%d okB=%d\n", reA ? 1 : 0, reB ? 1 : 0);
        return false;
    }

    m_fidelityCopies.baseA = std::move(copyA);
    m_fidelityCopies.baseB = std::move(copyB);
    m_fidelityCopies.frameA = m_playbackBaseFrame;
    m_fidelityCopies.frameB = m_baseFrame;
    ++m_fidelityCopies.generation;
    m_fidelityCopies.ready = true;

    const size_t totalBytes =
        m_fidelityCopies.baseA.bytes.size() + m_fidelityCopies.baseB.bytes.size();
    m_error.clear();
    m_status = L("Copied both base states into memory.");
    LOG(1,
        "[TAS][COPY] ready generation=%u A.frame=%u A.size=%u A.digest=0x%08X "
        "B.frame=%u B.size=%u B.digest=0x%08X totalBytes=%u\n",
        static_cast<unsigned int>(m_fidelityCopies.generation),
        m_fidelityCopies.frameA,
        static_cast<unsigned int>(m_fidelityCopies.baseA.bytes.size()),
        m_fidelityCopies.baseA.digest,
        m_fidelityCopies.frameB,
        static_cast<unsigned int>(m_fidelityCopies.baseB.bytes.size()),
        m_fidelityCopies.baseB.digest,
        static_cast<unsigned int>(totalBytes));
    return true;
}

void TasManager::ClearFidelityBaseCopies() {
    // Copy storage does not own the lifecycle of a running native-slot test.
    const bool hadCopies = m_fidelityCopies.ready;
    const uint64_t generation = m_fidelityCopies.generation;
    m_fidelityCopies = FidelityBaseCopies{};
    m_fidelityCopies.generation = generation;
    if (hadCopies) {
        LOG(1, "[TAS][COPY] released\n");
    }
}

bool TasManager::StartFidelityRun(FidelityRestoreSource source, FidelityStartPoint start) {
    // Reject before changing run identity or touching the live match. No slot fallback.
    if (source == FidelityRestoreSource::PrivateBytes ||
        (source != FidelityRestoreSource::Slot && source != FidelityRestoreSource::CopyToOriginalSlot)) {
        SetError("Private-byte restore is disabled: the native loader requires a registered state address.");
        return false;
    }
    if (start != FidelityStartPoint::AWithLeadIn && start != FidelityStartPoint::BDirect) {
        SetError("Unknown diagnostic start point.");
        return false;
    }
    if (source == FidelityRestoreSource::CopyToOriginalSlot && !m_fidelityCopies.ready) {
        SetError("Copy writeback requires a captured base pair.");
        return false;
    }
    if (!IsInTrainingMatch() || !g_gameVals.pFrameCount) {
        SetError(L("TAS mode is available only during a training match.").c_str());
        return false;
    }
    if (!m_active || m_movie.empty()) {
        SetError(L("Edit movie input first.").c_str());
        return false;
    }
    if (!HasBasePair()) {
        SetError(L("Save a base state first.").c_str());
        return false;
    }
    if (IsPlaybackRunning() || IsSeeking()) {
        SetError(L("Stop the current run first.").c_str());
        return false;
    }
    if (m_liveRecording) {
        SetError(L("Stop the live recording first.").c_str());
        return false;
    }
    if (m_fidelityDiagnosticRun) {
        SetError(L("Stop the current run first.").c_str());
        return false;
    }

    // Freeze the identity of the run before anything else can change it. Everything the analysis
    // will use to decide whether two runs are comparable is captured here, at one moment.
    ++m_fidelityRunId;
    m_fidelityDiagnosticRun = true;
    m_fidelityRunSource = source;
    m_fidelityRunStart = start;
    m_fidelityRunGeneration = m_fidelityCopies.ready ? m_fidelityCopies.generation : 0;
    m_fidelityRunSampleCount = 0;
    m_fidelityRunMovieDigest = 0;
    for (const TasFrameInput& frame : m_movie) {
        // Order-sensitive diagnostic fingerprint, not proof of equality. Compare
        // movie length and per-frame inputs as well; 32-bit hashes can collide.
        m_fidelityRunMovieDigest = (m_fidelityRunMovieDigest * 16777619u) ^ frame.p1;
        m_fidelityRunMovieDigest = (m_fidelityRunMovieDigest * 16777619u) ^ frame.p2;
    }

    const char* sourceName = FidelitySourceName(source);
    const char* startName = start == FidelityStartPoint::AWithLeadIn ? "A-leadin" : "B-direct";
    LOG(1,
        "[TAS][VERIFY] armed run=%u source=%s start=%s generation=%u base=%u playbackBase=%u "
        "movie=%u movieDigest=0x%08X gameFrame=%u\n",
        m_fidelityRunId,
        sourceName,
        startName,
        static_cast<unsigned int>(m_fidelityRunGeneration),
        m_baseFrame,
        m_playbackBaseFrame,
        static_cast<unsigned int>(m_movie.size()),
        m_fidelityRunMovieDigest,
        GetCurrentFrame());

    m_presentationMode = false;
    if (start == FidelityStartPoint::AWithLeadIn) {
        if (!LoadPlaybackBaseSnapshotAndLeadIn()) {
            FinishFidelityRun("restore-failed");
            ClearInputOverride();
            m_runState = TasRunState::PausedAtMovieFrame;
            g_gameVals.isFrameFrozen = true;
            g_gameVals.framesToReach = GetCurrentFrame();
            SetError("Base restore failed. Stop diagnostics and restart the training match.");
            return false;
        }
        m_error.clear();
        return true;
    }

    // Arm only. The existing post-increment callback restores B and schedules input
    // together, on the same side of the counter increment as the A lead-in landing.
    ClearInputOverride();
    m_pendingKeyframeSlot = -1;
    m_finalizePresentationLeadInAfterIncrement = false;
    m_fidelityStartDirectPlayback = true;
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    m_error.clear();
    return true;
}

// --- snapshot-copy verification, internals -------------------------------------------------

bool TasManager::RestoreBaseForFidelityRun(FidelityRestoreSource source, FidelityStartPoint start) {
    if (!m_snapshotOwner) {
        SetError(L("No native base state is available.").c_str());
        return false;
    }
    if ((source != FidelityRestoreSource::Slot && source != FidelityRestoreSource::CopyToOriginalSlot) ||
        (start != FidelityStartPoint::AWithLeadIn && start != FidelityStartPoint::BDirect)) {
        SetError("Unsupported diagnostic restore source or start point.");
        return false;
    }
    const bool fromA = start == FidelityStartPoint::AWithLeadIn;
    const int slot = fromA ? kTasPlaybackBaseSlot : kTasBaseSlot;
    const unsigned int expected = fromA ? m_playbackBaseFrame : m_baseFrame;
    const char* name = fromA ? "A" : "B";
    const auto& copy = fromA ? m_fidelityCopies.baseA : m_fidelityCopies.baseB;
    if (source == FidelityRestoreSource::CopyToOriginalSlot &&
        (!m_fidelityDiagnosticRun || !m_fidelityCopies.ready ||
            m_fidelityCopies.generation != m_fidelityRunGeneration)) {
        SetError("Diagnostic copies expired; no slot fallback was used.");
        return false;
    }
    TasLogSlotDigest("run-slot-before-A", kTasPlaybackBaseSlot);
    TasLogSlotDigest("run-slot-before-B", kTasBaseSlot);
    LOG(1, "[TAS][VERIFY] run=%u restore source=%s slot=%s\n",
        m_fidelityRunId, FidelitySourceName(source), name);
    const bool restored = source == FidelityRestoreSource::CopyToOriginalSlot
        ? m_snapshotOwner->RestoreCopyToOriginalSlot(slot, copy)
        : m_snapshotOwner->load_snapshot_index(slot);
    if (!restored) {
        SetError("Base restore failed; no slot fallback was used. Check the snapshot log.");
        return false;
    }
    TasLogSlotDigest("run-after-A", kTasPlaybackBaseSlot);
    TasLogSlotDigest("run-after-B", kTasBaseSlot);
    const unsigned int frame = GetCurrentFrame();
    LOG(1, "[TAS][VERIFY] run=%u restored frame=%u expected%s=%u\n",
        m_fidelityRunId, frame, name, expected);
    // Never repair the frame counter to conceal a restore mismatch.
    if (frame != expected) {
        SetError("The restored state is not the expected base frame.");
        return false;
    }
    return true;
}

void TasManager::FinishFidelityRun(const char* reason) {
    if (!m_fidelityDiagnosticRun) {
        return;
    }
    LOG(1,
        "[TAS][VERIFY] end run=%u source=%s start=%s generation=%u reason=%s sampledFrames=%u "
        "playhead=%u movie=%u gameFrame=%u\n",
        m_fidelityRunId,
        FidelitySourceName(m_fidelityRunSource),
        m_fidelityRunStart == FidelityStartPoint::AWithLeadIn ? "A-leadin" : "B-direct",
        static_cast<unsigned int>(m_fidelityRunGeneration),
        reason ? reason : "(null)",
        static_cast<unsigned int>(m_fidelityRunSampleCount),
        static_cast<unsigned int>(m_playhead),
        static_cast<unsigned int>(m_movie.size()),
        GetCurrentFrame());
    m_fidelityDiagnosticRun = false;
    m_fidelityStartDirectPlayback = false;
}

void TasManager::LogFidelityFrameBeforeIncrement(unsigned int gameFrame) {
    // Off unless a save/restore comparison is being run: this sits in the frame counter path
    // and writes six lines per frame, which is far too much for ordinary editing.
    if (!m_fidelityTraceEnabled) {
        return;
    }
    // Presentation playback only. A seek or a preview resumed mid-movie starts from whatever
    // the state already was, so its frames are not a restore of the base and comparing them
    // against a base run would report a divergence that is simply a different starting point.
    //
    // The exception is an explicit verification run that starts from the editing base: it is a
    // restore of a base and must be sampled, but it is not a presentation. Everything else -
    // preview, seek, re-simulation - is still excluded.
    const bool diagnosticRun = m_fidelityDiagnosticRun;
    if (m_runState != TasRunState::PlayingMovie || (!m_presentationMode && !diagnosticRun)) {
        return;
    }
    if (!m_hasScheduledInput) {
        return;
    }

    // The trace is a comparison aid, not a record of the whole movie: it is capped so a long
    // movie cannot turn the log into the dominant cost of the run. A run that stops here is
    // partially verified and must be reported as such.
    constexpr size_t kFidelityTraceFrameLimit = 1200;
    if (m_playhead >= kFidelityTraceFrameLimit) {
        return;
    }

    if (m_playhead == 0) {
        // The run header, emitted once. Everything the comparison needs to decide whether two
        // runs are comparable at all: the same base, the same movie, the same start index.
        //
        // A verification run reports its own source, generation and movie digest as well. Those
        // are what make a slot run and a bytes run comparable on purpose rather than by hope.
        if (diagnosticRun) {
            LOG(1,
                "[TAS][VERIFY] begin run=%u mode=%s source=%s start=%s generation=%u "
                "base=%u playbackBase=%u startIndex=%u movie=%u movieDigest=0x%08X gameFrame=%u\n",
                m_fidelityRunId,
                "snapshot-copy-verification",
                FidelitySourceName(m_fidelityRunSource),
                m_fidelityRunStart == FidelityStartPoint::AWithLeadIn ? "A-leadin" : "B-direct",
                static_cast<unsigned int>(m_fidelityRunGeneration),
                m_baseFrame,
                m_playbackBaseFrame,
                static_cast<unsigned int>(m_playhead),
                static_cast<unsigned int>(m_movie.size()),
                m_fidelityRunMovieDigest,
                gameFrame);
        } else {
            LOG(1,
                "[TAS][VERIFY] begin run=%u mode=%s base=%u playbackBase=%u "
                "startIndex=%u movie=%u gameFrame=%u\n",
                m_fidelityRunId,
                "presentation-from-A",
                m_baseFrame,
                m_playbackBaseFrame,
                static_cast<unsigned int>(m_playhead),
                static_cast<unsigned int>(m_movie.size()),
                gameFrame);
        }
        TasLogBasePairDigests("verify-run-begin");
    }

    if (diagnosticRun) {
        ++m_fidelityRunSampleCount;
    }

    LOG(1,
        "[TAS][VERIFY] run=%u phase=post-input-preinc index=%u gameFrame=%u "
        "scheduledP1=%u scheduledP2=%u\n",
        m_fidelityRunId,
        static_cast<unsigned int>(m_playhead),
        gameFrame,
        static_cast<unsigned int>(m_scheduledInput.p1),
        static_cast<unsigned int>(m_scheduledInput.p2));

    // The scheduled input above is what TAS asked for; these are what the game is actually
    // holding. Two runs that agree on the request but not on the resulting state are the
    // interesting case, which is why both go in.
    TasLogPlayerState("verify-post-input-preinc", "P1", g_interfaces.player1.GetData());
    TasLogPlayerState("verify-post-input-preinc", "P2", g_interfaces.player2.GetData());
}

bool TasManager::BeginMovieRun(TasRunState state, size_t target) {
    if (!m_active || !g_gameVals.pFrameCount || target > m_movie.size()) {
        return false;
    }
    m_runState = state;
    m_runTarget = target;
    m_lastScheduledFrame = *g_gameVals.pFrameCount;
    LOG(1, "[TAS] run state=%d playhead=%u target=%u movie=%u frame=%u\n",
        static_cast<int>(state), static_cast<unsigned int>(m_playhead),
        static_cast<unsigned int>(target), static_cast<unsigned int>(m_movie.size()),
        m_lastScheduledFrame);
    if (m_playhead >= m_runTarget) {
        FinishMovieRun(false);
        return true;
    }
    ScheduleMovieFrame();
    g_gameVals.isFrameFrozen = true;
    g_gameVals.framesToReach = m_lastScheduledFrame + 1;
    return true;
}

void TasManager::FinishMovieRun(bool completed) {
    // Close the verification run before anything else can reuse its id: the end line is what
    // tells an analysis that the trace is complete, and how many frames it actually contains.
    FinishFidelityRun(completed ? "completed" : "stopped");
    ClearInputOverride();
    m_runTarget = m_playhead;
    m_runState = completed ? TasRunState::Idle : TasRunState::PausedAtMovieFrame;
    LOG(1, "[TAS] run finished completed=%d playhead=%u movie=%u frame=%u\n",
        completed ? 1 : 0, static_cast<unsigned int>(m_playhead),
        static_cast<unsigned int>(m_movie.size()), GetCurrentFrame());
    g_gameVals.isFrameFrozen = !completed;
    g_gameVals.framesToReach = completed ? 0 : GetCurrentFrame();

    if (completed && m_autoLoadAfterPlayback && HasBaseSnapshot()) {
        LoadBaseSnapshot();
        m_playhead = 0;
        m_runState = TasRunState::PausedAtMovieFrame;
    }
}

void TasManager::Update() {
    if (m_active && !IsInTrainingMatch()) {
        Exit();
        return;
    }
    if (!m_active || !g_gameVals.pFrameCount) {
        return;
    }

    // GetFrameCounter invokes us immediately before incrementing the game's frame count.
    // The input hooks have therefore finished the current frame, and currentFrame + 1
    // is the exact movie position after this callback returns.
    // Recording is mutually exclusive with playback (StartLiveRecording refuses while a
    // run is going), so it gets the frame to itself and nothing below it can be reached.
    if (m_liveRecording) {
        SampleLiveRecordingFrame();
        return;
    }

    const unsigned int currentFrame = *g_gameVals.pFrameCount;

    // B-direct owns this boundary but restores only after the increment. Do not
    // advance an old playhead or sample the frame preceding that restore.
    if (m_fidelityStartDirectPlayback) {
        return;
    }

    // The base capture owns the frame entirely: it is not a movie run, so it must not reach
    // any of the playback bookkeeping below. The snapshots themselves are not written from
    // here either - this runs before the game increments its frame counter, and saving now
    // would store a frame number that is about to change.
    if (IsPreparingBase()) {
        UpdateBaseLeadIn();
        return;
    }

    if (m_runState == TasRunState::PresentationLeadIn) {
        // The lead-in is run from playback base A, so its own countdown is the one that
        // matters here; the two must not share a counter or the movie would start 60 frames
        // late whenever a base capture also happened to be counting down.
        if (m_baseLeadInRemaining > 0) {
            --m_baseLeadInRemaining;
        }
        if (m_baseLeadInRemaining == 0) {
            // Defer the movie start past the increment so movie frame 0 begins on a frame
            // boundary, which is what makes it line up with the editor's frame 0.
            m_finalizePresentationLeadInAfterIncrement = true;
            ClearInputOverride();
            return;
        }
        ScheduleNeutralFrame();
        g_gameVals.isFrameFrozen = false;
        g_gameVals.framesToReach = 0;
        return;
    }

    if (m_runState == TasRunState::PresentationLeadOut) {
        if (m_presentationFramesRemaining > 0) {
            --m_presentationFramesRemaining;
        }
        if (m_presentationFramesRemaining == 0) {
            StopPlayback(true);
            return;
        }
        ScheduleNeutralFrame();
        g_gameVals.isFrameFrozen = false;
        g_gameVals.framesToReach = 0;
        return;
    }

    if (m_runState != TasRunState::PlayingMovie && m_runState != TasRunState::ReplayingMovie) {
        return;
    }

    // Sampled here, not after the increment: this is the last point at which the movie index
    // and the state it produced are still aligned, because m_playhead has not moved yet and
    // ScheduleMovieFrame has not yet replaced m_scheduledInput with the next frame's input.
    // Logging after ++m_playhead would label every sample with the following frame's index,
    // which is exactly the off-by-one that makes two runs look different when they are not.
    LogFidelityFrameBeforeIncrement(currentFrame);

    ++m_playhead;
    m_lastScheduledFrame = currentFrame + 1;
    CaptureKeyframeIfDue(true);
    if (m_playhead >= m_runTarget || m_playhead >= m_movie.size()) {
        const bool completed = m_runState == TasRunState::PlayingMovie && m_playhead >= m_movie.size();
        if (completed) {
            // End the diagnostic at the last movie sample, independently of preview pause
            // or presentation lead-out behavior.
            FinishFidelityRun("completed");
            if (m_presentationMode) {
                StartPresentationLeadOut();
            } else {
                // Preview is an editing pass. Freeze on the resulting state so
                // the next frame can be authored without losing the combo.
                StopPlayback(false);
                m_status = L("Preview finished and paused at the movie end.");
            }
        } else {
            FinishMovieRun(false);
        }
        return;
    }

    ScheduleMovieFrame();
    if (m_runState == TasRunState::PlayingMovie) {
        g_gameVals.isFrameFrozen = false;
        g_gameVals.framesToReach = 0;
    } else {
        g_gameVals.isFrameFrozen = true;
        g_gameVals.framesToReach = currentFrame + 2;
    }
}

void TasManager::StartMovieFrames() {
    m_presentationFramesRemaining = 0;
    m_runState = TasRunState::PlayingMovie;
    m_runTarget = m_movie.size();
    m_lastScheduledFrame = GetCurrentFrame();
    // A new run, so the trace lines that follow are attributable to this one run and not to
    // the previous playback of the same movie. Without this the two runs of a repeat test
    // would be one indistinguishable block of frames.
    if (!m_fidelityDiagnosticRun) {
        ++m_fidelityRunId;
    }
    m_fidelityRunStartCursor = m_playhead;
    m_fidelityRunFromBase = m_presentationMode;
    ScheduleMovieFrame();
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    LOG(1, "[TAS] continuous playback started playhead=%u target=%u movie=%u frame=%u\n",
        static_cast<unsigned int>(m_playhead), static_cast<unsigned int>(m_runTarget),
        static_cast<unsigned int>(m_movie.size()), m_lastScheduledFrame);
}

void TasManager::StartPresentationLeadOut() {
    ClearInputOverride();
    m_runState = TasRunState::PresentationLeadOut;
    m_presentationFramesRemaining = kPresentationLeadOutFrames;
    ScheduleNeutralFrame();
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    LOG(1, "[TAS] presentation lead-out started frames=%u\n", kPresentationLeadOutFrames);
}

void TasManager::StartPlayback(bool presentationMode) {
    if (m_fidelityDiagnosticRun) {
        SetError(L("Stop the current run first.").c_str());
        return;
    }
    if (!m_active || m_movie.empty()) {
        SetError(L("Edit movie input first.").c_str());
        return;
    }

    if (presentationMode && !HasBasePair()) {
        // A presentation needs both halves: without A there is no state to replay the hidden
        // lead-in from, and starting from B would put movie frame 0 sixty frames late.
        SetError(L("Save a base state before recording a presentation.").c_str());
        return;
    }

    if (presentationMode) {
        // Presentation is a recording of the whole combo, so it restores playback base A and
        // replays the hidden 60 neutral frames from there. Those frames exist so that movie
        // frame 0 lands on editing base B - the state the combo was authored against - which
        // is what stops a recording from starting 60 frames away from what the editor shows.
        m_playbackUiHidden = true;
        auto* frameHistory = WindowManager::GetInstance().GetWindowContainer()
            ->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);
        m_frameHistoryWasOpenBeforePlayback = frameHistory && frameHistory->IsOpen();
        if (frameHistory) {
            frameHistory->Close();
        }
        m_presentationMode = true;
        m_runTarget = m_movie.size();
        if (!LoadPlaybackBaseSnapshotAndLeadIn()) {
            m_presentationMode = false;
            StopPlayback(false);
            return;
        }
        m_error.clear();
        return;
    }

    // Preview picks up from wherever the playhead is, so you can watch just the tail of a
    // combo you are working on. From the very end there is nowhere to go, so that one case
    // restarts from the base state instead.
    if (m_playhead >= m_movie.size()) {
        if (!LoadBaseSnapshot()) {
            return;
        }
        m_playhead = 0;
    }
    m_presentationMode = false;

    m_playbackUiHidden = false;
    auto* frameHistory = WindowManager::GetInstance().GetWindowContainer()
        ->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);
    m_frameHistoryWasOpenBeforePlayback = frameHistory && frameHistory->IsOpen();
    if (frameHistory) {
        frameHistory->Close();
    }

    StartMovieFrames();
    m_error.clear();
}

void TasManager::StopPlayback(bool completed) {
    m_playbackUiHidden = false;
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    auto* frameHistory = WindowManager::GetInstance().GetWindowContainer()
        ->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);
    if (frameHistory && m_frameHistoryWasOpenBeforePlayback) {
        frameHistory->Open();
    }
    m_frameHistoryWasOpenBeforePlayback = false;
    FinishMovieRun(completed);
}

void TasManager::EditAndAdvanceFrames(int count) {
    if (m_liveRecording) {
        SetError(L("Stop the recording before committing input.").c_str());
        return;
    }
    if (IsPlaybackRunning()) {
        SetError(L("Cannot advance frames during playback.").c_str());
        return;
    }
    if (!m_active || !HasBaseSnapshot() || count <= 0) {
        SetError(L("Save a base state before editing movie input.").c_str());
        return;
    }
    if (!m_inputsParsed && !ParseInputs()) {
        return;
    }

    const size_t frameCount = static_cast<size_t>(count);
    if (frameCount > (std::numeric_limits<size_t>::max)() - m_playhead) {
        SetError(L("Movie frame count exceeds the addressable size limit.").c_str());
        return;
    }
    PushUndoState();
    const size_t end = m_playhead + frameCount;
    if (m_playhead < m_movie.size()) {
        // This is the destructive edit the UI warns about: everything after the playhead
        // is replaced, which is exactly what makes a rerecord rather than an undo.
        LOG(1, "[TAS] overwrite at frame %u: movie %u -> %u frames\n",
            static_cast<unsigned int>(m_playhead),
            static_cast<unsigned int>(m_movie.size()),
            static_cast<unsigned int>(end));
        m_movie.resize(m_playhead);
        m_sections.erase(std::remove_if(m_sections.begin(), m_sections.end(),
            [this](const TasSection& section) { return section.frame >= m_playhead; }),
            m_sections.end());
        InvalidateSectionCheckpointsBefore(m_playhead);
        InvalidateKeyframesAfter(m_playhead);
        ++m_rerecordCount;
    }
    if (m_movie.size() < end) {
        m_movie.resize(end, TasFrameInput{});
    }
    for (size_t i = m_playhead; i < end; ++i) {
        if (m_commandCursor < m_commandFrames.size()) {
            m_movie[i] = m_commandFrames[m_commandCursor++];
        } else {
            m_movie[i] = TasFrameInput{};
        }
    }
    BeginMovieRun(TasRunState::ReplayingMovie, end);
    m_error.clear();
}

bool TasManager::CanEditMovie() const {
    return m_active && !IsPlaybackRunning() && !m_liveRecording;
}

void TasManager::PushUndoState() {
    m_undoStack.push_back(MovieState{ m_movie, m_sections, m_playhead });
    if (m_undoStack.size() > kUndoDepth) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_redoStack.clear();
}

void TasManager::ResyncAfterEdit(size_t firstChangedFrame) {
    InvalidateKeyframesAfter(firstChangedFrame);

    if (m_playhead > m_movie.size()) {
        m_playhead = m_movie.size();
    }
    // Frames the match has already played were rewritten, so the state on screen was produced
    // by input that no longer exists. Replay to the same position from the stored movie.
    if (firstChangedFrame < m_playhead && HasBaseSnapshot()) {
        SeekToFrame(m_playhead);
    }
}

bool TasManager::InsertNeutralFrames(size_t index, size_t count) {
    if (!CanEditMovie() || count == 0) {
        return false;
    }
    if (index > m_movie.size()) {
        index = m_movie.size();
    }
    PushUndoState();
    m_movie.insert(m_movie.begin() + static_cast<ptrdiff_t>(index), count, TasFrameInput{});
    for (TasSection& section : m_sections) {
        if (section.frame >= index) section.frame += count;
    }
    InvalidateSectionCheckpointsBefore(index);
    if (index <= m_playhead) {
        m_playhead += count;
    }
    LOG(1, "[TAS] insert %u neutral frame(s) at %u -> %u frames\n",
        static_cast<unsigned int>(count), static_cast<unsigned int>(index),
        static_cast<unsigned int>(m_movie.size()));
    ResyncAfterEdit(index);
    m_error.clear();
    return true;
}

bool TasManager::DeleteFrames(size_t index, size_t count) {
    if (!CanEditMovie() || count == 0 || index >= m_movie.size()) {
        return false;
    }
    count = (std::min)(count, m_movie.size() - index);
    PushUndoState();
    m_movie.erase(m_movie.begin() + static_cast<ptrdiff_t>(index),
        m_movie.begin() + static_cast<ptrdiff_t>(index + count));
    m_sections.erase(std::remove_if(m_sections.begin(), m_sections.end(),
        [index, count](const TasSection& section) {
            return section.frame >= index && section.frame < index + count;
        }), m_sections.end());
    InvalidateSectionCheckpointsBefore(index);
    for (TasSection& section : m_sections) {
        if (section.frame >= index + count) section.frame -= count;
    }
    if (m_playhead > index) {
        m_playhead -= (std::min)(count, m_playhead - index);
    }
    LOG(1, "[TAS] delete %u frame(s) at %u -> %u frames\n",
        static_cast<unsigned int>(count), static_cast<unsigned int>(index),
        static_cast<unsigned int>(m_movie.size()));
    ResyncAfterEdit(index);
    m_error.clear();
    return true;
}

bool TasManager::MoveFrames(size_t fromIndex, size_t count, size_t toIndex, size_t* outNewIndex) {
    if (!CanEditMovie() || count == 0 || fromIndex >= m_movie.size()) {
        return false;
    }
    count = (std::min)(count, m_movie.size() - fromIndex);
    if (toIndex > m_movie.size()) {
        toIndex = m_movie.size();
    }
    // Dropping inside the block being moved, or exactly where it already is, is a no-op.
    if (toIndex >= fromIndex && toIndex <= fromIndex + count) {
        return false;
    }

    PushUndoState();
    const std::vector<TasFrameInput> block(m_movie.begin() + static_cast<ptrdiff_t>(fromIndex),
        m_movie.begin() + static_cast<ptrdiff_t>(fromIndex + count));
    m_movie.erase(m_movie.begin() + static_cast<ptrdiff_t>(fromIndex),
        m_movie.begin() + static_cast<ptrdiff_t>(fromIndex + count));

    const size_t insertAt = toIndex > fromIndex ? toIndex - count : toIndex;
    m_movie.insert(m_movie.begin() + static_cast<ptrdiff_t>(insertAt), block.begin(), block.end());
    InvalidateSectionCheckpointsBefore((std::min)(fromIndex, toIndex));
    for (TasSection& section : m_sections) {
        const size_t frame = section.frame;
        if (frame >= fromIndex && frame < fromIndex + count) {
            section.frame = insertAt + (frame - fromIndex);
        } else if (toIndex > fromIndex && frame >= fromIndex + count && frame < toIndex) {
            section.frame -= count;
        } else if (toIndex < fromIndex && frame >= toIndex && frame < fromIndex) {
            section.frame += count;
        }
    }
    std::sort(m_sections.begin(), m_sections.end(), [](const TasSection& a, const TasSection& b) {
        return a.frame < b.frame;
    });

    if (outNewIndex) {
        *outNewIndex = insertAt;
    }
    LOG(1, "[TAS] move %u frame(s) from %u to %u\n",
        static_cast<unsigned int>(count), static_cast<unsigned int>(fromIndex),
        static_cast<unsigned int>(insertAt));
    ResyncAfterEdit((std::min)(fromIndex, insertAt));
    m_error.clear();
    return true;
}

bool TasManager::SetFrameInput(size_t index, TasFrameInput input) {
    if (!CanEditMovie() || index >= m_movie.size()) {
        return false;
    }
    PushUndoState();
    m_movie[index] = input;
    // Retyping a frame changes the frame contents but does not delete the frame itself.
    // A marker attached to this frame must therefore remain attached to it.
    InvalidateSectionCheckpointsBefore(index);
    ResyncAfterEdit(index);
    m_error.clear();
    return true;
}

bool TasManager::DuplicateFrames(size_t index, size_t count) {
    if (!CanEditMovie() || count == 0 || index >= m_movie.size()) {
        return false;
    }
    count = (std::min)(count, m_movie.size() - index);
    PushUndoState();
    const std::vector<TasFrameInput> block(m_movie.begin() + static_cast<ptrdiff_t>(index),
        m_movie.begin() + static_cast<ptrdiff_t>(index + count));
    m_movie.insert(m_movie.begin() + static_cast<ptrdiff_t>(index + count), block.begin(), block.end());
    ResyncAfterEdit(index + count);
    m_error.clear();
    return true;
}

bool TasManager::Undo() {
    if (!CanEditMovie() || m_undoStack.empty()) {
        return false;
    }
    m_redoStack.push_back(MovieState{ m_movie, m_sections, m_playhead });
    const MovieState state = m_undoStack.back();
    m_undoStack.pop_back();
    m_movie = state.movie;
    m_sections = state.sections;
    m_playhead = (std::min)(state.playhead, m_movie.size());
    ClearKeyframes();
    if (HasBaseSnapshot()) {
        SeekToFrame(m_playhead);
    }
    m_status = L("Undone.");
    return true;
}

bool TasManager::Redo() {
    if (!CanEditMovie() || m_redoStack.empty()) {
        return false;
    }
    m_undoStack.push_back(MovieState{ m_movie, m_sections, m_playhead });
    const MovieState state = m_redoStack.back();
    m_redoStack.pop_back();
    m_movie = state.movie;
    m_sections = state.sections;
    m_playhead = (std::min)(state.playhead, m_movie.size());
    ClearKeyframes();
    if (HasBaseSnapshot()) {
        SeekToFrame(m_playhead);
    }
    m_status = L("Redone.");
    return true;
}

void TasManager::ResetParsedInputs() {
    if (!m_active) {
        return;
    }
    if (!m_inputsParsed && !ParseInputs()) {
        return;
    }

    for (auto& frame : m_commandFrames) {
        frame = TasFrameInput{};
    }
    m_commandCursor = 0;
    m_error.clear();
    m_status = L("Parsed inputs reset to neutral.");
}

void TasManager::ResetMovie() {
    if (!m_active) {
        return;
    }
    StopPlayback(false);
    if (HasBaseSnapshot()) {
        LoadBaseSnapshot();
    }
    ClearInputOverride();
    m_movie.clear();
    m_sections.clear();
    ClearKeyframes();
    m_undoStack.clear();
    m_redoStack.clear();
    m_playhead = 0;
    m_runTarget = 0;
    m_commandCursor = 0;
    m_runState = TasRunState::Idle;
    m_error.clear();
    m_status = L("Movie reset.");
}

bool TasManager::AdvanceOneFrame() {
    return AdvanceFrames(1);
}

bool TasManager::AdvanceFrames(int count) {
    if (count <= 0) {
        return true;
    }
    if (IsPlaybackRunning()) {
        SetError(L("Cannot advance frames during playback.").c_str());
        return false;
    }
    EditAndAdvanceFrames(count);
    return m_error.empty();
}

bool TasManager::SeekToFrame(size_t targetFrame) {
    // StopLiveRecording clears the flag before it seeks the match back, so its own seek
    // still gets through here.
    if (m_liveRecording) {
        SetError(L("Stop the recording before moving through the combo.").c_str());
        return false;
    }
    if (IsPlaying()) {
        SetError(L("Cannot seek during playback.").c_str());
        return false;
    }
    if (!m_active || !HasBaseSnapshot()) {
        SetError(L("Save a base state before seeking.").c_str());
        return false;
    }
    if (targetFrame > m_movie.size()) {
        targetFrame = m_movie.size();
    }
    const int keyframe = FindKeyframeFor(targetFrame);
    if (keyframe >= 0 && m_keyframes[keyframe].movieFrame > 0) {
        // Restart from the nearest keyframe and re-simulate only the remainder.
        if (m_snapshotOwner->load_snapshot_index(kTasFirstKeyframeSlot + keyframe)) {
            ClearInputOverride();
            m_playhead = m_keyframes[keyframe].movieFrame;
            m_runTarget = m_playhead;
            m_lastScheduledFrame = GetCurrentFrame();
            g_gameVals.isFrameFrozen = true;
            g_gameVals.framesToReach = GetCurrentFrame();
            if (m_playhead == targetFrame) {
                m_runState = TasRunState::PausedAtMovieFrame;
                m_status = L("Seek complete.");
                return true;
            }
            m_status = L("Seeking...");
            return BeginMovieRun(TasRunState::ReplayingMovie, targetFrame);
        }
        LOG(1, "[TAS] keyframe load failed, falling back to the base state\n");
    }

    if (!LoadBaseSnapshot()) {
        return false;
    }

    // LoadBaseSnapshot leaves the playhead at 0, which is already the answer for a seek
    // to the start; anything further is re-simulated from the stored input.
    if (targetFrame == 0) {
        ClearInputOverride();
        m_runTarget = 0;
        m_runState = m_movie.empty() ? TasRunState::Idle : TasRunState::PausedAtMovieFrame;
        m_status = L("At the start of the movie.");
        return true;
    }

    m_status = L("Seeking...");
    return BeginMovieRun(TasRunState::ReplayingMovie, targetFrame);
}

bool TasManager::SeekRelative(int delta) {
    if (delta == 0) {
        return true;
    }
    if (delta > 0) {
        return AdvanceOrExtend(delta);
    }

    const size_t amount = static_cast<size_t>(-static_cast<long long>(delta));
    const size_t target = amount >= m_playhead ? 0 : m_playhead - amount;
    if (target == m_playhead) {
        return true;
    }
    return SeekToFrame(target);
}

bool TasManager::AdvanceOrExtend(int count) {
    if (count <= 0) {
        return true;
    }
    if (IsPlaybackRunning()) {
        SetError(L("Cannot seek during playback.").c_str());
        return false;
    }
    if (!m_active || !HasBaseSnapshot()) {
        SetError(L("Save a base state before seeking.").c_str());
        return false;
    }

    const size_t target = m_playhead + static_cast<size_t>(count);
    if (target > m_movie.size()) {
        // Running off the end is not an error: the combo simply gets that many idle frames,
        // which is what waiting looks like in a movie.
        m_movie.resize(target, TasFrameInput{});
    }

    // Forward movement never needs a reload - the match is already at m_playhead, so the
    // frames in between are simply played out from here.
    m_status.clear();
    return BeginMovieRun(TasRunState::ReplayingMovie, target);
}

// A recorded frame comes straight off the game's packed input word, which carries bits the
// notation cannot express (the SPECIAL macro button, and a direction nibble the game is free
// to leave at 0). Anything the composer could not parse back is dropped here, so the text
// this produces is always valid and always round-trips.
static uint16_t SanitizeRecordedInput(uint16_t packed) {
    const uint16_t direction = static_cast<uint16_t>(packed & 0x0F);
    const uint16_t buttons = static_cast<uint16_t>(packed & 0x1F0); // A B C D + taunt
    return static_cast<uint16_t>((direction >= 1 && direction <= 9 ? direction : 5) | buttons);
}

size_t TasManager::GetLiveRecordingFrameLimit() {
    // 60 seconds. Long enough for any combo, and short enough that the notation it produces
    // still fits the composer's text field even if every frame carries buttons.
    return 3600;
}

bool TasManager::StartLiveRecording(int player, std::vector<uint16_t> opponentFrames) {
    if (m_liveRecording || (player != 0 && player != 1)) {
        return false;
    }
    if (!m_active) {
        SetError(L("TAS mode is not active.").c_str());
        return false;
    }
    if (IsPlaybackRunning()) {
        SetError(L("Cannot record while playback is running.").c_str());
        return false;
    }
    // Stopping seeks back to where the editor was, and committing the capture re-simulates
    // from the base state. Both need one, so there is no point starting without it.
    if (!HasBaseSnapshot()) {
        SetError(L("Save a base state before recording live input.").c_str());
        return false;
    }
    if (!IsInTrainingMatch()) {
        SetError(L("TAS mode is available only during a training match.").c_str());
        return false;
    }
    if (!IsBattleInputHookInstalled()) {
        SetError(L("TAS mode needs the controller hooks. Enable EnableControllerHooks in settings.ini and restart the game.").c_str());
        return false;
    }

    m_liveRecording = true;
    m_liveRecordingPlayer = player;
    m_liveRecordingReturnFrame = m_playhead;
    m_liveRecordingHasFrame = false;
    m_liveRecordingLastFrame = 0;
    m_liveRecordingFrames.clear();
    m_liveRecordingOpponentFrames = std::move(opponentFrames);

    // Hand the pad back: the recorded player must be free of any override, and the match has
    // to actually run, which it does not after a commit leaves it frozen on a frame.
    ClearInputOverride();
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    m_error.clear();
    m_status = L("Recording live input. Press Stop to put it in the text field.");
    LOG(1, "[TAS] live recording started player=%d from movie frame %u\n",
        player, static_cast<unsigned int>(m_liveRecordingReturnFrame));
    return true;
}

void TasManager::StopLiveRecording() {
    if (!m_liveRecording) {
        return;
    }
    m_liveRecording = false;
    ClearInputOverride();
    LOG(1, "[TAS] live recording stopped player=%d frames=%u\n",
        m_liveRecordingPlayer, static_cast<unsigned int>(m_liveRecordingFrames.size()));

    // The frames the user just played were a capture, not a commit, so the match goes back
    // to where the editor left it. The movie is untouched either way.
    SeekToFrame(m_liveRecordingReturnFrame);
}

std::string TasManager::TakeLiveRecordingNotation(size_t maxChars) {
    std::string notation;
    size_t kept = 0;
    for (uint16_t packed : m_liveRecordingFrames) {
        const std::string token = FormatInput(packed);
        // Cutting mid-token would leave notation the composer rejects, so a frame either
        // fits whole or ends the string.
        if (notation.size() + token.size() > maxChars) {
            break;
        }
        notation += token;
        ++kept;
    }
    if (kept < m_liveRecordingFrames.size()) {
        m_status = L("The recording was longer than the text field holds and was cut short.");
    }
    m_liveRecordingFrames.clear();
    m_liveRecordingHasFrame = false;
    return notation;
}

void TasManager::SampleLiveRecordingFrame() {
    const unsigned int frame = *g_gameVals.pFrameCount;
    if (m_liveRecordingHasFrame && m_liveRecordingLastFrame == frame) {
        return;
    }
    m_liveRecordingLastFrame = frame;
    m_liveRecordingHasFrame = true;

    if (m_liveRecordingFrames.size() >= GetLiveRecordingFrameLimit()) {
        StopLiveRecording();
        m_status = L("Recording stopped at the frame limit.");
        return;
    }

    m_liveRecordingFrames.push_back(
        SanitizeRecordedInput(GetLastObservedBattleInputPacked(static_cast<uint32_t>(m_liveRecordingPlayer))));

    // Hold the other player to its own typed command, which is what a commit will give it.
    // Past the end of that command it goes neutral, matching how EditAndAdvanceFrames pads.
    const uint32_t other = m_liveRecordingPlayer == 0 ? 1u : 0u;
    const size_t index = m_liveRecordingFrames.size() - 1;
    const uint16_t otherPacked = index < m_liveRecordingOpponentFrames.size()
        ? m_liveRecordingOpponentFrames[index]
        : static_cast<uint16_t>(5);
    OverrideBattleInputPacked(other, otherPacked, 1);
}

void TasManager::ResumeGame() {
    FinishFidelityRun("resumed");
    ClearInputOverride();
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    m_runTarget = m_playhead;
    m_runState = m_movie.empty() ? TasRunState::Idle : TasRunState::PausedAtMovieFrame;
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
}

bool TasManager::SaveBaseSnapshot() {
    if (!m_active || !IsInTrainingMatch()) {
        SetError(L("Enter a training match and enable TAS mode first.").c_str());
        return false;
    }
    if (IsPreparingBase()) {
        SetError(L("A base capture is already running.").c_str());
        return false;
    }
    if (m_fidelityDiagnosticRun) {
        SetError(L("Stop the current run first.").c_str());
        return false;
    }
    ClearFidelityBaseCopies();
    m_projectBase = TasProjectFile::BasePair{};
    if (!m_snapshotOwner || !m_snapshotOwner->check_if_valid(
        g_interfaces.player1.GetData(), g_interfaces.player2.GetData())) {
        delete m_snapshotOwner;
        m_snapshotOwner = new SnapshotApparatus();
        // ReserveSlots falls back to sharing the whole ring when the reservation fails, which
        // would let every other feature write over both base states. Refusing the capture is
        // the only safe outcome: a base that another feature can overwrite is not a base.
        if (!m_snapshotOwner->ReserveSlots("tas_editor", kTasReservedSlots)) {
            delete m_snapshotOwner;
            m_snapshotOwner = nullptr;
            SetError(L("Could not reserve the TAS snapshot slots.").c_str());
            return false;
        }
        ClearKeyframes();
    }

    // A capture replaces both snapshots, so neither may be usable while it runs. Dropping
    // m_playbackBaseReady here is what stops the previous A from being presented as the new
    // one for the 60 frames the new capture is still running.
    m_playbackBaseReady = false;
    ClearInputOverride();
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    m_runState = TasRunState::WaitingBaseLeadInBoundary;
    // The click can land part-way through a game frame, so the boundary is waited for rather
    // than counted from here: that is what makes the lead-in exactly 60 frames.
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    m_error.clear();
    m_status = L("Saving the base state...");
    LOG(1, "[TAS] base capture requested frame=%u\n", GetCurrentFrame());
    // Fingerprint of the state the capture is about to start from. The lead-in runs 60 neutral
    // frames before slot 0 is written, so this is not literally what lands in the slot; what it
    // does give is the frame phase the capture was requested at, which is the value the import
    // anchor is derived from.
    TasLogSnapshotFingerprint("save-request");
    return true;
}

bool TasManager::LoadBaseSnapshot() {
    if (!m_active || !IsInTrainingMatch() || !HasBaseSnapshot()) {
        SetError(L("No native base state is available.").c_str());
        return false;
    }
    if (IsPreparingBase()) {
        SetError(L("Wait for the base state to finish saving.").c_str());
        return false;
    }
    if (m_liveRecording) {
        SetError(L("Stop live recording before restoring the base state.").c_str());
        return false;
    }

    // End any preview, seek, or presentation bookkeeping before loading. Otherwise the game
    // would be frozen at B while Update() still treats the old run as active.
    if (IsPlaybackRunning()) {
        StopPlayback(false);
    }
    ClearInputOverride();
    m_pendingKeyframeSlot = -1;
    m_pendingKeyframeFrame = 0;
    m_pendingKeyframeIsSection = false;
    m_presentationFramesRemaining = 0;
    m_baseLeadInRemaining = 0;
    m_capturePlaybackBaseAfterIncrement = false;
    m_finalizeBasePairAfterIncrement = false;
    m_finalizePresentationLeadInAfterIncrement = false;
    const unsigned int before = GetCurrentFrame();
    // State on the way in, so the pair of lines can be diffed: a field that changed across the
    // load is proof the native savestate carries it; a field that did not change is proof it
    // does not. The frame counter is the one exception - it is restored by the game and has to
    // differ.
    TasLogSnapshotFingerprint("restore-base-before");
    TasLogSlotDigest("restore-base-before", kTasBaseSlot);
    // A B restore is the other half of a repeat-run test, so it opens a cycle of its own.
    // The two runs of the same base are then distinguished by run id instead of by the
    // timestamps in the log, which say nothing about which restore produced which frames.
    ++m_fidelityRunId;
    LOG(1, "[TAS][VERIFY] restore-begin run=%u slot=B logical=%d base=%u playbackBase=%u\n",
        m_fidelityRunId, kTasBaseSlot, m_baseFrame, m_playbackBaseFrame);
    // Same substitution as the A path, for the editing base. An ordinary preview or a manual
    // "restore base" click still goes through the slot.
    if (!RestoreBaseForFidelityRun(
        m_fidelityDiagnosticRun ? m_fidelityRunSource : FidelityRestoreSource::Slot)) {
        SetError(L("Native base-state load failed.").c_str());
        return false;
    }
    TasLogSnapshotFingerprint("restore-base-after");
    // Loading does not rewrite the slot - the game reads it into the live match - so this digest
    // should match the one taken just above. If it does not, the load path is mutating the slot
    // and the next restore of the same base will not be the same state.
    TasLogSlotDigest("restore-base-after", kTasBaseSlot);

    // The slot has to hold the editing base, not merely "some" state: a base that was saved
    // before a re-save, or one another feature wrote over, would restore the wrong moment and
    // then look like a frame counter that simply failed to move. Logged on both sides of the
    // load so the next capture can be told apart from a load that silently did nothing.
    const unsigned int after = GetCurrentFrame();
    LOG(1, "[TAS] restore base slot=%d expectedB=%u frameBefore=%u frameAfter=%u\n",
        kTasBaseSlot, m_baseFrame, before, after);

    m_playhead = 0;
    m_runTarget = 0;
    m_runState = m_movie.empty() ? TasRunState::Idle : TasRunState::PausedAtMovieFrame;
    m_presentationMode = false;
    m_lastScheduledFrame = after;
    g_gameVals.isFrameFrozen = true;
    g_gameVals.framesToReach = after;
    m_error.clear();
    m_status = L("Restored the base state.");
    return true;
}

bool TasManager::LoadPlaybackBaseSnapshotAndLeadIn() {
    if (!m_active || !IsInTrainingMatch() || !HasBasePair()) {
        SetError(L("No base state is available.").c_str());
        return false;
    }
    if (IsPreparingBase()) {
        SetError(L("Wait for the base state to finish saving.").c_str());
        return false;
    }
    ClearInputOverride();
    m_presentationFramesRemaining = 0;
    // Diagnostic runs already own their ID from armed through end.
    if (!m_fidelityDiagnosticRun) {
        ++m_fidelityRunId;
    }
    // A is the half the actual movie run starts from, so it gets the same before/after
    // fingerprint as B. If A restores faithfully and the 60-frame lead-in then lands on a B
    // that does not match the B the editor sees, the lead-in itself is the divergence.
    LOG(1, "[TAS][VERIFY] restore-begin run=%u slot=A logical=%d base=%u playbackBase=%u\n",
        m_fidelityRunId, kTasPlaybackBaseSlot, m_baseFrame, m_playbackBaseFrame);
    TasLogSnapshotFingerprint("restore-baseA-before");
    TasLogSlotDigest("restore-baseA-before", kTasPlaybackBaseSlot);
    TasLogSlotDigest("restore-baseA-before-B", kTasBaseSlot);
    // Forward the exact source; A and B share validation, not playback timing.
    if (!RestoreBaseForFidelityRun(
        m_fidelityDiagnosticRun ? m_fidelityRunSource : FidelityRestoreSource::Slot,
        FidelityStartPoint::AWithLeadIn)) {
        SetError(L("Playback base-state load failed.").c_str());
        return false;
    }
    TasLogSnapshotFingerprint("restore-baseA-after");
    TasLogSlotDigest("restore-baseA-after", kTasPlaybackBaseSlot);

    // A is 60 frames before B, so the lead-in runs from here and lands exactly on B. That is
    // what puts movie frame 0 of the recording on the state the editor starts from.
    m_playhead = 0;
    m_runTarget = 0;
    m_runState = TasRunState::PresentationLeadIn;
    m_baseLeadInRemaining = kPresentationLeadInFrames;
    ScheduleNeutralFrame();
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    m_error.clear();
    LOG(1, "[TAS] presentation lead-in started A.frame=%u B.frame=%u frames=%u\n",
        m_playbackBaseFrame, m_baseFrame, kPresentationLeadInFrames);
    return true;
}

void TasManager::FinalizeBasePairCaptureAfterIncrement() {
    if (!m_capturePlaybackBaseAfterIncrement) {
        return;
    }
    m_capturePlaybackBaseAfterIncrement = false;
    if (!m_snapshotOwner || !m_snapshotOwner->save_snapshot_index(kTasPlaybackBaseSlot)) {
        AbortBaseLeadIn();
        SetError(L("Base-state save failed.").c_str());
        return;
    }
    // A is taken here, on the frame boundary, and the lead-in starts on the next full frame.
    // Its own frame number is not needed: everything downstream is measured from B. It is
    // recorded anyway because the pair check compares the two, and a mismatch means the
    // capture was interrupted rather than merely late.
    m_playbackBaseFrame = GetCurrentFrame();
    m_playbackSnapshotSize = m_snapshotOwner->get_last_saved_snapshot_size();
    m_projectBase = TasProjectFile::BasePair{};
    if (!CaptureProjectBase(kTasPlaybackBaseSlot, m_projectBase.a)) {
        LOG(0, "[TAS][PROJECT] A archive capture failed; native base remains available, project export disabled\n");
    }
    m_playbackBaseReady = true;
    m_baseLeadInRemaining = kPresentationLeadInFrames;
    LOG(1, "[TAS] playback base A ready frame=%u leadin=%u\n",
        m_playbackBaseFrame, kPresentationLeadInFrames);
    // A has just been written, so this is the state slot 1 actually holds. It is the value a
    // later restore of A has to reproduce: the "save-request" fingerprint is taken 60 frames
    // earlier and is only the phase the capture was requested at, so it must never be used as
    // the reference for what A contains.
    TasLogSlotDigest("capture-A-postinc", kTasPlaybackBaseSlot);
    TasLogSnapshotFingerprint("capture-A-postinc");
}

void TasManager::UpdateBaseLeadIn() {
    if (m_runState == TasRunState::WaitingBaseLeadInBoundary) {
        // Save A only after this already-started game frame completes. The next full frame
        // after A is the first of the 60 lead-in frames.
        m_runState = TasRunState::PreparingBaseLeadIn;
        m_capturePlaybackBaseAfterIncrement = true;
        ClearInputOverride();
        g_gameVals.isFrameFrozen = false;
        g_gameVals.framesToReach = 0;
        LOG(1, "[TAS] base capture armed for A at frame boundary frame=%u\n", GetCurrentFrame());
        return;
    }

    if (m_baseLeadInRemaining > 0) {
        --m_baseLeadInRemaining;
    }
    if (m_baseLeadInRemaining == 0) {
        // The B snapshot is written after the increment, so the frame it records is the one
        // the movie timeline will treat as frame 0.
        m_finalizeBasePairAfterIncrement = true;
        ClearInputOverride();
        LOG(1, "[TAS] base lead-in complete; B deferred after increment\n");
        return;
    }

    ScheduleNeutralFrame();
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
}

void TasManager::AbortBaseLeadIn() {
    m_projectBase = TasProjectFile::BasePair{};
    FinishFidelityRun("leadin-aborted");
    ClearInputOverride();
    m_baseLeadInRemaining = 0;
    m_capturePlaybackBaseAfterIncrement = false;
    m_finalizeBasePairAfterIncrement = false;
    m_finalizePresentationLeadInAfterIncrement = false;
    m_playbackBaseReady = false;
    m_runState = TasRunState::Idle;
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
}

void TasManager::FinalizeBasePairAfterIncrement() {
    if (!m_finalizeBasePairAfterIncrement) {
        return;
    }
    m_finalizeBasePairAfterIncrement = false;
    if (!m_snapshotOwner || !m_snapshotOwner->save_snapshot_index(kTasBaseSlot)) {
        AbortBaseLeadIn();
        SetError(L("Base-state save failed.").c_str());
        return;
    }

    // Read after the increment, so this is the same frame number the snapshot recorded. The
    // 60 lead-in frames are deliberately not part of it: they belong to the base pair, not to
    // the movie, which is what keeps editing and presentation on the same timeline.
    m_baseFrame = GetCurrentFrame();
    m_snapshotSize = m_snapshotOwner->get_last_saved_snapshot_size();
    ClearKeyframes();
    m_playhead = 0;
    m_runTarget = 0;
    m_lastScheduledFrame = m_baseFrame;
    m_runState = TasRunState::PausedAtMovieFrame;
    g_gameVals.isFrameFrozen = true;
    g_gameVals.framesToReach = m_baseFrame;

    if (!m_playbackBaseReady || m_playbackBaseFrame >= m_baseFrame ||
        m_baseFrame - m_playbackBaseFrame != kPresentationLeadInFrames) {
        // Without A exactly 60 frames before B, a presentation recording would start from a
        // state the editor never sees. Refusing the pair is the only honest outcome.
        m_playbackBaseReady = false;
        AbortBaseLeadIn();
        SetError(L("Base snapshots are not exactly 60 frames apart.").c_str());
        LOG(0, "[TAS] base pair rejected A.frame=%u B.frame=%u expectedLeadIn=%u\n",
            m_playbackBaseFrame, m_baseFrame, kPresentationLeadInFrames);
        return;
    }

    m_projectBase.ready = false;
    if (!m_projectBase.a.payload.empty() && CaptureProjectBase(kTasBaseSlot, m_projectBase.b)) {
        try {
            // Provenance only, not a cross-version compatibility certificate.
            std::ostringstream identity;
            identity << "schema=bbcfx86-native-evidence-v4 mod=tas-project-archive-4-runtime-evidence"
                << " moduleAddress=" << reinterpret_cast<uintptr_t>(GetBbcfBaseAdress())
                << " executableFingerprint=unknown restore=unsupported";
            m_projectBase.compatibility = identity.str();
            m_projectBase.ready = true;
        } catch (const std::exception&) { m_projectBase.ready = false; }
    }
    if (!m_projectBase.ready) m_projectBase = TasProjectFile::BasePair{};
    m_error.clear();
    m_status = m_projectBase.ready ? L("Base state saved.") :
        "Native base saved, but project archive capture failed; project export is unavailable.";
    LOG(1, "[TAS] base pair ready A.frame=%u B.frame=%u leadin=%u sizeA=%d sizeB=%d\n",
        m_playbackBaseFrame, m_baseFrame, kPresentationLeadInFrames,
        m_playbackSnapshotSize, m_snapshotSize);
    // Digest both halves of the pair at the moment they became usable. These are the values to
    // compare against a later restore of the same slots: identical digests mean the game
    // reproduces the slot byte-for-byte, different ones mean the capture itself is not stable.
    TasLogBasePairDigests("base-pair");
    // Same values under a name that says when they were taken: B is written after the frame
    // increment, so this is the state the editing base actually holds, and it is the sample a
    // lead-in landing on B has to match.
    TasLogSlotDigest("capture-B-postinc", kTasBaseSlot);
    TasLogSnapshotFingerprint("base-pair-ready");
}

void TasManager::FinalizePresentationLeadInAfterIncrement() {
    // Reuse the existing post-increment hook; no new naked-hook call site.
    if (m_fidelityStartDirectPlayback) {
        m_fidelityStartDirectPlayback = false;
        if (!m_active || !IsInTrainingMatch() || !m_fidelityDiagnosticRun) {
            FinishFidelityRun("cancelled");
            return;
        }
        if (!RestoreBaseForFidelityRun(m_fidelityRunSource)) {
            FinishFidelityRun("restore-failed");
            ClearInputOverride();
            m_runState = TasRunState::PausedAtMovieFrame;
            g_gameVals.isFrameFrozen = true;
            g_gameVals.framesToReach = GetCurrentFrame();
            return;
        }
        m_playhead = 0;
        StartMovieFrames();
        LOG(1, "[TAS][VERIFY] run=%u B-direct playback started phase=postinc gameFrame=%u\n",
            m_fidelityRunId, GetCurrentFrame());
        return;
    }
    if (!m_finalizePresentationLeadInAfterIncrement) {
        return;
    }
    m_finalizePresentationLeadInAfterIncrement = false;
    if (m_runState != TasRunState::PresentationLeadIn) {
        return;
    }
    // The lead-in must land exactly on B. If it did not, the recording would start from a
    // state the editor's movie was never authored against.
    const unsigned int frame = GetCurrentFrame();
    if (m_baseFrame == 0 || frame != m_baseFrame) {
        AbortBaseLeadIn();
        SetError(L("The presentation lead-in did not end on the editing base frame.").c_str());
        LOG(0, "[TAS] presentation lead-in mismatch B.frame=%u reached=%u expectedLeadIn=%u\n",
            m_baseFrame, frame, kPresentationLeadInFrames);
        return;
    }

    LOG(1, "[TAS] presentation lead-in ready A.frame=%u B.frame=%u movieFrame=0\n",
        m_playbackBaseFrame, m_baseFrame);
    m_playhead = 0;
    // The lead-in has just landed on B's frame number, so this is the state a presentation
    // replay actually starts from. Comparing it against "capture-B-postinc" answers the
    // question the frame number alone cannot: does restoring A and running the 60 neutral
    // frames land on the same state the editor starts from, or only on the same frame number?
    // Both samples are taken after a frame increment and before any movie input, so they are
    // the same phase of the frame and a difference between them is a real divergence.
    TasLogSnapshotFingerprint("leadin-B-postinc");
    StartMovieFrames();
}

bool TasManager::AddSection(size_t frame, const std::string& name) {
    if (!CanEditMovie() || name.empty() || frame > m_movie.size()) {
        return false;
    }
    const auto it = std::lower_bound(m_sections.begin(), m_sections.end(), frame,
        [](const TasSection& section, size_t value) { return section.frame < value; });
    if (it != m_sections.end() && it->frame == frame) {
        return false;
    }
    PushUndoState();
    m_sections.insert(it, TasSection{ frame, name });
    if (frame + 1 == m_playhead) {
        CaptureKeyframeIfDue();
    }
    return true;
}

bool TasManager::RemoveSection(size_t index) {
    if (!CanEditMovie() || index >= m_sections.size()) {
        return false;
    }
    PushUndoState();
    m_sections.erase(m_sections.begin() + static_cast<ptrdiff_t>(index));
    return true;
}

int TasManager::FindSectionAtOrBefore(size_t frame) const {
    int found = -1;
    for (size_t i = 0; i < m_sections.size(); ++i) {
        if (m_sections[i].frame > frame) {
            break;
        }
        found = static_cast<int>(i);
    }
    return found;
}

bool TasManager::CaptureProjectBase(int logicalSlot, TasProjectFile::Base& out) {
    // Called only immediately after the native save on the existing frame boundary.
    try {
        std::string evidence = "captureComplete=false reason=opt-in-disabled\n";
        char evidenceSwitch[4]{};
        if (GetEnvironmentVariableA("BBCF_TAS_CAPTURE_EVIDENCE", evidenceSwitch, sizeof(evidenceSwitch)) == 1 &&
            evidenceSwitch[0] == '1') {
            try {
                evidence = TasRuntimeEvidence::Capture(GetBbcfBaseAdress());
            } catch (const std::exception&) {
                evidence = "captureComplete=false reason=capture-exception\n";
            }
            // Preserve diagnostics even if an auxiliary archive capture fails below.
            LOG(1, "[TAS][EVIDENCE] begin logical=%d frame=%u bytes=%u\n", logicalSlot,
                GetCurrentFrame(), static_cast<unsigned int>(evidence.size()));
            std::istringstream lines(evidence);
            std::string line;
            while (std::getline(lines, line)) LOG(1, "[TAS][EVIDENCE] %s\n", line.c_str());
            LOG(1, "[TAS][EVIDENCE] end logical=%d\n", logicalSlot);
        }
        SnapshotApparatus::SlotBytes copy;
        if (!m_snapshotOwner || !m_snapshotOwner->CopyLogicalSlot(logicalSlot, &copy) ||
            copy.frame != GetCurrentFrame()) {
            LOG(1, "[TAS][PROJECT] capture failed logical=%d stage=main-payload-or-frame\n", logicalSlot);
            return false;
        }
        TasProjectFile::Base base;
        base.frame = copy.frame;
        base.firstSize = static_cast<uint32_t>(copy.firstSize);
        base.secondOffset = static_cast<uint32_t>(copy.secondOffset);
        base.secondSize = static_cast<uint32_t>(copy.secondSize);
        base.sourceManager = copy.sourceManager; base.sourceBuffer = copy.sourceBuffer;
        base.sourcePhysicalSlot = static_cast<uint32_t>(copy.sourcePhysicalSlot);
        base.sourceEpoch = copy.sourceEpoch; base.sourceSaveSerial = copy.sourceSaveSerial;
        base.descriptor = copy.descriptor;
        if (!m_snapshotOwner->CopyRestoreQueue(logicalSlot, copy, &base.restoreQueue)) {
            LOG(1, "[TAS][PROJECT] capture failed logical=%d stage=copy-queue\n", logicalSlot);
            return false;
        }
        base.restoreQueueCaptured = true;
        if (!m_snapshotOwner->CopyAuxiliaryState(logicalSlot, copy, &base.dependencies)) {
            LOG(1, "[TAS][PROJECT] capture failed logical=%d stage=auxiliary\n", logicalSlot);
            return false;
        }
        // Ensure the earlier queue capture still belongs to the same completed base.
        std::array<unsigned char, 0x400> queueAfter{};
        if (!m_snapshotOwner->CopyRestoreQueue(logicalSlot, copy, &queueAfter) || queueAfter != base.restoreQueue) {
            LOG(1, "[TAS][PROJECT] capture failed logical=%d stage=queue-recheck\n", logicalSlot);
            return false;
        }
        std::ostringstream details;
        details << "phase=post-increment frame=" << copy.frame << '\n';
        CharData* players[] = { g_interfaces.player1.GetData(), g_interfaces.player2.GetData() };
        for (int i = 0; i < 2; ++i) {
            const CharData* p = players[i];
            if (!p) return false;
            details << "P" << i + 1 << " address=" << static_cast<const void*>(p)
                << " character=" << p->charIndex << " facing=" << p->facingLeft
                << " x=" << p->position_x << " y=" << p->position_y
                << " hp=" << p->currentHP << " maxHP=" << p->maxHP
                << " heat=" << p->heatMeter << " barrier=" << p->barrier
                << " overdrive=" << p->overdriveMeter
                << " actionTime=" << p->actionTime << " actionTimeNoHitstop=" << p->actionTimeNoHitstop
                << " hitstop=" << p->hitstop << " hitstun=" << p->hitstun << " blockstun=" << p->blockstun
                << " actionHash=" << TasDigestName(p->currentAction, sizeof(p->currentAction))
                << " scriptLine=" << reinterpret_cast<uintptr_t>(p->nextScriptLineLocationInMemory) << '\n';
        }
        details << "rng=opaque-native-payload coverage=not-independently-measured\n";
        base.details = details.str();
        base.payload = std::move(copy.bytes);
        base.runtimeEvidence = std::move(evidence);
        out = std::move(base);
        return true;
    } catch (const std::exception&) { return false; }
}

bool TasManager::ExportProject(const std::string& path) {
    if (path.empty() || !m_active || !IsInTrainingMatch() || !m_projectBase.ready ||
        !HasBasePair() || IsPlaybackRunning() || m_liveRecording || m_fidelityDiagnosticRun) {
        SetError("Project export requires an idle TAS session with a captured base pair.");
        return false;
    }
    // Same-directory temporary file; failed writes must not truncate an older project.
    const std::string pending = path + ".pending";
    HANDLE reservation = CreateFileA(pending.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reservation == INVALID_HANDLE_VALUE) {
        SetError("Cannot reserve project temporary file; check path or an existing .pending file.");
        return false;
    }
    CloseHandle(reservation);
    std::string error;
    std::ofstream output(pending.c_str(), std::ios::binary | std::ios::trunc);
    bool ok = output && TasProjectFile::Write(output, m_projectBase, m_movie, m_sections, m_playhead, error);
    output.close();
    ok = ok && !output.fail();
    if (!ok) {
        DeleteFileA(pending.c_str());
        SetError(error.empty() ? "Could not finish writing the project." : error.c_str());
        return false;
    }
    if (!MoveFileExA(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SetError("Could not replace project file; completed archive remains in .pending.");
        return false;
    }
    m_error.clear();
    m_status = "Saved inputs and bound A/B capture archive. Independent base restoration is not implemented yet.";
    return true;
}

bool TasManager::ValidateProjectFile(const std::string& path) {
    // Deliberately no native calls or changes to the current movie/base, even on success.
    std::ifstream input(path.c_str(), std::ios::binary);
    TasProjectFile::Project project;
    std::string error;
    if (!input || !TasProjectFile::Read(input, project, error)) {
        SetError(error.empty() ? "Could not open the project." : error.c_str());
        return false;
    }
    m_error.clear();
    TasRestorePlan::Plan planA, planB;
    std::string issueA, issueB;
    const bool plannedA = TasRestorePlan::Analyze(project.base.a, planA, issueA);
    const bool plannedB = TasRestorePlan::Analyze(project.base.b, planB, issueB);
    m_status = plannedA && plannedB ?
        "Archive and second-stream jobs verified. First-stream call schema and runtime ownership unresolved; no restoration." :
        "Archive decoded; restore preflight unsupported: A=" + issueA + " B=" + issueB + ". Current session unchanged.";
    LOG(1, "[TAS][PROJECT] plan A=%d jobs=%u B=%d jobs=%u issueA=%s issueB=%s; nativeLoadAllowed=0\n",
        plannedA ? 1 : 0, static_cast<unsigned int>(planA.jobs.size()), plannedB ? 1 : 0,
        static_cast<unsigned int>(planB.jobs.size()), issueA.c_str(), issueB.c_str());
    LOG(1, "[TAS][PROJECT] callbackRequirements A=%u B=%u firstStreamSchemaAvailable=0 blockersA=%s blockersB=%s\n",
        static_cast<unsigned int>(planA.historyUses.size()), static_cast<unsigned int>(planB.historyUses.size()),
        planA.blockers.c_str(), planB.blockers.c_str());
    LOG(1, "[TAS][PROJECT] validated frames=%u A=%u B=%u bytesA=%u bytesB=%u; no native restore\n",
        static_cast<unsigned int>(project.movie.size()), project.base.a.frame, project.base.b.frame,
        static_cast<unsigned int>(project.base.a.payload.size()), static_cast<unsigned int>(project.base.b.payload.size()));
    return true;
}

bool TasManager::ExportMovie(const std::string& path, bool includeInitialConditions) {
    if (m_movie.empty() || path.empty()) {
        SetError(m_movie.empty() ? "There is no movie to export." : "Choose an export filename.");
        return false;
    }
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
    if (!output) {
        SetError(L("Could not open the TAS file for writing.").c_str());
        return false;
    }
    output << kTasMovieHeaderV2 << '\n';
    output << "frames " << m_movie.size() << '\n';
    if (includeInitialConditions) {
        output << "initial_conditions\n";
        if (m_projectBase.ready) {
            output << "# Saved editing base B, not the current live state:\n";
            std::istringstream details(m_projectBase.b.details);
            std::string detail;
            while (std::getline(details, detail)) output << "# " << detail << '\n';
        } else {
            output << "# Saved-base capture details unavailable.\n";
        }
        output << "base_frame " << m_baseFrame << '\n';
        output << "cursor " << m_playhead << '\n';
        output << "base_snapshot " << (HasBaseSnapshot() ? "available_current_process_only" : "not_saved") << '\n';
        output << "end_initial_conditions\n";
    }
    if (!m_sections.empty()) {
        output << "sections\n";
        for (const TasSection& section : m_sections) {
            output << "section " << section.frame << ' ' << section.name << '\n';
        }
        output << "end_sections\n";
    }
    output << "# Inputs use numpad notation: 7 8 9 / 4 5 6 / 1 2 3; suffixes A B C D are buttons, ap is the taunt button.\n";
    output << "# A frame with no direction written is neutral, so \"D\" means the same as \"5D\".\n";
    for (size_t i = 0; i < m_movie.size(); ++i) {
        output << i << " | P1=" << HumanInput(m_movie[i].p1)
               << " | P2=" << HumanInput(m_movie[i].p2) << '\n';
    }
    if (!output) {
        SetError(L("Failed while writing the TAS file.").c_str());
        return false;
    }
    m_error.clear();
    {
        char formatted[512];
        std::snprintf(formatted, sizeof(formatted), Messages.Exported_s(), path.c_str());
        m_status = formatted;
    }
    LOG(1, "[TAS] exported movie path=%s frames=%u\n", path.c_str(), static_cast<unsigned int>(m_movie.size()));
    return true;
}

bool TasManager::ImportMovie(const std::string& path) {
    if (!m_active || !IsInTrainingMatch() || !g_gameVals.pFrameCount) {
        SetError("Enter a training match and enable TAS mode before importing.");
        return false;
    }
    if (IsPreparingBase() || IsPlaybackRunning() || IsSeeking() || m_liveRecording ||
        m_fidelityDiagnosticRun || m_fidelityStartDirectPlayback) {
        SetError("Stop the current TAS operation before importing.");
        return false;
    }
    std::ifstream input(path.c_str());
    if (!input) {
        SetError(L("Could not open the selected TAS file.").c_str());
        return false;
    }
    std::string header;
    if (!std::getline(input, header)) {
        SetError(L("The TAS file is empty.").c_str());
        return false;
    }
    if (!header.empty() && header.back() == '\r') header.pop_back();
    std::string framesLabel;
    size_t declaredCount = 0;
    if (header == kTasMovieHeaderV1) {
        if (!(input >> framesLabel >> declaredCount) || framesLabel != "frames" || declaredCount == 0) {
            SetError(L("The V1 TAS file has an invalid frame count.").c_str());
            return false;
        }
    } else if (header == kTasMovieHeaderV2) {
        if (!(input >> framesLabel >> declaredCount) || framesLabel != "frames" || declaredCount == 0) {
            SetError(L("The V2 TAS file has an invalid frame count.").c_str());
            return false;
        }
        std::string line;
        std::vector<TasSection> importedSections;
        unsigned int importedBaseFrame = 0;
        size_t importedCursor = 0;
        bool hasImportedBaseFrame = false;
        bool hasImportedCursor = false;
        bool invalidImportedInitialCondition = false;
        std::getline(input, line);
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line == "initial_conditions") {
                while (std::getline(input, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line == "end_initial_conditions") break;
                    std::istringstream condition(line);
                    std::string key;
                    condition >> key;
                    if (key == "base_frame") {
                        unsigned long long value = 0;
                        if (!(condition >> value) || value > UINT_MAX) {
                            invalidImportedInitialCondition = true;
                        } else {
                            importedBaseFrame = static_cast<unsigned int>(value);
                            hasImportedBaseFrame = true;
                        }
                    } else if (key == "cursor") {
                        unsigned long long value = 0;
                        if (!(condition >> value) || value > (std::numeric_limits<size_t>::max)()) {
                            invalidImportedInitialCondition = true;
                        } else {
                            importedCursor = static_cast<size_t>(value);
                            hasImportedCursor = true;
                        }
                    }
                }
                if (invalidImportedInitialCondition) {
                    SetError("The V2 TAS file contains invalid initial conditions.");
                    return false;
                }
                continue;
            }
            if (line == "sections") {
                while (std::getline(input, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line == "end_sections") break;
                    if (line.rfind("section ", 0) != 0) continue;
                    std::istringstream sectionStream(line.substr(8));
                    size_t frame = 0;
                    if (!(sectionStream >> frame)) continue;
                    std::string name;
                    std::getline(sectionStream, name);
                    if (!name.empty() && name.front() == ' ') name.erase(0, 1);
                    if (!name.empty() && frame <= declaredCount) {
                        importedSections.push_back(TasSection{ frame, name });
                    }
                }
                continue;
            }
            break;
        }
        std::stable_sort(importedSections.begin(), importedSections.end(), [](const TasSection& a, const TasSection& b) {
            return a.frame < b.frame;
        });
        for (size_t i = 1; i < importedSections.size(); ++i) {
            if (importedSections[i - 1].frame == importedSections[i].frame) {
                SetError(L("The V2 TAS file contains duplicate sections.").c_str());
                return false;
            }
        }
        if (input && !line.empty() && line[0] != '#') {
            std::vector<TasFrameInput> imported;
            for (size_t expected = 0; expected < declaredCount; ++expected) {
                if (expected != 0 && !std::getline(input, line)) {
                    SetError(L("The V2 TAS file is missing frame data.").c_str());
                    return false;
                }
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t firstBar = line.find(" | P1=");
                const size_t secondBar = line.find(" | P2=", firstBar == std::string::npos ? 0 : firstBar + 6);
                if (firstBar == std::string::npos || secondBar == std::string::npos) {
                    SetError(L("The V2 TAS file contains an invalid frame.").c_str());
                    return false;
                }
                std::istringstream indexStream(line.substr(0, firstBar));
                size_t index = 0;
                if (!(indexStream >> index) || index != expected) {
                    SetError(L("The V2 TAS file contains an invalid frame index.").c_str());
                    return false;
                }
                const std::string p1Text = line.substr(firstBar + 6, secondBar - (firstBar + 6));
                const std::string p2Text = line.substr(secondBar + 6);
                uint16_t p1 = 0, p2 = 0;
                if (!ParseHumanInput(p1Text, &p1) || !ParseHumanInput(p2Text, &p2)) {
                    SetError(L("The V2 TAS file contains an invalid input.").c_str());
                    return false;
                }
                imported.push_back(TasFrameInput{p1, p2});
            }
            if (imported.size() != declaredCount) {
                SetError(L("The V2 TAS file is missing frame data.").c_str());
                return false;
            }
            // File base_frame/cursor describe the source run; neither restores game state.
            ClearInputOverride();
            ClearSnapshot();
            m_movie.swap(imported);
            m_sections.swap(importedSections);
            m_undoStack.clear();
            m_redoStack.clear();
            m_commandFrames.clear();
            m_commandCursor = 0;
            m_inputsParsed = false;
            m_playhead = 0;
            m_runTarget = 0;
            m_presentationFramesRemaining = 0;
            m_presentationMode = false;
            m_runState = TasRunState::Idle;
            // No native state was restored. A future Save base captures the current match,
            // not the file's base_frame; its cursor cannot be resumed yet either.
            m_error.clear();
            m_status = "Imported inputs only; source base state was not restored. Save a new base before playback.";
            LOG(1, "[TAS] imported V2 inputs path=%s frames=%u fileBase=%u hasBase=%d fileCursor=%u hasCursor=%d; base not restored\n",
                path.c_str(), static_cast<unsigned int>(m_movie.size()), importedBaseFrame,
                hasImportedBaseFrame ? 1 : 0, static_cast<unsigned int>(importedCursor), hasImportedCursor ? 1 : 0);
            return true;
        }
        SetError(L("The V2 TAS file contains no frame data.").c_str());
        return false;
    } else {
        SetError(L("The TAS file has an unknown format.").c_str());
        return false;
    }
    std::vector<TasFrameInput> imported;
    imported.reserve(declaredCount);
    for (size_t expectedIndex = 0; expectedIndex < declaredCount; ++expectedIndex) {
        size_t index = 0; unsigned int p1 = 0, p2 = 0;
        if (!(input >> index >> p1 >> p2) || index != expectedIndex || p1 > UINT16_MAX || p2 > UINT16_MAX ||
            (p1 & 0xF) < 1 || (p1 & 0xF) > 9 || (p2 & 0xF) < 1 || (p2 & 0xF) > 9) {
            SetError(L("The V1 TAS file contains an invalid frame.").c_str()); return false;
        }
        imported.push_back(TasFrameInput{static_cast<uint16_t>(p1), static_cast<uint16_t>(p2)});
    }
    ClearInputOverride(); ClearSnapshot(); m_movie.swap(imported); m_sections.clear();
    m_undoStack.clear(); m_redoStack.clear();
    m_commandFrames.clear(); m_commandCursor = 0; m_inputsParsed = false;
    m_playhead = 0; m_runTarget = 0; m_presentationFramesRemaining = 0;
    m_presentationMode = false; m_runState = TasRunState::Idle;
    m_error.clear();
    m_status = "Imported inputs only; source base state was not restored. Save a new base before playback.";
    LOG(1, "[TAS] imported movie path=%s frames=%u\n", path.c_str(), static_cast<unsigned int>(m_movie.size()));
    return true;
}

bool TasManager::TryParseCommand(const std::string& text, std::vector<uint16_t>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    uint16_t pendingButtons = 0;
    bool sawDirection = false;

    // A command that opens with a button has no direction written in front of it, which is how
    // people actually write a button-only frame: "D", "ABCD", "ap" for the taunt. Neutral is what
    // the game reads when no direction is held, so an implied 5 is both the obvious reading and
    // the correct one. Without this the parser rejected the whole command.
    const auto startImpliedNeutralFrame = [&]() {
        if (!sawDirection || out->empty()) {
            out->push_back(5);
            sawDirection = true;
        }
    };

    for (size_t i = 0; i < text.size(); ) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
        if (ch >= '1' && ch <= '9') {
            if (pendingButtons && !out->empty()) {
                out->back() = static_cast<uint16_t>(out->back() + pendingButtons);
                pendingButtons = 0;
            }
            out->push_back(static_cast<uint16_t>(ch - '0'));
            sawDirection = true;
            ++i;
        } else if (ch == 'A' && i + 1 < text.size() &&
                   static_cast<char>(std::toupper(static_cast<unsigned char>(text[i + 1]))) == 'P') {
            // "ap" = taunt button attached to the current frame. Checked before the single
            // 'A' branch so 5ap does not get parsed as "5A" followed by an invalid 'p'.
            if (pendingButtons & kInputButtonTaunt) {
                return false;
            }
            startImpliedNeutralFrame();
            pendingButtons = static_cast<uint16_t>(pendingButtons + kInputButtonTaunt);
            i += 2;
        } else if (ch == 'A' || ch == 'B' || ch == 'C' || ch == 'D') {
            startImpliedNeutralFrame();
            pendingButtons = static_cast<uint16_t>(pendingButtons + ButtonValue(ch));
            ++i;
        } else if (ch != ' ' && ch != ',' && ch != '-') {
            return false;
        } else {
            ++i;
        }
    }
    if (pendingButtons && !out->empty()) {
        out->back() = static_cast<uint16_t>(out->back() + pendingButtons);
    }
    return !out->empty();
}

bool TasManager::ParseInputs() {
    std::vector<uint16_t> p1;
    std::vector<uint16_t> p2;
    if (!TryParseCommand(m_p1Text, &p1)) {
        SetError(L("Invalid P1 input. Use examples such as 5C, 28D, 623C, 656, 5ap, or D on its own.").c_str());
        return false;
    }
    if (!TryParseCommand(m_p2Text, &p2)) {
        SetError(L("Invalid P2 input. Use examples such as 5C, 28D, 623C, 656, 5ap, or D on its own.").c_str());
        return false;
    }

    const size_t frameCount = p1.size() > p2.size() ? p1.size() : p2.size();
    m_commandFrames.assign(frameCount, TasFrameInput{});
    for (size_t i = 0; i < frameCount; ++i) {
        m_commandFrames[i].p1 = i < p1.size() ? p1[i] : 5;
        m_commandFrames[i].p2 = i < p2.size() ? p2[i] : 5;
    }
    m_commandCursor = 0;
    m_inputsParsed = true;
    m_error.clear();
    return true;
}

bool TasManager::SetInputText(const std::string& p1, const std::string& p2) {
    m_p1Text = p1;
    m_p2Text = p2;
    return ParseInputs();
}

std::string TasManager::FormatInput(uint16_t packed) {
    std::string result(1, static_cast<char>('0' + (packed & 0x0F)));
    if (packed & 16) result += 'A';
    if (packed & 32) result += 'B';
    if (packed & 64) result += 'C';
    if (packed & 128) result += 'D';
    if (packed & kInputButtonTaunt) result += "ap";
    return result;
}

TasFrameInput TasManager::GetMovieFrame(size_t index) const {
    return index < m_movie.size() ? m_movie[index] : TasFrameInput{};
}

TasFrameInput TasManager::GetCommandInput() const {
    if (m_commandCursor >= m_commandFrames.size()) {
        return TasFrameInput{};
    }
    return m_commandFrames[m_commandCursor];
}

TasFrameInput TasManager::GetMovieInput() const {
    return m_playhead < m_movie.size() ? m_movie[m_playhead] : TasFrameInput{};
}

TasFrameInput TasManager::GetCurrentInput() const {
    return IsEditingRecording() || IsPlaying() || m_runState == TasRunState::ReplayingMovie
        ? GetMovieInput() : GetCommandInput();
}

TasFrameInput TasManager::GetCurrentPlaybackInput() const {
    return m_hasScheduledInput ? m_scheduledInput : GetMovieInput();
}

TasFrameInput TasManager::GetLastRecordedInput() const {
    return m_movie.empty() ? TasFrameInput{} : m_movie.back();
}

unsigned int TasManager::GetCurrentFrame() const {
    return g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0;
}