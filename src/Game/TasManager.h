#pragma once

#include "Game/SnapshotApparatus/SnapshotApparatus.h"

#include <cstdint>
#include <string>
#include <vector>

#include "Game/TasProjectFile.h"

// --- snapshot-copy verification ------------------------------------------------------------
// PrivateBytes stays disabled. CopyToOriginalSlot is a restricted diagnostic, not
// independent private-byte loading.
enum class FidelityRestoreSource {
    Slot,
    PrivateBytes,
    CopyToOriginalSlot
};

// Which base a run starts from. A is played through the 60-frame lead-in, so the movie begins
// on B; B is entered directly and the movie begins on the frame the editor is already on.
enum class FidelityStartPoint {
    AWithLeadIn,
    BDirect
};

enum class TasRunState {
    Idle,
    PausedAtMovieFrame,
    // The base capture is a two-snapshot operation: the playback base A is taken where the
    // editor clicked, then 60 neutral frames run, then the editing base B is taken. These two
    // states exist so the 60 frames are exactly 60 and not 59 or 61, which is what happens if
    // the click is counted from part-way through a frame.
    //
    // WaitingBaseLeadInBoundary owns nothing: it is entered from a UI click that can land
    // mid-frame, and it only waits for the next frame boundary to arm the capture. The lead-in
    // proper then runs in PreparingBaseLeadIn.
    WaitingBaseLeadInBoundary,
    PreparingBaseLeadIn,
    PresentationLeadIn,
    PlayingMovie,
    PresentationLeadOut,
    ReplayingMovie
};

class TasManager {
public:
    static TasManager& Instance();

    bool IsActive() const { return m_active; }
    bool HasBaseSnapshot() const { return m_snapshotOwner != nullptr && m_snapshotOwner->snapshot_count > 0 && m_snapshotSize > 0; }
    // Both snapshots of the pair are needed before anything can run: A is where presentation
    // playback starts, B is where editing and seeking start. A capture that only got as far as
    // A must not look like a usable base.
    bool HasBasePair() const { return HasBaseSnapshot() && m_playbackBaseReady; }
    bool IsPreparingBase() const {
        return m_runState == TasRunState::WaitingBaseLeadInBoundary ||
            m_runState == TasRunState::PreparingBaseLeadIn;
    }
    bool IsInTrainingMatch() const;
    // --- live recording ----------------------------------------------------------------
    // Hands one player back to whoever is holding the controller and samples what they do,
    // one packed input per game frame. Nothing is written to the movie: the capture comes
    // back as notation for the composer's text field, so a played sequence becomes an
    // ordinary typed command the user can edit and commit (or throw away) as usual.
    // The other player is held to opponentFrames (its own text field, already parsed, and
    // neutral once it runs out) because that is exactly what a commit will replay for it.
    // Holding it to anything else - the movie's current contents included - would record
    // the capture against an opponent it never plays back against.
    bool StartLiveRecording(int player, std::vector<uint16_t> opponentFrames);
    void StopLiveRecording();
    bool IsLiveRecording() const { return m_liveRecording; }
    // 0 or 1 while recording, -1 otherwise.
    int GetLiveRecordingPlayer() const { return m_liveRecording ? m_liveRecordingPlayer : -1; }
    size_t GetLiveRecordingFrameCount() const { return m_liveRecordingFrames.size(); }
    // Capped so a recording left running cannot outgrow the composer's text field.
    static size_t GetLiveRecordingFrameLimit();
    // The finished capture as numpad notation, empty if nothing was recorded. Truncates on
    // a frame boundary if it will not fit in maxChars (and says so in the status), so the
    // caller always gets something the composer can parse. Clears the capture.
    std::string TakeLiveRecordingNotation(size_t maxChars);

    bool IsRecording() const { return false; }
    bool HasRecording() const { return !m_movie.empty(); }
    bool IsPlaying() const {
        return m_runState == TasRunState::PresentationLeadIn ||
            m_runState == TasRunState::PlayingMovie ||
            m_runState == TasRunState::PresentationLeadOut;
    }
    bool IsPlaybackRunning() const {
        return m_fidelityStartDirectPlayback || IsPlaying() ||
            m_runState == TasRunState::ReplayingMovie || IsPreparingBase();
    }
    bool IsEditingRecording() const { return m_runState == TasRunState::PausedAtMovieFrame && !m_movie.empty(); }
    bool IsPlaybackUiHidden() const { return m_playbackUiHidden; }
    size_t GetRecordedFrameCount() const { return m_movie.size(); }

