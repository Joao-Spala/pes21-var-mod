#pragma once

#include "game_structs.h"
#include "i18n.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace var_mod {

enum class VarEventType : int32_t {
    NoFoul    = 0,
    Foul      = 1,
    Penalty   = 2,
    Advantage = 3,
    CardOnly  = 4,
};

enum class IncidentKind : int32_t {
    None         = 0,
    CloseContact = 1,
    BodyCharge   = 2,
    StandTackle  = 3,
    VictimFall   = 4,
    SlideTackle  = 5,
    Advantage    = 6,
    YellowCard   = 7,
    Foul         = 8,
    RedCard      = 9,
    Penalty      = 10,
    Dive         = 11, // simulation (dive / simulation flag), never a foul by the offender
};

enum class VarAction : int32_t {
    None                = 0,
    EditCard            = 1, // Scenario 1: alter card (none/yellow/red/direct)
    ConvertToPenalty    = 2, // Scenario 2A: Free Kick -> Penalty (+ card)
    ConvertToFreeKick   = 3, // Scenario 2B: Penalty -> Free Kick outside box (+ card)
    RetroactiveFreeKick = 4, // Scenario 3A: Play-on -> Free Kick (+ card)
    RetroactivePenalty  = 5, // Scenario 3B: Play-on -> Penalty (+ card)
    OverturnCancel      = 6, // Scenario 4: Overturn Foul/Penalty + revert card + Dropped Ball
    RestartCorner       = 7, // Manual restart as Corner Kick
    RestartGoalKick     = 8, // Manual restart as Goal Kick
    RestartThrowIn      = 9, // Manual restart as Throw-In
};

enum class BannerStyle : uint8_t {
    Info    = 0, // Cyan
    Applied = 1, // Emerald
    RedCard = 2, // Crimson
    Amber   = 3, // Amber/Overturn
};

// User-remappable shortcuts. Each action has 2 keyboard slots (Win32 VK codes, 0 = empty) and 1 XInput
// button mask (0 = empty). Directions and the debug toggle keep fixed gamepad inputs (D-Pad + left stick).
enum class BindAction : int32_t {
    ToggleFocus = 0,
    Confirm,
    Back,
    CancelVerdict,
    PrevTab,
    NextTab,
    QuickSwap,
    PrevIncident,
    NextIncident,
    PlayPause,
    Up,
    Down,
    Left,
    Right,
    ToggleDebug,
    Count
};
constexpr int kBindActionCount = static_cast<int>(BindAction::Count);
constexpr int kKeySlotsPerAction = 2;

struct InputBindings {
    std::array<std::array<uint8_t, kKeySlotsPerAction>, kBindActionCount> keys{};
    std::array<uint16_t, kBindActionCount> pad{};

    static constexpr InputBindings Defaults() {
        InputBindings b{};
        auto set = [&b](BindAction a, uint8_t k1, uint8_t k2, uint16_t padMask) {
            const auto i = static_cast<size_t>(a);
            b.keys[i] = { k1, k2 };
            b.pad[i] = padMask;
        };
        // v4.0 shortcuts (identical defaults, so nothing changes for users who never remap)
        set(BindAction::ToggleFocus,   'V',  0x76 /*F7*/,       0x0080 /*R3*/);
        set(BindAction::Confirm,       0x0D /*Enter*/, 0,       0);
        set(BindAction::Back,          0x1B /*Esc*/, 0x08 /*Backspace*/, 0);
        set(BindAction::CancelVerdict, 0x2E /*Delete*/, 0,      0);
        set(BindAction::PrevTab,       0x21 /*PgUp*/, 'Q',      0);
        set(BindAction::NextTab,       0x22 /*PgDn*/, 'E',      0);
        set(BindAction::QuickSwap,     0x09 /*Tab*/, 0,         0);
        set(BindAction::PrevIncident,  0xDB /*[*/, 0,           0);
        set(BindAction::NextIncident,  0xDD /*]*/, 0,           0);
        set(BindAction::PlayPause,     'P',  0,                 0);
        set(BindAction::Up,            0x26 /*Up*/, 'W',        0);
        set(BindAction::Down,          0x28 /*Down*/, 'S',      0);
        set(BindAction::Left,          0x25 /*Left*/, 'A',      0);
        set(BindAction::Right,         0x27 /*Right*/, 'D',     0);
        set(BindAction::ToggleDebug,   0x79 /*F10*/, 0,         0);
        return b;
    }

    // The gamepad never drives the VAR panel: it only opens / collapses it (R3 by default, remappable).
    // Everything else in the panel is keyboard and mouse, and the pad keeps controlling the Replay.
    static constexpr bool IsPadRemappable(BindAction a) {
        return a == BindAction::ToggleFocus;
    }
    // Actions that must always keep at least one shortcut so the user can never lock themselves out
    static constexpr bool IsEssential(BindAction a) {
        return a == BindAction::ToggleFocus || a == BindAction::Confirm || a == BindAction::Back ||
               a == BindAction::Up || a == BindAction::Down;
    }

