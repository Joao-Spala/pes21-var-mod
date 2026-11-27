#pragma once

#include "i18n.h"
#include "input_manager.h"
#include "var_state.h"

#include <array>
#include <string>
#include <vector>

struct ImFont;

namespace var_mod {

enum class HudPage : int32_t {
    Main      = 0, // REVISAO
    Squad     = 1, // CARTOES (22 jogadores)
    Config    = 2, // OPCOES
    Configure = 3, // Confirmar decisao (dinamica e enxuta)
    Controls  = 4, // OPCOES > Controles (remap de teclas e botoes)
};

// OPCOES rows: 0..5 toggles, 6 = Idioma, 7 = Controles, 8 = Voltar
constexpr int kSettingsRowReplayAfterDecision = 6;
constexpr int kSettingsRowLanguage = 7;
constexpr int kSettingsRowControls = 8;
constexpr int kSettingsRowBack     = 9;
constexpr int kSettingsRowCount    = 10;
// Controles rows: one per BindAction, then Restaurar Padrao, then Voltar
constexpr int kControlsRowRestore  = kBindActionCount;
constexpr int kControlsRowBack     = kBindActionCount + 1;
constexpr int kControlsRowCount    = kBindActionCount + 2;

enum class ConfigField : int32_t {
    Card        = 0,
    RestartType = 1,
    Team        = 2,
    Offender    = 3,
    Spot        = 4,
    Confirm     = 5,
    Back        = 6,
};

struct HudMenuOption {
    const char* label        = "";
    const char* sublabel     = "";
    VarAction   action       = VarAction::None;
    CardKind    presetCard   = CardKind::None;
    bool        keepDecision = false;
    bool        openSquad    = false;
    bool        invertRoles  = false;
    bool        useLiveFoul  = false; // acts on the foul / penalty the match is currently stopped for
    int8_t      annulGoal    = -1;    // annuls the goal the match is stopped for (0 foul or handball, 1 offside)
};

// Pure geometric viewport for the 2D Broadcast Pitch Radar (supports full pitch + auto penalty-box zoom)
struct PitchRadarViewport {
    float minX     = -54.5f;
    float maxX     =  54.5f;
    float minZ     = -36.0f;
    float maxZ     =  36.0f;
    bool  isZoomed = false; // penalty box zoom
    int   zoomSide = 0; // -1 = left goal (-X), +1 = right goal (+X), 0 = full pitch or play view
    bool  isPlayView = false; // automatic close-up of the play away from the boxes (tokens readable)

    static PitchRadarViewport Compute(const PitchDims& dims, const Vec3& focusSpot,
                                      bool autoZoomBox, int zoomOverride, float aspectRatio = 1.65f);

    void WorldToNorm(float wx, float wz, float& outU, float& outV) const;
    Vec3 NormToWorld(float u, float v, const PitchDims& dims) const;
};

// Pure state-machine controller for VAR HUD v4.0 (100% keyboard, gamepad & mouse parity; unit-testable)
class VarHudController {
public:
    VarHudController() = default;

    void UpdateContext(const LiveTelemetry& tel,
                       const ReplaySyncResult& sync,
                       const std::array<PlayerSlotInfo, 22>& players,
                       const std::array<TeamDisplayInfo, 2>& teams,
                       const std::vector<VarEvent>& history,
                       const MatchVarStats& stats,
                       const HudConfig& cfg,
                       uint32_t rosterRev = 0,
                       uint32_t teamsRev = 0,
                       uint32_t historyRev = 0);

    void UpdateReplayMarkers(const std::vector<ReplayMarker>& markers);
    void HandleAction(NavAction action, VarState& state);

    // Direct navigation & Replay Seek helpers (shared by keyboard/gamepad/mouse & unit tests)
    void SelectTab(HudPage tab);
    void SelectMainOption(int index, VarState& state);
    void SelectSquadTeam(int teamIdx);
    void SelectSquadPlayer(int slot);
    void SelectReplayMarker(int markerIndex, VarState& state, bool triggerSeek = true);
    void StepReplayMarker(int delta, VarState& state);
    void ScrubReplayToNorm(float norm, VarState& state);
    void ToggleReplayPlayPause(VarState& state);
    void CycleCameraFocus(VarState& state);
    void InvertDetectedRoles();
    void ToggleRadarZoom();
    void SetDraftCardDirect(CardKind card);
    void SetDraftDirectCardPreset(int preset);
    void SetDraftRestartAction(VarAction restartAction);
    void SetDraftBenefitedTeam(int32_t team);
    void TriggerConfigField(ConfigField field, int delta, VarState& state);
    void TriggerSettingsRow(int row, int delta, VarState& state);
    void SetSelectedIndex(int idx);
    void SetDraftCustomSpot(const Vec3& worldPos);
    void CollapseToReplay();

