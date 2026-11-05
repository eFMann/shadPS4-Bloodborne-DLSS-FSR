//  SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <map>
#include <ranges>
#include <utility>
#include <ImGuiFileDialog.h>
#include <SDL3/SDL_events.h>
#include <cmrc/cmrc.hpp>
#include <stb_image.h>

#include "common/copy_pool.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/render_affinity.h"
#include "core/bb_game_fixes.h"
#include "core/bb_resolution.h"
#include "core/cpu_patches.h"
#include "core/devtools/layer.h"
#include "imgui/bb_settings_file.h"
#include "imgui/imgui_std.h"
#include "input/input_handler.h"
#include "settings_dialog_imgui.h"

CMRC_DECLARE(res);

constexpr float gameImageSize = 200.f;
constexpr float settingsIconSize = 125.f;

namespace ImGuiEmuSettings {

int SettingsWindow::GetComboIndex(std::string selection, std::vector<std::string> options) {
    for (int i = 0; i < options.size(); i++) {
        if (selection == options[i])
            return i;
    }

    return 0;
}

// The restart prompt's starting values, by profile, kept for the whole run (F3 can be opened
// and closed many times while the game runs).
static std::map<std::string, std::vector<std::pair<const char*, int>>>& RestartSnapshots() {
    static std::map<std::string, std::vector<std::pair<const char*, int>>> snapshots;
    return snapshots;
}

void SettingsWindow::LoadSettings(std::string profile) {
    // In game: on a copy, applied in one step (see EmulatorSettingsImpl::ReloadScope).
    const EmulatorSettingsImpl::ReloadScope reload;
    // Global-only settings
    EmulatorSettings.Load();
    scanDepthSetting = EmulatorSettings.GetBigPictureFolderDepth();

    const bool isSpecific = currentProfile != "Global";
    if (isSpecific) {
        EmulatorSettings.Load(profile);
    }

    /////////// General Tab
    int languageIndex = EmulatorSettings.GetConsoleLanguage();
    std::string language;
    for (const auto& [key, value] : languageMap) {
        if (value == languageIndex) {
            language = key;
        }
    }
    consoleLanguageSetting = GetComboIndex(language, languageOptions);
    volumeSetting = EmulatorSettings.GetVolumeSlider();
    showSplashSetting = EmulatorSettings.IsShowSplash();
    audioBackendSetting = EmulatorSettings.GetAudioBackend();

    /////////// Graphics Tab
    fullscreenModeSetting =
        GetComboIndex(EmulatorSettings.GetFullScreenMode(), fullscreenModeOptions);
    presentModeSetting = GetComboIndex(EmulatorSettings.GetPresentMode(), presentModeOptions);
    windowHeightSetting = EmulatorSettings.GetWindowHeight();
    windowWidthSetting = EmulatorSettings.GetWindowWidth();
    hdrAllowedSetting = EmulatorSettings.IsHdrAllowed();
    fsrEnabledSetting = EmulatorSettings.IsFsrEnabled();
    rcasEnabledSetting = EmulatorSettings.IsRcasEnabled();
    rcasAttenuationSetting = static_cast<float>(EmulatorSettings.GetRcasAttenuation() * 0.001f);
    threadedRendererSetting = EmulatorSettings.IsThreadedRenderer();
    threadedRendererDiagnosticsSetting = EmulatorSettings.IsThreadedRendererDiagnostics();
    threadedRendererGpuProfileSetting = EmulatorSettings.IsThreadedRendererGpuProfile();
    bbReadbackQueueSetting = EmulatorSettings.IsBbReadbackQueue();
    bbStreamingSetting = EmulatorSettings.IsBbStreaming();
    bbMergedCopiesSetting = EmulatorSettings.IsBbMergedCopies();
    bbMergedNoStreamSourcesSetting = EmulatorSettings.IsBbMergedNoStreamSources();
    bbMergedLargeBatchesSetting = EmulatorSettings.IsBbMergedLargeBatches();
    bbFastLocksSetting = EmulatorSettings.IsBbFastLocks();
    bbFlipWhenReadySetting = EmulatorSettings.IsBbFlipWhenReady();
    bbVramReclaimSetting = EmulatorSettings.IsBbVramReclaim();
    bbBufferEvictionSetting = EmulatorSettings.IsBbBufferEviction();
    bbLoadScreenCleanupSetting = EmulatorSettings.IsBbLoadScreenCleanup();
    bbMoreVramSetting = EmulatorSettings.IsBbMoreVram();
    bbVramPastBudgetSetting = EmulatorSettings.IsBbVramPastBudget();
    bbBackgroundTextureCopiesSetting = EmulatorSettings.IsBbBackgroundTextureCopies();
    bbGeometryInPlaceSetting = EmulatorSettings.IsBbGeometryInPlace();
    bbSkipEmptyDrawsSetting = EmulatorSettings.IsBbSkipEmptyDraws();
    bbBackgroundPipelinesSetting = EmulatorSettings.IsBbBackgroundPipelines();
    bbExactProtectionSetting = EmulatorSettings.IsBbExactProtection();
    bbBatchedUploadsSetting = EmulatorSettings.IsBbBatchedUploads();
    bbTextureSetMemoSetting = EmulatorSettings.IsBbTextureSetMemo();
    bbRingGeometrySetting = EmulatorSettings.IsBbRingGeometry();
    bbSubmitOverlapSetting = EmulatorSettings.IsBbSubmitOverlap();
    bbOverlapFramesSetting = EmulatorSettings.IsBbOverlapFrames();
    bbEarlyWritebackSetting = EmulatorSettings.IsBbEarlyWriteback();
    bbOcclusionInOrderSetting = EmulatorSettings.IsBbOcclusionInOrder();
    bbFastPixelMathSetting = EmulatorSettings.IsBbFastPixelMath();
    bbConstantsNoWaitSetting = EmulatorSettings.IsBbConstantsNoWait();
    bbHardwareInterpolationSetting = EmulatorSettings.IsBbHardwareInterpolation();
    bbRealOcclusionSetting = EmulatorSettings.IsBbRealOcclusion();
    bbCopyThreadSetting = EmulatorSettings.IsBbCopyThread();
    bbRenderDuringPresentSetting = EmulatorSettings.IsBbRenderDuringPresent();
    bbSkipLeftOutLevelsSetting = EmulatorSettings.IsBbSkipLeftOutLevels();
    bbDirectTextureCopiesSetting = EmulatorSettings.IsBbDirectTextureCopies();
    {
        const int threads = EmulatorSettings.GetBbCopyThreads();
        bbCopyThreadsSetting = threads >= 8 ? 3 : threads >= 6 ? 2 : threads >= 4 ? 1 : 0;
    }
    bbCachedTextureStagingSetting = EmulatorSettings.IsBbCachedTextureStaging();
    renderdocSetting = EmulatorSettings.IsRenderdocEnabled();
    bbRdocCaptureFramesSetting = EmulatorSettings.GetBbRdocCaptureFrames();
    bbLowerDetailFirstSetting = EmulatorSettings.IsBbLowerDetailFirst();
    bbClearShaderWrittenSetting = EmulatorSettings.IsBbClearShaderWritten();
    bbCeSkipHugeWritesSetting = EmulatorSettings.IsBbCeSkipHugeWrites();
    bbEvictKeepProtectionSetting = EmulatorSettings.IsBbEvictKeepProtection();
    bbEvictKeepReturningSetting = EmulatorSettings.IsBbEvictKeepReturning();
    bbProtectHelperThreadSetting = EmulatorSettings.IsBbProtectHelperThread();
    bbCpuCopyShaderSetting = EmulatorSettings.IsBbCpuCopyShader();
    bbCopySkipSameSetting = EmulatorSettings.IsBbCopySkipSame();
    bbClearOneLayerSetting = EmulatorSettings.IsBbClearOneLayer();
    bbSpreadLoadsSetting = EmulatorSettings.IsBbSpreadLoads();
    bbDrawDataMemorySetting = std::clamp(EmulatorSettings.GetBbDrawDataMemory(), 0, 2);
    bbIgnoreDisplayScaleSetting = EmulatorSettings.IsBbIgnoreDisplayScale();
    bbStutterDetailSetting = EmulatorSettings.IsBbStutterDetail();
    bbVramBudgetTestSetting = EmulatorSettings.GetBbVramBudgetTestMb();

    /////////// Input Tab
    motionControlsSetting = EmulatorSettings.IsMotionControlsEnabled();
    backgroundControllerSetting = EmulatorSettings.IsBackgroundControllerInput();
    cursorStateSetting = EmulatorSettings.GetCursorState();
    cursorTimeoutSetting = EmulatorSettings.GetCursorHideTimeout();

    /////////// Trophy Tab
    trophyPopupDisabledSetting = EmulatorSettings.IsTrophyPopupDisabled();
    trophySideSetting =
        GetComboIndex(EmulatorSettings.GetTrophyNotificationSide(), trophySideOptions);
    trophyDurationSetting = static_cast<float>(EmulatorSettings.GetTrophyNotificationDuration());

    /////////// Log Tab
    logEnableSetting = EmulatorSettings.IsLogEnable();
    logSeparateSetting = EmulatorSettings.IsLogSeparate();
    logSyncSetting = EmulatorSettings.IsLogSync();

    /////////// Experimental Tab
    if (isSpecific) {
        readbacksModeSetting = EmulatorSettings.GetReadbacksMode();
        readbackLinearImagesSetting = EmulatorSettings.IsReadbackLinearImagesEnabled();
        directMemoryAccessSetting = EmulatorSettings.IsDirectMemoryAccessEnabled();
        windowsGuestRedZoneProtectionModeSetting = EmulatorSettings.IsRedZonePatchingEnabled();
        devkitConsoleSetting = EmulatorSettings.IsDevKit();
        neoModeSetting = EmulatorSettings.IsNeo();
        shadnetEnabledSetting = EmulatorSettings.IsShadNetEnabledSetting();
        connectedNetworkSetting = EmulatorSettings.IsConnectedToNetwork();
        pipelineCacheEnabledSetting = EmulatorSettings.IsPipelineCacheEnabled();
        pipelineCacheArchiveSetting = EmulatorSettings.IsPipelineCacheArchived();
        extraDmemSetting = EmulatorSettings.GetExtraDmemInMBytes();
        vblankFrequencySetting = EmulatorSettings.GetVblankFrequency();
    }
    // What the running game started with, for the restart prompt: this profile's values the
    // first time F3 loads it in this run (later loads may hold changes saved since).
    auto& started = RestartSnapshots()[profile];
    if (started.empty()) {
        started = RestartValues();
    }
}

// Settings that only apply when the game starts: their values now, named as in the menu.
std::vector<std::pair<const char*, int>> SettingsWindow::RestartValues() const {
    return {
        // The main part switches at once, but the GPU readback queue, the kept upload memory and
        // overlapped frames are set up at a start: a restart gives all of it.
        {"Threaded Renderer", threadedRendererSetting},
        {"Resizable BAR / Smart Access Memory", bbDrawDataMemorySetting},
        {"Faster Windows locks and sleeps", bbFastLocksSetting},
        {"Overlap game and GPU frames", bbOverlapFramesSetting},
        {"Draw data copy thread", bbCopyThreadSetting},
        {"Fast pixel shader math", bbFastPixelMathSetting},
        {"GPU interpolation where safe", bbHardwareInterpolationSetting},
        {"GPU readbacks on the copy queue", bbReadbackQueueSetting},
        {"Copy threads", bbCopyThreadsSetting},
        {"VRAM cleanup", bbVramReclaimSetting},
        {"Also free idle buffers", bbBufferEvictionSetting},
        {"Go past the budget", bbVramPastBudgetSetting},
        {"Faster pipeline lookup (1.09)", EmulatorSettings.IsBbGxPipelines()},
        {"Windows Guest Red Zone Protection", windowsGuestRedZoneProtectionModeSetting},
    };
}

void SettingsWindow::SaveSettings(std::string profile) {
    const bool isSpecific = currentProfile != "Global";
    // What this save changes, for the log (a save in game has crashed the GPU: which setting).
    const auto settings_before = EmulatorSettings.Snapshot();
    // The display mode before: it is applied again only when it changed.
    const bool fullscreenBefore = EmulatorSettings.IsFullScreen();
    const std::string fullscreenModeBefore = EmulatorSettings.GetFullScreenMode();

    /////////// General Tab
    EmulatorSettings.SetConsoleLanguage(languageMap.at(languageOptions.at(consoleLanguageSetting)),
                                        isSpecific);
    EmulatorSettings.SetVolumeSlider(volumeSetting, isSpecific);
    EmulatorSettings.SetShowSplash(showSplashSetting, isSpecific);
    EmulatorSettings.SetAudioBackend(audioBackendSetting, isSpecific);

    /////////// Graphics Tab
    bool isFullscreen = fullscreenModeSetting != 0;
    EmulatorSettings.SetFullScreen(isFullscreen, isSpecific);
    EmulatorSettings.SetFullScreenMode(fullscreenModeOptions.at(fullscreenModeSetting), isSpecific);
    EmulatorSettings.SetPresentMode(presentModeOptions.at(presentModeSetting), isSpecific);
    EmulatorSettings.SetWindowHeight(windowHeightSetting, isSpecific);
    EmulatorSettings.SetWindowWidth(windowWidthSetting, isSpecific);
    EmulatorSettings.SetHdrAllowed(hdrAllowedSetting, isSpecific);
    EmulatorSettings.SetFsrEnabled(fsrEnabledSetting, isSpecific);
    EmulatorSettings.SetRcasEnabled(rcasEnabledSetting, isSpecific);
    EmulatorSettings.SetRcasAttenuation(static_cast<int>(rcasAttenuationSetting * 1000),
                                        isSpecific);
    EmulatorSettings.SetThreadedRenderer(threadedRendererSetting, isSpecific);
    EmulatorSettings.SetThreadedRendererDiagnostics(threadedRendererDiagnosticsSetting, isSpecific);
    EmulatorSettings.SetThreadedRendererGpuProfile(threadedRendererGpuProfileSetting, isSpecific);
    EmulatorSettings.SetBbReadbackQueue(bbReadbackQueueSetting, isSpecific);
    EmulatorSettings.SetBbStreaming(bbStreamingSetting, isSpecific);
    EmulatorSettings.SetBbMergedCopies(bbMergedCopiesSetting, isSpecific);
    EmulatorSettings.SetBbMergedNoStreamSources(bbMergedNoStreamSourcesSetting, isSpecific);
    EmulatorSettings.SetBbMergedLargeBatches(bbMergedLargeBatchesSetting, isSpecific);
    EmulatorSettings.SetBbFastLocks(bbFastLocksSetting, isSpecific);
    EmulatorSettings.SetBbFlipWhenReady(bbFlipWhenReadySetting, isSpecific);
    EmulatorSettings.SetBbVramReclaim(bbVramReclaimSetting, isSpecific);
    EmulatorSettings.SetBbBufferEviction(bbBufferEvictionSetting, isSpecific);
    EmulatorSettings.SetBbLoadScreenCleanup(bbLoadScreenCleanupSetting, isSpecific);
    EmulatorSettings.SetBbMoreVram(bbMoreVramSetting, isSpecific);
    EmulatorSettings.SetBbVramPastBudget(bbVramPastBudgetSetting, isSpecific);
    EmulatorSettings.SetBbBackgroundTextureCopies(bbBackgroundTextureCopiesSetting, isSpecific);
    EmulatorSettings.SetBbGeometryInPlace(bbGeometryInPlaceSetting, isSpecific);
    EmulatorSettings.SetBbSkipEmptyDraws(bbSkipEmptyDrawsSetting, isSpecific);
    EmulatorSettings.SetBbBackgroundPipelines(bbBackgroundPipelinesSetting, isSpecific);
    EmulatorSettings.SetBbExactProtection(bbExactProtectionSetting, isSpecific);
    EmulatorSettings.SetBbBatchedUploads(bbBatchedUploadsSetting, isSpecific);
    EmulatorSettings.SetBbTextureSetMemo(bbTextureSetMemoSetting, isSpecific);
    EmulatorSettings.SetBbRingGeometry(bbRingGeometrySetting, isSpecific);
    EmulatorSettings.SetBbSubmitOverlap(bbSubmitOverlapSetting, isSpecific);
    EmulatorSettings.SetBbOverlapFrames(bbOverlapFramesSetting, isSpecific);
    EmulatorSettings.SetBbEarlyWriteback(bbEarlyWritebackSetting, isSpecific);
    EmulatorSettings.SetBbOcclusionInOrder(bbOcclusionInOrderSetting, isSpecific);
    EmulatorSettings.SetBbFastPixelMath(bbFastPixelMathSetting, isSpecific);
    EmulatorSettings.SetBbConstantsNoWait(bbConstantsNoWaitSetting, isSpecific);
    EmulatorSettings.SetBbHardwareInterpolation(bbHardwareInterpolationSetting, isSpecific);
    EmulatorSettings.SetBbRealOcclusion(bbRealOcclusionSetting, isSpecific);
    EmulatorSettings.SetBbCopyThread(bbCopyThreadSetting, isSpecific);
    EmulatorSettings.SetBbRenderDuringPresent(bbRenderDuringPresentSetting, isSpecific);
    EmulatorSettings.SetBbSkipLeftOutLevels(bbSkipLeftOutLevelsSetting, isSpecific);
    EmulatorSettings.SetBbDirectTextureCopies(bbDirectTextureCopiesSetting, isSpecific);
    EmulatorSettings.SetBbCopyThreads(
        std::array{0, 4, 6, 8}[std::clamp(bbCopyThreadsSetting, 0, 3)], isSpecific);
    EmulatorSettings.SetBbCachedTextureStaging(bbCachedTextureStagingSetting, isSpecific);
    EmulatorSettings.SetRenderdocEnabled(renderdocSetting, isSpecific);
    EmulatorSettings.SetBbRdocCaptureFrames(std::clamp(bbRdocCaptureFramesSetting, 1, 120),
                                            isSpecific);
    EmulatorSettings.SetBbLowerDetailFirst(bbLowerDetailFirstSetting, isSpecific);
    EmulatorSettings.SetBbClearShaderWritten(bbClearShaderWrittenSetting, isSpecific);
    EmulatorSettings.SetBbCeSkipHugeWrites(bbCeSkipHugeWritesSetting, isSpecific);
    EmulatorSettings.SetBbEvictKeepProtection(bbEvictKeepProtectionSetting, isSpecific);
    EmulatorSettings.SetBbEvictKeepReturning(bbEvictKeepReturningSetting, isSpecific);
    EmulatorSettings.SetBbProtectHelperThread(bbProtectHelperThreadSetting, isSpecific);
    EmulatorSettings.SetBbCpuCopyShader(bbCpuCopyShaderSetting, isSpecific);
    EmulatorSettings.SetBbCopySkipSame(bbCopySkipSameSetting, isSpecific);
    EmulatorSettings.SetBbClearOneLayer(bbClearOneLayerSetting, isSpecific);
    EmulatorSettings.SetBbSpreadLoads(bbSpreadLoadsSetting, isSpecific);
    EmulatorSettings.SetBbDrawDataMemory(bbDrawDataMemorySetting, isSpecific);
    EmulatorSettings.SetBbIgnoreDisplayScale(bbIgnoreDisplayScaleSetting, isSpecific);
    EmulatorSettings.SetBbStutterDetail(bbStutterDetailSetting, isSpecific);
    EmulatorSettings.SetBbVramBudgetTestMb(bbVramBudgetTestSetting, isSpecific);

    /////////// Input Tab
    EmulatorSettings.SetMotionControlsEnabled(motionControlsSetting, isSpecific);
    EmulatorSettings.SetBackgroundControllerInput(backgroundControllerSetting, isSpecific);
    EmulatorSettings.SetCursorState(cursorStateSetting, isSpecific);
    EmulatorSettings.SetCursorHideTimeout(cursorTimeoutSetting, isSpecific);

    /////////// Trophy Tab
    EmulatorSettings.SetTrophyPopupDisabled(trophyPopupDisabledSetting, isSpecific);
    EmulatorSettings.SetTrophyNotificationSide(trophySideOptions.at(trophySideSetting), isSpecific);
    EmulatorSettings.SetTrophyNotificationDuration(static_cast<double>(trophyDurationSetting));

    /////////// Log Tab
    EmulatorSettings.SetLogEnable(logEnableSetting, isSpecific);
    EmulatorSettings.SetLogSeparate(logSeparateSetting, isSpecific);
    EmulatorSettings.SetLogSync(logSyncSetting, isSpecific);

    /////////// Experimental Tab
    if (isSpecific) {
        EmulatorSettings.SetReadbacksMode(readbacksModeSetting, true);
        EmulatorSettings.SetReadbackLinearImagesEnabled(readbackLinearImagesSetting, true);
        EmulatorSettings.SetDirectMemoryAccessEnabled(directMemoryAccessSetting, true);
        // Windows static guest red-zone protection
        EmulatorSettings.SetRedZonePatchingEnabled(windowsGuestRedZoneProtectionModeSetting, true);
        EmulatorSettings.SetDevKit(devkitConsoleSetting, true);
        EmulatorSettings.SetNeo(neoModeSetting, true);
        EmulatorSettings.SetShadNetEnabled(shadnetEnabledSetting, true);
        EmulatorSettings.SetConnectedToNetwork(connectedNetworkSetting, true);
        EmulatorSettings.SetPipelineCacheEnabled(pipelineCacheEnabledSetting, true);
        EmulatorSettings.SetPipelineCacheArchived(pipelineCacheArchiveSetting, true);
        EmulatorSettings.SetExtraDmemInMBytes(extraDmemSetting, true);
        EmulatorSettings.SetVblankFrequency(vblankFrequencySetting, true);
    }

    {
        const auto settings_after = EmulatorSettings.Snapshot();
        u32 changes = 0;
        for (const auto& [layer, sections] : settings_after.items()) {
            for (const auto& [section, keys] : sections.items()) {
                if (!keys.is_object()) {
                    continue;
                }
                for (const auto& [key, value] : keys.items()) {
                    const auto& old_layer = settings_before[layer];
                    const bool had = old_layer.contains(section) &&
                                     old_layer[section].is_object() &&
                                     old_layer[section].contains(key);
                    if (!had || old_layer[section][key] != value) {
                        LOG_INFO(Config, "Settings save changes {} {}.{}: {} -> {}", layer, section,
                                 key, had ? old_layer[section][key].dump() : "(none)",
                                 value.dump());
                        ++changes;
                    }
                }
            }
        }
        LOG_INFO(Config, "Settings save: {} changed ({} profile)", changes,
                 isSpecific ? "game" : "global");
    }
    isSpecific ? EmulatorSettings.Save(profile) : EmulatorSettings.Save();

    // Switch the open window to the chosen display mode now rather than at the next launch, only
    // when it changed: applying it again switches the screen's mode in exclusive fullscreen while
    // the game renders (the GPU device was lost on saving other settings in game).
    if (EmulatorSettings.IsFullScreen() != fullscreenBefore ||
        EmulatorSettings.GetFullScreenMode() != fullscreenModeBefore) {
        SDL_Event event{};
        event.type = SDL_EVENT_APPLY_DISPLAY_MODE;
        SDL_PushEvent(&event);
    }
}

void SettingsWindow::SaveInstallDirs() {
    // In game: on a copy, applied in one step (see EmulatorSettingsImpl::ReloadScope).
    const EmulatorSettingsImpl::ReloadScope reload;
    std::string profile;
    const bool isGlobal = currentProfile == "Global";
    if (!isGlobal) {
        profile = currentProfile.substr(0, 9);
        EmulatorSettings.Load();
    }

    EmulatorSettings.SetAllGameInstallDirs(m_GameInstallDirs);
    EmulatorSettings.Save();

    if (!isGlobal) {
        EmulatorSettings.Load(profile);
    }

    if (!isGameRunning) {
        GetProfileInfo();
    }
}

void SettingsWindow::GetProfileInfo() {
    GetGameIconInfo(profileIcons);

    BigPictureMode::IconInfo global;
    global.title = "Global";
    profileIcons.emplace(profileIcons.begin(), global);
}

SettingsWindow::SettingsWindow(bool gameRunning) : isGameRunning(gameRunning) {
    auto resource = cmrc::res::get_filesystem();
    auto loadTexture = [&](const std::string& resourcePath,
                           std::variant<SDL_Texture*, ImGui::RefCountedTexture>& texture) {
        auto file = resource.open(resourcePath);
        std::vector<u8> texData = std::vector<u8>(file.begin(), file.end());
        gameRunning ? texture = ImGui::RefCountedTexture::DecodePngTexture(texData)
                    : texture = BigPictureMode::LoadSdlTextureData(texData);
    };

    loadTexture("src/resources/big_picture/settings.png", generalTexture);
    loadTexture("src/resources/big_picture/experimental.png", experimentalTexture);
    loadTexture("src/resources/big_picture/graphics.png", graphicsTexture);
    loadTexture("src/resources/big_picture/bloodborne.png", bloodborneTexture);
    loadTexture("src/resources/big_picture/controller.png", inputTexture);
    loadTexture("src/resources/big_picture/trophy.png", trophyTexture);
    loadTexture("src/resources/big_picture/log.png", logTexture);
    loadTexture("src/resources/big_picture/folder.png", foldersTexture);
    loadTexture("src/resources/big_picture/profiles.png", profilesTexture);

    auto languageKeys = std::views::keys(languageMap);
    languageOptions.assign(languageKeys.begin(), languageKeys.end());

    currentProfile = "Global";
    m_GameInstallDirs = EmulatorSettings.GetAllGameInstallDirs();
    currentCategory = isGameRunning ? SettingsCategory::General : SettingsCategory::Profiles;

    bool customConfigFound = false;
    if (isGameRunning) {
        runningGameSerial = std::string(Common::ElfInfo::Instance().GameSerial());
        std::filesystem::path customConfigFile =
            Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
            (runningGameSerial + ".json");

        if (std::filesystem::exists(customConfigFile)) {
            customConfigFound = true;
            currentProfile =
                runningGameSerial + " - " + std::string(Common::ElfInfo::Instance().Title());
        }
    } else {
        GetProfileInfo();
    }

    customConfigFound ? LoadSettings(runningGameSerial) : LoadSettings("Global");
}

void SettingsWindow::Prepare() {
    uiScale = EmulatorSettings.GetBigPictureScale() / 1000.f;
    openedFrame = ImGui::GetFrameCount();
}

// Esc, Circle on a controller, or (in game) F3 again: closes the window like Cancel. Only while
// it has the focus (not another window such as the F1 one), not while a popup, a dropdown or an
// edited field takes the key, nor in the first frames, which still see the key press that
// opened it.
bool SettingsWindow::CloseKeyPressed() const {
    if (ImGui::GetFrameCount() <= openedFrame + 2 ||
        !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return false;
    }
    return ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
           (isGameRunning && ImGui::IsKeyPressed(ImGuiKey_F3, false));
}

void SettingsWindow::DeInit() {
    // In game: on a copy, applied in one step (see EmulatorSettingsImpl::ReloadScope).
    const EmulatorSettingsImpl::ReloadScope reload;
    EmulatorSettings.Load();
    EmulatorSettings.SetBigPictureScale(static_cast<int>(uiScale * 1000));
    EmulatorSettings.Save();

    if (isGameRunning && !runningGameSerial.empty()) {
        EmulatorSettings.Load(runningGameSerial);
    }
}
void SettingsWindow::DrawSettings(bool* open, const std::function<void()>& applySettings) {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 1.00f)); // black
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.40f, 0.70f, 1.00f));   // blue
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          ImVec4(0.25f, 0.50f, 0.85f, 1.00f)); // lighter blue
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,
                          ImVec4(0.26f, 0.59f, 0.98f, 0.80f)); // another light blue
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,
                          ImVec4(0.26f, 0.59f, 0.98f, 0.80f)); // another light blue

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * uiScale, 10.0f * uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * uiScale, 10.0f * uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.5f * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * uiScale, 20.0f * uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 20.0f * uiScale);

    rowFocused = false;
    SetupWindow();
    DrawCategoryTabs();
    DrawMainContent(open, applySettings);

    ImGui::PopStyleVar(8);
    ImGui::PopStyleColor(5);

    ImGui::End();
}