    bool operator==(const InputBindings&) const = default;
};

// Binding edits used by the Controls page. column: 0 / 1 = keyboard slots, 2 = gamepad.
// code: Win32 VK (keyboard) or single XInput button mask (gamepad).
enum class BindEditResult : int32_t {
    Saved    = 0, // stored, no conflict
    Moved    = 1, // stored, the same input was taken from another action/slot (swapped)
    Rejected = 2, // invalid input or it would leave an essential action without any shortcut
};
BindEditResult AssignBinding(InputBindings& b, BindAction action, int column, uint16_t code);
BindEditResult ClearBinding(InputBindings& b, BindAction action, int column);
bool HasAnyBinding(const InputBindings& b, BindAction action);

struct HudConfig {
    InputBindings bindings = InputBindings::Defaults();
    int  language        = 0;     // 0 = Portugues (BR), 1 = English
    bool dockLeft        = false; // false = right side, true = left side
    int  scalePreset     = 1;     // 0 = Compact (0.88x), 1 = Default (1.00x), 2 = Large (1.14x)
    bool broadcastAlerts = true;  // Show brief reviewable-incident toast during live play
    bool allowMouse      = true;  // Enable mouse clicks on panel, timeline & radar while panel is focused
    bool showRadar       = true;  // Show broadcast tactical 2D pitch radar inside VAR panel
    bool autoZoomBox     = true;  // Automatically zoom 2D radar into penalty area when play is near box
    bool autoOpenPanel   = true;  // Open VAR panel automatically upon entering Manual Replay
};

struct TeamDisplayInfo {
    char     fullName[64]   = {};
    char     shortName[8]   = {};
    RgbColor primaryColor   = { 54, 152, 245 };
    RgbColor secondaryColor = { 240, 245, 252 };
    bool     hasRealData    = false;
};

struct MatchVarStats {
    uint32_t totalFoulsRecorded     = 0;
    uint32_t totalPenaltiesRecorded = 0;
    uint32_t totalVarInterventions  = 0;
    uint32_t homeYellows            = 0;
    uint32_t homeReds               = 0;
    uint32_t awayYellows            = 0;
    uint32_t awayReds               = 0;
};

struct VarEvent {
    uint32_t     id                   = 0;
    VarEventType type                 = VarEventType::NoFoul;
    int32_t      matchPhase           = 0;
    float        time60Hz             = 0.0f;
    uint32_t     matchTick            = 0;

    SetPlayKind  setPlayKind          = SetPlayKind::None;
    int32_t      benefitedTeam        = 0;  // 0 = Home, 1 = Away
    int32_t      offenderSlot         = -1; // 0..21 or -1
    int32_t      victimSlot           = -1; // 0..21 or -1
    CardKind     cardKind             = CardKind::None;
    uint8_t      offenderYellowsAfter = 0;
    uint8_t      offenderRedsAfter    = 0;
    // Roster index (TeamInfo + 0x04[slot % 11], 0..39) of who held the slot when the event happened.
    // A slot changes owner on substitution: cards and fouls must follow the player, not the slot.
    int8_t       offenderRosterIdx    = -1;
    int8_t       victimRosterIdx      = -1;
    // Signed distance to the offender's defending box with the sides of the moment it was recorded
    float        boxDistM             = 0.0f;
    bool         boxDistValid         = false;

    Vec3         pos{};
    bool         wasAdvantage         = false;
    bool         reviewedByVar        = false;
    char         varNote[64]          = {};
};

struct PlaySnapshot {
    int32_t  matchPhase          = 0;
    float    time60Hz            = 0.0f;
    uint32_t matchTick           = 0;
    Vec3     ballPos{};
    Vec3     playerPos[22]{};
    uint8_t  playerOnPitch[22]{};
    int8_t   playerActionId[22]{};    // PlayerAction + 0x01 (PlayerActionId: 11/12 slide, 13 tackle, 16/17 contact reaction)
    uint32_t playerActionFlags[22]{}; // PlayerAction + 0x08
    int8_t   playerRosterIdx[22]{};   // TeamInfo + 0x04[slot % 11] at this moment (-1 = empty)
    int8_t   playerCauser[22]{};      // PlayerAction + 0x3D: who caused this player's contact reaction (-1 / 0xFF none)
    int32_t  closestA            = -1;
    int32_t  closestB            = -1;
    float    closestDistSq       = 999999.0f;
    int32_t  ballOwnerSlot       = -1; // Real BallState + 0x2601C or closest player fallback
    int32_t  recentBallOwnerSlot = -1; // Last player who held the ball within ~0.8s
};