    // Controls (remap) page
    void OpenControlsPage();
    void SetControlsColumn(int col);
    void BeginBindingCapture(int row, int col, VarState& state);
    void ApplyCapturedBinding(uint16_t code, VarState& state); // called when InputManager captured an input
    void CancelBindingCapture();
    void ClearBindingCell(int row, int col, VarState& state);
    void RestoreDefaultBindings(VarState& state);
    int  ControlsColumn() const { return controlsColumn_; }
    bool IsCapturingBinding() const { return capturing_; }
    int  CaptureRow() const { return captureRow_; }
    int  CaptureColumn() const { return captureCol_; }
    const char* ControlsMessage() const; // translated, nullptr when expired
    bool ControlsMessageIsError() const { return controlsMsgError_; }
    const char* PoolNoticeMessage() const; // translated, nullptr when expired
    // Confirmation toast after a decision is confirmed: translated text and age in ms (nullptr when none)
    const char* ConfirmToast(uint64_t nowMs, uint64_t& outAgeMs) const;
    static constexpr uint64_t kConfirmToastMs = 2600;
    // Summaries rebuilt in the CURRENT language from the verdict fields (stored text is only for logs)
    std::string FormatSummaryOf(const VarVerdict& v) const;
    std::string FormatPoolSummary(const VerdictPool& pool) const;
    void ClearPoolNotice() { poolNoticeMsg_ = nullptr; poolNoticeUntilMs_ = 0; }
    static const char* BindActionLabel(int action); // translated

    bool IsInManualReplay() const { return inManualReplay_; }
    bool IsFocused() const { return focused_; }
    HudPage Page() const { return page_; }
    int SelectedIndex() const { return selectedIndex_; }
    int SquadTeamFilter() const { return squadTeamFilter_; }
    int SelectedMarkerIndex() const { return selectedMarkerIndex_; }
    // The selected marker is the play shown on the card (its context was applied, cursor within 1.8 s of it)
    bool SelectedMarkerApplied() const { return selectedMarkerApplied_; }
    int RadarZoomOverride() const { return radarZoomOverride_; }
    bool GetLockedMarkerTime(int32_t& outPhase, float& outTime) const;

    const std::vector<HudMenuOption>& MainOptions() const { return mainOptions_; }
    const std::vector<ConfigField>& ActiveConfigFields() const { return configFields_; }
    const std::vector<VarEvent>& History() const { return history_; }
    const std::vector<ReplayMarker>& ReplayMarkers() const { return replayMarkers_; }
    const MatchVarStats& Stats() const { return stats_; }
    const HudConfig& Config() const { return config_; }
    const ReplaySyncResult& Sync() const { return sync_; }
    const LiveTelemetry& Telemetry() const { return tel_; }
    const std::array<PlayerSlotInfo, 22>& Players() const { return players_; }
    const std::array<TeamDisplayInfo, 2>& Teams() const { return teams_; }

    // Active incident info (auto-filled from selected timeline marker or live cursor)
    int32_t      DetectedOffenderSlot() const { return detectedOffenderSlot_; }
    int32_t      DetectedVictimSlot() const { return detectedVictimSlot_; }
    bool         HasDetectedContact() const { return hasDetectedContact_; }
    // Offender / victim are real roles (official call, proven contact or set by the user); otherwise the two
    // players are only shown as a duel
    bool         DetectedRolesConfirmed() const { return detectedRolesConfirmed_; }
    IncidentKind DetectedIncidentKind() const { return detectedKind_; }
    // Stored in PT-BR, returned in the selected UI language
    const char*  DetectedIncidentLabel() const { return Tr(detectedLabel_); }
    Vec3         EffectivePreviewSpot() const;
    float        ActiveSignedBoxDistance() const;
    bool         IsEffectiveRadarZoomed() const;