    void Enter();
    void Exit();
    void Update();

    // --- base-state fidelity diagnostics ------------------------------------------------
    // Read-only view of the base bookkeeping, for the save/load fingerprints. Exposed as
    // accessors rather than making the logging helpers friends so the diagnostic code cannot
    // accidentally become a second writer of TAS state.
    struct BaseDiagnostics {
        SnapshotApparatus* snapshotOwner = nullptr;
        unsigned int snapshotCount = 0;
        unsigned int baseFrame = 0;
        unsigned int playbackBaseFrame = 0;
        bool playbackBaseReady = false;
        size_t playhead = 0;
        int runState = 0;
        int lastSavedLogicalSlot = -1;
        int lastSavedPhysicalSlot = -1;
        int lastSavedSnapshotSize = 0;
    };
    BaseDiagnostics GetBaseDiagnostics() const;
    // Hashes the game's saved-state slot backing one of this manager's logical slots.
    // `hashedBytes` receives how much of the buffer was read. Returns 0 when the slot is empty.
    uint32_t DigestLogicalSlot(int logicalSlot, size_t* hashedBytes = nullptr) const;

    // --- snapshot-copy verification entry points ----------------------------------------
    // Read-only copies of the current base pair for byte inspection. Never passed to
    // the native loader. Re-save, import and match exit invalidate the pair.
    struct FidelityBaseCopies {
        SnapshotApparatus::SlotBytes baseA;
        SnapshotApparatus::SlotBytes baseB;
        unsigned int frameA = 0;
        unsigned int frameB = 0;
        // Bumped on every successful copy so a run header can name the exact pair it used; a
        // second copy of the same match is otherwise indistinguishable from the first.
        uint64_t generation = 0;
        bool ready = false;
    };

    // Copies logical slots 1 (A) and 0 (B) into memory. Both must succeed and A must sit exactly
    // kPresentationLeadInFrames before B, matching the pair rule the capture already enforces.
    // A failed capture never publishes a partial pair; any previous valid pair is retained.
    bool CaptureFidelityBaseCopies();
    // Drops read-only copies without changing a native-slot run's lifecycle.
    void ClearFidelityBaseCopies();
    bool HasFidelityBaseCopies() const { return m_fidelityCopies.ready; }
    const FidelityBaseCopies& GetFidelityBaseCopies() const { return m_fidelityCopies; }

    // Native-slot baseline or restricted original-slot writeback. PrivateBytes stays rejected.
    // A uses the lead-in; B restores and starts at the existing post-increment boundary.
    bool StartFidelityRun(FidelityRestoreSource source, FidelityStartPoint start);
    // Which source the run in progress is fed from, for the sampling gate and the run header.
    bool IsFidelityDiagnosticRun() const { return m_fidelityDiagnosticRun; }

    void StartPlayback(bool presentationMode);
    void StopPlayback(bool completed = false);
    void EditAndAdvanceFrames(int count);
    void ResetParsedInputs();
    void ResetMovie();
    // --- structural edits -------------------------------------------------------------
    // The movie is just a list, so it can be edited anywhere, not only at the playhead.
    // Editing at or after the playhead costs nothing; editing behind it re-seeks so what is
    // on screen still matches the list. All of them are undoable and refuse to run mid-run.
    bool InsertNeutralFrames(size_t index, size_t count);
    bool DeleteFrames(size_t index, size_t count);
    // outNewIndex receives where the block ended up, so a caller can keep it selected.
    bool MoveFrames(size_t fromIndex, size_t count, size_t toIndex, size_t* outNewIndex = nullptr);
    bool SetFrameInput(size_t index, TasFrameInput input);
    bool DuplicateFrames(size_t index, size_t count);

    bool CanUndo() const { return !m_undoStack.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }
    bool Undo();
    bool Redo();

    bool ExportMovie(const std::string& path, bool includeInitialConditions);
    bool ImportMovie(const std::string& path);
    // Capture archive stage: file validation is NOT native restoration.
    bool ExportProject(const std::string& path);
    bool ValidateProjectFile(const std::string& path);
    bool HasProjectBase() const { return m_projectBase.ready; }