struct DetectedIncident {
    int32_t      matchPhase   = 0;
    float        time60Hz     = 0.0f;
    uint32_t     matchTick    = 0;
    IncidentKind kind         = IncidentKind::None;
    int32_t      priority     = 0;
    int32_t      offenderSlot = -1;
    int32_t      victimSlot   = -1;
    Vec3         pos{};
    float        contactDistM = 99.0f;
    bool         insideBox    = false;
    bool         nearFoul     = false; // the victim went down from a contact with an identified opponent
    // Signed distance to the relevant penalty box with the sides of THAT moment (sides swap at half-time)
    float        boxDistM     = 0.0f;
    bool         boxDistValid = false;
    float        lastSeenTime60Hz = 0.0f; // last frame of this duel (clustering window)
};

struct PlayerSlotInfo {
    int32_t  slot          = 0;
    int32_t  team          = 0;
    uint32_t playerId      = 0;
    uint8_t  shirtNumber   = 0; // Real kit number (1..99)
    uint8_t  yellows       = 0;
    uint8_t  reds          = 0;
    uint8_t  onPitch       = 1;
    int8_t   rosterIdx     = -1; // TeamInfo + 0x04[slot % 11]: who holds the slot now (0..39)
    char     name[64]      = {};
    char     shirtName[32] = {};

    // Formats broadcast label "#10 NOME" (falls back to name when shirtNumber == 0)
    void FormatBroadcastName(char* outBuf, size_t outSize) const {
        if (!outBuf || outSize == 0) return;
        const char* base = (name[0] != '\0') ? name : shirtName;
        const bool hasNum = (shirtNumber > 0 && shirtNumber <= 99);
        if (hasNum && base[0]) {
            std::snprintf(outBuf, outSize, "#%u %s", static_cast<unsigned>(shirtNumber), base);
        } else if (base[0]) {
            std::snprintf(outBuf, outSize, "%s", base);
        } else if (hasNum) {
            std::snprintf(outBuf, outSize, "#%u", static_cast<unsigned>(shirtNumber));
        } else {
            // Unknown player: generic side + position in the starting XI (the HUD localizes it)
            std::snprintf(outBuf, outSize, "%s %d", team == 1 ? "Fora" : "Casa", (slot % 11) + 1);
        }
    }
};

struct VarVerdict {
    bool     armed            = false;
    uint32_t sequence         = 0;
    VarAction action          = VarAction::None;
    uint32_t sourceEventId    = 0;
    int32_t  origOffenderSlot = -1; // Original offender of source event (supports mistaken identity)
    int32_t  offenderSlot     = -1; // 0..21
    int32_t  victimSlot       = -1; // 0..21
    // Roster index of the player meant by each slot (-1 = whoever holds the slot now). Set from the source
    // event when the slot is the event's, so a substitution in between never moves a card to the substitute.
    int8_t   origOffenderRosterIdx = -1;
    int8_t   offenderRosterIdx     = -1;
    int8_t   victimRosterIdx       = -1;
    int32_t  benefitedTeam    = 0;  // 0 = Home, 1 = Away
    CardKind origCard         = CardKind::None;
    CardKind targetCard       = CardKind::None;
    bool     directCardEdit   = false; // When true, sets exact (directYellows, directReds) on offenderSlot
    uint8_t  directYellows    = 0;
    uint8_t  directReds       = 0;
    Vec3     spotPos{};
    bool     customSpot       = false; // True when user manually adjusted spot on radar/presets
    bool     targetsLiveSetPlay = false; // overturn of the foul the match is stopped for (restart allowed)
    bool     fromLiveFoul       = false; // any decision made on the foul the match is stopped for
    int8_t   liveSetPlayKind    = -1;    // SetPlayInfo kind of that stoppage when armed (identifies it later)
    // Stoppage counter when armed (LiveTelemetry::stoppageCount) and the replay moment of the play: a play
    // decision belongs to the stoppage it was made in; after the game moved on (restart, goal, half-time)
    // it is never applied to a later stoppage.
    uint32_t armStoppage        = 0;
    int32_t  playPhase          = -1;
    float    playTime60Hz       = 0.0f;
    // Real card counts of the offender after execution (read back from the game), -1 = unknown
    int8_t   ledgerYellows      = -1;
    int8_t   ledgerReds         = -1;
    char     summary[128]     = {};
};

struct CardPoolEntry {
    bool       active  = false;
    uint32_t   order   = 0;
    VarVerdict verdict{};
};