    // Draft verdict fields (Page::Configure)
    VarAction DraftAction() const { return draftAction_; }
    CardKind  DraftCard() const { return draftCard_; }
    CardKind  DraftOrigCard() const { return draftOrigCard_; }
    bool      DraftDirectCardMode() const { return draftDirectCardMode_; }
    int       DraftDirectCardPreset() const { return draftDirectCardPreset_; }
    int32_t   DraftOrigOffenderSlot() const { return draftOrigOffenderSlot_; }
    int32_t   DraftOffenderSlot() const { return draftOffenderSlot_; }
    int32_t   DraftVictimSlot() const { return draftVictimSlot_; }
    uint32_t  DraftSourceEventId() const { return draftSourceEventId_; }
    bool      DraftTargetsLive() const { return draftTargetsLive_; }
    int8_t    DraftAnnulGoal() const { return draftAnnulGoal_; }
    const char* DraftTitle() const; // translated
    int8_t    CurrentRosterOf(int32_t slot) const {
        return (slot >= 0 && slot < 22) ? players_[static_cast<size_t>(slot)].rosterIdx : static_cast<int8_t>(-1);
    }
    int32_t   DraftBenefitedTeam() const { return draftBenefitedTeam_; }
    Vec3      DraftSpotPos() const { return draftSpotPos_; }
    int       DraftSpotPreset() const { return draftSpotPreset_; }
    bool      HasActiveEvent() const { return hasActiveEvent_; }
    const VarEvent& ActiveEvent() const { return activeEvent_; }
    bool      IsPostState13RedRescindWarning() const;

    // Broadcast formatting helpers (#10 NOME, real team names/abbrevs)
    void FormatPlayerBroadcastBuf(int32_t slot, char* outBuf, size_t outSize) const;
    std::string FormatPlayerBroadcast(int32_t slot) const;
    const char* TeamFullName(int32_t team) const;
    const char* TeamShortName(int32_t team) const;
    std::string FormatActiveContextTitle() const;
    std::string FormatVerdictSummary() const;
    void FormatNextCameraFocusLabelBuf(char* outBuf, size_t outSize) const;
    std::string FormatNextCameraFocusLabel() const;

    // Second-to-last defender X coordinate for subtle offside/DOGSO radar reference line
    static bool ComputeSecondLastDefenderX(const PlaySnapshot& snap, int32_t defendingTeam,
                                           float defenderAttackDir, float& outX);

    static const char* CardLabel(CardKind c);
    static const char* DirectCardPresetLabel(int preset);
    static const char* SpotPresetLabel(int preset);
    static const char* EventTypeLabel(VarEventType t);
    static const char* ActionLabel(VarAction a);
    static const char* ScalePresetLabel(int preset);

    // Recorded foul and live FoulInfo describe the same stoppage (same half, close in match time)
    static bool IsSameStoppage(const VarEvent& a, const VarEvent& b);

private:
    void RebuildMainOptions();
    void RebuildConfigFields();
    void ApplySelectedMarkerToContext(const ReplayMarker& m);
    void PreFillDraft(VarAction action, CardKind presetCard, bool invertRoles = false);
    void PreFillDirectCardForSlot(int32_t slot);
    void SyncDirectPresetFromPlayer(int32_t slot);
    void ApplySpotPreset(int preset);
    void ArmCurrentDraft(VarState& state);
    void AdjustField(ConfigField field, int delta, VarState& state);
    void CycleTopLevelTab(int dir);

    bool    inManualReplay_ = false;
    bool    focused_        = false;
    HudPage page_           = HudPage::Main;
    HudPage returnPage_     = HudPage::Main;
    int     selectedIndex_  = 0;
    int     squadTeamFilter_ = 0; // 0 = Home (0..10), 1 = Away (11..21)

    LiveTelemetry tel_{};
    ReplaySyncResult sync_{};
    std::array<PlayerSlotInfo, 22> players_{};
    std::array<TeamDisplayInfo, 2> teams_{};
    std::vector<VarEvent> history_;
    std::vector<ReplayMarker> replayMarkers_;
    MatchVarStats stats_{};
    HudConfig config_{};
    uint32_t cachedRosterRev_  = 0;
    uint32_t cachedTeamsRev_   = 0;
    uint32_t cachedHistoryRev_ = 0;

    // Active timeline marker sync state
    int      selectedMarkerIndex_   = -1;
    bool     manualMarkerLock_      = false;
public:
    bool IsMarkerManuallySelected() const { return manualMarkerLock_; }
    // Plays the user opened this match (click on the bar, arrows, [ ]): the suspect stops pulsing once opened
    bool WasPlayOpened(const ReplayMarker& m) const {
        for (int i = 0; i < openedCount_; ++i) {
            if (openedPhase_[i] == m.matchPhase && std::fabs(openedTime_[i] - m.time60Hz) < 1.0f) return true;
        }
        return false;
    }
private:
    static constexpr int kOpenedPlays = 16;
    int32_t openedPhase_[kOpenedPlays] = {};
    float   openedTime_[kOpenedPlays] = {};
    int     openedCount_ = 0;
    int     openedNext_ = 0;
    bool     restoreMarkerAfterLive_ = false; // context was switched to the stopped-for foul
    bool     detectedOffenderUnknown_ = false; // roles inverted with no known opponent: offender to be picked
    float    lastTrackedCursorTime_ = -1000.0f;
    uint64_t seekLockUntilMs_       = 0;
    int      radarZoomOverride_     = -1; // -1 = Auto, 0 = Full Pitch, 1 = Box Zoom