void SettingsWindow::SetupWindow() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::Begin("Settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::DrawPrettyBackground();
    ImGui::SetWindowFontScale(uiScale);

    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }

    // Navigate categories with Tab / R1 / L1, in the order the tabs are shown
    const bool next = ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_Tab);
    const bool previous = ImGui::IsKeyPressed(ImGuiKey_GamepadL1);
    if (next || previous) {
        const auto categories = VisibleCategories();
        const auto it = std::ranges::find(categories, currentCategory);
        const int count = static_cast<int>(categories.size());
        const int index = it == categories.end() ? 0 : static_cast<int>(it - categories.begin());
        currentCategory = categories[(index + (next ? 1 : count - 1)) % count];
    }
}

std::vector<SettingsWindow::SettingsCategory> SettingsWindow::VisibleCategories() const {
    std::vector<SettingsCategory> categories;
    if (!isGameRunning) {
        categories.push_back(SettingsCategory::Profiles);
    }
    categories.insert(categories.end(), {SettingsCategory::General, SettingsCategory::Graphics,
                                         SettingsCategory::Input, SettingsCategory::Trophy,
                                         SettingsCategory::Folders, SettingsCategory::Log});
    if (currentProfile != "Global") {
        categories.push_back(SettingsCategory::Experimental);
    }
    categories.push_back(SettingsCategory::Bloodborne);
    return categories;
}