// Truncates a UTF-8 string in place so it never ends inside a multi-byte sequence.
inline void Utf8TrimIncompleteTail(char* s) {
    if (!s) return;
    size_t len = std::strlen(s);
    if (len == 0) return;
    size_t i = len;
    size_t cont = 0;
    while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80 && cont < 3) { --i; ++cont; }
    if (i == 0) { s[0] = '\0'; return; }
    const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
    const size_t need = (lead >= 0xF0) ? 3 : (lead >= 0xE0) ? 2 : (lead >= 0xC0) ? 1 : 0;
    if (lead >= 0xC0 && cont < need) s[i - 1] = '\0';
}

// Composite VAR Decision Pool:
// Holds at most ONE play/restart decision (two simultaneous fouls/restarts are invalid) plus independent
// card decisions, all executed atomically and in chronological order when leaving Manual Replay.
// Card entries are keyed so edits never silently replace each other:
//  - direct edit (exact Y/R for a player): one entry per player slot
//  - edit of an official event's card: one entry per source event (re-editing the same event updates it)
//  - edit without a source event: one entry per player slot
constexpr size_t kMaxCardPoolEntries = 32;

inline bool SameCardPoolKey(const VarVerdict& a, const VarVerdict& b) {
    if (a.directCardEdit != b.directCardEdit) return false;
    if (a.directCardEdit) return a.offenderSlot == b.offenderSlot && a.offenderRosterIdx == b.offenderRosterIdx;
    if (a.sourceEventId != 0 || b.sourceEventId != 0) return a.sourceEventId == b.sourceEventId;
    return a.offenderSlot == b.offenderSlot;
}

struct VerdictPool {
    uint32_t                       sequence        = 0;
    bool                           hasPlayDecision = false;
    uint32_t                       playOrder       = 0;
    VarVerdict                     playDecision{};
    std::array<CardPoolEntry, kMaxCardPoolEntries> cardEntries{};

    // Latest active card entry that targets this player (nullptr if none)
    const CardPoolEntry* FindCardForSlot(int32_t slot) const {
        const CardPoolEntry* best = nullptr;
        for (const auto& e : cardEntries) {
            if (e.active && e.verdict.offenderSlot == slot && (!best || e.order > best->order)) best = &e;
        }
        return best;
    }

    // Active card entry with the same key as `v` (nullptr if none)
    const CardPoolEntry* FindSameKey(const VarVerdict& v) const {
        for (const auto& e : cardEntries) {
            if (e.active && SameCardPoolKey(e.verdict, v)) return &e;
        }
        return nullptr;
    }

    size_t CardCount() const {
        size_t n = 0;
        for (const auto& e : cardEntries) {
            if (e.active) ++n;
        }
        return n;
    }

    size_t Count() const {
        return (hasPlayDecision ? 1U : 0U) + CardCount();
    }

    bool Empty() const {
        return !hasPlayDecision && CardCount() == 0;
    }

    bool HasExecutableItems() const {
        if (hasPlayDecision && playDecision.armed && playDecision.action != VarAction::None) {
            return true;
        }
        for (const auto& e : cardEntries) {
            if (e.active && e.verdict.armed && e.verdict.action != VarAction::None) {
                return true;
            }
        }
        return false;
    }

    void FormatCombinedSummary(char* outBuf, size_t outSize) const {
        if (!outBuf || outSize == 0) return;
        outBuf[0] = '\0';
        const size_t total = Count();
        if (total == 0) return;

        const char* first = nullptr;
        const char* second = nullptr;
        if (hasPlayDecision && playDecision.summary[0] != '\0') {
            first = playDecision.summary;
        }
        // Collect active card summaries sorted by insertion order
        std::array<const CardPoolEntry*, kMaxCardPoolEntries> sorted{};
        size_t sc = 0;
        for (const auto& e : cardEntries) {
            if (e.active) sorted[sc++] = &e;
        }
        std::sort(sorted.begin(), sorted.begin() + sc, [](const CardPoolEntry* a, const CardPoolEntry* b) {
            return a->order < b->order;
        });
        for (size_t i = 0; i < sc; ++i) {
            const char* s = sorted[i]->verdict.summary;
            if (!s[0]) continue;
            if (!first) first = s;
            else if (!second) { second = s; break; }
        }

        if (!first) {
            std::snprintf(outBuf, outSize, "%s", "DECISÃO PRONTA"); // translated at display time
            return;
        }
        if (total == 1 || !second) {
            std::snprintf(outBuf, outSize, "%s", first);
        } else if (total == 2) {
            std::snprintf(outBuf, outSize, "%s  +  %s", first, second);
        } else {
            std::snprintf(outBuf, outSize, "%s  (+%zu)", first, total - 1);
        }
        Utf8TrimIncompleteTail(outBuf);
    }
};

struct ReplayControlCommand {
    bool    hasSeek        = false;
    bool    seekByTime     = false;
    int32_t targetPhase    = 0;
    float   targetTime60Hz = 0.0f;
    float   targetNorm     = 0.0f;
    bool    pauseAfterSeek = true;

