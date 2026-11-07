// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <imgui.h>

#include "common/guest_stats.h"
#include "common/path_util.h"
#include "core/bb_frame_rate.h"
#include "core/bb_game_fixes.h"
#include "core/bb_mouse_camera.h"
#include "core/bb_resolution.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/file_sys/bb_hdr_compat.h"
#include "imgui/bb_settings_file.h"
#include "imgui/big_picture/settings_dialog_layer.h"
#include "imgui/dlss_layer.h"
#include "imgui/friends_layer.h"
#include "imgui/imgui_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "input/bb_kbm.h"
#include "input/bb_route.h"
#include "input/controller.h"
#include "input/input_mouse.h"
#include "video_core/renderer_vulkan/vk_bb_frame_gen.h"
#include "video_core/renderer_vulkan/vk_bb_perf_meter.h"
#include "video_core/renderer_vulkan/vk_bb_temporal_dlss.h"
#include "video_core/renderer_vulkan/vk_hdr_meter.h"
#include "video_core/renderer_vulkan/vk_hdr_mod.h"
#include "video_core/renderer_vulkan/vk_retro_hdr.h"

namespace ImGui::Dlss {
namespace {

// The panel edits the same settings file the renderer polls, so it needs no renderer access.
using SettingsFile = ImGui::BbSettingsFile;

constexpr std::array RecommendedPatches{"Disable AA", "Disable Chromatic Aberration",
                                        "Disable DoF"};
// Removes the depth and motion data the upscaler needs; the full name ends in "(Perf Increase)".
constexpr char MotionBlurPatch[] = "Disable Motion Blur[^\"]*";
// Left out while an upscaler is installed (memory_patcher.cpp): it breaks DLSS and FSR.
constexpr char PerformancePatch[] = "Performance Patch[^\"]*";

// With RenoDX HDR, Disable AA turns skin blue: its HDR output expects what the game's AA pass
// leaves in the image.
bool Wanted(size_t i) {
    return !(Vulkan::RenoDxLoaded() && i < 2);
}

std::filesystem::path PatchFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::PatchesDir) / "shadPS4" / "Bloodborne.xml";
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::regex PatchPattern(const char* name) {
    return std::regex{std::string{"(<Metadata [^>]*Name=\""} + name +
                      "\"[^>]*isEnabled=\")(true|false)(\")"};
}

// Patches in the shadPS4 patch file, for the recommended-visuals check.
struct PatchState {
    bool file_found{};
    std::array<bool, RecommendedPatches.size()> enabled{};
    bool motion_blur_disabled{};
    bool performance_patch{};
};

PatchState ReadPatches() {
    PatchState state;
    const auto text = ReadAll(PatchFile());
    state.file_found = !text.empty();
    for (size_t i = 0; i < RecommendedPatches.size(); ++i) {
        std::smatch match;
        state.enabled[i] = std::regex_search(text, match, PatchPattern(RecommendedPatches[i])) &&
                           match[2] == "true";
    }
    std::smatch match;
    state.motion_blur_disabled =
        std::regex_search(text, match, PatchPattern(MotionBlurPatch)) && match[2] == "true";
    state.performance_patch =
        std::regex_search(text, match, PatchPattern(PerformancePatch)) && match[2] == "true";
    return state;
}

bool EnableRecommendedPatches() {
    auto text = ReadAll(PatchFile());
    if (text.empty())
        return false;
    for (size_t i = 0; i < RecommendedPatches.size(); ++i)
        text = std::regex_replace(text, PatchPattern(RecommendedPatches[i]),
                                  Wanted(i) ? "$1true$3" : "$1false$3");
    text = std::regex_replace(text, PatchPattern(MotionBlurPatch), "$1false$3");
    std::ofstream file{PatchFile(), std::ios::binary | std::ios::trunc};
    file << text;
    return bool(file);
}

// Settings waiting for a restart: their notes, the footer and its button.
constexpr ImVec4 RestartColor{1.0f, 0.75f, 0.3f, 1.0f};

void RestartHint(bool pending, const char* text) {
    if (pending)
        TextColored(RestartColor, "%s", text);
    else
        TextDisabled("%s", text);
}

// This menu's UI scale (F1's Other tab, apart from F3's): its text, spacing and widths
// together.
float menu_scale = 1.0f;
float Px(float size) {
    return size * menu_scale;
}

// Relaunches the game for settings that apply at a start, after a confirmation: it goes back to
// the last autosave, like quitting from the system menu.
void RestartButton() {
    const ImVec2 padding = GetStyle().FramePadding;
    PushStyleVar(ImGuiStyleVar_FramePadding, {padding.x * 2.0f, padding.y * 2.0f});
    const bool pressed = Button("Apply and restart the game");
    PopStyleVar();
    if (pressed)
        OpenPopup("Restart the game?");
    SetNextWindowPos(GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    SetNextWindowSize({Px(360.0f), 0.0f});
    if (BeginPopupModal("Restart the game?", nullptr,
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        TextWrapped("The game restarts with the new settings. You continue from your last "
                    "autosave.");
        TextWrapped("Don't restart while the save icon is showing.");
        if (Button("Restart"))
            ::Core::BbResolution::RestartGame();
        SameLine();
        if (Button("Cancel"))
            CloseCurrentPopup();
        EndPopup();
    }
}

// Screen-space reflections: hidden until "Better" is better (the setting is reset to the game's
// own, BbDefaultsVersion 4).
constexpr bool ShowReflections = true;
// Better reflections' tuning and debug views: hidden, at their defaults (sharp, minimum gloss 0.5,
// 8 frames), until they are settled.
constexpr bool ShowReflectionTuning = false;

constexpr std::array UpscalerValues{"auto", "dlss", "fsr", "fsr4", "amdfsr"};
constexpr std::array UpscalerNames{"Automatic", "DLSS", "FSR 3.1", "FSR 4 (add-on)", "FSR (AMD)"};
constexpr std::array PresetValues{13, 11, 10, 12, 0};
constexpr std::array PresetNames{"M (recommended)", "K", "J", "L", "NVIDIA default"};

class DlssLayer final : public ImGui::Layer {
public:
    void Draw() override;
    bool ShouldKeepDrawing() override {
        // Also while another window holds the mouse, to give it back to the camera after.
        return open || toggle_requested || mouse_released;
    }
    // Keyboard and mouse: the mouse turns the camera only while no shadPS4 window is open.
    void UpdateMouseLook();
    bool mouse_released = false;
    int release_wait = 0;       // frames until the release request is repeated
    bool camera_before = false; // the mouse turned the camera when a window took it
    void Open() {
        settings.Load();
        patches = ReadPatches();
        patch_message.clear();
        kbm_message.clear();
        capture_action = -1;
        open = true;
        focus = true;
        // Like shadPS4's other prompts: the game stops reading the controller while the panel
        // is open, and the mouse cursor is shown.
        ImGui::Core::AcquireGamepadInputCapture();
        GetIO().MouseDrawCursor = true;
    }
    void Close() {
        if (!open)
            return;
        open = false;
        ImGui::Core::ReleaseGamepadInputCapture();
        GetIO().MouseDrawCursor = false;
    }
    bool open{};
    bool focus{};
    bool registered{};
    std::atomic<bool> toggle_requested{}; // set by the input thread, handled while drawing

private:
    SettingsFile settings;
    PatchState patches;
    std::string patch_message;
    std::string kbm_message;
    // Rebinding: the action and slot (0 key, 1 mouse) waiting for an input, -1 none.
    int capture_action = -1;
    int capture_slot = 0;
    int capture_frames = 0;
    void DrawKeyBindings();
    // The settings as the game started with them, for the ones that apply at a start.
    struct Launch {
        bool taken{};
        SettingsFile file;
        bool frame_hooks{};
        int anisotropy{};
    } launch;
    void DrawWindow();
    float drawn_scale = 0.0f; // the size the window was last drawn at
    ImVec2 window_size{};
    int menu_size_edit = -1; // the UI scale slider's value while it is dragged
    bool patches_changed{};
    // Settings changed that apply at the next start (this frame's, and the last frame's for the
    // footer's height).
    std::vector<const char*> restart_needed;
    bool restart_footer{};
};

DlssLayer layer;
std::atomic<float> game_x{}, game_y{}, game_w{}, game_h{};

// The upscaler's mode from the render size's share of the output (DLSS and FSR use the same
// ratios), or the share in percent for a size in between.
std::string UpscalerMode(u32 render_height, u32 output_height) {
    if (output_height == 0)
        return {};
    const float share = float(render_height) / float(output_height);
    static constexpr std::array<std::pair<float, const char*>, 4> Modes{{
        {2.0f / 3.0f, "Quality"},
        {1.0f / 1.7f, "Balanced"},
        {0.5f, "Performance"},
        {1.0f / 3.0f, "Ultra Performance"},
    }};
    for (const auto& [ratio, name] : Modes)
        if (std::abs(share - ratio) < 0.015f)
            return name;
    return fmt::format("{}%", int(std::lround(share * 100.0f)));
}

// The game's frame rate in a corner, and the displayed one while frame generation runs. Drawn
// into the game image, so it does not stop the game reaching the screen directly the way
// overlay windows do.
class CounterLayer final : public ImGui::Layer {
public:
    void Draw() override {
        if (const auto hdr = Vulkan::HdrMeter::Get(); hdr.valid) {
            // Debug readout (SHADPS4_HDR_METER=1), below the fps counter.
            SetNextWindowPos({game_x + 12.0f, game_y + 60.0f});
            SetNextWindowBgAlpha(1.0f);
            if (Begin("##bb_hdr_meter", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoDocking))
                Text("HDR nits: peak %.0f  99.9%% %.0f  99%% %.0f  median %.0f", hdr.peak, hdr.p999,
                     hdr.p99, hdr.median);
            End();
        }
        if (!Vulkan::FrameGen::CounterVisible())
            return;
        const bool show_fps = EmulatorSettings.IsBbCounterFps();
        const bool show_ms = EmulatorSettings.IsBbCounterMs();
        const bool show_upscaler = EmulatorSettings.IsBbCounterUpscaler();
        const bool show_graph = EmulatorSettings.IsBbCounterGraph();
        const auto status = Vulkan::BbTemporalDlssStatus();
        // Nothing to show (only the upscaler, which isn't running): no box.
        const bool show_meter = EmulatorSettings.IsBbPerfMeter();
        const bool show_line =
            show_fps || show_ms || (show_upscaler && status.active) || show_meter;
        if (!show_line && !show_graph)
            return;
        const auto stats = Vulkan::FrameGen::GetStats();
        // Inside the game image, in the chosen corner: frame generation leaves the bars around it
        // alone.
        const int corner = std::clamp(EmulatorSettings.GetBbCounterCorner(), 0, 3);
        const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
        constexpr float Margin = 12.0f;
        SetNextWindowPos({right ? game_x + game_w - Margin : game_x + Margin,
                          bottom ? game_y + game_h - Margin : game_y + Margin},
                         ImGuiCond_Always, {right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f});
        // Where the graph goes: under the box (above it in a bottom corner), or at the corner.
        ImVec2 box_min{right ? game_x + game_w - Margin : game_x + Margin,
                       bottom ? game_y + game_h - Margin : game_y + Margin};
        ImVec2 box_max = box_min;
        float font_size = GetFontSize();
        // Opaque: frame generation keeps the whole box still, so no scene may show through it.
        SetNextWindowBgAlpha(1.0f);
        if (show_line && Begin("##bb_fps_counter", nullptr,
                               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
            // The same size whether "Menus sized by screen pixels" is on or not.
            if (EmulatorSettings.IsBbIgnoreDisplayScale())
                SetWindowFontScale(ImGui::Core::DisplayScale());
            // "60FPS  16.7ms  DLSS K Quality 2560x1440", with frame generation "60FPS -> 120FPS
            // ...": the game's frame rate and frame time, then the upscaler (DLAA or Native AA at
            // the output resolution), DLSS in green and FSR in red; two spaces apart.
            const float gap = CalcTextSize("  ").x;
            const ImVec4 green{0.4f, 1.0f, 0.4f, 1.0f}, red{1.0f, 0.35f, 0.3f, 1.0f};
            const float fps = stats.base_fps;
            bool first = true;
            const auto next = [&] {
                if (!first)
                    SameLine(0.0f, gap);
                first = false;
            };
            if (show_fps) {
                next();
                Text("%.0fFPS", fps);
                if (stats.generating) {
                    SameLine();
                    TextColored(green, "-> %.0fFPS", stats.output_fps);
                }
            }
            if (show_ms) {
                next();
                if (fps > 0.0f)
                    Text("%.1fms", 1000.0f / fps);
                else
                    Text("-ms");
            }
            if (show_upscaler && status.active) {
                const bool native = status.render_width >= status.output_width &&
                                    status.render_height >= status.output_height;
                const bool dlss = status.backend == "DLSS";
                std::string upscaler;
                if (dlss)
                    upscaler = native ? "DLAA" : "DLSS";
                else
                    upscaler = native ? status.backend + " Native AA" : status.backend;
                if (EmulatorSettings.IsBbCounterPreset() && status.preset >= 1 &&
                    status.preset <= 26)
                    upscaler += fmt::format(" {}", char('A' + status.preset - 1));
                next();
                TextColored(dlss ? green : red, "%s", upscaler.c_str());
                // The mode and the output resolution in white, one space after.
                if (EmulatorSettings.IsBbCounterMode() && !native) {
                    SameLine();
                    TextUnformatted(
                        UpscalerMode(status.render_height, status.output_height).c_str());
                }
                if (EmulatorSettings.IsBbCounterResolution()) {
                    SameLine();
                    Text("%ux%u", status.output_width, status.output_height);
                }
            }
            if (show_meter) {
                // Performance meter: the stage holding the frame rate, and each stage's busy
                // time a frame (the command thread's largest steps in brackets).
                const auto meter = Vulkan::PerfMeter::Get();
                if (!meter.valid) {
                    TextDisabled("Performance meter: waiting (Bloodborne, threaded renderer)");
                } else {
                    const ImVec4 amber{1.0f, 0.75f, 0.3f, 1.0f};
                    if (meter.limit_load > 0.0) {
                        TextColored(amber, "Limit: %s %.0f%%", meter.limit.c_str(),
                                    meter.limit_load * 100.0);
                    } else {
                        TextColored(amber, "Limit: %s", meter.limit.c_str());
                    }
                    SameLine();
                    Text("| next: %s %.0f%%", meter.next.c_str(), meter.next_load * 100.0);
                    const std::string gpu =
                        meter.gpu_ms >= 0.0 ? fmt::format("{:.1f} ms", meter.gpu_ms) : "-";
                    Text("CP %.1f ms%s | recorder %.1f ms | GPU %s | %.0f draws", meter.cmd_ms,
                         meter.cmd_steps.empty() ? ""
                                                 : fmt::format(" ({})", meter.cmd_steps).c_str(),
                         meter.rec_ms, gpu.c_str(), meter.draws_per_frame);
                }
            }
            box_min = GetWindowPos();
            box_max = {box_min.x + GetWindowSize().x, box_min.y + GetWindowSize().y};
            font_size = GetFontSize();
        }
        if (show_line)
            End();
        if (show_graph)
            DrawGraph(box_min, box_max, font_size, right, bottom);
    }
    // The frame times of the last 120 frames as a bare line, as wide as the box. In a top corner
    // it runs halfway between the box and the game's health bar (its top at about 5.7% of the
    // picture's height, at any resolution); in a bottom corner just above the box. No background:
    // the game's UI stays visible around it. Each frame is drawn against the usual one (the
    // median): a usual frame always sits on the same height, whatever the frame rate, and a slower
    // one rises above it, reaching the box at 2.5 times the usual time; the line never moves down
    // to make room for a spike.
    static void DrawGraph(ImVec2 box_min, ImVec2 box_max, float font_size, bool right,
                          bool bottom) {
        const float width = std::max(box_max.x - box_min.x, font_size * 12.0f);
        float ceiling, baseline, floor;
        const float health_bar = game_y + game_h * 0.057f;
        if (!bottom && health_bar - box_max.y >= 6.0f) {
            ceiling = box_max.y + 1.0f;
            baseline = (box_max.y + health_bar) * 0.5f;
            floor = baseline + (baseline - ceiling) * 0.33f;
        } else if (!bottom) {
            ceiling = box_max.y + 2.0f;
            baseline = ceiling + font_size * 0.75f;
            floor = ceiling + font_size;
        } else {
            floor = box_min.y - 2.0f;
            baseline = floor - font_size * 0.25f;
            ceiling = floor - font_size;
        }
        const ImVec2 pos{right ? box_max.x - width : box_min.x, ceiling};
        SetNextWindowPos(pos);
        SetNextWindowSize({width, floor - ceiling + 1.0f});
        PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        PushStyleVar(ImGuiStyleVar_WindowMinSize, {1.0f, 1.0f});
        if (Begin("##bb_frame_time_graph", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_NoDocking)) {
            const auto times = Vulkan::FrameGen::GetFrameTimes();
            std::vector<float> sorted;
            for (const float t : times)
                if (t > 0.0f)
                    sorted.push_back(t);
            if (!sorted.empty()) {
                std::ranges::nth_element(sorted, sorted.begin() + sorted.size() / 2);
                const float usual = sorted[sorted.size() / 2];
                std::array<ImVec2, Vulkan::FrameGen::FrameTimeCount> points;
                for (size_t i = 0; i < times.size(); ++i) {
                    const float t = times[i] > 0.0f ? times[i] : usual;
                    const float over = std::clamp(t / usual - 1.0f, -0.5f, 1.5f);
                    const float y = over >= 0.0f ? baseline - over / 1.5f * (baseline - ceiling)
                                                 : baseline - over / 0.5f * (floor - baseline);
                    // On whole pixels (plus a half), so a flat line covers one row, not two.
                    points[i] = {pos.x + width * float(i) / float(times.size() - 1),
                                 std::floor(y) + 0.5f};
                }
                // One screen pixel thick, without a shadow.
                const float pixel = 1.0f / std::max(GetIO().DisplayFramebufferScale.y, 1.0f);
                GetWindowDrawList()->AddPolyline(points.data(), int(points.size()),
                                                 IM_COL32(255, 255, 255, 230), ImDrawFlags_None,
                                                 pixel);
            }
        }
        End();
        PopStyleVar(3);
    }
    // It changes with the game's frames, so it never asks for repeated presents: those would
    // switch frame generation off at 30 fps.
    bool ShouldKeepDrawing() override {
        return false;
    }
};

CounterLayer counter;

void DlssLayer::UpdateMouseLook() {
    // Any shadPS4 window takes the mouse (F7 pressed in one too); closing them gives it back to
    // the camera if Mouse look is on.
    const bool window_open = open || ImGuiEmuSettings::IsInGameSettingsDialogOpen() ||
                             Overlay::IsQuitWindowOpen() || ImGui::Friends::IsOpen() ||
                             ImGui::Core::IsGamepadInputCaptured();
    if (window_open) {
        mouse_released = true; // checked again when the last window closes
        if (release_wait > 0)
            --release_wait;
        if (Input::GetMouseMode() == Input::MouseMode::Joystick && release_wait == 0) {
            Input::BbKbm::RequestMouseLook(false);
            camera_before = true;
            release_wait = 10;
        }
    } else if (mouse_released) {
        // The last window closed: the camera follows Mouse look, whatever it was before.
        mouse_released = false;
        release_wait = 0;
        // Without our controls (shadPS4's own F7 mouse mode): as it was.
        const bool want = Input::BbKbm::Active() ? Input::BbKbm::MouseLookWanted() : camera_before;
        camera_before = false;
        const bool camera = Input::GetMouseMode() == Input::MouseMode::Joystick;
        if (want != camera)
            Input::BbKbm::RequestMouseLook(want);
    }
}

void DlssLayer::Draw() {
    if (!launch.taken) {
        launch.taken = true;
        launch.file.Load();
        launch.frame_hooks = EmulatorSettings.IsBbFrameHooks();
        launch.anisotropy = EmulatorSettings.GetBbAnisotropy();
    }
    UpdateMouseLook();
    if (toggle_requested.exchange(false)) {
        if (open)
            Close();
        else
            Open();
    }
    if (!open)
        return;
    if (focus) {
        SetNextWindowFocus();
        focus = false;
    }
    // UI scale: the text through the font size, the spacing through the style, the fixed widths
    // through Px(). Saved, so it stays from one launch to the next.
    menu_scale = float(std::clamp(EmulatorSettings.GetBbF1Scale(), 50, 250)) * 0.01f;
    const ImGuiStyle& style = GetStyle();
    const auto scaled = [](ImVec2 v) { return ImVec2{v.x * menu_scale, v.y * menu_scale}; };
    PushFont(nullptr, style.FontSizeBase * menu_scale);
    PushStyleVar(ImGuiStyleVar_WindowPadding, scaled(style.WindowPadding));
    PushStyleVar(ImGuiStyleVar_FramePadding, scaled(style.FramePadding));
    PushStyleVar(ImGuiStyleVar_ItemSpacing, scaled(style.ItemSpacing));
    PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, scaled(style.ItemInnerSpacing));
    PushStyleVar(ImGuiStyleVar_IndentSpacing, style.IndentSpacing * menu_scale);
    PushStyleVar(ImGuiStyleVar_ScrollbarSize, style.ScrollbarSize * menu_scale);
    PushStyleVar(ImGuiStyleVar_GrabMinSize, style.GrabMinSize * menu_scale);
    DrawWindow();
    PopStyleVar(7);
    PopFont();
}

void DlssLayer::DrawWindow() {
    // Resizable, never taller or wider than the screen; the content scrolls inside it.
    const ImVec2 screen = GetMainViewport()->WorkSize;
    SetNextWindowSizeConstraints(
        {std::min(Px(420.0f), screen.x * 0.95f), std::min(Px(240.0f), screen.y * 0.9f)},
        {screen.x * 0.95f, screen.y * 0.9f});
    SetNextWindowSize({Px(520.0f), screen.y * 0.75f}, ImGuiCond_FirstUseEver);
    // A new menu size: the window grows or shrinks with it.
    if (drawn_scale > 0.0f && drawn_scale != menu_scale && window_size.x > 0.0f) {
        const float ratio = menu_scale / drawn_scale;
        SetNextWindowSize({window_size.x * ratio, window_size.y * ratio});
    }
    drawn_scale = menu_scale;
    // Where it was last left, as a share of the free room left and right, above and below (0: at
    // the left or top edge, 1: at the right or bottom, 0.5: centred), so it stays on screen at
    // another resolution: one moved to the top right at 4K opens at the top right at 1080p too.
    const ImVec2 origin = GetMainViewport()->WorkPos;
    const ImVec2 place{float(std::clamp(EmulatorSettings.GetBbF1PosX(), 0, 1000)) * 0.001f,
                       float(std::clamp(EmulatorSettings.GetBbF1PosY(), 0, 1000)) * 0.001f};
    SetNextWindowPos({origin.x + screen.x * place.x, origin.y + screen.y * place.y},
                     ImGuiCond_Appearing, place);
    bool keep_open = true;
    if (!Begin("Upscaling  (F1 / Esc to close)", &keep_open,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        End();
        return;
    }
    window_size = GetWindowSize();
    // Moved or resized: the new place is saved once the mouse is let go.
    if (!IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 pos = GetWindowPos();
        const auto share = [](float at, float start, float room) {
            return room > 0.0f
                       ? std::clamp(int(std::lround((at - start) / room * 1000.0f)), 0, 1000)
                       : 500;
        };
        const int x = share(pos.x, origin.x, screen.x - window_size.x);
        const int y = share(pos.y, origin.y, screen.y - window_size.y);
        if (std::abs(x - EmulatorSettings.GetBbF1PosX()) > 1 ||
            std::abs(y - EmulatorSettings.GetBbF1PosY()) > 1) {
            EmulatorSettings.SetBbF1PosX(x);
            EmulatorSettings.SetBbF1PosY(y);
            EmulatorSettings.Save();
        }
    }
    if (!keep_open ||
        (capture_action < 0 && IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         IsKeyPressed(ImGuiKey_Escape, false))) {
        End();
        Close();
        return;
    }
    restart_needed.clear();
    const auto changed = [&](const char* key, const char* fallback) {
        return settings.Get(key, fallback) != launch.file.Get(key, fallback);
    };
    // The footer with the restart button stays at the bottom; the settings scroll above it.
    BeginChild("##f1_settings", {0.0f, restart_footer ? -(GetFrameHeightWithSpacing() * 2.0f +
                                                          GetStyle().FramePadding.y * 2.0f +
                                                          GetStyle().ItemSpacing.y * 2.0f)
                                                      : 0.0f});
    const auto status = Vulkan::BbTemporalDlssStatus();
    if (status.active) {
        TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "Active");
        SameLine();
        Text("%s  %ux%u -> %ux%u", status.backend.c_str(), status.render_width,
             status.render_height, status.output_width, status.output_height);
    } else {
        TextColored({1.0f, 0.75f, 0.3f, 1.0f}, "Not active");
        if (!status.reason.empty())
            TextWrapped("%s", status.reason.c_str());
    }
    if (!status.gpu.empty())
        TextDisabled("GPU: %s", status.gpu.c_str());
    if (patches.motion_blur_disabled) {
        PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.4f, 0.4f, 1.0f});
        TextWrapped("The Disable Motion Blur patch is on. It removes the depth and motion data "
                    "the upscaler needs: turn it off below and restart the game. To get rid of "
                    "motion blur, leave Game motion blur (Graphics tab) unticked instead.");
        PopStyleColor();
    }
    if (patches.performance_patch) {
        TextDisabled("The Performance Patch is on but left out: it changes the game's render "
                     "formats and fixes some sizes at 1080p, which breaks DLSS and FSR.");
    }

    if (!Wanted(0) && patches.enabled[0] &&
        !::Core::FileSys::BbHdrCompat::ShouldSkipPatch(RecommendedPatches[0])) {
        PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.4f, 0.4f, 1.0f});
        TextWrapped("The Disable AA patch is on. With RenoDX HDR it makes skin look blue: turn it "
                    "off below and restart the game.");
        PopStyleColor();
    }

    if (EmulatorSettings.GetReadbacksMode() == 0) {
        PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.4f, 0.4f, 1.0f});
        TextWrapped("GPU readbacks are off. Bloodborne then shows stretched polygons and missing "
                    "parts of the world at random (with or without upscaling). Press F3, go to "
                    "Experimental and set Readbacks Mode to Relaxed, then restart the game.");
        PopStyleColor();
    }

    // Tabs. Display: frame rate and resolution. Graphics: textures and the image features, the same
    // settings as F3's (Bloodborne, Graphics), both always showing the same. Other: this menu's UI
    // scale, the FPS counter, keyboard and mouse, the benchmark route.
    bool main_tab = true, display_tab = false, graphics_tab = false, other_tab = false;
    if (BeginTabBar("##f1_tabs")) {
        main_tab = BeginTabItem("Main");
        if (main_tab)
            EndTabItem();
        display_tab = BeginTabItem("Display");
        if (display_tab)
            EndTabItem();
        graphics_tab = BeginTabItem("Graphics");
        if (graphics_tab)
            EndTabItem();
        other_tab = BeginTabItem("Other");
        if (other_tab)
            EndTabItem();
        EndTabBar();
    }
    if (display_tab) {
        SeparatorText("Frame rate");
        {
            namespace FrameRate = ::Core::BbFrameRate;
            bool hooks = EmulatorSettings.IsBbFrameHooks();
            if (Checkbox("Unlock frame rate", &hooks)) {
                EmulatorSettings.SetBbFrameHooks(hooks);
                EmulatorSettings.Save();
            }
            if (IsItemHovered())
                SetTooltip("90 FPS++ (by Kyo) with its values set for the FPS cap below. Your own "
                           "frame rate patches are left out.\nApplies at the next start.");
            if (hooks != launch.frame_hooks) {
                SameLine();
                RestartHint(true, "(restart to apply)");
                restart_needed.push_back("Unlock frame rate");
            }
            static constexpr std::array<int, 4> limits = {60, 90, 120, 0};
            static constexpr std::array<const char*, 4> labels = {"60", "90", "120", "Uncapped"};
            const int limit = EmulatorSettings.GetBbFpsLimit();
            int index = int(limits.size()) - 1;
            for (int i = 0; i < int(limits.size()); ++i) {
                if (limits[i] == limit)
                    index = i;
            }
            BeginDisabled(!FrameRate::Active());
            SetNextItemWidth(Px(200.0f));
            if (BeginCombo("FPS cap", labels[index])) {
                for (int i = 0; i < int(limits.size()); ++i) {
                    if (Selectable(labels[i], i == index)) {
                        EmulatorSettings.SetBbFpsLimit(limits[i]);
                        EmulatorSettings.Save();
                        FrameRate::SetLimit(limits[i]);
                    }
                }
                EndCombo();
            }
            EndDisabled();
            if (IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                SetTooltip("Changes at once. Set the vblank frequency at least as high.");
            if (FrameRate::Active()) {
                SameLine();
                TextDisabled("%.0f FPS", double(FrameRate::HeldRate()));
            }
            bool even = EmulatorSettings.IsBbEvenCappedFlips();
            BeginDisabled(!FrameRate::Active());
            if (Checkbox("Even frames with a cap", &even)) {
                EmulatorSettings.SetBbEvenCappedFlips(even);
                EmulatorSettings.Save();
            }
            EndDisabled();
            if (IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                SetTooltip(
                    "With an FPS cap the game moves by exactly the cap's frame time every "
                    "frame, but finishes its frames a millisecond or two early or late.\nOn: "
                    "each frame is shown one cap period after the last (an early one waits "
                    "for its turn).\nOff: each as soon as it is ready. Changes at once.");
        }

        SeparatorText("Resolution");
        {
            namespace Res = ::Core::BbResolution;
            bool res_control = settings.Get("res_control", "0") == "1";
            if (Checkbox("Set the resolution here (restart)", &res_control))
                settings.Set("res_control", res_control ? "1" : "0");
            if (IsItemHovered())
                SetTooltip(
                    "The game's output resolution (its frame, post-processing and UI) and the "
                    "render resolution of\nthe 3D scene, which the upscaler brings up to the "
                    "output. Patches that set the same sizes (Resolution, Decoupled,\nHD "
                    "Motion Blur) are skipped while this is on. Bloodborne 1.09.");
            const auto next = Res::Preview();
            BeginDisabled(!res_control);
            {
                static constexpr std::array OutputValues{"auto",      "1280x720",  "1600x900",
                                                         "1920x1080", "2560x1440", "3840x2160"};
                const auto output = settings.Get("res_output", "auto");
                int index = 0;
                for (size_t i = 0; i < OutputValues.size(); ++i)
                    if (output == OutputValues[i])
                        index = int(i);
                const std::string window_name =
                    index == 0
                        ? fmt::format("Window ({}x{})", next.output_width, next.output_height)
                        : std::string{"Window"};
                std::array<const char*, OutputValues.size()> names{};
                names[0] = window_name.c_str();
                for (size_t i = 1; i < OutputValues.size(); ++i)
                    names[i] = OutputValues[i];
                SetNextItemWidth(Px(200.0f));
                if (Combo("Output resolution", &index, names.data(), int(names.size())))
                    settings.Set("res_output", OutputValues[index]);
            }
            {
                // Native, then the usual 16:9 sizes below the output, largest first. Above 1080p
                // the scene needs the bigger graphics heap: more VRAM and extra dmem.
                auto heights = Res::RenderHeights(next.output_height);
                std::ranges::reverse(heights);
                const auto vram = [](u32 h) {
                    return h > Res::HeapRenderHeight ? ", more VRAM" : "";
                };
                std::vector<std::string> values{"native"}, labels;
                labels.push_back(next.output_height > Res::HeapRenderHeight
                                     ? "Native (DLAA / FSR Native AA, more VRAM)"
                                     : "Native (DLAA / FSR Native AA)");
                for (const u32 h : heights) {
                    values.push_back(std::to_string(h));
                    labels.push_back(fmt::format("{}x{} ({}%{})", h * 16 / 9, h,
                                                 h * 100 / next.output_height, vram(h)));
                }
                int index = 0;
                for (size_t i = 1; i < values.size(); ++i)
                    if (std::stoul(values[i]) == next.render_height)
                        index = int(i);
                std::vector<const char*> names;
                for (const auto& label : labels)
                    names.push_back(label.c_str());
                SetNextItemWidth(Px(200.0f));
                if (Combo("Upscale from", &index, names.data(), int(names.size())))
                    settings.Set("res_render", values[index]);
            }
            {
                // Decoupled: the game's frame (post-processing, UI) at the output size, only the 3D
                // scene lower. Regular: all of it lower, as a Resolution Patch: faster.
                static constexpr std::array ModeValues{"decoupled", "regular"};
                static constexpr std::array ModeNames{"Output (sharp UI)",
                                                      "Render resolution (faster)"};
                int index = next.regular ? 1 : 0;
                SetNextItemWidth(Px(200.0f));
                if (Combo("UI and effects at", &index, ModeNames.data(), int(ModeNames.size())))
                    settings.Set("res_mode", ModeValues[index]);
                if (IsItemHovered())
                    SetTooltip(
                        "Output: only the 3D scene renders at the lower resolution; the game's "
                        "post-processing and UI\nstay at the output resolution (Decoupled UI). "
                        "Sharpest.\nRender resolution: the whole frame renders at the lower "
                        "resolution, as a regular Resolution Patch,\nand the upscaler brings "
                        "it up to the output. Faster on slower GPUs, the UI is a little "
                        "softer.");
            }
            {
                // The game's graphics heap, raised for a scene above 1080p (Res::Install).
                static constexpr std::array HeapValues{"auto", "full", "first", "smaller", "off"};
                static constexpr std::array HeapNames{"Auto", "Full Heap Patch", "First Heap Only",
                                                      "Smaller 2nd Heap", "Off"};
                const auto heap = next.heap;
                int index = 0;
                for (size_t i = 0; i < HeapValues.size(); ++i)
                    if (heap == HeapValues[i])
                        index = int(i);
                SetNextItemWidth(Px(200.0f));
                if (Combo("Graphics Heap Size", &index, HeapNames.data(), int(HeapNames.size())))
                    settings.Set("res_heap", HeapValues[index]);
                if (IsItemHovered())
                    SetTooltip(
                        "Memory the game sets aside for graphics. A scene above 1080p needs "
                        "more than its own.\nAuto: raised above 1080p, when a heap size patch is "
                        "on in the patch file, or with texture packs (BB PC Remaster);\nboth heaps "
                        "(as "
                        "Smaller 2nd "
                        "Heap) on cards with 8 GB or more, otherwise the first heap only.\nFull "
                        "Heap "
                        "Patch: the Increased Graphics Heap Sizes "
                        "patch. First Heap Only: only the first heap raised.\nSmaller 2nd "
                        "Heap: the second heap a little less than the full patch (the full patch "
                        "can "
                        "cause missing "
                        "textures\nand flicker on some cards).\nOff: the game's own sizes, unless "
                        "a "
                        "heap size patch is enabled in the patch file.");
            }
            EndDisabled();
            const auto& now = Res::Applied();
            if (res_control) {
                Text("%ux%u -> %ux%u", next.render_width, next.render_height, next.output_width,
                     next.output_height);
                SameLine();
                const bool in_use = now.enabled && now.output_width == next.output_width &&
                                    now.output_height == next.output_height &&
                                    now.render_width == next.render_width &&
                                    now.render_height == next.render_height &&
                                    now.heap == next.heap && now.regular == next.regular;
                if (in_use) {
                    TextDisabled("(in use)");
                } else {
                    RestartHint(now.problem.empty(), "(restart the game to apply)");
                    if (now.problem.empty())
                        restart_needed.push_back("Resolution");
                }
                if (!now.problem.empty())
                    TextDisabled("%s", now.problem.c_str());
                // A scene above 1080p raises the game's graphics heap: it needs extra dmem.
                if (next.render_height > Res::HeapRenderHeight &&
                    EmulatorSettings.GetExtraDmemInMBytes() < 4000)
                    TextColored({1.0f, 0.75f, 0.3f, 1.0f},
                                "Above 1080p needs extra dmem of 4000 MB or more (game settings).");
                // Patches from the patch file or launcher that set the same sizes: not applied.
                if (now.enabled && !Res::SkippedPatches().empty())
                    TextWrapped("Ignored while this is on: %s",
                                fmt::format("{}", fmt::join(Res::SkippedPatches(), ", ")).c_str());
            } else {
                if (now.enabled) {
                    RestartHint(true, "Restart the game to use the patch file's resolution again.");
                    restart_needed.push_back("Resolution");
                }
                TextWrapped(
                    "Off: the render resolution comes from the Resolution Patch you enable "
                    "for Bloodborne (shadPS4 patches or BBLauncher), upscaled to your window.");
            }
        }
    }
    if (main_tab) {
        SeparatorText("Settings");
        bool enabled = settings.Get("enabled", "1") != "0";
        if (Checkbox("Upscaling enabled", &enabled))
            settings.Set("enabled", enabled ? "1" : "0");

        const auto upscaler = settings.Get("upscaler", "auto");
        int upscaler_index = 0;
        for (size_t i = 0; i < UpscalerValues.size(); ++i)
            if (upscaler.starts_with(UpscalerValues[i]))
                upscaler_index = int(i); // "fsr4" also starts with "fsr" and comes later
        // AMD FSR is named with its runtime's version, e.g. "FSR 4.1.2 (AMD)".
        const auto upscaler_name = [&](int index) {
            return index == 4 && !status.amd_fsr_label.empty() ? status.amd_fsr_label.c_str()
                                                               : UpscalerNames[index];
        };
        // FSR 4 is listed only when its add-on is installed, AMD FSR only when its folder is.
        const auto upscaler_listed = [&](int index) {
            return index == 3   ? status.fsr4_installed
                   : index == 4 ? !status.amd_fsr_label.empty()
                                : true;
        };
        if (BeginCombo("Upscaler", upscaler_name(upscaler_index))) {
            for (int i = 0; i < int(UpscalerValues.size()); ++i) {
                if (upscaler_listed(i) && Selectable(upscaler_name(i), i == upscaler_index)) {
                    upscaler_index = i;
                    settings.Set("upscaler", UpscalerValues[i]);
                }
            }
            EndCombo();
        }
        if (!status.dlss_problem.empty() && upscaler_index != 2 && upscaler_index != 4)
            TextDisabled("%s", status.dlss_problem.c_str());

        const int preset = std::atoi(settings.Get("preset", "13").c_str());
        int preset_index = 0;
        for (size_t i = 0; i < PresetValues.size(); ++i)
            if (PresetValues[i] == preset)
                preset_index = int(i);
        BeginDisabled(status.backend != "DLSS" || upscaler_index >= 2);
        if (Combo("DLSS model", &preset_index, PresetNames.data(), int(PresetNames.size())))
            settings.Set("preset", std::to_string(PresetValues[preset_index]));
        EndDisabled();

        float sharpness = float(std::atof(settings.Get("sharpness", "0.6").c_str()));
        if (SliderFloat("Sharpness", &sharpness, 0.0f, 1.0f, "%.2f"))
            settings.Set("sharpness", fmt::format("{:.2f}", sharpness));

        if (status.nr_installed) {
            SeparatorText("DLSS 5 NR");
            bool nr = settings.Get("nr", "0") != "0";
            if (Checkbox("Neural rendering", &nr))
                settings.Set("nr", nr ? "1" : "0");
            // CC_SMALL_FP8 is listed only when its model is installed.
            static constexpr std::array NetworkNames{"Automatic", "CC_SILVER_AARDWOLD",
                                                     "CC_SMALL_FP8"};
            static constexpr std::array NetworkValues{"auto", "silver", "small"};
            const auto network = settings.Get("nr_network", "auto");
            int network_index = 0;
            for (size_t i = 0; i < NetworkValues.size(); ++i)
                if (network.starts_with(NetworkValues[i]))
                    network_index = int(i);
            const int network_count = status.nr_small_model ? 3 : 2;
            if (Combo("Network", &network_index, NetworkNames.data(), network_count))
                settings.Set("nr_network", NetworkValues[network_index]);
            float nr_scale = float(std::atof(settings.Get("nr_scale", "0.67").c_str()));
            if (SliderFloat("Network resolution", &nr_scale, 0.33f, 1.0f, "%.2f",
                            ImGuiSliderFlags_AlwaysClamp))
                settings.Set("nr_scale", fmt::format("{:.2f}", nr_scale));
            float nr_intensity = float(std::atof(settings.Get("nr_intensity", "1").c_str()));
            if (SliderFloat("Intensity", &nr_intensity, 0.0f, 2.0f, "%.2f",
                            ImGuiSliderFlags_AlwaysClamp))
                settings.Set("nr_intensity", fmt::format("{:.2f}", nr_intensity));
            // Wrapped: the add-on's messages run longer than the window is wide.
            if (!status.nr_state.empty()) {
                PushStyleColor(ImGuiCol_Text, GetStyleColorVec4(ImGuiCol_TextDisabled));
                TextWrapped("%s", status.nr_state.c_str());
                PopStyleColor();
            }
            Separator();
        }

        if (Vulkan::RenoDxLoaded()) {
            bool hdr_compat = settings.Get("hdr_mod_compat", "1") != "0";
            if (Checkbox("Remaster HDR compatibility (restart)", &hdr_compat))
                settings.Set("hdr_mod_compat", hdr_compat ? "1" : "0");
            BeginDisabled(!hdr_compat);
            bool moon_restore = settings.Get("hdr_moon_restore", "0") == "1";
            if (Checkbox("Restore original moon lighting (restart)", &moon_restore))
                settings.Set("hdr_moon_restore", moon_restore ? "1" : "0");
            EndDisabled();
            if (changed("hdr_mod_compat", "1"))
                restart_needed.push_back("Remaster HDR compatibility");
            if (changed("hdr_moon_restore", "0"))
                restart_needed.push_back("Moon lighting");
        }

        bool frame_gen = settings.Get("frame_gen", "0") != "0";
        if (Checkbox("Frame generation", &frame_gen))
            settings.Set("frame_gen", frame_gen ? "1" : "0");
        if (frame_gen && !Vulkan::FrameGen::Requested())
            restart_needed.push_back("Frame generation");
        if (!Vulkan::FrameGen::Active()) {
            const auto problem = Vulkan::FrameGen::Problem();
            if (frame_gen && !problem.empty()) {
                TextDisabled("%s", problem.c_str());
            } else {
                SameLine();
                RestartHint(frame_gen, frame_gen ? "(restart the game to turn it on)"
                                                 : "(DLSS on RTX 40/50, FSR on other GPUs)");
            }
        } else {
            const auto stats = Vulkan::FrameGen::GetStats();
            SameLine();
            if (stats.generating)
                TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "%s  %.0f -> %.0f fps",
                            Vulkan::FrameGen::BackendName(), stats.base_fps, stats.output_fps);
            else
                TextDisabled("%s  %.0f fps, not generating", Vulkan::FrameGen::BackendName(),
                             stats.base_fps);
        }

        // Which frame generation runs; Streamline or FSR's swapchain is set up at launch.
        {
            static constexpr std::array BackendNames{"Automatic", "DLSS (RTX 40/50)", "FSR"};
            static constexpr std::array BackendValues{"auto", "dlss", "fsr"};
            const auto backend = settings.Get("fg_backend", "auto");
            int index = 0;
            for (size_t i = 0; i < BackendValues.size(); ++i)
                if (backend == BackendValues[i])
                    index = int(i);
            SetNextItemWidth(Px(200.0f));
            if (Combo("Frame gen type", &index, BackendNames.data(), int(BackendNames.size())))
                settings.Set("fg_backend", BackendValues[index]);
            if (index != 2 && std::string_view{Vulkan::FrameGen::BackendName()} == "FSR" &&
                Vulkan::RenoDxLoaded())
                TextDisabled(
                    "RenoDX HDR is loaded: DLSS frame generation does not work with its HDR "
                    "output, FSR is used.");
            SameLine();
            RestartHint(changed("fg_backend", "auto"), "(restart to apply)");
            if (changed("fg_backend", "auto"))
                restart_needed.push_back("Frame gen type");
        }
        if (Vulkan::FrameGen::Active()) {
            // 3x and up only where Streamline offers them (RTX 50 series).
            const int max = int(Vulkan::FrameGen::MaxMultiplier());
            const int chosen =
                std::clamp(std::atoi(settings.Get("fg_multiplier", "2").c_str()), 2, 6);
            if (max > 2 || chosen > 2) {
                int index = std::min(chosen, max) - 2;
                static constexpr std::array Names{"2x", "3x", "4x", "5x", "6x"};
                SetNextItemWidth(Px(120.0f));
                if (Combo("Frame gen multiplier", &index, Names.data(), max - 1))
                    settings.Set("fg_multiplier", std::to_string(index + 2));
                if (chosen > max) {
                    SameLine();
                    TextDisabled("(%dx needs an RTX 50 card)", chosen);
                }
            }
        }

        // DLSS frame generation's model, set in the NVIDIA driver's profile for shadPS4.exe when
        // the game starts (as the NVIDIA app's model override does).
        {
            static constexpr std::array ModelNames{"Default", "B", "C (faster)"};
            static constexpr std::array ModelValues{"default", "b", "c"};
            const auto model = settings.Get("fg_model", "default");
            int index = 0;
            for (size_t i = 0; i < ModelValues.size(); ++i)
                if (model == ModelValues[i])
                    index = int(i);
            BeginDisabled(std::string_view{Vulkan::FrameGen::BackendName()} == "FSR");
            SetNextItemWidth(Px(200.0f));
            if (Combo("DLSS frame gen model", &index, ModelNames.data(), int(ModelNames.size())))
                settings.Set("fg_model", ModelValues[index]);
            EndDisabled();
            if (IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                SetTooltip(
                    "Default: the driver's choice (the NVIDIA app's override, if set).\n"
                    "B: the standard model. C: a lighter model, faster, a little softer on "
                    "the UI;\nneeds DLSS frame generation 310.9.16 or newer (nvngx_dlssg.dll), "
                    "older ones use B.\nWritten to the NVIDIA driver profile for shadPS4.exe "
                    "when the game starts.");
            SameLine();
            RestartHint(changed("fg_model", "default"), "(restart to apply)");
            if (changed("fg_model", "default"))
                restart_needed.push_back("DLSS frame gen model");
        }

        // NVIDIA Reflex's wait before each frame: less input delay with frame generation, but on
        // some setups a lower base frame rate. Takes effect within a second.
        bool reflex_sleep = settings.Get("fg_reflex_sleep", "0") != "0";
        BeginDisabled(std::string_view{Vulkan::FrameGen::BackendName()} != "DLSS");
        if (Checkbox("Lower input delay (Reflex)", &reflex_sleep))
            settings.Set("fg_reflex_sleep", reflex_sleep ? "1" : "0");
        EndDisabled();
        SameLine();
        TextDisabled("(less input delay, may lower fps)");

        bool menu_fix = settings.Get("menu_fix", "1") != "0";
        if (Checkbox("Menu fix", &menu_fix))
            settings.Set("menu_fix", menu_fix ? "1" : "0");
        if (IsItemHovered())
            SetTooltip("Draws the HUD, menus and dialogue (health bar, blood echoes, inventory, "
                       "NPCs) as their own layer\nover the upscaled scene, so the scene doesn't "
                       "flicker or shimmer through their see-through parts.\nThe scene stays "
                       "upscaled. Upscaled to a higher resolution or DLAA; not with Decoupled UI.");
        SameLine();
        TextDisabled("(steady HUD and menus)");

        bool texture_lod_bias = settings.Get("texture_lod_bias", "1") != "0";
        if (Checkbox("Sharper textures", &texture_lod_bias))
            settings.Set("texture_lod_bias", texture_lod_bias ? "1" : "0");
        if (IsItemHovered())
            SetTooltip(
                "While upscaling, the scene's textures are sampled for the output resolution, "
                "not the lower render\nresolution (NVIDIA's DLSS guide). Off: the game's own "
                "texture detail. Can add a little shimmer\non fine patterns.");
        SameLine();
        TextDisabled("(texture detail of the output resolution)");

        bool object_motion = settings.Get("object_motion", "1") != "0";
        if (Checkbox("Object motion vectors", &object_motion))
            settings.Set("object_motion", object_motion ? "1" : "0");
        if (IsItemHovered())
            SetTooltip(
                "Motion of moving objects (doors, elevators, enemies) for the upscaler. Off: "
                "only the camera's motion\nis used. Turn it off to check whether ghosting "
                "comes from it.");
        SameLine();
        TextDisabled("(less ghosting on moving objects)");

        const bool renodx = Vulkan::RenoDxLoaded();
        bool character_motion = !renodx && settings.Get("character_motion", "1") != "0";
        BeginDisabled(renodx);
        if (Checkbox("Character motion vectors", &character_motion))
            settings.Set("character_motion", character_motion ? "1" : "0");
        EndDisabled();
        if (IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            SetTooltip(renodx
                           ? "Off while RenoDX is loaded: RenoDX finds the shaders it replaces by "
                             "their exact code,\nand the motion code would hide the character "
                             "shaders from it."
                           : "Per-vertex motion of animated characters, cloth and weapons for the "
                             "upscaler. Off: they get\nthe object or camera motion instead. Turn "
                             "it off to check whether ghosting comes from it.");
        SameLine();
        TextDisabled(renodx ? "(off with RenoDX)" : "(sharper moving characters)");

        SeparatorText("SDR to HDR");
        namespace RetroHdr = ::Vulkan::RetroHdr;
        const auto& retro_hdr = RetroHdr::GetSession();
        BeginDisabled(retro_hdr.state != RetroHdr::State::Active &&
                      retro_hdr.state != RetroHdr::State::Disabled);
        bool retro_hdr_on = std::atoi(settings.Get("retro_hdr", "0").c_str()) != 0;
        if (Checkbox("SDR to HDR", &retro_hdr_on))
            settings.Set("retro_hdr", retro_hdr_on ? "1" : "0");
        if (IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            SetTooltip(
                "Brightens the finished SDR image into HDR, like Special K's SDR to HDR. Works "
                "on any GPU, with frame generation.\nUnlike RenoDX it can't recover detail the "
                "game already clipped: very bright lights stay flat white,\nand the HUD is "
                "brightened too. For the best HDR, use RenoDX where it works (NVIDIA, "
                "2.1.0-pre.1 or 1.8.6).");
        SameLine();
        if (retro_hdr.state == RetroHdr::State::Active)
            TextDisabled(retro_hdr_on ? "(untick to compare with SDR)" : "(showing SDR)");
        else if (retro_hdr.state == RetroHdr::State::Disabled)
            RestartHint(retro_hdr_on, retro_hdr_on ? "(restart the game to turn it on)"
                                                   : "(Special K style HDR from the SDR image)");
        if (retro_hdr_on && retro_hdr.state == RetroHdr::State::Disabled)
            restart_needed.push_back("SDR to HDR");
        else
            TextDisabled("(%s)", RetroHdr::Describe(retro_hdr.state).data());

        int peak_override = int(RetroHdr::ParseFloat(settings.Get("retro_hdr_peak", "0"), 0.0f));
        const float peak = RetroHdr::PeakNits(float(peak_override));
        const float max_brightness = RetroHdr::MaxBrightness(peak);
        float brightness = RetroHdr::ParseFloat(settings.Get("retro_hdr_brightness", "2.00"),
                                                RetroHdr::DefaultBrightness);
        brightness = std::clamp(brightness, RetroHdr::MinBrightness, max_brightness);
        if (SliderFloat("HDR brightness", &brightness, RetroHdr::MinBrightness, max_brightness,
                        "%.2fx"))
            settings.Set("retro_hdr_brightness", fmt::format("{:.2f}", brightness));
        SameLine();
        if (Button("Default##retro_hdr_brightness"))
            settings.Set(
                "retro_hdr_brightness",
                fmt::format("{:.2f}", std::min(RetroHdr::DefaultBrightness, max_brightness)));
        TextDisabled("white %.0f nits, 18%% gray %.0f nits",
                     brightness * RetroHdr::BoostedNits(1.0f),
                     brightness * RetroHdr::BoostedNits(0.18f));
        if (SliderInt("HDR peak", &peak_override, 0, 4000,
                      peak_override < 100 ? "auto" : "%d nits"))
            settings.Set("retro_hdr_peak", std::to_string(peak_override < 100 ? 0 : peak_override));
        SameLine();
        if (Button("Default##retro_hdr_peak"))
            settings.Set("retro_hdr_peak", "0");
        SameLine();
        if (retro_hdr.windows_peak_nits >= 100.0f)
            TextDisabled("(Windows reports %.0f nits)", retro_hdr.windows_peak_nits);
        else
            TextDisabled("(no Windows value, %.0f nits used)", peak);
        EndDisabled();

        SeparatorText("Recommended game patches");
        if (!patches.file_found) {
            TextWrapped("Bloodborne patch file not found. Download patches in shadPS4 first.");
        } else {
            bool all = true;
            for (size_t i = 0; i < RecommendedPatches.size(); ++i) {
                const bool skipped =
                    patches.enabled[i] &&
                    ::Core::FileSys::BbHdrCompat::ShouldSkipPatch(RecommendedPatches[i]);
                const bool good = (patches.enabled[i] && !skipped) == Wanted(i);
                all &= good;
                TextColored(good ? ImVec4{0.4f, 1.0f, 0.4f, 1.0f} : ImVec4{1.0f, 0.75f, 0.3f, 1.0f},
                            "%s  %s%s", patches.enabled[i] && !skipped ? "on " : "off",
                            RecommendedPatches[i],
                            skipped     ? " (ignored for HDR)"
                            : Wanted(i) ? ""
                                        : " (must be off with RenoDX HDR)");
            }
            if (patches.motion_blur_disabled) {
                all = false;
                TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "on   Disable Motion Blur (must be off)");
            }
            if (!all) {
                TextWrapped("The game's own anti-aliasing, chromatic aberration and depth of field "
                            "blur the image before the upscaler sees it.");
                if (Button("Apply recommended patches")) {
                    patches_changed = EnableRecommendedPatches();
                    patch_message = patches_changed ? "Done. Restart the game to apply."
                                                    : "Could not write the patch file.";
                    patches = ReadPatches();
                }
            }
            if (patches_changed)
                RestartHint(true, patch_message.c_str());
            else if (!patch_message.empty())
                TextWrapped("%s", patch_message.c_str());
        }
        if (patches_changed)
            restart_needed.push_back("Game patches");

        // Applies at once: toggled while watching the game.
    }
    if (other_tab) {
        SeparatorText("F1 menu");
        // Its own UI scale, apart from F3's. Applied when let go, so the slider doesn't move under
        // the mouse while dragged.
        {
            float size =
                float(menu_size_edit >= 0 ? menu_size_edit : EmulatorSettings.GetBbF1Scale()) *
                0.01f;
            SetNextItemWidth(Px(200.0f));
            if (SliderFloat("UI scale", &size, 0.5f, 2.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
                menu_size_edit = int(std::lround(size * 20.0f)) * 5; // steps of 0.05
            if (IsItemDeactivated() && menu_size_edit >= 0) {
                EmulatorSettings.SetBbF1Scale(menu_size_edit);
                EmulatorSettings.Save();
                menu_size_edit = -1;
            }
            if (IsItemHovered())
                SetTooltip(
                    "The size of this menu's text and controls (F3 has its own), kept for the "
                    "next launch.\nCtrl+click to type a value.");
            SameLine();
            if (Button("Default##ui_scale")) {
                EmulatorSettings.SetBbF1Scale(100);
                EmulatorSettings.Save();
            }
        }

        SeparatorText("FPS counter");
        {
            bool fps_counter = settings.Get("fps_counter", "0") != "0";
            if (Checkbox("Show FPS counter", &fps_counter))
                settings.Set("fps_counter", fps_counter ? "1" : "0");
            BeginDisabled(!fps_counter);
            const auto part = [](const char* name, bool value, auto set) {
                if (Checkbox(name, &value)) {
                    set(value);
                    EmulatorSettings.Save();
                }
            };
            part("FPS", EmulatorSettings.IsBbCounterFps(),
                 [](bool v) { EmulatorSettings.SetBbCounterFps(v); });
            SameLine();
            part("Frame time (ms)", EmulatorSettings.IsBbCounterMs(),
                 [](bool v) { EmulatorSettings.SetBbCounterMs(v); });
            const bool upscaler = EmulatorSettings.IsBbCounterUpscaler();
            part("DLSS / FSR", upscaler, [](bool v) { EmulatorSettings.SetBbCounterUpscaler(v); });
            if (IsItemHovered())
                SetTooltip("The upscaler: DLSS, DLAA, FSR 4, FSR 3.1 Native AA...");
            BeginDisabled(!upscaler);
            Indent();
            part("Preset", EmulatorSettings.IsBbCounterPreset(),
                 [](bool v) { EmulatorSettings.SetBbCounterPreset(v); });
            if (IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                SetTooltip("DLSS's preset: K, M, L, J...");
            SameLine();
            part("Mode", EmulatorSettings.IsBbCounterMode(),
                 [](bool v) { EmulatorSettings.SetBbCounterMode(v); });
            if (IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                SetTooltip("Quality, Balanced, Performance or Ultra Performance, from the render "
                           "resolution (a percentage for sizes in between).");
            SameLine();
            part("Resolution", EmulatorSettings.IsBbCounterResolution(),
                 [](bool v) { EmulatorSettings.SetBbCounterResolution(v); });
            if (IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                SetTooltip("The output resolution the upscaler brings the image up to.");
            Unindent();
            EndDisabled();
            part("Frame time graph", EmulatorSettings.IsBbCounterGraph(),
                 [](bool v) { EmulatorSettings.SetBbCounterGraph(v); });
            if (IsItemHovered())
                SetTooltip("A line under the counter with the time each of the last 120 frames "
                           "took: flat is smooth, spikes are stutters.");
            static constexpr std::array Corners{"Top left", "Top right", "Bottom left",
                                                "Bottom right"};
            int corner = std::clamp(EmulatorSettings.GetBbCounterCorner(), 0, 3);
            SetNextItemWidth(Px(200.0f));
            if (Combo("Corner", &corner, Corners.data(), int(Corners.size()))) {
                EmulatorSettings.SetBbCounterCorner(corner);
                EmulatorSettings.Save();
            }
            EndDisabled();
        }

        SeparatorText("Keyboard & mouse");
        if (!Input::BbKbm::Active()) {
            TextWrapped(
                "Bloodborne PC controls: WASD to move, the mouse for the camera, left click to "
                "attack, all rebindable. Your current controls are kept as a backup.");
            if (Button("Use keyboard & mouse controls"))
                kbm_message = Input::BbKbm::Apply()
                                  ? "Done. The mouse turns the camera when this panel is closed."
                                  : "Could not write the input config.";
        } else {
            bool mouse_look = Input::BbKbm::MouseLookWanted();
            if (Checkbox("Mouse look", &mouse_look))
                Input::BbKbm::SetMouseLook(mouse_look);
            SameLine();
            TextDisabled("(F7 toggles it in game)");
            float sensitivity =
                std::clamp(::Vulkan::RetroHdr::ParseFloat(settings.Get("mouse_sensitivity", "1.50"),
                                                          Input::BbKbm::DefaultSensitivity),
                           Input::BbKbm::MinSensitivity, Input::BbKbm::MaxSensitivity);
            if (SliderFloat("Mouse sensitivity", &sensitivity, Input::BbKbm::MinSensitivity,
                            Input::BbKbm::MaxSensitivity, "%.2f"))
                settings.Set("mouse_sensitivity", fmt::format("{:.2f}", sensitivity));
            if (IsItemDeactivatedAfterEdit())
                Input::BbKbm::Apply();
            SameLine();
            if (Button("Default##mouse_sensitivity")) {
                settings.Set("mouse_sensitivity",
                             fmt::format("{:.2f}", Input::BbKbm::DefaultSensitivity));
                Input::BbKbm::Apply();
            }
            bool invert_y = settings.Get("mouse_invert_y", "0") == "1";
            if (Checkbox("Invert mouse Y", &invert_y))
                settings.Set("mouse_invert_y", invert_y ? "1" : "0");
            SameLine();
            bool invert_x = settings.Get("mouse_invert_x", "0") == "1";
            if (Checkbox("Invert mouse X", &invert_x))
                settings.Set("mouse_invert_x", invert_x ? "1" : "0");
            bool auto_rotation = settings.Get("camera_auto_rotation", "0") == "1";
            if (Checkbox("Camera auto-rotation (restart)", &auto_rotation))
                settings.Set("camera_auto_rotation", auto_rotation ? "1" : "0");
            if (changed("camera_auto_rotation", "0"))
                restart_needed.push_back("Camera auto-rotation");
            if (IsItemHovered())
                SetTooltip(
                    "The game turning the camera by itself as you move. Off with keyboard and "
                    "mouse: it fights\nevery mouse turn.");
            if (::Core::BbMouseCamera::Installed())
                TextDisabled("Native mouse camera: on");
            else
                TextDisabled(
                    "Native mouse camera: restart the game to turn it on (Bloodborne 1.09)");
            if (TreeNode("Key bindings")) {
                DrawKeyBindings();
                TreePop();
            }
            if (Button("Restore previous controls"))
                kbm_message = Input::BbKbm::Restore() ? "Previous controls restored."
                                                      : "Could not restore the input config.";
        }
        if (!kbm_message.empty())
            TextWrapped("%s", kbm_message.c_str());

        SeparatorText("Benchmark route");
        {
            namespace Route = ::Input::BbRoute;
            const auto mode = Route::GetMode();
            TextWrapped("Record a route once, then play it back to repeat the same movement and "
                        "measure the frame rate. Start each run from the same spot (rest at a lamp "
                        "or load the same save). Enemies can act differently each run.");
            switch (mode) {
            case Route::Mode::Idle:
                if (Button("Record route"))
                    Route::StartRecording();
                if (IsItemHovered())
                    SetTooltip(
                        "Records from when F1 is closed. Open F1 again to stop.\nThe "
                        "controller, keys and mouse camera are recorded; loading screens are "
                        "left out.");
                SameLine();
                BeginDisabled(!Route::HasRoute());
                if (Button("Play route"))
                    Route::Play();
                EndDisabled();
                if (Route::HasRoute()) {
                    SameLine();
                    TextDisabled("(%.0f s recorded)", Route::RouteSeconds());
                }
                break;
            case Route::Mode::Recording:
                TextColored(RestartColor, "Recording: %.0f s", Route::ElapsedSeconds());
                SameLine();
                if (Button("Stop recording"))
                    Route::StopRecording();
                break;
            case Route::Mode::Armed:
                TextColored(RestartColor, "Close F1 to start the replay");
                SameLine();
                if (Button("Cancel"))
                    Route::Stop();
                break;
            case Route::Mode::Playing:
                TextColored(RestartColor, "Playing: %.0f of %.0f s", Route::ElapsedSeconds(),
                            Route::RouteSeconds());
                SameLine();
                if (Button("Stop"))
                    Route::Stop();
                break;
            }
            if (const auto r = Route::LastResult(); r.valid) {
                Text("Last replay: %.1f FPS average, %.1f FPS 1%% low, worst frame %.1f ms, %u "
                     "stutters",
                     r.average_fps, r.low_fps, r.worst_ms, r.stutters);
                TextDisabled("(each replay is added to routes/results.csv in the user folder)");
            }
        }
    }
    if (graphics_tab) {
        settings.Load(); // F3 may have changed them
        SeparatorText("Textures");
        {
            static constexpr std::array AnisotropyNames{"Game default", "2x", "4x", "8x", "16x"};
            int index = BbAnisotropyIndex(EmulatorSettings.GetBbAnisotropy());
            SetNextItemWidth(Px(200.0f));
            if (Combo("Anisotropic filtering", &index, AnisotropyNames.data(),
                      int(AnisotropyNames.size()))) {
                EmulatorSettings.SetBbAnisotropy(BbAnisotropySamples(index));
                EmulatorSettings.Save();
            }
            if (IsItemHovered())
                SetTooltip("Sharper ground and stairs at an angle.");
            if (EmulatorSettings.GetBbAnisotropy() != launch.anisotropy) {
                SameLine();
                RestartHint(true, "(restart to apply)");
                restart_needed.push_back("Anisotropic filtering");
            }
            static constexpr std::array QualityNames{"Full", "High", "Medium"};
            int quality = std::clamp(EmulatorSettings.GetBbTextureQuality(), 0, 2);
            SetNextItemWidth(Px(200.0f));
            if (Combo("Texture quality", &quality, QualityNames.data(), int(QualityNames.size()))) {
                EmulatorSettings.SetBbTextureQuality(quality);
                EmulatorSettings.Save();
            }
            if (IsItemHovered())
                SetTooltip("Less VRAM for big textures (the BB Remaster mod's) by leaving out "
                           "their sharpest level:\nHigh: textures over 2048, Medium: over 1024 "
                           "(about 500 MB less in busy areas).\nApplies to textures loaded after "
                           "a change: from the next loading screen.");
        }

        SeparatorText("Ambient occlusion");
        {
            bool vbao = EmulatorSettings.IsBbVbao();
            if (Checkbox("Better ambient occlusion (GT-VBAO)", &vbao)) {
                EmulatorSettings.SetBbVbao(vbao);
                EmulatorSettings.Save();
            }
            if (IsItemHovered())
                SetTooltip("The game's ambient occlusion replaced with GT-VBAO: deeper, cleaner "
                           "contact shadows in corners,\nunder objects and in carvings, without "
                           "the game's dark halos around thin things.");
            BeginDisabled(!vbao);
            // Stored as hundredths; saved when the slider is let go.
            const auto slider = [](const char* name, int value, auto set) {
                float shown = float(value) * 0.01f;
                SetNextItemWidth(Px(200.0f));
                if (SliderFloat(name, &shown, 0.25f, 4.0f, "%.2f"))
                    set(int(std::lround(shown * 100.0f)));
                if (IsItemDeactivatedAfterEdit())
                    EmulatorSettings.Save();
            };
            slider("Strength", EmulatorSettings.GetBbVbaoStrength(),
                   [](int v) { EmulatorSettings.SetBbVbaoStrength(v); });
            if (IsItemHovered())
                SetTooltip("How dark occluded places get (1.50 by default).");
            slider("Reach (metres)", EmulatorSettings.GetBbVbaoRadius(),
                   [](int v) { EmulatorSettings.SetBbVbaoRadius(v); });
            if (IsItemHovered())
                SetTooltip("How far away something can be and still shade a surface (1.50 by "
                           "default).");
            bool half_res = EmulatorSettings.IsBbVbaoHalfRes();
            if (Checkbox("Half resolution (faster)", &half_res)) {
                EmulatorSettings.SetBbVbaoHalfRes(half_res);
                EmulatorSettings.Save();
            }
            if (IsItemHovered())
                SetTooltip(
                    "Computed at half the scene's size and upscaled along the edges: about a "
                    "quarter of the cost,\nslightly softer fine detail.");
            if (Button("Defaults")) {
                EmulatorSettings.SetBbVbaoHalfRes(true);
                EmulatorSettings.SetBbVbaoStrength(150);
                EmulatorSettings.SetBbVbaoRadius(150);
                EmulatorSettings.Save();
            }
            EndDisabled();
        }

        if (ShowReflections) {
            SeparatorText("Reflections");
            {
                static constexpr std::array<const char*, 3> modes = {"Game's", "Better", "Off"};
                const int mode = std::clamp(EmulatorSettings.GetBbSsrMode(), 0, 2);
                SetNextItemWidth(Px(200.0f));
                if (BeginCombo("Screen space reflections", modes[mode])) {
                    for (int i = 0; i < int(modes.size()); ++i) {
                        if (Selectable(modes[i], i == mode)) {
                            EmulatorSettings.SetBbSsrMode(i);
                            EmulatorSettings.Save();
                        }
                    }
                    EndCombo();
                }
                if (IsItemHovered())
                    SetTooltip(
                        "Built in, no patch needed. Game's: as the game draws them.\nBetter: "
                        "traced more precisely, sharper, to the screen's edges. Off: none "
                        "at all, no cost.\nSwitching between Off and on needs a restart.");
                if (::Core::BbGameFixes::SsrNeedsRestart(mode)) {
                    SameLine();
                    TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "restart to apply");
                } else if (::Core::BbGameFixes::GetSsrPatch() ==
                           ::Core::BbGameFixes::SsrPatch::Unavailable) {
                    SameLine();
                    TextDisabled("not available (needs game version 1.09)");
                }
                BeginDisabled(mode != 1);
                bool full_res = EmulatorSettings.IsBbSsrFullRes();
                if (Checkbox("Full resolution", &full_res)) {
                    EmulatorSettings.SetBbSsrFullRes(full_res);
                    EmulatorSettings.Save();
                }
                if (IsItemHovered())
                    SetTooltip(
                        "Better reflections at the full render size instead of the game's half "
                        "size: a little sharper,\nat more GPU time.");
                if constexpr (ShowReflectionTuning) {
                    bool rough_rays = EmulatorSettings.IsBbSsrRoughRays();
                    if (Checkbox("Sharp reflections (test)", &rough_rays)) {
                        EmulatorSettings.SetBbSsrRoughRays(rough_rays);
                        EmulatorSettings.Save();
                    }
                    if (IsItemHovered())
                        SetTooltip(
                            "No fade towards the screen's edges, no blur between neighbouring "
                            "pixels, and a short history\n(less smearing). Changes at once.");
                    BeginDisabled(!rough_rays || !full_res);
                    float min_gloss = float(EmulatorSettings.GetBbSsrMinGloss()) * 0.01f;
                    SetNextItemWidth(Px(200.0f));
                    if (SliderFloat("Minimum gloss", &min_gloss, 0.0f, 1.0f, "%.2f"))
                        EmulatorSettings.SetBbSsrMinGloss(int(std::lround(min_gloss * 100.0f)));
                    if (IsItemDeactivatedAfterEdit())
                        EmulatorSettings.Save();
                    if (IsItemHovered())
                        SetTooltip(
                            "Only surfaces at least this glossy reflect (the Gloss debug view: "
                            "puddles about 0.8, metal about 0.5,\ndry stone near 0). Higher: "
                            "fewer, steadier reflections and less GPU time. Sharp at full "
                            "resolution only.");
                    EndDisabled();
                    BeginDisabled(!rough_rays);
                    int frames = std::clamp(EmulatorSettings.GetBbSsrSharpFrames(), 2, 16);
                    SetNextItemWidth(Px(200.0f));
                    if (SliderInt("Steadiness (frames)", &frames, 2, 16))
                        EmulatorSettings.SetBbSsrSharpFrames(frames);
                    if (IsItemDeactivatedAfterEdit())
                        EmulatorSettings.Save();
                    if (IsItemHovered())
                        SetTooltip(
                            "How many frames the reflections are averaged over (8 by default). "
                            "More: less flicker, but more\nsmearing when the camera moves. "
                            "Sharp reflections only.");
                    EndDisabled();
                    constexpr std::array DebugNames{"Off", "All magenta", "Hits magenta", "Gloss",
                                                    "Raw (no denoiser)"};
                    int debug = std::clamp(EmulatorSettings.GetBbSsrDebug(), 0, 4);
                    if (Combo("Reflections debug", &debug, DebugNames.data(),
                              int(DebugNames.size()))) {
                        EmulatorSettings.SetBbSsrDebug(debug); // not saved
                    }
                    if (IsItemHovered())
                        SetTooltip(
                            "All magenta: every pixel's reflection is bright magenta, so the "
                            "picture shows where the game's lighting\nuses reflections and how "
                            "strongly. Hits magenta: only where a ray hit something. Gloss: shiny "
                            "green, rough red.\nRaw: the reflections as traced, without "
                            "the denoiser. Off when the game restarts.");
                }
                EndDisabled();
            }
        }

        // Down here on purpose: hardly anyone wants it back.
        SeparatorText("Game effects");
        bool motion_blur = settings.Get("motion_blur", "0") != "0";
        if (Checkbox("Game motion blur", &motion_blur))
            settings.Set("motion_blur", motion_blur ? "1" : "0");
        SameLine();
        TextDisabled("(off = sharper in motion)");
        // Debugging tools, with F3's advanced settings shown.
        if (EmulatorSettings.IsBbShowThreadedSettings()) {
            // The whole picture but the character blurred (after a respawn): every input of the
            // game's motion blur passes goes to the log, to compare with a frame that looks right.
            if (Button("Log motion blur inputs"))
                Common::GuestStats::blur_dump_requested.store(true, std::memory_order_relaxed);
            SameLine();
            TextDisabled("(press while the picture is blurred, and once when it looks right)");
            // A texture that looks wrong: uploaded again from game memory. If it then looks right,
            // the emulator's copy was out of date (the log lists textures still waiting for their
            // largest level).
            if (Button("Reload textures from game memory"))
                Common::GuestStats::texture_reload_requested.store(true, std::memory_order_relaxed);
            SameLine();
            TextDisabled("(press while a texture looks wrong)");
        }
    }
    EndChild();
    restart_footer = !restart_needed.empty();
    if (restart_footer) {
        Separator();
        TextColored(RestartColor, "Applies after a restart: %s",
                    fmt::format("{}", fmt::join(restart_needed, ", ")).c_str());
        RestartButton();
    }
    End();
}

// shadPS4's name for an ImGui key, empty if it can't be bound (F keys stay hotkeys).
std::string KeyName(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return std::string(1, char('a' + (key - ImGuiKey_A)));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return std::string(1, char('0' + (key - ImGuiKey_0)));
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
        return fmt::format("kp{}", int(key - ImGuiKey_Keypad0));
    switch (key) {
    case ImGuiKey_Space:
        return "space";
    case ImGuiKey_Tab:
        return "tab";
    case ImGuiKey_Enter:
        return "enter";
    case ImGuiKey_Escape:
        return "escape";
    case ImGuiKey_Backspace:
        return "backspace";
    case ImGuiKey_CapsLock:
        return "capslock";
    case ImGuiKey_LeftShift:
        return "lshift";
    case ImGuiKey_RightShift:
        return "rshift";
    case ImGuiKey_LeftCtrl:
        return "lctrl";
    case ImGuiKey_RightCtrl:
        return "rctrl";
    case ImGuiKey_LeftAlt:
        return "lalt";
    case ImGuiKey_RightAlt:
        return "ralt";
    case ImGuiKey_UpArrow:
        return "up";
    case ImGuiKey_DownArrow:
        return "down";
    case ImGuiKey_LeftArrow:
        return "left";
    case ImGuiKey_RightArrow:
        return "right";
    case ImGuiKey_Insert:
        return "insert";
    case ImGuiKey_Home:
        return "home";
    case ImGuiKey_End:
        return "end";
    case ImGuiKey_PageUp:
        return "pgup";
    case ImGuiKey_PageDown:
        return "pgdown";
    case ImGuiKey_Minus:
        return "minus";
    case ImGuiKey_Equal:
        return "equals";
    case ImGuiKey_LeftBracket:
        return "lbracket";
    case ImGuiKey_RightBracket:
        return "rbracket";
    case ImGuiKey_Backslash:
        return "backslash";
    case ImGuiKey_Semicolon:
        return "semicolon";
    case ImGuiKey_Apostrophe:
        return "apostrophe";
    case ImGuiKey_Comma:
        return "comma";
    case ImGuiKey_Period:
        return "period";
    case ImGuiKey_Slash:
        return "slash";
    case ImGuiKey_GraveAccent:
        return "grave";
    default:
        return {};
    }
}

void DlssLayer::DrawKeyBindings() {
    const auto actions = Input::BbKbm::Actions();
    // Waiting for an input: the first key or mouse button pressed after the click.
    if (capture_action >= 0 && ++capture_frames > 2) {
        const auto& action = actions[capture_action];
        auto binding = Input::BbKbm::GetBinding(action);
        bool done = false;
        if (capture_slot == 0) {
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END && !done; ++key) {
                if (!IsKeyPressed(ImGuiKey(key), false))
                    continue;
                const std::string name = KeyName(ImGuiKey(key));
                if (name.empty())
                    continue;
                binding.key = name;
                done = true;
            }
        } else {
            static constexpr const char* Buttons[] = {"leftbutton", "rightbutton", "middlebutton",
                                                      "sidebuttonback", "sidebuttonforward"};
            for (int button = 0; button < 5 && !done; ++button) {
                if (IsMouseClicked(button)) {
                    binding.mouse = Buttons[button];
                    done = true;
                }
            }
            const float wheel = GetIO().MouseWheel;
            if (!done && wheel != 0.0f) {
                binding.mouse = wheel > 0.0f ? "mousewheelup" : "mousewheeldown";
                done = true;
            }
        }
        if (done) {
            Input::BbKbm::SetBinding(action, binding);
            Input::BbKbm::Apply();
            capture_action = -1;
        }
    }

    TextDisabled("Click a cell, then press the key or mouse button. Right-click a cell to clear "
                 "it.");
    if (BeginTable("kbm_bindings", 3,
                   ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                       ImGuiTableFlags_SizingStretchProp)) {
        TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        TableSetupColumn("Mouse", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        TableHeadersRow();
        for (int i = 0; i < int(actions.size()); ++i) {
            const auto& action = actions[i];
            const auto binding = Input::BbKbm::GetBinding(action);
            TableNextRow();
            TableNextColumn();
            TextUnformatted(action.label);
            for (int slot = 0; slot < 2; ++slot) {
                TableNextColumn();
                const bool waiting = capture_action == i && capture_slot == slot;
                const std::string label =
                    waiting ? std::string(slot == 0 ? "press a key" : "press a button")
                            : Input::BbKbm::DisplayName(slot == 0 ? binding.key : binding.mouse);
                PushID(i * 2 + slot);
                if (Button(label.c_str(), {-FLT_MIN, 0.0f})) {
                    capture_action = waiting ? -1 : i;
                    capture_slot = slot;
                    capture_frames = 0;
                }
                if (IsItemClicked(ImGuiMouseButton_Right)) {
                    auto cleared = binding;
                    (slot == 0 ? cleared.key : cleared.mouse).clear();
                    Input::BbKbm::SetBinding(action, cleared);
                    Input::BbKbm::Apply();
                    capture_action = -1;
                }
                PopID();
            }
        }
        EndTable();
    }
    if (Button("Default keys")) {
        Input::BbKbm::ResetBindings();
        Input::BbKbm::Apply();
        capture_action = -1;
    }
}

} // namespace

void Register() {
    if (!layer.registered) {
        ImGui::Layer::AddLayer(&layer);
        ImGui::Layer::AddLayer(&counter);
        layer.registered = true;
    }
}

void Unregister() {
    if (layer.registered) {
        ImGui::Layer::RemoveLayer(&counter);
        ImGui::Layer::RemoveLayer(&layer);
        layer.registered = false;
    }
}

void Toggle() {
    layer.toggle_requested = true;
}

void SetGameArea(float x, float y, float width, float height) {
    game_x = x;
    game_y = y;
    game_w = width;
    game_h = height;
}

} // namespace ImGui::Dlss