void SettingsWindow::DrawCategoryTabs() {
    ImVec4 settingsColor = ImVec4(0.1f, 0.1f, 0.12f, 0.8f); // Darker gray
    ImGui::PushStyleColor(ImGuiCol_ChildBg, settingsColor);

    float vertSize = (settingsIconSize * uiScale + ImGui::CalcTextSize("Profiles").y) +
                     ImGui::GetStyle().FramePadding.y * 4.f + 20.0 * uiScale;
    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::BeginChild("Categories", ImVec2(0, vertSize), child_flags, window_flags);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    struct Tab {
        const char* name;
        std::variant<SDL_Texture*, ImGui::RefCountedTexture>* texture;
    };
    const auto tab_of = [&](SettingsCategory category) -> Tab {
        switch (category) {
        case SettingsCategory::Profiles:
            return {"Profiles", &profilesTexture};
        case SettingsCategory::General:
            return {"General", &generalTexture};
        case SettingsCategory::Graphics:
            return {"Graphics", &graphicsTexture};
        case SettingsCategory::Input:
            return {"Input", &inputTexture};
        case SettingsCategory::Trophy:
            return {"Trophy", &trophyTexture};
        case SettingsCategory::Folders:
            return {"Game Folders", &foldersTexture};
        case SettingsCategory::Log:
            return {"Log", &logTexture};
        case SettingsCategory::Experimental:
            return {"Experimental", &experimentalTexture};
        case SettingsCategory::Bloodborne:
        default:
            return {"Bloodborne", &bloodborneTexture};
        }
    };

    // Icons evenly spaced 30 apart; a name wider than its tab is shortened with "..." (the full
    // name shows when the icon is hovered).
    const auto categories = VisibleCategories();
    const float icon_width = settingsIconSize * uiScale + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float pitch = icon_width + 30.0f * uiScale;
    const float max_text_width = pitch - 8.0f * uiScale;
    const float first_center = ImGui::GetCursorPosX() + icon_width * 0.5f;
    for (size_t i = 0; i < categories.size(); ++i) {
        const Tab tab = tab_of(categories[i]);
        AddCategory(tab.name, *tab.texture, categories[i],
                    first_center + pitch * static_cast<float>(i), max_text_width);
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

void SettingsWindow::AddCategory(std::string name,
                                 std::variant<SDL_Texture*, ImGui::RefCountedTexture> texture,
                                 SettingsCategory category, float center_x, float max_text_width) {
    ImGui::SameLine();
    ImGui::BeginGroup();

    // Icon and name centred on center_x (DrawCategoryTabs spaces the tabs).
    const float icon_width = settingsIconSize * uiScale + ImGui::GetStyle().FramePadding.x * 2.0f;
    std::string label = name;
    if (ImGui::CalcTextSize(label.c_str()).x > max_text_width) {
        while (!label.empty() && ImGui::CalcTextSize((label + "...").c_str()).x > max_text_width) {
            label.pop_back();
        }
        while (!label.empty() && label.back() == ' ') {
            label.pop_back();
        }
        label += "...";
    }
    const float text_width = ImGui::CalcTextSize(label.c_str()).x;
    ImGui::SetCursorPosX(center_x - icon_width * 0.5f);

    // make button appear hovered as long as category is selected, otherwise dull its hovered color
    currentCategory == category
        ? ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered])
        : ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.235f, 0.392f, 0.624f, 1.00f));

    ImTextureID id = nullptr;
    if (std::holds_alternative<SDL_Texture*>(texture)) {
        id = ImTextureID(std::get<SDL_Texture*>(texture));
    } else if (std::holds_alternative<ImGui::RefCountedTexture>(texture)) {
        id = std::get<ImGui::RefCountedTexture>(texture).GetTexture().im_id;
    }

    if (id != nullptr) {
        if (ImGui::ImageButton(name.c_str(), id,
                               ImVec2(settingsIconSize * uiScale, settingsIconSize * uiScale))) {
            currentCategory = category;
        }
        if (label != name && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", name.c_str());
        }
    }

    ImGui::PopStyleColor();

    ImGui::SetCursorPosX(center_x - text_width * 0.5f);
    ImGui::Text("%s", label.c_str());
    ImGui::EndGroup();
}