    bool    hasPlayToggle  = false;
    bool    targetPlaying  = false;

    bool    hasFocusTarget = false;
    int32_t focusSlot      = 0; // 0 = Ball, 1..22 = Player (slot + 1)
};

// Positions some time before the replay cursor, for the tactical pitch trails and heading arrows
struct MotionSample {
    bool    valid = false;
    uint32_t matchTick = 0;
    Vec3    ball{};
    Vec3    pos[22]{};
    uint8_t onPitch[22]{};
};

struct ReplaySyncResult {
    bool         hasEvent    = false;
    VarEvent     event{};
    bool         hasSnapshot = false;
    PlaySnapshot snapshot{};
    float        deltaTicks  = 0.0f;
};

// Timeline marker for Replay progress bar (official referee events + physical engine detections)
struct ReplayMarker {
    float        time60Hz        = 0.0f;
    int32_t      matchPhase      = 0;
    float        timelineNorm    = 0.0f; // 0.0 .. 1.0 along current replay window
    bool         isOfficialEvent = false;
    uint32_t     officialEventId = 0;
    IncidentKind kind            = IncidentKind::None;
    int32_t      priority        = 0;
    VarEventType eventType       = VarEventType::NoFoul;
    CardKind     cardKind        = CardKind::None;
    int32_t      offenderSlot    = -1;
    int32_t      victimSlot      = -1;
    Vec3         pos{};
    float        contactDistM    = 99.0f;
    float        signedBoxDistM  = 99.0f;
    bool         nearFoul        = false;
    // 0..100: how likely this is a foul the referee did not give (official events score 0). The single
    // strongest marker of the window at or above kSuspectThreshold is flagged as the play to review.
    int32_t      suspicion       = 0;
    bool         topSuspect      = false;
    char         label[48]       = {};
};

struct LiveTelemetry {
    bool enabled          = true;
    bool offlineAllowed   = true;
    bool siderOverlayOpen = false;
    bool hooksInstalled   = false;
    bool replaySeekReady  = false;
    bool matchActive      = false;

    uintptr_t gmatchPtr       = 0;
    uintptr_t matchFsmPtr     = 0;
    uintptr_t matchControlPtr = 0;
    uintptr_t matchInfoPtr    = 0;
    uintptr_t replayMgrPtr    = 0;

    int32_t  matchPhase      = 0;
    int32_t  matchStatus     = 0;
    int32_t  fsmState        = -1;
    uint32_t matchTick       = 0;
    float    time60Hz        = 0.0f;
    uint8_t  specialModeKind = 0;

    uint8_t  replayMode             = 0;
    uint8_t  replayManualFlag       = 0;
    uint8_t  replayIsPlaying        = 0;
    uint8_t  replayUiHidden         = 0;
    float    replayCursorTime       = 0.0f;
    int32_t  replayCursorPhase      = 0;
    float    replayWindowStartTime  = 0.0f;
    int32_t  replayWindowStartPhase = 0;
    float    replayWindowEndTime    = 0.0f;
    int32_t  replayWindowEndPhase   = 0;
    int32_t  replayFocusSlot        = 0; // 0 = ball, 1..22 = players
    float    replayTimelineNorm     = 0.0f;

    int32_t activeSetPlayKind   = 0;
    uint32_t stoppageCount      = 0; // ball in play <-> out of play switches in this match (sim thread)
    int32_t activeBenefitedTeam = 0;
    uint8_t activeFoulValid     = 0;
    int32_t activeFoulKind      = 0;
    int32_t activeFoulCard      = 0;
    int32_t activeOffenderSlot  = -1;
    int32_t activeVictimSlot    = -1;

    PitchDims pitchDims{};
    float     teamAttackDir[2] = { -1.0f, 1.0f };
    Vec3      liveBallPos{};

    uint64_t mainTickCount      = 0;
    uint64_t finalizeFrameCount = 0;
    uint64_t foulCommitCount    = 0;

    char        lastBannerText[128]  = {};
    uint64_t    lastBannerStartMs    = 0;
    uint64_t    lastBannerUntilMs    = 0;
    BannerStyle lastBannerStyle      = BannerStyle::Applied;

    char        alertBannerText[128] = {};
    uint64_t    alertBannerStartMs   = 0;
    uint64_t    alertBannerUntilMs   = 0;
    BannerStyle alertBannerStyle     = BannerStyle::Info;
};

// Reusable render-thread cache populated under a SINGLE mutex lock per frame (zero heap churn!)
struct HudRenderSnapshot {
    LiveTelemetry                  tel{};
    VarVerdict                     pending{};
    VerdictPool                    pool{};
    MatchVarStats                  stats{};
    HudConfig                      cfg{};
    std::array<PlayerSlotInfo, 22> players{};
    std::array<TeamDisplayInfo, 2> teams{};
    std::vector<VarEvent>          history;
    ReplaySyncResult               sync{};
    std::vector<ReplayMarker>      markers;