    bool SaveBaseSnapshot();
    bool LoadBaseSnapshot();
    // Restores playback base A, then runs the 60 neutral lead-in frames, leaving the match on
    // editing base B with the playhead at movie frame 0. This is how presentation playback
    // starts: the lead-in is what puts movie frame 0 on the same game state for both the
    // editor (which starts from B) and the recording (which starts from A).
    bool LoadPlaybackBaseSnapshotAndLeadIn();
    // Completes the two base captures after the game's frame counter increment. Each saved
    // state has to be taken on the same side of the increment as the frame number it records,
    // otherwise the stored frame and the live frame disagree by one.
    void FinalizeBasePairCaptureAfterIncrement();
    void FinalizeBasePairAfterIncrement();
    void FinalizeKeyframeAfterIncrement();
    void FinalizePresentationLeadInAfterIncrement();
    // How many keyframes are currently held, for the UI to show why seeking is fast or slow.
    int GetKeyframeCount() const;
    bool SetInputText(const std::string& p1, const std::string& p2);
    bool AdvanceOneFrame();
    bool AdvanceFrames(int count);
    void ResumeGame();

    // Moves the match to a frame of the movie without changing it: reloads the base state
    // and re-simulates the stored input up to that point. This is how the transport bar
    // scrubs. Nothing is deleted until the user actually commits new input there.
    bool SeekToFrame(size_t targetFrame);
    // Seek relative to the playhead. Negative goes back. Forward past the end of the movie
    // extends it with neutral frames rather than stopping, so stepping forward always does
    // something - overshooting the end by 5 simply appends 5 idle frames.
    bool SeekRelative(int delta);
    // Move forward `count` frames, appending neutral frames if that runs off the end.
    // Never destroys anything: frames between here and the target are replayed as they are.
    bool AdvanceOrExtend(int count);
    // True while a seek or a commit is still re-simulating its way to the target.
    bool IsSeeking() const { return m_runState == TasRunState::ReplayingMovie; }
    size_t GetRunTarget() const { return m_runTarget; }

    // Parses one player's numpad-notation command into one packed input per frame,
    // without touching manager state, so the UI can validate as the user types.
    static bool TryParseCommand(const std::string& text, std::vector<uint16_t>* outFrames);
    // Renders one packed input back into numpad notation ("5", "3C", "2AB").
    static std::string FormatInput(uint16_t packed);

    // Committed movie frames, for drawing the timeline. Out-of-range reads give neutral.
    TasFrameInput GetMovieFrame(size_t index) const;
    // Frames waiting in the parsed command queue, and how many have been consumed.
    size_t GetQueuedFrameCount() const { return m_commandFrames.size(); }
    size_t GetQueueCursor() const { return m_commandCursor; }
    TasRunState GetRunState() const { return m_runState; }

    TasFrameInput GetCurrentPlaybackInput() const;
    TasFrameInput GetLastRecordedInput() const;
    TasFrameInput GetCurrentInput() const;
    TasFrameInput GetCommandInput() const;
    TasFrameInput GetMovieInput() const;

    bool IsAutoLoadAfterPlayback() const { return m_autoLoadAfterPlayback; }
    void SetAutoLoadAfterPlayback(bool enabled) { m_autoLoadAfterPlayback = enabled; }
    unsigned int GetRerecordCount() const { return m_rerecordCount; }

    const std::string& GetP1Text() const { return m_p1Text; }
    const std::string& GetP2Text() const { return m_p2Text; }
    void SetP1Text(const std::string& value) { m_p1Text = value; m_inputsParsed = false; }
    void SetP2Text(const std::string& value) { m_p2Text = value; m_inputsParsed = false; }

    const std::string& GetError() const { return m_error; }
    const std::string& GetStatus() const { return m_status; }
    unsigned int GetBaseFrame() const { return m_baseFrame; }
    unsigned int GetCurrentFrame() const;
    size_t GetCursor() const { return m_playhead; }
    size_t GetFrameCount() const { return m_movie.size(); }

    const std::vector<TasSection>& GetSections() const { return m_sections; }
    bool AddSection(size_t frame, const std::string& name);
    bool RemoveSection(size_t index);
    int FindSectionAtOrBefore(size_t frame) const;

private:
    TasManager() = default;
    ~TasManager();
    TasManager(const TasManager&) = delete;
    TasManager& operator=(const TasManager&) = delete;