void SettingsWindow::DrawMainContent(bool* open, const std::function<void()>& applySettings) {
    ImVec4 settingsColor = ImVec4(0.1f, 0.1f, 0.12f, 0.8f); // Darker gray
    ImGui::PushStyleColor(ImGuiCol_ChildBg, settingsColor);

    std::string centeredText;
    currentCategory == SettingsCategory::Folders
        ? centeredText = "Manage shadPS4 Game Folders"
        : centeredText = "Selected Profile: " + currentProfile;

    ImGui::Separator();
    Overlay::TextCentered(centeredText.c_str());
    ImGui::Separator();

    if (currentCategory == SettingsCategory::Profiles) {
        DrawProfileSelector();
    } else if (currentCategory == SettingsCategory::Folders) {
        DrawGameFolderManager();
    } else {
        DrawSettingsTable(currentCategory);
    }

    ImGui::PopStyleColor();

    ImGui::Separator();
    ImGui::SetNextItemWidth(300.0f * uiScale);
    static float sliderScale2 = 1.0f;
    if (ImGui::IsWindowAppearing()) {
        sliderScale2 = uiScale;
    }

    ImGui::SliderFloat("UI Scale", &sliderScale2, 0.25f, 3.0f);
    // Only update when user is not interacting with slider
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        uiScale = sliderScale2;
    }
    ImGui::SameLine();

    // Align buttons right. The Bloodborne tab's Show/Hide advanced settings goes before Save.
    const bool bb_tab = currentCategory == SettingsCategory::Bloodborne;
    const bool advanced_shown = EmulatorSettings.IsBbShowThreadedSettings();
    const char* advanced_label =
        advanced_shown ? "Hide advanced settings" : "Show advanced settings";
    float buttonsWidth = ImGui::CalcTextSize("Save").x + ImGui::CalcTextSize("Cancel").x +
                         ImGui::CalcTextSize("Apply").x + ImGui::GetStyle().FramePadding.x * 6.0f +
                         ImGui::GetStyle().ItemSpacing.x * 2;
    if (bb_tab) {
        buttonsWidth += ImGui::CalcTextSize(advanced_label).x +
                        ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    }
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonsWidth);
    if (bb_tab) {
        if (ImGui::Button(advanced_shown ? "Hide advanced settings##bb_advanced"
                                         : "Show advanced settings##bb_advanced")) {
            EmulatorSettings.SetBbShowThreadedSettings(!advanced_shown);
            EmulatorSettings.Save();
        }
        ImGui::SameLine();
    }

    // Controller shortcuts: d-pad right on a setting jumps to Save (right again: Apply,
    // Cancel); Options saves and closes (with the usual confirmation).
    const bool shortcuts =
        ImGui::GetFrameCount() > openedFrame + 2 && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (shortcuts && rowFocused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false)) {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::Button("Save") || (shortcuts && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))) {
        closeOnSave = true;
        ImGui::OpenPopup("Save Confirmation");
    }

    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        ImGui::OpenPopup("Save Confirmation");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save Confirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", ("Profile Saved:\n" + currentProfile).c_str());
        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(250 * uiScale, 0))) {
            std::string profile = currentProfile;
            if (currentProfile != "Global") {
                profile = currentProfile.substr(0, 9);
            }

            SaveSettings(profile);
            // In game, saving the profile the game runs with: settings that only apply at a
            // start and changed since the game started ask about a restart first.
            restartChanged.clear();
            const std::string running_profile =
                runningGameSerial.empty() || !RestartSnapshots().contains(runningGameSerial)
                    ? std::string{"Global"}
                    : runningGameSerial;
            if (isGameRunning && profile == running_profile) {
                const auto now = RestartValues();
                const auto& started = RestartSnapshots()[profile];
                for (size_t i = 0; i < now.size() && i < started.size(); ++i) {
                    if (now[i].second != started[i].second) {
                        restartChanged.emplace_back(now[i].first);
                    }
                }
            }
            if (!restartChanged.empty()) {
                restartPromptRequest = true;
            } else if (closeOnSave) {
                DeInit();
                *open = false;
                closeOnSave = false;
            }
            ImGui::CloseCurrentPopup();
            if (applySettings) {
                applySettings();
            }
        }

        ImGui::EndPopup();
    }

    // Restart needed: the relaunch F1 uses for the resolution (on its own thread, back to the
    // last autosave), or later.
    if (restartPromptRequest) {
        restartPromptRequest = false;
        ImGui::OpenPopup("Restart needed##bb_restart");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Restart needed##bb_restart", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("These changes apply after a restart:");
        for (const auto& name : restartChanged) {
            ImGui::BulletText("%s", name.c_str());
        }
        ImGui::Spacing();
        ImGui::TextDisabled("The game closes and starts again; you continue from your last "
                            "autosave.\nDon't restart while the save icon is showing.");
        ImGui::Spacing();
        if (ImGui::Button("Restart now")) {
            ImGui::CloseCurrentPopup();
            DeInit();
            *open = false;
            closeOnSave = false;
            ::Core::BbResolution::RestartGame();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later")) {
            ImGui::CloseCurrentPopup();
            if (closeOnSave) {
                DeInit();
                *open = false;
                closeOnSave = false;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel") || CloseKeyPressed()) {
        DeInit();
        *open = false;
        if (applySettings) {
            applySettings();
        }
    }
}

void SettingsWindow::DrawProfileSelector() {
    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    ImVec4 settingsColor = ImVec4(0.1f, 0.1f, 0.12f, 0.8f); // Darker gray
    ImGui::PushStyleColor(ImGuiCol_ChildBg, settingsColor);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);

    ImGui::BeginChild("Profile Selection", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                      child_flags, window_flags);

    ImGui::PopStyleColor();

    if (ImGui::BeginTable("ProfilesTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("CreateDeleteButton", ImGuiTableColumnFlags_WidthFixed,
                                400.0f * uiScale);
        ImGui::TableSetupColumn("Profile");

        for (int i = 0; i < profileIcons.size(); i++) {
            const std::filesystem::path customConfigFile =
                Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
                (profileIcons[i].serial + ".json");
            const bool gameConfigExists = std::filesystem::exists(customConfigFile);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::string deleteButtonLabel = "Delete game-specific config##" + std::to_string(i);

            if (gameConfigExists) {
                // Different shades of red depending on button state
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.0f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.0f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.0f, 0.0f, 1.0f));

                if (ImGui::Button(deleteButtonLabel.c_str(),
                                  ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
                    deleteProfileIndex = i;
                }

                ImGui::PopStyleColor(3);
            }

            ImGui::TableNextColumn();
            std::string profileLabel = i == 0
                                           ? "Global"
                                           : profileIcons[i].serial + " - " +
                                                 profileIcons[i].title + "##" + std::to_string(i);
            if (ImGui::Button(profileLabel.c_str(),
                              ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {

                currentProfile = profileLabel;
                if (currentProfile == "Global") {
                    LoadSettings("Global");
                } else {
                    LoadSettings(profileIcons[i].serial);
                    if (!gameConfigExists) {
                        SaveSettings(profileIcons[i].serial);
                    }
                }
            }
        }

        ImGui::EndTable();
    }

    if (deleteProfileIndex != -1) {
        ImGui::OpenPopup("Confirm Delete");
    }

    ImGui::PopStyleVar(3);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string title = profileIcons[deleteProfileIndex].title;
        const std::string message = "Delete game-specific config file for " + title + "?";
        const std::filesystem::path path =
            Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
            (profileIcons[deleteProfileIndex].serial + ".json");

        ImGui::Text("%s", message.c_str());
        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120 * uiScale, 0))) {
            try {
                std::filesystem::remove(path);
            } catch (const std::exception& e) {
                LOG_ERROR(ImGui, "Could not delete config file {}: {}", path.string(), e.what());
            }

            deleteProfileIndex = -1;
            currentProfile = "Global";
            LoadSettings("Global");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120 * uiScale, 0))) {
            deleteProfileIndex = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

void SettingsWindow::DrawGameFolderManager() {
    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    ImGui::BeginChild("ContentRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), child_flags,
                      window_flags);

    if (ImGui::Button("Add Folder", ImVec2(400.f * uiScale, 0))) {
        ImGuiFileDialog::Instance()->OpenDialog("OpenFolder", "Add shadPS4 game folder", nullptr,
                                                ".", 1, nullptr,
                                                ImGuiFileDialogFlags_DisableCreateDirectoryButton |
                                                    ImGuiFileDialogFlags_DontShowHiddenFiles);

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
    }

    float buttonWidth = ImGui::GetItemRectSize().x;
    ImGui::PushItemWidth(buttonWidth);
    if (ImGui::SliderInt("Folder Scan Depth", &scanDepthSetting, 0, 5)) {
        // In game: on a copy, applied in one step (see EmulatorSettingsImpl::ReloadScope).
        const EmulatorSettingsImpl::ReloadScope reload;
        std::string profile;
        const bool isGlobal = currentProfile == "Global";
        if (!isGlobal) {
            profile = currentProfile.substr(0, 9);
            EmulatorSettings.Load();
        }

        EmulatorSettings.SetBigPictureFolderDepth(scanDepthSetting);
        EmulatorSettings.Save();

        if (!isGlobal) {
            EmulatorSettings.Load(profile);
        }

        if (!isGameRunning) {
            GetProfileInfo();
        }
    }
    ImGui::PopItemWidth();

    if (ImGuiFileDialog::Instance()->Display("OpenFolder", child_flags | ImGuiWindowFlags_NoMove)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            GameInstallDir dir;
            dir.path = ImGuiFileDialog::Instance()->GetCurrentPath();
            dir.enabled = true;

#ifdef WIN32
            // replace \ with / on windows
            std::string pathString = dir.path.string();
            size_t pos = 0;
            while ((pos = pathString.find("\\", pos)) != std::string::npos) {
                pathString.replace(pos, 1, "/");
                pos += 2;
            }

            dir.path = pathString;
#endif

            m_GameInstallDirs.push_back(dir);
            SaveInstallDirs();
        }

        ImGuiFileDialog::Instance()->Close();
    }

    ImGui::BeginChild("Game Folder List", ImVec2(0, 0), true, child_flags);

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 5.0f * uiScale));
    if (ImGui::BeginTable("FoldersTable", 3)) {
        ImGui::TableSetupColumn("FolderButton", ImGuiTableColumnFlags_WidthFixed, 300.0f * uiScale);
        ImGui::TableSetupColumn("FolderEnabled", ImGuiTableColumnFlags_WidthFixed, 0.0f);
        ImGui::TableSetupColumn("FolderPath");

        for (int i = 0; i < m_GameInstallDirs.size(); i++) {
            std::string buttonLabel = "Remove Folder##" + std::to_string(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Button(buttonLabel.c_str(), ImVec2(280 * uiScale, 0))) {
                m_GameInstallDirs.erase(m_GameInstallDirs.begin() + i);
                SaveInstallDirs();
            }

            ImGui::TableNextColumn();
            std::string checkboxLabel = "##EnableFolder" + std::to_string(i);
            bool previousState = m_GameInstallDirs[i].enabled;
            ImGui::Checkbox(checkboxLabel.c_str(), &m_GameInstallDirs[i].enabled);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(5.0 * uiScale, 0));

            if (m_GameInstallDirs[i].enabled != previousState) {
                SaveInstallDirs();
            }

            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", m_GameInstallDirs[i].path.string().c_str());
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::EndChild();
}