    bool     hasActiveEvent_ = false;
    VarEvent activeEvent_{};
    // The official foul / penalty the match is stopped for right now (independent of the replay cursor)
    bool     hasLiveFoul_ = false;
    VarEvent liveFoulEvent_{};
    bool     draftTargetsLive_ = false;
    int8_t   draftAnnulGoal_ = -1;
    int      mainIndexBeforeConfigure_ = 0; // Back returns to the option the user came from
    bool     selectedMarkerApplied_ = false;
    bool     draftFromLiveFoul_ = false;
    bool     IsActiveEventTheLiveFoul() const;

    bool         hasDetectedContact_   = false;
    bool         detectedRolesConfirmed_ = false;
    int32_t      detectedOffenderSlot_ = -1;
    int32_t      detectedVictimSlot_   = -1;
    Vec3         detectedSpotPos_{};
    IncidentKind detectedKind_         = IncidentKind::None;
    char         detectedLabel_[48]    = {};

    std::vector<HudMenuOption> mainOptions_;
    std::vector<ConfigField>   configFields_;

    // Controls page state
    int         controlsColumn_     = 0;     // 0 / 1 = keyboard slots, 2 = gamepad
    bool        capturing_          = false;
    int         captureRow_         = -1;
    int         captureCol_         = 0;
    const char* controlsMsg_        = nullptr; // PT-BR key, translated on read
    bool        controlsMsgError_   = false;
    uint64_t    controlsMsgUntilMs_ = 0;
    const char* poolNoticeMsg_      = nullptr; // PT-BR key, translated on read
    const char* toastMsg_           = nullptr; // PT-BR key, translated on read
    uint64_t    toastStartMs_       = 0;
    uint64_t    poolNoticeUntilMs_  = 0;
    void SetControlsMessage(const char* ptMsg, bool isError);

    // Draft verdict configuration
    VarAction draftAction_           = VarAction::None;
    CardKind  draftOrigCard_         = CardKind::None;
    CardKind  draftCard_             = CardKind::None;
    bool      draftDirectCardMode_   = false;
    int       draftDirectCardPreset_ = 0; // 0=0A/0V, 1=1A/0V, 2=2A/1V, 3=0A/1V
    int32_t   draftOrigOffenderSlot_ = -1;
    // Source event slots and who held them then (roster index), captured with the draft
    int32_t   draftEventOffSlot_     = -1;
    int32_t   draftEventVicSlot_     = -1;
    int8_t    draftEventOffRoster_   = -1;
    int8_t    draftEventVicRoster_   = -1;
    int32_t   draftOffenderSlot_     = 0;
    int32_t   draftVictimSlot_       = 11;
    int32_t   draftBenefitedTeam_    = 1;
    Vec3      draftSpotPos_{};
    int       draftSpotPreset_       = 0; // 0=Local do Lance, 1=Entrada da Area, 2=Centro, 3=Personalizado
    bool      draftCustomSpot_       = false;
    uint32_t  draftSourceEventId_    = 0;
};

class VarHudRenderer {
public:
    // Builds the HUD fonts rasterized at the exact UI scale of this resolution (crisp text from 720p to
    // 4K). Must be called OUTSIDE NewFrame/Render. Returns true when the font atlas was (re)built, so the
    // renderer backend must recreate its font texture (ImGui_ImplDX11_InvalidateDeviceObjects).
    static bool EnsureFonts(float viewportW, float viewportH);
    // UI scale for a backbuffer: same share of the screen at any resolution (see the definition)
    static float ComputeUiScale(float viewportW, float viewportH, int preset);
    static constexpr float kMinUiScale = 0.60f;
    static constexpr float kMaxUiScale = 3.00f;
    static void ResetFonts(); // ImGui context destroyed
    static void TickIdle();
    static void RenderFrame(float viewportW, float viewportH);
    static void RequestReset();
    // Read-only view of the live controller (tests / diagnostics)
    static HudPage CurrentPage();
    static bool IsCapturingBinding();
};

} // namespace var_mod