    bool ParseInputs();
    // Snapshot the movie before a structural edit so it can be stepped back through.
    void PushUndoState();
    // Bring the match back in line with the movie after frames at or before the playhead
    // changed underneath it.
    void ResyncAfterEdit(size_t firstChangedFrame);
    bool CanEditMovie() const;
    bool BeginMovieRun(TasRunState state, size_t target);
    void ScheduleMovieFrame();
    // One line per movie frame of the state that frame consumed its input into, plus the
    // input TAS asked for. Logged before the playhead moves, so the frames of two runs can be
    // lined up by movie index instead of by wall-clock time. Read-only.
    void LogFidelityFrameBeforeIncrement(unsigned int gameFrame);
    void FinishMovieRun(bool completed);
    void ClearInputOverride();
    void ClearSnapshot();
    bool CaptureProjectBase(int logicalSlot, TasProjectFile::Base& out);
    TasProjectFile::BasePair m_projectBase; // independent archive, bound at A/B capture
    // Keyframes are savestates taken part-way through the movie so a seek can restart from
    // the nearest one instead of replaying from the base every time. They live in the slots
    // this manager reserved; there is no per-frame state, which would need 10 MiB a frame.
    void ClearKeyframes();
    void CaptureKeyframeIfDue(bool deferUntilFrameIncrement = false);
    void InvalidateKeyframesAfter(size_t frame);
    void InvalidateSectionCheckpointsBefore(size_t frame);
    // Best starting point for a seek: the latest keyframe at or before the target.
    // Returns -1 when the base state is the best we have.
    int FindKeyframeFor(size_t targetFrame) const;
    void SetError(const char* message);
    void StartMovieFrames();
    void StartPresentationLeadOut();
    void ScheduleNeutralFrame();
    // Per-frame step of the 60 hidden neutral frames between playback base A and editing base
    // B. The lead-in owns those frames entirely: it is not a movie run and must not reach any
    // of the playback bookkeeping.
    void UpdateBaseLeadIn();
    // Leaves the lead-in without writing B, for an abandoned or failed capture.
    void AbortBaseLeadIn();
    // Snapshot-copy verification internals. Each restores one half of the pair from the source
    // the run was started with and refuses rather than falling back to the other source.
    bool RestoreBaseForFidelityRun(FidelityRestoreSource source,
        FidelityStartPoint start = FidelityStartPoint::BDirect);
    // Ends the run's trace identity: writes the closing line with the frames it actually
    // sampled and drops the diagnostic flag, so a later preview is not mistaken for a test.
    void FinishFidelityRun(const char* reason);

    void SampleLiveRecordingFrame();

    bool m_active = false;
    bool m_liveRecording = false;
    int m_liveRecordingPlayer = 0;
    // Where the playhead sat when recording began: the match runs on while the user plays,
    // so stopping seeks back here and leaves the editor exactly where they left it.
    size_t m_liveRecordingReturnFrame = 0;
    bool m_liveRecordingHasFrame = false;
    unsigned int m_liveRecordingLastFrame = 0;
    std::vector<uint16_t> m_liveRecordingFrames;
    std::vector<uint16_t> m_liveRecordingOpponentFrames;
    bool m_p2KeyboardOverrideWasEnabled = false;
    bool m_frameHistoryOpenedByTas = false;
    SnapshotApparatus* m_snapshotOwner = nullptr;

    // Logical slots 0/1 hold B/A; keyframes use only slots 2..5.
    struct Keyframe {
        size_t movieFrame = 0;
        bool valid = false;
        // Cache metadata only; the section itself remains a frame-attached timeline marker.
        bool sectionCheckpoint = false;
    };
    std::vector<Keyframe> m_keyframes;
    size_t m_nextKeyframeSlot = 0;
    int m_pendingKeyframeSlot = -1;
    size_t m_pendingKeyframeFrame = 0;
    bool m_pendingKeyframeIsSection = false;
    Snapshot* m_snapshotBuffer = nullptr;
    int m_snapshotSize = 0;
    // Editing base B: where the editor, seeks and every movie playback begin. This is the
    // frame number `base_frame` writes to the file, because it is the one the timeline is
    // measured from.
    unsigned int m_baseFrame = 0;
    // Playback base A is 60 frames before B. Presentation playback restores A,
    // then runs the neutral lead-in so movie frame 0 lands on B.
    unsigned int m_playbackBaseFrame = 0;
    int m_playbackSnapshotSize = 0;
    bool m_playbackBaseReady = false;
    unsigned int m_rerecordCount = 0;