    uint32_t rosterRev      = 0;
    uint32_t teamsRev       = 0;
    uint32_t historyRev     = 0;
    uint32_t snapRev        = 0;
    uint32_t syncDataRev    = 0;
    uint32_t syncSnapRev    = 0;
    int32_t  syncPhase      = -999;
    float    syncCursorTime = -1e9f;
    uint32_t markersDataRev = 0;
    uint32_t markersSnapRev = 0;
    int32_t  markersPhase   = -999;
    float    markersWinStart = -1e9f;
    float    markersWinEnd   = -1e9f;
};

class VarState {
public:
    static constexpr size_t kMaxEvents    = 128;
    // One snapshot per simulation frame (60 Hz, the replay's own frame rate): 90 s, about 2.6 MB
    static constexpr size_t kMaxSnapshots = 5400;
    static constexpr size_t kMaxIncidents = 160; // Clustered physical incidents

    static VarState& Instance();

    void ResetMatch();
    // Bumped by every ResetMatch so the sim thread knows its pushed names were wiped
    uint32_t MatchGeneration() const { return matchGeneration_.load(std::memory_order_acquire); }

    // Controls (called by Lua / UI)
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled_.load(std::memory_order_relaxed); }

    void SetOfflineAllowed(bool allowed);
    bool IsOfflineAllowed() const { return offlineAllowed_.load(std::memory_order_relaxed); }

    void SetSiderOverlayOpen(bool open);
    bool IsSiderOverlayOpen() const;

    void SetHooksInstalled(bool installed);
    bool AreHooksInstalled() const { return hooksInstalled_.load(std::memory_order_relaxed); }

    void SetReplaySeekReady(bool ready);
    bool IsReplaySeekReady() const { return replaySeekReady_.load(std::memory_order_relaxed); }

    // Panel focus, manual replay & mouse hit-test state (shared between HUD and input hooks)
    void SetPanelFocused(bool focused);
    bool IsPanelFocused() const { return panelFocused_.load(std::memory_order_acquire); }
    bool ShouldBlockGameInputNow(uint64_t nowMs) const;
    void ClearReplayFocusAndGrace();

    void SetInManualReplay(bool inManual);
    bool IsInManualReplay() const { return inManualReplay_.load(std::memory_order_acquire); }

    void SetMouseOverPanel(bool over) { mouseOverPanel_.store(over, std::memory_order_relaxed); }
    bool IsMouseOverPanel() const { return mouseOverPanel_.load(std::memory_order_relaxed); }

    void SetDebugOverlayOpen(bool open) { debugOverlayOpen_.store(open, std::memory_order_relaxed); }
    bool IsDebugOverlayOpen() const { return debugOverlayOpen_.load(std::memory_order_relaxed); }

    // Lock-free fast-path check for DX11 Present (skips ImGui::NewFrame/Render when nothing visible)
    bool ShouldRenderAnyHud(uint64_t nowMs) const;

    // HUD Config & INI persistence (var_mod.ini)
    void SetIniPath(const std::wstring& path);
    void LoadHudConfig();
    void SaveHudConfig() const;
    HudConfig GetHudConfig() const;
    void SetHudConfig(const HudConfig& cfg, bool persist = true);
    bool IsMouseAllowed() const { return allowMouse_.load(std::memory_order_relaxed); }
    // Bumped on every SetHudConfig: lets the render thread re-read bindings lock-free only on change
    uint32_t ConfigRevision() const { return configRevision_.load(std::memory_order_acquire); }

    // Recording API (called from simulation thread / live poll)
    uint32_t PushEvent(const VarEvent& ev);
    // Id of the most recent official (engine-called) foul or penalty event of this offender, 0 if none
    uint32_t FindLatestOfficialFoulEventId(int32_t offenderSlot) const;
    // Removes non-direct card edits made on one official event
    void RemoveCardEntriesForEvent(uint32_t eventId);
    void PushSnapshot(const PlaySnapshot& snap);
    void PushDetectedIncident(const DetectedIncident& inc);
    void UpdatePlayers(const std::array<PlayerSlotInfo, 22>& players);
    void UpdateTeams(const std::array<TeamDisplayInfo, 2>& teams);
    void UpdateTelemetry(const LiveTelemetry& tel);
    void PatchLiveReplayTelemetry(bool matchActive, uintptr_t gmatchPtr, uintptr_t matchInfoPtr,
                                  uintptr_t replayMgrPtr, int32_t matchPhase, int32_t matchStatus,
                                  int32_t fsmState, uint8_t specialModeKind,
                                  uint8_t replayMode, uint8_t replayManualFlag,
                                  uint8_t replayIsPlaying, uint8_t replayUiHidden,
                                  float replayCursorTime, int32_t replayCursorPhase,
                                  float replayWinStartTime, int32_t replayWinStartPhase,
                                  float replayWinEndTime, int32_t replayWinEndPhase,
                                  int32_t replayFocusSlot, float replayTimelineNorm,
                                  int32_t activeSetPlayKind, int32_t activeBenefitedTeam,
                                  uint8_t activeFoulValid, int32_t activeFoulKind,
                                  int32_t activeFoulCard, int32_t activeOffenderSlot,
                                  int32_t activeVictimSlot);

    // Query API (called from DX11 HUD / Lua)
    void CaptureHudRenderSnapshot(HudRenderSnapshot& io) const;
    LiveTelemetry GetTelemetry() const;
    std::array<PlayerSlotInfo, 22> GetPlayers() const;
    std::array<TeamDisplayInfo, 2> GetTeams() const;
    std::vector<VarEvent> GetRecentEvents(size_t maxCount = 32) const;
    MatchVarStats GetStats() const;
    ReplaySyncResult SyncToReplayCursor(int32_t cursorPhase, float cursorTime60Hz, float eventToleranceTicks = 210.0f) const;
    // out[k] = the frame (k + 1) * kMotionStepFrames simulation frames before the frame nearest to the cursor
    // (same match phase, within 3 frames of that target, never across a gap). Real time, whatever the match
    // length. Used for 1.5 s trails and the heading of every player.
    static constexpr int      kMotionSamples    = 10;
    static constexpr uint32_t kMotionStepFrames = 9; // 0.15 s
    void SampleMotionHistory(int32_t phase, float time60Hz, MotionSample (&out)[kMotionSamples]) const;
    std::vector<ReplayMarker> GetReplayWindowMarkers(int32_t winPhase, float winStartTime, float winEndTime) const;

    // Native ReplayController command queue (HUD -> ManualReplayController::SyncToReplayManager)
    void RequestReplaySeekToTime(int32_t phase, float time60Hz, float fallbackNorm, bool pauseAfterSeek = true);
    void RequestReplaySeekToNorm(float norm, bool pauseAfterSeek = true);
    void RequestReplaySetPlaying(bool playing);
    void RequestReplayCameraFocus(int32_t focusSlot);
    bool ConsumeReplayCommand(ReplayControlCommand& outCmd);

    // Verdict Pool queue (DX11 HUD -> Simulation thread)
    bool HasArmedVerdict() const { return hasArmedVerdict_.load(std::memory_order_acquire); }
    void ArmVerdict(const VarVerdict& verdict);
    void AddToVerdictPool(const VarVerdict& verdict, bool* outReplacedPlay = nullptr);
    void RemovePlayFromPool();
    // Replaces the summary of the pending play decision (after a card edit was merged into it)
    void SetPlayDecisionSummary(const char* summary);
    void RemoveCardFromPool(int32_t slot);          // every card entry targeting this player
    void RemoveCardPoolEntry(uint32_t sequence);    // one entry, by its unique sequence
    // Same as AddToVerdictPool, also reports when a full pool evicted its oldest card edit
    // Returns false when the decision was refused (VAR off, offline mode blocked, nothing to execute)
    bool AddToVerdictPoolEx(const VarVerdict& verdict, bool* outReplacedPlay, bool* outEvictedCard);
    void CancelVerdict();
    VarVerdict PeekVerdict() const;
    VerdictPool PeekVerdictPool() const;
    bool ConsumeVerdict(VarVerdict& outVerdict);
    bool ConsumeVerdictPool(VerdictPool& outPool);
    // Consumes only if gate(pool, user) returns true; the gate runs under the same lock as the consume, so no
    // edit can slip in between the check and the take.
    bool ConsumeVerdictPoolIf(bool (*gate)(const VerdictPool&, void*), void* user, VerdictPool& outPool);
    // Puts back a consumed pool that wrote nothing, only into the same match. Items armed since then win on the
    // same key and on the play slot. Returns true when restored.
    bool RequeueVerdictPool(const VerdictPool& pool, uint32_t matchGeneration);
    // Removes a pending play decision armed in an earlier stoppage (cards of the pool stay). Returns true
    // when one was removed. Overturns of an already taken call are kept (they only revoke a card).
    bool DropStalePlayDecision(uint32_t currentStoppage);
    // Offender of the strongest physical incident in [time - window, time] of this half, or -1
    int32_t RecentIncidentOffender(int32_t matchPhase, float time60Hz, float window60Hz) const;
    void UpdateEventAfterVerdict(const VarVerdict& applied);
    void IncrementInterventions();
    void SetPostApplyBanner(const char* text, uint64_t durationMs = 4500, BannerStyle style = BannerStyle::Applied);
    void SetBroadcastAlert(const char* text, uint64_t durationMs = 3400, BannerStyle style = BannerStyle::Info);

    // Pure incident/contact detection helpers (100% unit-testable)
    static void ComputeClosestContact(PlaySnapshot& snap);
    static bool DetectPhysicalIncident(const PlaySnapshot& snap, const PitchDims& dims,
                                       const float teamAttackDir[2], DetectedIncident& outInc);
    static void FormatIncidentLabel(IncidentKind kind, bool insideBox, char* outBuf, size_t outSize);
    // Suspicion of a missed foul for a timeline marker (see ReplayMarker::suspicion)
    static constexpr int32_t kSuspectThreshold = 60;
    static int32_t SuspicionScore(const ReplayMarker& m);
    static void FlagTopSuspect(std::vector<ReplayMarker>& markers);