void SettingsWindow::DrawSettingsTable(SettingsCategory category) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f * uiScale, 10.0f * uiScale));
    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    ImGui::BeginChild("ContentRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), child_flags,
                      window_flags);

    if (category == SettingsCategory::General) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCombo("Console Language", consoleLanguageSetting, languageOptions);
            AddSettingSliderInt("Volume", volumeSetting, 0, 500);
            AddSettingCheckbox("Show Splash Screen When Launching Game", showSplashSetting);
            AddSettingCombo("Audio Backend", audioBackendSetting, audioBackendOptions);

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Graphics) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCombo("Display Mode", fullscreenModeSetting, fullscreenModeOptions);
            AddSettingCombo("Present Mode", presentModeSetting, presentModeOptions);
            AddSettingSliderInt("Window Width", windowWidthSetting, 0, 8000);
            AddSettingSliderInt("Window Height", windowHeightSetting, 0, 7000);
            AddSettingCheckbox("Enable HDR", hdrAllowedSetting);
            AddSettingCheckbox("Enable FSR", fsrEnabledSetting);

            if (fsrEnabledSetting) {
                AddSettingCheckbox("Enable RCAS", rcasEnabledSetting);
            }

            if (rcasEnabledSetting && fsrEnabledSetting) {
                AddSettingSliderFloat("RCAS Attenuation", rcasAttenuationSetting, 0.0f, 3.0f, 3);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Bloodborne) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            // Bloodborne-only: everything the threaded renderer work added. Off (the switch, or one
            // toggle) is the standard shadPS4 behaviour. Each section shows its main settings;
            // "Show advanced settings" adds the rest in every section.
            AddSettingSection("Threaded Renderer",
                              "Used by Bloodborne only. If something goes wrong, turn off "
                              "Threaded Renderer to get standard shadPS4 behaviour.",
                              true);
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextWrapped("Threaded Renderer");
                ImGui::TableNextColumn();
                ImGui::Checkbox("##Threaded Renderer", &threadedRendererSetting);
                NoteRowFocus();
                ImGui::SameLine();
                AddHint("Faster rendering and smoother frame pacing. Most settings below need it "
                        "(needs a restart)");
            }
            const bool advanced = EmulatorSettings.IsBbShowThreadedSettings();
            const bool tr = threadedRendererSetting;
            // Global settings saved at once (a game's profile would not keep them).
            const auto live = [&](const char* name, bool value, auto set, const char* hint) {
                bool shown = value;
                AddSettingCheckbox(name, shown, hint);
                if (shown != value) {
                    set(shown);
                    EmulatorSettings.Save();
                }
                return shown;
            };
            // Settings that can cause visual artefacts: marked, and turning one on asks first
            // (a pop-up says what to look out for).
            const auto risky = [&](const char* name, bool& value, const char* hint,
                                   const char* risk) {
                const bool before = value;
                AddSettingCheckbox(name, value, hint);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.2f, 1.0f), "(!)");
                if (ImGui::IsItemHovered()) {
                    ShowTooltip(risk);
                }
                if (!before && value) {
                    value = false;
                    bbRiskTarget = &value;
                    bbRiskName = name;
                    bbRiskText = risk;
                    bbRiskOpenRequest = true;
                }
            };
            const auto subheading = [&](const char* title) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "%s", title);
            };

            // ---------------------------------------------------------------------------------
            AddSettingSection("Performance", "Frame rate and smoothness.", false);
            AddSettingCombo("Resizable BAR / Smart Access Memory", bbDrawDataMemorySetting,
                            drawDataMemoryOptions);
            if (ImGui::IsItemHovered()) {
                ShowTooltip("Puts the data the game changes every frame straight into "
                            "graphics card memory.\nNeeds Resizable BAR (NVIDIA, Intel) or "
                            "Smart Access Memory (AMD) enabled in the BIOS.\nAuto detects "
                            "it. Try Off if you get stutter or crashes. (needs a restart)");
            }
            if (tr) {
                AddSettingCheckbox("Show frames when ready", bbFlipWhenReadySetting,
                                   "Smoother above 60 fps. Off: frames on vblank ticks");
                risky("Real occlusion tests", bbRealOcclusionSetting, "More fps in busy areas",
                      "Can make textures flicker white for a moment,\nfor example when coming "
                      "round a corner.");
                risky("Merged copy shaders", bbMergedCopiesSetting, "Less GPU copy work",
                      "Can make particles flicker (seen with Intel 12th gen CPUs and newer;\n"
                      "possibly fixed, not confirmed yet).");
                if (advanced && bbMergedCopiesSetting) {
                    AddSettingCheckbox("    Sources from buffer memory",
                                       bbMergedNoStreamSourcesSetting,
                                       "Flicker test: small copy sources not snapshotted from "
                                       "game memory");
                    AddSettingCheckbox("    Large batches", bbMergedLargeBatchesSetting,
                                       "Flicker test: the old 64 MB batches instead of 64 KB");
                }
            }
            if (tr && advanced) {
#ifdef _WIN32
                AddSettingCheckbox("Faster Windows locks and sleeps", bbFastLocksSetting,
                                   "Less CPU overhead (needs a restart)");
#endif
                AddSettingCheckbox("Overlap GPU submissions", bbSubmitOverlapSetting,
                                   "The next submission is decoded while the last is recorded");
                AddSettingCheckbox("Overlap game and GPU frames", bbOverlapFramesSetting,
                                   "The game starts its next frame while the last one finishes "
                                   "(needs a restart)");
                AddSettingCheckbox("Skip draws with no output", bbSkipEmptyDrawsSetting,
                                   "Occlusion test geometry the emulator does not use");
                AddSettingCheckbox("Reuse texture lookups", bbTextureSetMemoSetting,
                                   "A draw's textures are found once, not every draw");
                AddSettingCheckbox("Reuse static geometry", bbGeometryInPlaceSetting,
                                   "Vertex and index data that never changes isn't copied for "
                                   "every draw");
                AddSettingCheckbox("Copy small geometry early", bbRingGeometrySetting,
                                   "Small vertex and index data copied by the decoding thread");
                AddSettingCheckbox("Constant copies without waiting", bbConstantsNoWaitSetting,
                                   "The game's constant copies don't wait for queued draws");
                AddSettingCheckbox("Occlusion results without waiting", bbOcclusionInOrderSetting,
                                   "Occlusion results queued behind the draws instead of "
                                   "waited for");
                AddSettingCheckbox("Draw data copy thread", bbCopyThreadSetting,
                                   "Draw memory copies on their own thread (needs a restart)");
                AddSettingCheckbox("Keep rendering while presenting", bbRenderDuringPresentSetting,
                                   "Frame limiters like RTSS don't hold up rendering");
                AddSettingCheckbox("Protect only changed pages", bbExactProtectionSetting,
                                   "Less CPU time protecting game memory the GPU reads");
                AddSettingCheckbox("Batched texture uploads", bbBatchedUploadsSetting,
                                   "A draw's new textures are uploaded together");
                AddSettingCheckbox("Fast pixel shader math", bbFastPixelMathSetting,
                                   "The driver fuses multiply-adds; shaders rebuilt (needs a "
                                   "restart)");
                AddSettingCheckbox("GPU interpolation where safe", bbHardwareInterpolationSetting,
                                   "NVIDIA: less pixel shader code (needs a restart)");
            }
            if (tr && advanced) {
                subheading("Experimental");
                live(
                    "Draw recorder on two threads", EmulatorSettings.IsBbDrawFinisher(),
                    [](bool v) { EmulatorSettings.SetBbDrawFinisher(v); },
                    "The draw recorder's command recording on a thread of its own");
                live(
                    "Submit on the recording thread", EmulatorSettings.IsBbAsyncSubmit(),
                    [](bool v) { EmulatorSettings.SetBbAsyncSubmit(v); },
                    "The GPU threads don't wait for recording to finish before submitting");
                live(
                    "Faster pipeline lookup (1.09)", EmulatorSettings.IsBbGxPipelines(),
                    [](bool v) { EmulatorSettings.SetBbGxPipelines(v); },
                    "Pipelines taken from the engine's objects; stops by itself on any "
                    "difference (needs a restart)");
#ifdef _WIN32
                const bool off_core0 = live(
                    "Keep render threads off core 0", EmulatorSettings.IsBbRenderThreadsOffCore0(),
                    [](bool v) { EmulatorSettings.SetBbRenderThreadsOffCore0(v); },
                    "Windows: the render threads avoid the core that handles most interrupts");
                Common::RenderAffinity::Set(off_core0);
#endif
            }

            // ---------------------------------------------------------------------------------
            AddSettingSection("Stutters", "Hitches when new areas, objects and textures load.",
                              false);
            if (tr) {
                risky("Background shader compiles", bbBackgroundPipelinesSetting,
                      "No stutter when a new object needs a shader",
                      "New objects can appear a frame late (a brief pop-in).");
                AddSettingCheckbox("Background copies", bbBackgroundTextureCopiesSetting,
                                   "New textures and geometry are copied on other threads");
                AddSettingCombo(
                    "    Copy threads", bbCopyThreadsSetting,
                    {fmt::format("Auto ({})", Common::CopyPool::AutoWorkers()), "4", "6", "8"});
                if (ImGui::IsItemHovered()) {
                    ShowTooltip("Threads for background copies and large uploads when an area "
                                "streams in: more copy faster but compete with the game's "
                                "threads. Auto: half the CPU's threads, at most 8 (needs a "
                                "restart)");
                }
                if (bbBackgroundTextureCopiesSetting) {
                    AddSettingCheckbox("    Protect on a helper thread",
                                       bbProtectHelperThreadSetting,
                                       "New textures' and geometry's memory protected off the "
                                       "draw recorder");
                }
                risky("Spread area loading", bbSpreadLoadsSetting,
                      "Area loads upload textures over a few frames",
                      "New textures can show black for a few frames while an area loads.");
                risky("Lower detail first", bbLowerDetailFirstSetting,
                      "A new texture's sharpest level arrives a few frames later",
                      "New textures can look soft (blurry) for a few frames.");
            }
            if (tr && advanced) {
                AddSettingCheckbox("Streaming optimisations", bbStreamingSetting,
                                   "Smaller hitches entering new areas");
                AddSettingCheckbox("GPU readbacks on the copy queue", bbReadbackQueueSetting,
                                   "Fewer small stutters (needs a restart)");
                if (bbReadbackQueueSetting) {
                    AddSettingCheckbox("    Early GPU data write-back", bbEarlyWritebackSetting,
                                       "GPU data the game writes over is copied back before it "
                                       "waits");
                }
                AddSettingCheckbox("Direct texture copies", bbDirectTextureCopiesSetting,
                                   "Texture data read straight from game memory");
                AddSettingCheckbox("Cached texture staging", bbCachedTextureStagingSetting,
                                   "Texture uploads staged in cached memory (faster copies)");
                AddSettingCheckbox("Copy only kept texture levels", bbSkipLeftOutLevelsSetting,
                                   "Texture quality High/Medium: less data copied per upload");
                AddSettingCheckbox("Clear shader-written images", bbClearShaderWrittenSetting,
                                   "New images a compute shader fully overwrites are cleared, "
                                   "not uploaded");
                AddSettingCheckbox("Keep preparing draws during area loads",
                                   bbCeSkipHugeWritesSetting,
                                   "The GPU thread doesn't wait for the draw recorder in area "
                                   "loads");
                AddSettingCheckbox("Small GPU copies on the CPU", bbCpuCopyShaderSetting,
                                   "The game doesn't wait for the GPU for its small copies");
                if (bbCpuCopyShaderSetting) {
                    AddSettingCheckbox("    Skip copies that change nothing", bbCopySkipSameSetting,
                                       "Fewer buffer uploads");
                }
                subheading("Experimental");
                live(
                    "Stage uploads in VRAM", EmulatorSettings.IsBbVramUploadStaging(),
                    [](bool v) { EmulatorSettings.SetBbVramUploadStaging(v); },
                    "Resizable BAR: new data copied straight into a fixed 128 MB of VRAM");
            }

            // ---------------------------------------------------------------------------------
            AddSettingSection("Graphics", "Picture quality.", false);
            DrawBbImageSettings();
            if (tr && advanced) {
                AddSettingCheckbox("Fix light wedges in lamp shadows", bbClearOneLayerSetting,
                                   "Clears each shadow cube face before it's drawn");
            }

            // ---------------------------------------------------------------------------------
            AddSettingSection(
                "VRAM", "Graphics card memory in long sessions and with texture mods.", false);
            AddSettingCheckbox("VRAM cleanup", bbVramReclaimSetting,
                               "Frees unused VRAM in long sessions (needs a restart)");
            if (bbVramReclaimSetting) {
                AddSettingCheckbox(
                    "    Also free idle buffers", bbBufferEvictionSetting,
                    "Buffer memory unused for 30 s near the limit (needs a restart)");
                if (tr && advanced && bbBufferEvictionSetting) {
                    AddSettingCheckbox("        Cheaper buffer refill",
                                       bbEvictKeepProtectionSetting,
                                       "Freed buffers come back without protecting their "
                                       "memory again");
                    AddSettingCheckbox("        Keep returning buffers",
                                       bbEvictKeepReturningSetting,
                                       "Loading screens free buffers the game keeps using last");
                }
                AddSettingCheckbox("    Clean up at loading screens", bbLoadScreenCleanupSetting,
                                   "Frees what the last area used; the next one loads its own");
                AddSettingCheckbox("    Use more VRAM", bbMoreVramSetting,
                                   "Cleans up only close to the driver's VRAM budget");
                if (bbMoreVramSetting) {
                    AddSettingCheckbox("        Go past the budget", bbVramPastBudgetSetting,
                                       "Up to the card's size; Windows moves other programs out "
                                       "(needs a restart)");
                }
            }
            if (advanced) {
                // Testing the VRAM cleanup: act as if the GPU had this much VRAM (0 = real).
                AddSettingSliderInt("VRAM budget override (MB, 0 = real)", bbVramBudgetTestSetting,
                                    0, 24000);
            }

            // ---------------------------------------------------------------------------------
            AddSettingSection("Interface", "The emulator's in-game windows (F1, F3, pop-ups).",
                              false);
            AddSettingCheckbox("Menus sized by screen pixels", bbIgnoreDisplayScaleSetting,
                               "Ignores the desktop's display scaling (125%, 150%), as games do");

            // ---------------------------------------------------------------------------------
            AddSettingSection("Diagnostics", "For troubleshooting. Slower when on: leave off.",
                              false);
            AddSettingCheckbox("Diagnostics in the log", threadedRendererDiagnosticsSetting,
                               "Frame time, stutter, VRAM and readback reports");
            AddSettingCheckbox("Detailed stutter reports", bbStutterDetailSetting,
                               "With diagnostics: where each stutter's time went");
            // Times every draw: heavy on the CPU.
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.2f, 1.0f), "(costs fps)");
            live(
                "Performance meter", EmulatorSettings.IsBbPerfMeter(),
                [](bool v) { EmulatorSettings.SetBbPerfMeter(v); },
                "Under the FPS counter: which stage holds the frame rate; a log summary every "
                "20 s");
            if (advanced) {
                AddSettingCheckbox("GPU time per pass in the log",
                                   threadedRendererGpuProfileSetting,
                                   "Which render passes take the GPU's time");
                live(
                    "Recorder split census", EmulatorSettings.IsBbRecorderCensus(),
                    [](bool v) { EmulatorSettings.SetBbRecorderCensus(v); },
                    "Developer: what each half of the draw recorder does, in the log every 20 s");
                live(
                    "Check GX draw capture", EmulatorSettings.IsBbGxObserve(),
                    [](bool v) { EmulatorSettings.SetBbGxObserve(v); },
                    "Developer (1.09): the engine's draws compared with the decoded ones "
                    "(needs a restart)");
            }
            // RenderDoc captures (F12) and frames per capture: hidden for now (the settings stay
            // in the config).

            ImGui::EndTable();
        }
        // Turning on a setting that can cause visual artefacts: asked first.
        if (bbRiskOpenRequest) {
            bbRiskOpenRequest = false;
            ImGui::OpenPopup("Possible visual artefacts##bb_risk");
        }
        if (ImGui::BeginPopupModal("Possible visual artefacts##bb_risk", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "%s", bbRiskName.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted(bbRiskText.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("If you see this, turn the setting off again.");
            ImGui::Spacing();
            if (ImGui::Button("Turn on")) {
                if (bbRiskTarget) {
                    *bbRiskTarget = true;
                }
                bbRiskTarget = nullptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                bbRiskTarget = nullptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    } else if (category == SettingsCategory::Input) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCheckbox("Enable Motion Controls", motionControlsSetting);
            AddSettingCheckbox("Enable Background Controller Input", backgroundControllerSetting);
            AddSettingCombo("Hide Cursor", cursorStateSetting, hideCursorOptions);

            if (cursorStateSetting == 1) {
                AddSettingSliderInt("Hide Cursor Idle Timeout", cursorTimeoutSetting, 1, 10);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Trophy) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCheckbox("Disable Trophy Notification", trophyPopupDisabledSetting);
            if (!trophyPopupDisabledSetting) {
                AddSettingCombo("Trophy Notification Position", trophySideSetting,
                                trophySideOptions);
                AddSettingSliderFloat("Trophy Notification Duration", trophyDurationSetting, 0.f,
                                      10.f, 1);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Log) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCheckbox("Enable Logging", logEnableSetting);
            if (logEnableSetting) {
                AddSettingCheckbox("Separate Log Files", logSeparateSetting);
                AddSettingCheckbox("Log Sync", logSyncSetting);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Experimental) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingSliderInt("Additional DMem Allocation", extraDmemSetting, 0, 20000);
            AddSettingSliderInt("Vblank Frequency", vblankFrequencySetting, 30, 360);
            AddSettingCombo("Readbacks Mode", readbacksModeSetting, readbacksModeOptions);
            AddSettingCheckbox("Enable Readback Linear Images", readbackLinearImagesSetting);
            AddSettingCheckbox("Enable Direct Memory Access", directMemoryAccessSetting);
#ifdef _WIN32
            // Windows static guest red-zone protection
            AddSettingCheckbox("Windows Guest Red Zone Protection (Requires Restart)",
                               windowsGuestRedZoneProtectionModeSetting);
#endif
            AddSettingCheckbox("Enable Devkit Console Mode", devkitConsoleSetting);
            AddSettingCheckbox("Enable PS4 Neo Mode", neoModeSetting);
            AddSettingCheckbox("Enable ShadNet", shadnetEnabledSetting);
            AddSettingCheckbox("Set Network Connected to True", connectedNetworkSetting);
            AddSettingCheckbox("Enable Shader Cache", pipelineCacheEnabledSetting);

            if (pipelineCacheEnabledSetting) {
                AddSettingCheckbox("Compress Shader Cache to Zip File",
                                   pipelineCacheArchiveSetting);
            }

            ImGui::EndTable();
        }
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// A sub-setting's indent (leading spaces, 4 a level): its name and its control both move in.
static float SubSettingIndent(const std::string& name) {
    size_t spaces = 0;
    while (spaces < name.size() && name[spaces] == ' ') {
        ++spaces;
    }
    return ImGui::GetFontSize() * 1.25f * static_cast<float>(spaces / 4);
}

// The L-shaped connector of a sub-setting's name at the cursor: from the row above down to the
// middle of a row this high, then across to the indented name.
static void DrawSubSettingConnector(int level, float row_height) {
    const float step = ImGui::GetFontSize() * 1.25f;
    const float line_height = ImGui::GetTextLineHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImU32 colour = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float thickness = std::max(1.0f, ImGui::GetFontSize() / 12.0f);
    const float x = pos.x + step * static_cast<float>(level - 1) + step * 0.4f;
    const float y_mid = pos.y + row_height * 0.5f;
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(ImVec2(x, y_mid - line_height * 0.95f), ImVec2(x, y_mid), colour, thickness);
    draw_list->AddLine(ImVec2(x, y_mid), ImVec2(x + step * 0.45f, y_mid), colour, thickness);
}

// The same settings as F1's Graphics tab (ambient occlusion, reflections, the game's effects), read
// and written directly, so that F1 and F3 always show the same (not loaded into F3's copies and
// saved with them).
void SettingsWindow::DrawBbImageSettings() {
    const auto saved = [] { EmulatorSettings.Save(); };
    // A row with a check box: the setting's value, changed and saved at once.
    const auto checkbox = [&](const char* name, bool value, auto set, const char* tooltip) {
        bool shown = value;
        AddSettingCheckbox(name, shown);
        if (ImGui::IsItemHovered()) {
            ShowTooltip(tooltip);
        }
        if (shown != value) {
            set(shown);
            saved();
        }
    };
    // A row with a slider for a value stored in hundredths, saved when it is let go.
    const auto slider = [&](const char* name, int value, auto set, const char* tooltip) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawSettingName(name);
        ImGui::TableNextColumn();
        const float indent = SubSettingIndent(name);
        if (indent > 0.0f) {
            ImGui::Indent(indent);
        }
        float shown = float(value) * 0.01f;
        if (ImGui::SliderFloat((std::string("##") + name).c_str(), &shown, 0.25f, 4.0f, "%.2f")) {
            set(int(std::lround(shown * 100.0f)));
        }
        NoteRowFocus();
        if (indent > 0.0f) {
            ImGui::Unindent(indent);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            saved();
        }
        if (ImGui::IsItemHovered()) {
            ShowTooltip(tooltip);
        }
    };

    // Texture filtering and quality: the same settings as F1's (Graphics), saved at once.
    int anisotropy = ImGui::BbAnisotropyIndex(EmulatorSettings.GetBbAnisotropy());
    const int anisotropy_before = anisotropy;
    AddSettingCombo("Anisotropic filtering", anisotropy, anisotropyOptions);
    if (ImGui::IsItemHovered()) {
        ShowTooltip("Sharper ground and stairs at an angle (needs a restart)");
    }
    if (anisotropy != anisotropy_before) {
        EmulatorSettings.SetBbAnisotropy(ImGui::BbAnisotropySamples(anisotropy));
        saved();
    }
    int texture_quality = std::clamp(EmulatorSettings.GetBbTextureQuality(), 0, 2);
    const int texture_quality_before = texture_quality;
    AddSettingCombo("Texture quality", texture_quality, textureQualityOptions);
    if (ImGui::IsItemHovered()) {
        ShowTooltip("Less VRAM for big textures (the BB Remaster mod's) by leaving "
                    "out their sharpest level:\nHigh: textures over 2048, Medium: "
                    "over 1024 (about 500 MB less in busy areas).\nApplies to "
                    "textures loaded after a change: from the next loading screen.");
    }
    if (texture_quality != texture_quality_before) {
        EmulatorSettings.SetBbTextureQuality(texture_quality);
        saved();
    }

    const bool vbao = EmulatorSettings.IsBbVbao();
    checkbox(
        "Better ambient occlusion (GT-VBAO)", vbao, [](bool v) { EmulatorSettings.SetBbVbao(v); },
        "Deeper, cleaner contact shadows in corners, under objects and in carvings");
    if (vbao) {
        slider(
            "    Strength", EmulatorSettings.GetBbVbaoStrength(),
            [](int v) { EmulatorSettings.SetBbVbaoStrength(v); },
            "How dark occluded places get (1.50 by default)");
        slider(
            "    Reach (metres)", EmulatorSettings.GetBbVbaoRadius(),
            [](int v) { EmulatorSettings.SetBbVbaoRadius(v); },
            "How far away something can be and still shade a surface (1.50 by default)");
        checkbox(
            "    Half resolution (faster)", EmulatorSettings.IsBbVbaoHalfRes(),
            [](bool v) { EmulatorSettings.SetBbVbaoHalfRes(v); },
            "About a quarter of the cost, slightly softer fine detail");
    }

    static const std::vector<std::string> ssr_modes = {"Game's", "Better", "Off"};
    int ssr_mode = std::clamp(EmulatorSettings.GetBbSsrMode(), 0, 2);
    const int ssr_before = ssr_mode;
    AddSettingCombo("Screen space reflections", ssr_mode, ssr_modes);
    if (ImGui::IsItemHovered()) {
        ShowTooltip("Built in, no patch needed. Better: traced more precisely, sharper, to the "
                    "screen's edges. Off: no cost. Switching between Off and on needs a restart");
    }
    if (ssr_mode != ssr_before) {
        EmulatorSettings.SetBbSsrMode(ssr_mode);
        saved();
        // Off to on or back: the game's own reflection code is patched in at a start.
        if (isGameRunning && ::Core::BbGameFixes::SsrNeedsRestart(ssr_mode)) {
            restartChanged = {"Screen space reflections"};
            restartPromptRequest = true;
        }
    }
    if (ssr_mode == 1) {
        checkbox(
            "    Full resolution", EmulatorSettings.IsBbSsrFullRes(),
            [](bool v) { EmulatorSettings.SetBbSsrFullRes(v); },
            "A little sharper, at more GPU time");
    }

    // The game's motion blur: the upscaling settings file's (F1's).
    ImGui::BbSettingsFile file;
    file.Load();
    const bool motion_blur = file.Get("motion_blur", "0") != "0";
    checkbox(
        "Game motion blur", motion_blur,
        [&file](bool v) { file.Set("motion_blur", v ? "1" : "0"); }, "Off: sharper in motion");
}

// A setting's name. Leading spaces (4 a level) make it a sub-setting of the row above: indented,
// a little dimmer, with a connector line from its parent so it reads as part of it.
void SettingsWindow::DrawSettingName(const std::string& name) {
    size_t spaces = 0;
    while (spaces < name.size() && name[spaces] == ' ') {
        ++spaces;
    }
    const int level = static_cast<int>(spaces / 4);
    if (level == 0) {
        ImGui::TextWrapped("%s", name.c_str());
        return;
    }
    const float step = ImGui::GetFontSize() * 1.25f;
    DrawSubSettingConnector(level, ImGui::GetTextLineHeight());
    ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    text.w *= 0.8f;
    const float indent = step * static_cast<float>(level);
    ImGui::Indent(indent);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    ImGui::TextWrapped("%s", name.c_str() + spaces);
    ImGui::PopStyleColor();
    ImGui::Unindent(indent);
}

void SettingsWindow::AddSettingCheckbox(std::string name, bool& value) {
    std::string label = "##" + name;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    DrawSettingName(name);
    ImGui::TableNextColumn();
    const float indent = SubSettingIndent(name);
    if (indent > 0.0f) {
        ImGui::Indent(indent);
    }
    ImGui::Checkbox(label.c_str(), &value);
    NoteRowFocus();
    if (indent > 0.0f) {
        ImGui::Unindent(indent);
    }
}

// "(needs a restart)" in a hint or tooltip stands out in amber, without the brackets.
constexpr std::string_view RestartNote = "(needs a restart)";
constexpr ImVec4 RestartColour{1.0f, 0.78f, 0.35f, 1.0f};

static std::pair<std::string_view, bool> SplitRestartNote(std::string_view text) {
    const auto at = text.find(RestartNote);
    if (at == std::string_view::npos) {
        return {text, false};
    }
    auto before = text.substr(0, at);
    while (!before.empty() && before.back() == ' ') {
        before.remove_suffix(1);
    }
    return {before, true};
}

void SettingsWindow::AddHint(std::string_view hint) {
    const auto [text, restart] = SplitRestartNote(hint);
    ImGui::TextDisabled("%.*s", static_cast<int>(text.size()), text.data());
    if (restart) {
        ImGui::SameLine();
        ImGui::TextColored(RestartColour, "needs a restart");
    }
}

void SettingsWindow::ShowTooltip(std::string_view tooltip) {
    const auto [text, restart] = SplitRestartNote(tooltip);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    if (restart) {
        ImGui::TextColored(RestartColour, "Needs a restart");
    }
    ImGui::EndTooltip();
}

void SettingsWindow::AddSettingCheckbox(std::string name, bool& value, const char* hint) {
    AddSettingCheckbox(name, value);
    ImGui::SameLine();
    AddHint(hint);
}

void SettingsWindow::AddSettingSection(const char* title, const char* note, bool experimental) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.5f));
    ImGui::Separator();
    // Amber for experimental settings, the usual text colour otherwise.
    const ImVec4 colour =
        experimental ? ImVec4(1.0f, 0.72f, 0.25f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::TextColored(colour, "%s", title);
    ImGui::TableNextColumn();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.5f));
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", note);
    ImGui::PopTextWrapPos();
}