    TasRunState m_runState = TasRunState::Idle;
    std::vector<TasFrameInput> m_movie;
    std::vector<TasSection> m_sections;

    // A movie is a few bytes a frame, so undo just keeps whole copies rather than a diff.
    struct MovieState {
        std::vector<TasFrameInput> movie;
        std::vector<TasSection> sections;
        size_t playhead = 0;
    };
    std::vector<MovieState> m_undoStack;
    std::vector<MovieState> m_redoStack;
    size_t m_playhead = 0;
    size_t m_runTarget = 0;
    unsigned int m_presentationFramesRemaining = 0;
    bool m_presentationMode = false;
    // The 60 neutral frames between A and B. Counted down from the frame boundary the capture
    // was armed on; the B snapshot is written when it reaches zero.
    unsigned int m_baseLeadInRemaining = 0;
    // Set on the last lead-in frame and consumed by the frame-counter hook after it increments
    // the counter, so the stored frame number and the live frame number are the same value.
    bool m_capturePlaybackBaseAfterIncrement = false;
    bool m_finalizeBasePairAfterIncrement = false;
    // Set when the presentation lead-in on A runs out; the movie start is deferred to after
    // the increment so movie frame 0 begins on a frame boundary.
    bool m_finalizePresentationLeadInAfterIncrement = false;
    TasFrameInput m_scheduledInput{};
    bool m_hasScheduledInput = false;
    unsigned int m_lastScheduledFrame = 0;

    bool m_inputsParsed = false;
    std::vector<TasFrameInput> m_commandFrames;
    size_t m_commandCursor = 0;
    // --- repeat-run fidelity trace ----------------------------------------------------
    // The trace is on while base save/restore fidelity is being verified. It only covers
    // presentation playback (see the gate in LogFidelityFrameBeforeIncrement), so ordinary
    // editing is unaffected; turn it off once the fidelity question is settled, because the
    // per-frame lines dominate the log at about six lines a frame.
    bool m_fidelityTraceEnabled = true;
    // Which playback run the trace lines belong to. Incremented when a movie run starts, so
    // two runs of the same base are told apart by the log alone instead of by time stamps.
    unsigned int m_fidelityRunId = 0;
    // The frame index a run was started from, for the run header. Diagnostic only.
    size_t m_fidelityRunStartCursor = 0;
    // Whether the run started from the playback base (a presentation lead-in) or from
    // whatever the cursor was on. A run resumed mid-movie is not a base restore test and
    // must not be compared against one.
    bool m_fidelityRunFromBase = false;
    // --- snapshot-copy verification state ---------------------------------------------
    // The one pair of copies this session is testing against, and how it is being fed.
    FidelityBaseCopies m_fidelityCopies;
    // True from the moment a diagnostic run arms until it ends. It does two things: it lets the
    // per-frame trace cover a B-direct run (which is not a presentation and would otherwise go
    // unsampled), and it marks the run as a test whose header and end line must both appear.
    bool m_fidelityDiagnosticRun = false;
    FidelityRestoreSource m_fidelityRunSource = FidelityRestoreSource::Slot;
    FidelityStartPoint m_fidelityRunStart = FidelityStartPoint::AWithLeadIn;
    uint64_t m_fidelityRunGeneration = 0;
    // Frames actually sampled by the run in progress, so an analysis can reject a run that
    // ended early instead of comparing whatever prefix both runs happened to share.
    size_t m_fidelityRunSampleCount = 0;
    // Digest of the movie the run is playing. Two runs of different movies are not comparable
    // however alike their first frames look.
    uint32_t m_fidelityRunMovieDigest = 0;
    // Arms B-direct. Update() skips the old frame; the existing post-increment
    // finalizer restores B and schedules movie input atomically at that boundary.
    bool m_fidelityStartDirectPlayback = false;
    bool m_autoLoadAfterPlayback = false;
    bool m_playbackUiHidden = false;
    bool m_frameHistoryWasOpenBeforePlayback = false;
    std::string m_p1Text = "5";
    std::string m_p2Text = "5";
    std::string m_error;
    std::string m_status;
};