private:
    VarState();
    ReplaySyncResult SyncToReplayCursorLocked(int32_t cursorPhase, float cursorTime60Hz, float eventToleranceTicks) const;
    void BuildReplayWindowMarkersLocked(int32_t winPhase, float winStartTime, float winEndTime,
                                        std::vector<ReplayMarker>& outMarkers) const;

    mutable std::mutex mutex_;
    std::atomic<bool> enabled_{ true };
    std::atomic<bool> offlineAllowed_{ true };
    std::atomic<bool> siderOverlayOpen_{ false };
    std::atomic<uint64_t> siderOverlayLastSeenMs_{ 0 };
    std::atomic<bool> hooksInstalled_{ false };
    std::atomic<bool> replaySeekReady_{ false };
    std::atomic<bool> panelFocused_{ false };
    std::atomic<bool> inManualReplay_{ false };
    std::atomic<bool> mouseOverPanel_{ false };
    std::atomic<bool> debugOverlayOpen_{ false };
    std::atomic<bool> allowMouse_{ true };
    std::atomic<uint32_t> configRevision_{ 1 };
    std::atomic<bool> broadcastAlerts_{ true };
    std::atomic<bool> liveManualReplaySignal_{ false };
    std::atomic<bool> hasArmedVerdict_{ false };
    std::atomic<uint64_t> inputBlockGraceUntilMs_{ 0 };
    std::atomic<uint64_t> activeBannerUntilMs_{ 0 };
    std::atomic<uint64_t> activeAlertUntilMs_{ 0 };

    std::wstring iniPath_;
    HudConfig hudConfig_{};
    MatchVarStats stats_{};

    std::array<VarEvent, kMaxEvents> events_{};
    size_t eventHead_ = 0;
    size_t eventCount_ = 0;
    uint32_t nextEventId_ = 1;

    std::vector<PlaySnapshot> snapshots_;
    // Phase and time of each ring slot, so the replay cursor search walks 8-byte keys instead of whole snapshots.
    // Interpolation limits: frames more than 3 simulation frames apart are not neighbours (dropped frames
    // tolerated), and a move above 3 m between two consecutive frames is a teleport (restart, substitution).
    static constexpr uint32_t kMaxInterpolationFrameGap = 3;
    static constexpr float kMaxInterpolationStepSq  = 9.0f;
    void InterpolateSnapshotLocked(size_t idx, size_t age, float cursorTime60Hz, PlaySnapshot& out) const;
    // time60Hz is the match clock, which runs faster than real time with shorter match lengths (2.3 ticks per
    // simulation frame measured in a user log). Neighbours, gaps and speeds use matchTick, the 60 Hz simulation
    // frame counter (MatchInfo + 0x1198), so they hold for any match length.
    struct SnapKey { int32_t phase; float time60Hz; uint32_t tick; };
    std::vector<SnapKey> snapKeys_;
    size_t snapHead_ = 0;
    size_t snapCount_ = 0;
    // Snapshot pushes only invalidate what reads snapshots (sync and markers), not the event history
    uint32_t snapRevision_ = 1;

    std::array<DetectedIncident, kMaxIncidents> incidents_{};
    size_t incHead_ = 0;
    size_t incCount_ = 0;

    uint32_t dataRevision_   = 1;
    uint32_t rosterRevision_ = 1;
    uint32_t teamsRevision_  = 1;
    std::atomic<uint32_t> matchGeneration_{ 0 };

    std::array<PlayerSlotInfo, 22> players_{};
    std::array<TeamDisplayInfo, 2> teams_{};
    LiveTelemetry telemetry_{};

    VerdictPool verdictPool_{};
    uint32_t verdictSeq_ = 1;

    ReplayControlCommand pendingReplayCmd_{};
    std::atomic<bool> hasPendingReplayCmd_{ false };
};

} // namespace var_mod