void SettingsWindow::AddSettingSliderInt(std::string name, int& value, int min, int max) {
    std::string label = "##" + name;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    DrawSettingName(name);

    ImGui::TableNextColumn();
    ImGui::SliderInt(label.c_str(), &value, min, max);
    NoteRowFocus();
}

void SettingsWindow::AddSettingSliderFloat(std::string name, float& value, int min, int max,
                                           int precision) {
    std::string label = "##" + name;
    std::string precisionString = "%." + std::to_string(precision) + "f";

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    DrawSettingName(name);

    ImGui::TableNextColumn();
    ImGui::SliderFloat(label.c_str(), &value, min, max, precisionString.c_str());
    NoteRowFocus();
}

void SettingsWindow::AddSettingCombo(std::string name, int& value,
                                     std::vector<std::string> options) {
    std::string label = "##" + name;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    DrawSettingName(name);

    ImGui::TableNextColumn();
    const float indent = SubSettingIndent(name);
    if (indent > 0.0f) {
        ImGui::Indent(indent);
    }
    const char* combo_value = options[value].c_str();
    const bool combo_open = ImGui::BeginCombo(label.c_str(), combo_value);
    if (indent > 0.0f) {
        ImGui::Unindent(indent);
    }
    if (!combo_open) {
        NoteRowFocus();
    }
    if (combo_open) {
        for (int i = 0; i < options.size(); i++) {
            const bool selected = (i == value);
            if (ImGui::Selectable(options[i].c_str(), selected))
                value = i;

            // Set the initial focus when opening the combo
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

} // namespace ImGuiEmuSettings
