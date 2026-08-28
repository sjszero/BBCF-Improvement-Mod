#include "TasWindow.h"

#include "Core/interfaces.h"
#include "Game/TasManager.h"
#include "Game/gamestates.h"
#include "Overlay/WindowContainer/WindowContainer.h"

// TAS playback uses the game's native virtual stick display.

void TasWindow::Update() {
    TasManager& manager = TasManager::Instance();
    if (!manager.IsActive()) {
        if (IsOpen()) {
            manager.Exit();
        }
        return;
    }

    if (manager.IsPlaying() && manager.IsPlaybackUiHidden()) {
        if (ImGui::IsKeyPressed(g_modVals.tas_parse_keycode)) {
            manager.StopPlayback();
        }
        return;
    }
    if (!IsOpen()) {
        manager.Exit();
        return;
    }

    if (ImGui::IsKeyPressed(g_modVals.tas_parse_keycode)) {
        manager.SetInputText(m_p1Input, m_p2Input);
    }
    if (ImGui::IsKeyPressed(g_modVals.tas_rewind_keycode)) {
        manager.RewindFrames(m_frameCount);
    }
    if (ImGui::IsKeyPressed(g_modVals.tas_advance_keycode)) {
        if (manager.IsEditingRecording()) {
            manager.EditAndAdvanceFrames(m_frameCount);
        } else {
            manager.AdvanceFrames(m_frameCount);
        }
    }
    IWindow::Update();
}

void TasWindow::Draw() {
    TasManager& manager = TasManager::Instance();

    if (!manager.IsActive()) {
        if (IsOpen()) {
            Close();
        }
        ImGui::TextUnformatted("TAS mode is not active.");
        if (ImGui::Button("Enter TAS mode")) {
            manager.Enter();
        }
        if (!manager.GetError().empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", manager.GetError().c_str());
        }
        return;
    }

    ImGui::TextUnformatted("TAS mode: frame editor for training matches");
    ImGui::SameLine();
    if (ImGui::SmallButton("Import##tas_top")) {
        manager.ImportMovie();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Export##tas_top")) {
        manager.ExportMovie();
    }
    ImGui::Separator();
    ImGui::Text("Current frame: %u", manager.GetCurrentFrame());
    ImGui::Text("Base frame: %s%u", manager.HasBaseSnapshot() ? "" : "not saved / ", manager.GetBaseFrame());
    ImGui::Text("Input progress: %u / %u",
        static_cast<unsigned int>(manager.GetCursor()),
        static_cast<unsigned int>(manager.GetFrameCount()));
    ImGui::Text("Rerecord count: %u", manager.GetRerecordCount());
    bool autoLoad = manager.IsAutoLoadAfterPlayback();
    if (ImGui::Checkbox("Auto load base state and freeze after playback", &autoLoad)) {
        manager.SetAutoLoadAfterPlayback(autoLoad);
    }
    if (ImGui::Button("Save base state")) {
        manager.SaveBaseSnapshot();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load base state")) {
        manager.LoadBaseSnapshot();
    }
    ImGui::SameLine();
    if (ImGui::Button("Resume game")) {
        manager.ResumeGame();
    }
    ImGui::SameLine();
    if (ImGui::Button("Exit TAS mode")) {
        manager.Exit();
        Close();
        return;
    }

    ImGui::SameLine();
    if (manager.IsPlaying()) {
        if (ImGui::Button("Stop playback")) {
            manager.StopPlayback();
        }
    } else {
        if (ImGui::Button("Preview playback")) {
            manager.StartPlayback(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Presentation playback")) {
            manager.StartPlayback(true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset movie")) {
        manager.ResetMovie();
    }
    ImGui::Text("Movie frames: %u", static_cast<unsigned int>(manager.GetRecordedFrameCount()));
    if (manager.IsEditingRecording()) {
        ImGui::Text("Movie edit frame: %u", static_cast<unsigned int>(manager.GetCursor()));
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Input commands");
    ImGui::TextUnformatted("Examples: 5C, 4C, 28D, 623C, 656 (dash). Each digit uses one frame; 66 means two frames holding 6, while 656 performs a dash.");

    if (ImGui::InputText("P1##tas_p1", m_p1Input, sizeof(m_p1Input))) {
        manager.SetP1Text(m_p1Input);
    }
    if (ImGui::InputText("P2##tas_p2", m_p2Input, sizeof(m_p2Input))) {
        manager.SetP2Text(m_p2Input);
    }

    if (ImGui::Button("Parse input")) {
        manager.SetInputText(m_p1Input, m_p2Input);
    }
    ImGui::SameLine();
    ImGui::InputInt("Frame count##tas_frame_count", &m_frameCount);
    if (m_frameCount < 1) {
        m_frameCount = 1;
    }

    ImGui::SameLine();
    if (ImGui::Button("Advance N frames")) {
        if (manager.IsEditingRecording()) {
            manager.EditAndAdvanceFrames(m_frameCount);
        } else {
            manager.AdvanceFrames(m_frameCount);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Rewind N frames")) {
        manager.RewindFrames(m_frameCount);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset parsed input")) {
        manager.ResetParsedInputs();
    }

    const TasFrameInput input = manager.GetCurrentInput();
    ImGui::Text("Current input: P1=%u P2=%u", input.p1, input.p2);

    ImGui::Separator();
    ImGui::TextUnformatted("Instructions");
    ImGui::TextWrapped("1. Enter a training match and save a base state.");
    ImGui::TextWrapped("2. Use numpad directions and buttons, for example 623C means 6, 2, then 3+C.");
    ImGui::TextWrapped("3. Set a frame count and use Advance N Frames. Inputs are applied frame by frame.");
    ImGui::TextWrapped("4. Preview starts immediately and freezes at the movie end for continued frame editing. Presentation hides TAS UI, holds neutral for 60 game frames, plays the movie, then holds neutral for 240 frames.");
    ImGui::TextWrapped("5. Rewind N Frames reloads the base state, replays to the target, and deletes all later movie frames.");
    ImGui::TextWrapped("6. Import and Export at the top exchange tas_movie.txt. Save the matching base state after import.");
    ImGui::TextWrapped("Directions: 7 8 9 / 4 5 6 / 1 2 3; 5 means neutral. Parsed commands hold neutral after their final frame.");

    if (!manager.GetStatus().empty()) {
        ImGui::TextWrapped("%s", manager.GetStatus().c_str());
    }
    if (!manager.GetError().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", manager.GetError().c_str());
    }
}

// Lifecycle cleanup is handled by Update(), including a close via the window X button.