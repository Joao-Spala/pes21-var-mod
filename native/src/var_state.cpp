#include "var_state.h"
#include "i18n.h"
#include "version.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace var_mod {

// Body reach (metres between player centres, the engine positions are the feet): two bodies touch at about
// 0.9 m, a standing tackle reaches about 1.1 m, a slide about 1.8 m. A causer the engine recorded further than
// 3 m away belongs to an earlier contact.
constexpr float kTouchDistM          = 0.9f;
constexpr float kStandReachM         = 1.1f;
constexpr float kSlideReachM         = 1.8f;
constexpr float kAdjacentChallengerM = 1.3f;
constexpr float kCauserMaxDistM      = 3.0f;

namespace {

std::string TrimAscii(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool ParseBoolStr(const std::string& v, bool fallback) {
    if (v == "1" || v == "true" || v == "TRUE") return true;
    if (v == "0" || v == "false" || v == "FALSE") return false;
    return fallback;
}

// INI names for key_* / pad_* entries, indexed by BindAction
constexpr const char* kBindIniNames[kBindActionCount] = {
    "toggle_panel", "confirm", "back", "cancel_verdict", "prev_tab", "next_tab", "swap",
    "prev_incident", "next_incident", "play_pause", "up", "down", "left", "right", "debug",
};

int FindBindAction(const char* iniName) {
    for (int a = 0; a < kBindActionCount; ++a) {
        if (std::strcmp(kBindIniNames[a], iniName) == 0) return a;
    }
    return -1;
}

bool IsBindableVk(uint8_t vk) {
    if (vk == 0) return false;
    if (vk <= 0x06 && vk != 0x03) return false;      // mouse buttons are reserved for the UI
    if (vk >= 0x10 && vk <= 0x12) return false;      // generic Shift/Ctrl/Alt (L/R variants are used)
    if (vk == 0xFF) return false;
    return true;
}

bool IsBindablePadMask(uint16_t m) {
    constexpr uint16_t kAllowed = 0x0010 | 0x0020 | 0x0040 | 0x0080 | 0x0100 | 0x0200 |
                                  0x1000 | 0x2000 | 0x4000 | 0x8000;
    return m != 0 && (m & (m - 1)) == 0 && (m & kAllowed) == m; // exactly one face/shoulder/stick/menu button
}

// Makes any INI/UI-provided binding set valid: no invalid codes, no duplicates across actions,
// compacted slots and every essential action reachable. Essential actions win duplicate conflicts.
void SanitizeBindings(InputBindings& b) {
    const InputBindings def = InputBindings::Defaults();
    // 1. Normalize every cell: invalid codes out, compact slots, pad only where it is remappable
    for (int a = 0; a < kBindActionCount; ++a) {
        auto& k = b.keys[static_cast<size_t>(a)];
        for (auto& vk : k) if (!IsBindableVk(vk)) vk = 0;
        if (k[0] == k[1]) k[1] = 0;
        if (k[0] == 0 && k[1] != 0) { k[0] = k[1]; k[1] = 0; }
        const BindAction act = static_cast<BindAction>(a);
        uint16_t& p = b.pad[static_cast<size_t>(a)];
        if (!InputBindings::IsPadRemappable(act)) {
            p = 0;
        } else if (p != 0 && !IsBindablePadMask(p)) {
            p = 0;
        }
    }

    // 2. Restore unreachable essential actions, then remove duplicates (essential actions claim their
    //    inputs first). Repeated because removing a duplicate can leave an essential action empty again.
    for (int iter = 0; iter < 3; ++iter) {
        bool restored = false;
        for (int a = 0; a < kBindActionCount; ++a) {
            const BindAction act = static_cast<BindAction>(a);
            auto& k = b.keys[static_cast<size_t>(a)];
            const bool padOk = InputBindings::IsPadRemappable(act) && b.pad[static_cast<size_t>(a)] != 0;
            if (InputBindings::IsEssential(act) && k[0] == 0 && k[1] == 0 && !padOk) {
                k = def.keys[static_cast<size_t>(a)];
                if (InputBindings::IsPadRemappable(act)) b.pad[static_cast<size_t>(a)] = def.pad[static_cast<size_t>(a)];
                restored = true;
            }
        }
        if (iter > 0 && !restored) break;

        bool usedKey[256] = {};
        uint16_t usedPad = 0;
        for (int pass = 0; pass < 2; ++pass) {
            for (int a = 0; a < kBindActionCount; ++a) {
                const BindAction act = static_cast<BindAction>(a);
                if (InputBindings::IsEssential(act) != (pass == 0)) continue;
                auto& k = b.keys[static_cast<size_t>(a)];
                for (auto& vk : k) {
                    if (vk == 0) continue;
                    if (usedKey[vk]) vk = 0; else usedKey[vk] = true;
                }
                if (k[0] == 0 && k[1] != 0) { k[0] = k[1]; k[1] = 0; }
                uint16_t& p = b.pad[static_cast<size_t>(a)];
                if (p != 0) {
                    if (usedPad & p) p = 0; else usedPad |= p;
                }
            }
        }
    }
}

bool IsPhaseCompatible(int32_t a, int32_t b) {
    return (a / 2) == (b / 2);
}

} // namespace

// Counts keyboard slots plus the remappable gamepad button. Directions are fixed on the D-Pad, so
// for Up/Down "reachable" means at least one keyboard key (keyboard-only users must never get stuck).
bool HasAnyBinding(const InputBindings& b, BindAction action) {
    const auto i = static_cast<size_t>(action);
    if (i >= static_cast<size_t>(kBindActionCount)) return false;
    if (b.keys[i][0] != 0 || b.keys[i][1] != 0) return true;
    return InputBindings::IsPadRemappable(action) && b.pad[i] != 0;
}

BindEditResult AssignBinding(InputBindings& b, BindAction action, int column, uint16_t code) {
    const int a = static_cast<int>(action);
    if (a < 0 || a >= kBindActionCount || column < 0 || column > 2) return BindEditResult::Rejected;
    const InputBindings before = b;

    if (column == 2) {
        if (!InputBindings::IsPadRemappable(action) || !IsBindablePadMask(code)) return BindEditResult::Rejected;
        uint16_t& target = b.pad[static_cast<size_t>(a)];
        const uint16_t prev = target;
        if (prev == code) return BindEditResult::Saved;
        int owner = -1;
        for (int o = 0; o < kBindActionCount; ++o) {
            if (o != a && InputBindings::IsPadRemappable(static_cast<BindAction>(o)) &&
                b.pad[static_cast<size_t>(o)] == code) { owner = o; break; }
        }
        target = code;
        if (owner < 0) return BindEditResult::Saved;
        b.pad[static_cast<size_t>(owner)] = prev; // swap
        if (InputBindings::IsEssential(static_cast<BindAction>(owner)) &&
            !HasAnyBinding(b, static_cast<BindAction>(owner))) {
            b = before;
            return BindEditResult::Rejected;
        }
        return BindEditResult::Moved;
    }

    if (code > 0xFF || !IsBindableVk(static_cast<uint8_t>(code))) return BindEditResult::Rejected;
    const uint8_t vk = static_cast<uint8_t>(code);
    uint8_t& target = b.keys[static_cast<size_t>(a)][static_cast<size_t>(column)];
    const uint8_t prev = target;
    if (prev == vk) return BindEditResult::Saved;
    int ownerA = -1, ownerS = -1;
    for (int o = 0; o < kBindActionCount && ownerA < 0; ++o) {
        for (int s = 0; s < kKeySlotsPerAction; ++s) {
            if ((o != a || s != column) && b.keys[static_cast<size_t>(o)][static_cast<size_t>(s)] == vk) {
                ownerA = o; ownerS = s; break;
            }
        }
    }
    target = vk;
    if (ownerA < 0) return BindEditResult::Saved;
    b.keys[static_cast<size_t>(ownerA)][static_cast<size_t>(ownerS)] = prev; // swap
    if (InputBindings::IsEssential(static_cast<BindAction>(ownerA)) &&
        !HasAnyBinding(b, static_cast<BindAction>(ownerA))) {
        b = before;
        return BindEditResult::Rejected;
    }
    return BindEditResult::Moved;
}

BindEditResult ClearBinding(InputBindings& b, BindAction action, int column) {
    const int a = static_cast<int>(action);
    if (a < 0 || a >= kBindActionCount || column < 0 || column > 2) return BindEditResult::Rejected;
    if (column == 2 && !InputBindings::IsPadRemappable(action)) return BindEditResult::Rejected;
    const InputBindings before = b;
    if (column == 2) b.pad[static_cast<size_t>(a)] = 0;
    else b.keys[static_cast<size_t>(a)][static_cast<size_t>(column)] = 0;
    if (InputBindings::IsEssential(action) && !HasAnyBinding(b, action)) {
        b = before;
        return BindEditResult::Rejected;
    }
    return BindEditResult::Saved;
}

namespace {

float DistSq2D(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

void InitDefaultTeams(std::array<TeamDisplayInfo, 2>& teams) {
    teams[0] = TeamDisplayInfo{};
    teams[0].primaryColor   = RgbColor{ 48, 152, 245 };
    teams[0].secondaryColor = RgbColor{ 240, 246, 255 };
    teams[0].hasRealData    = false;

    teams[1] = TeamDisplayInfo{};
    teams[1].primaryColor   = RgbColor{ 238, 76, 86 };
    teams[1].secondaryColor = RgbColor{ 255, 244, 244 };
    teams[1].hasRealData    = false;
}

} // namespace

VarState& VarState::Instance() {
    static VarState inst;
    return inst;
}

VarState::VarState() {
    ResetMatch();
}

void VarState::ResetMatch() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshots_.size() != kMaxSnapshots) {
        snapshots_.resize(kMaxSnapshots);
    }
    if (snapKeys_.size() != kMaxSnapshots) {
        snapKeys_.resize(kMaxSnapshots);
    }
    eventHead_ = 0;
    eventCount_ = 0;
    nextEventId_ = 1;
    snapHead_ = 0;
    snapCount_ = 0;
    ++snapRevision_;
    incHead_ = 0;
    incCount_ = 0;
    ++dataRevision_;
    ++rosterRevision_;
    ++teamsRevision_;
    matchGeneration_.fetch_add(1, std::memory_order_acq_rel);
    verdictPool_ = VerdictPool{};
    hasArmedVerdict_.store(false, std::memory_order_release);
    pendingReplayCmd_ = ReplayControlCommand{};
    hasPendingReplayCmd_.store(false, std::memory_order_release);
    stats_ = MatchVarStats{};
    panelFocused_.store(false, std::memory_order_relaxed);
    inManualReplay_.store(false, std::memory_order_relaxed);
    liveManualReplaySignal_.store(false, std::memory_order_relaxed);
    mouseOverPanel_.store(false, std::memory_order_relaxed);
    inputBlockGraceUntilMs_.store(0, std::memory_order_relaxed);
    activeBannerUntilMs_.store(0, std::memory_order_relaxed);
    activeAlertUntilMs_.store(0, std::memory_order_relaxed);

    InitDefaultTeams(teams_);
    for (int i = 0; i < 22; ++i) {
        players_[i] = PlayerSlotInfo{};
        players_[i].slot = i;
        players_[i].team = (i < 11) ? 0 : 1;
        players_[i].shirtNumber = 0; // unknown until the real roster is read from the game
        players_[i].onPitch = 1;
    }

    telemetry_.matchActive = false;
    telemetry_.matchTick = 0;
    telemetry_.time60Hz = 0.0f;
    telemetry_.replayMode = 0;
    telemetry_.replayManualFlag = 0;
    telemetry_.replayIsPlaying = 0;
    telemetry_.lastBannerText[0] = '\0';
    telemetry_.lastBannerStartMs = 0;
    telemetry_.lastBannerUntilMs = 0;
    telemetry_.alertBannerText[0] = '\0';
    telemetry_.alertBannerStartMs = 0;
    telemetry_.alertBannerUntilMs = 0;
}

void VarState::SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        CancelVerdict();
        ClearReplayFocusAndGrace();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    telemetry_.enabled = enabled;
}

void VarState::SetOfflineAllowed(bool allowed) {
    offlineAllowed_.store(allowed, std::memory_order_relaxed);
    if (!allowed) {
        CancelVerdict();
        ClearReplayFocusAndGrace();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    telemetry_.offlineAllowed = allowed;
}

void VarState::SetPanelFocused(bool focused) {
    // Lock-free relaxed fast-return avoids per-frame LOCK XCHG when state is unchanged
    if (panelFocused_.load(std::memory_order_relaxed) == focused) {
        return;
    }
    const bool prev = panelFocused_.exchange(focused, std::memory_order_acq_rel);
    if (prev && !focused) {
        inputBlockGraceUntilMs_.store(GetTickCount64() + 260ULL, std::memory_order_release);
    } else if (focused) {
        inputBlockGraceUntilMs_.store(0, std::memory_order_release);
    }
}

void VarState::SetInManualReplay(bool inManual) {
    // Fast-return only when there is nothing left to clear (focus is always dropped on exit)
    if (inManualReplay_.load(std::memory_order_relaxed) == inManual &&
        (inManual || !panelFocused_.load(std::memory_order_relaxed))) {
        return;
    }
    inManualReplay_.store(inManual, std::memory_order_release);
    if (!inManual) {
        panelFocused_.store(false, std::memory_order_release);
        mouseOverPanel_.store(false, std::memory_order_relaxed);
        inputBlockGraceUntilMs_.store(0, std::memory_order_release);
        // A seek / pause / focus queued in this replay must never fire in the next one
        hasPendingReplayCmd_.store(false, std::memory_order_release);
    }
}

void VarState::ClearReplayFocusAndGrace() {
    panelFocused_.store(false, std::memory_order_release);
    inManualReplay_.store(false, std::memory_order_release);
    liveManualReplaySignal_.store(false, std::memory_order_release);
    mouseOverPanel_.store(false, std::memory_order_relaxed);
    inputBlockGraceUntilMs_.store(0, std::memory_order_release);
    hasPendingReplayCmd_.store(false, std::memory_order_release);
}

bool VarState::ShouldBlockGameInputNow(uint64_t nowMs) const {
    // Sub-nanosecond relaxed check for 99.9% of frames outside VAR panel focus
    const bool focused = panelFocused_.load(std::memory_order_acquire);
    const uint64_t graceUntil = inputBlockGraceUntilMs_.load(std::memory_order_acquire);
    if (!focused && graceUntil == 0) {
        return false;
    }
    if (!enabled_.load(std::memory_order_relaxed) ||
        !offlineAllowed_.load(std::memory_order_relaxed) ||
        IsSiderOverlayOpen()) {
        return false;
    }
    if (!inManualReplay_.load(std::memory_order_acquire) &&
        !liveManualReplaySignal_.load(std::memory_order_acquire)) {
        return false;
    }
    if (focused) {
        return true;
    }
    return nowMs < graceUntil;
}

void VarState::SetSiderOverlayOpen(bool open) {
    if (open) {
        siderOverlayLastSeenMs_.store(GetTickCount64(), std::memory_order_relaxed);
        siderOverlayOpen_.store(true, std::memory_order_relaxed);
    } else {
        siderOverlayLastSeenMs_.store(0, std::memory_order_relaxed);
        siderOverlayOpen_.store(false, std::memory_order_relaxed);
    }
}

bool VarState::IsSiderOverlayOpen() const {
    if (!siderOverlayOpen_.load(std::memory_order_relaxed)) {
        return false;
    }
    const uint64_t lastSeen = siderOverlayLastSeenMs_.load(std::memory_order_relaxed);
    if (lastSeen == 0) {
        return false;
    }
    const uint64_t now = GetTickCount64();
    return (now >= lastSeen) && ((now - lastSeen) < 150ULL);
}

void VarState::SetHooksInstalled(bool installed) {
    hooksInstalled_.store(installed, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    telemetry_.hooksInstalled = installed;
}

void VarState::SetReplaySeekReady(bool ready) {
    replaySeekReady_.store(ready, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    telemetry_.replaySeekReady = ready;
}

bool VarState::ShouldRenderAnyHud(uint64_t nowMs) const {
    if (debugOverlayOpen_.load(std::memory_order_relaxed)) {
        return true;
    }
    if (!enabled_.load(std::memory_order_relaxed) || !offlineAllowed_.load(std::memory_order_relaxed)) {
        return false;
    }
    if (liveManualReplaySignal_.load(std::memory_order_acquire) ||
        inManualReplay_.load(std::memory_order_acquire)) {
        return true;
    }
    if (nowMs < activeBannerUntilMs_.load(std::memory_order_relaxed)) {
        return true;
    }
    if (broadcastAlerts_.load(std::memory_order_relaxed) &&
        nowMs < activeAlertUntilMs_.load(std::memory_order_relaxed)) {
        return true;
    }
    return false;
}

void VarState::SetIniPath(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    iniPath_ = path;
}

void VarState::LoadHudConfig() {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = iniPath_;
    }
    if (path.empty()) return;

    FILE* fp = _wfopen(path.c_str(), L"rb");
    if (!fp) return;

    HudConfig cfg{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = hudConfig_;
    }

    char lineBuf[256];
    while (std::fgets(lineBuf, sizeof(lineBuf), fp)) {
        std::string line(lineBuf);
        const size_t hashPos = line.find('#');
        if (hashPos != std::string::npos) line.resize(hashPos);
        const size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;
        const std::string k = TrimAscii(line.substr(0, eqPos));
        const std::string v = TrimAscii(line.substr(eqPos + 1));

        if (k == "hud_side") {
            cfg.dockLeft = (v == "left" || v == "1");
        } else if (k == "hud_scale") {
            const int p = std::atoi(v.c_str());
            cfg.scalePreset = std::clamp(p, 0, 2);
        } else if (k == "broadcast_alerts") {
            cfg.broadcastAlerts = ParseBoolStr(v, cfg.broadcastAlerts);
        } else if (k == "allow_mouse") {
            cfg.allowMouse = true;
        } else if (k == "show_radar") {
            cfg.showRadar = ParseBoolStr(v, cfg.showRadar);
        } else if (k == "auto_zoom_box") {
            cfg.autoZoomBox = ParseBoolStr(v, cfg.autoZoomBox);
        } else if (k == "auto_open") {
            cfg.autoOpenPanel = ParseBoolStr(v, cfg.autoOpenPanel);
        } else if (k == "replay_after_decision") {
            cfg.replayAfterDecision = ParseBoolStr(v, cfg.replayAfterDecision);
        } else if (k == "auto_update" || k == "check_updates") { // check_updates: name used by v4.5.0
            cfg.autoUpdate = ParseBoolStr(v, cfg.autoUpdate);
        } else if (k == "language") {
            cfg.language = (i18n::ParseLanguageCode(v.c_str()) == Language::English) ? 1 : 0;
        } else if (k.rfind("key_", 0) == 0 || k.rfind("pad_", 0) == 0) {
            const bool isKey = (k[0] == 'k');
            const int a = FindBindAction(k.c_str() + 4);
            if (a < 0) continue;
            if (isKey) {
                // "0x56,0x76" -> two VK codes (0 = empty slot)
                const size_t comma = v.find(',');
                const unsigned long k1 = std::strtoul(v.substr(0, comma).c_str(), nullptr, 0);
                const unsigned long k2 = (comma != std::string::npos)
                    ? std::strtoul(v.substr(comma + 1).c_str(), nullptr, 0) : 0UL;
                cfg.bindings.keys[static_cast<size_t>(a)] = {
                    static_cast<uint8_t>(k1 & 0xFFU), static_cast<uint8_t>(k2 & 0xFFU) };
            } else if (InputBindings::IsPadRemappable(static_cast<BindAction>(a))) {
                cfg.bindings.pad[static_cast<size_t>(a)] =
                    static_cast<uint16_t>(std::strtoul(v.c_str(), nullptr, 0) & 0xFFFFU);
            }
        }
    }
    std::fclose(fp);

    SetHudConfig(cfg, false);
}

void VarState::SaveHudConfig() const {
    std::wstring path;
    HudConfig cfg{};
    bool enabledVal = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = iniPath_;
        cfg = hudConfig_;
        enabledVal = enabled_.load(std::memory_order_relaxed);
    }
    if (path.empty()) return;

    FILE* in = _wfopen(path.c_str(), L"rb");
    if (in) {
        char lineBuf[256];
        while (std::fgets(lineBuf, sizeof(lineBuf), in)) {
            std::string line(lineBuf);
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = TrimAscii(line.substr(0, eq));
            const std::string v = TrimAscii(line.substr(eq + 1));
            if (k == "enabled") {
                enabledVal = ParseBoolStr(v, enabledVal);
            }
        }
        std::fclose(in);
    }

    // Written to a temporary file and moved over the old one: a reader (the Lua module saves the same file)
    // never sees a truncated file
    const std::wstring tmpPath = path + L".tmp";
    FILE* out = _wfopen(tmpPath.c_str(), L"wb");
    if (!out) return;
    std::fprintf(out, "# PES 2021 VAR Mod v" VAR_MOD_VERSION " - Configuracao\n");
    std::fprintf(out, "enabled = %d\n", enabledVal ? 1 : 0);
    std::fprintf(out, "hud_side = %s\n", cfg.dockLeft ? "left" : "right");
    std::fprintf(out, "hud_scale = %d\n", std::clamp(cfg.scalePreset, 0, 2));
    std::fprintf(out, "broadcast_alerts = %d\n", cfg.broadcastAlerts ? 1 : 0);
    std::fprintf(out, "allow_mouse = %d\n", cfg.allowMouse ? 1 : 0);
    std::fprintf(out, "show_radar = %d\n", cfg.showRadar ? 1 : 0);
    std::fprintf(out, "auto_zoom_box = %d\n", cfg.autoZoomBox ? 1 : 0);
    std::fprintf(out, "auto_open = %d\n", cfg.autoOpenPanel ? 1 : 0);
    std::fprintf(out, "replay_after_decision = %d\n", cfg.replayAfterDecision ? 1 : 0);
    std::fprintf(out, "# auto_update = 1: a cada abertura do jogo le a ultima versao no GitHub e instala uma versao nova assinada (0 = sem rede)\n");
    std::fprintf(out, "auto_update = %d\n", cfg.autoUpdate ? 1 : 0);
    std::fprintf(out, "language = %s\n", i18n::LanguageCode(cfg.language == 1 ? Language::English : Language::PtBr));
    std::fprintf(out, "# Atalhos: key_* = ate 2 codigos de tecla virtual (hex, 0x00 = vazio), pad_* = botao XInput (hex)\n");
    for (int a = 0; a < kBindActionCount; ++a) {
        const auto& k = cfg.bindings.keys[static_cast<size_t>(a)];
        std::fprintf(out, "key_%s = 0x%02X,0x%02X\n", kBindIniNames[a], k[0], k[1]);
        if (InputBindings::IsPadRemappable(static_cast<BindAction>(a))) {
            std::fprintf(out, "pad_%s = 0x%04X\n", kBindIniNames[a], cfg.bindings.pad[static_cast<size_t>(a)]);
        }
    }
    const bool ok = (std::fflush(out) == 0) && !std::ferror(out);
    std::fclose(out);
    if (!ok || !MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
    }
}

HudConfig VarState::GetHudConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hudConfig_;
}

void VarState::SetHudConfig(const HudConfig& cfg, bool persist) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hudConfig_ = cfg;
        hudConfig_.scalePreset = std::clamp(hudConfig_.scalePreset, 0, 2);
        hudConfig_.language = (hudConfig_.language == 1) ? 1 : 0;
        SanitizeBindings(hudConfig_.bindings);
        allowMouse_.store(hudConfig_.allowMouse, std::memory_order_relaxed);
        broadcastAlerts_.store(hudConfig_.broadcastAlerts, std::memory_order_relaxed);
        i18n::SetLanguage(hudConfig_.language == 1 ? Language::English : Language::PtBr);
        configRevision_.fetch_add(1, std::memory_order_acq_rel);
    }
    if (persist) {
        SaveHudConfig();
    }
}

uint32_t VarState::PushEvent(const VarEvent& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (eventCount_ > 0) {
        const size_t lastIdx = (eventHead_ + kMaxEvents - 1) % kMaxEvents;
        const auto& prev = events_[lastIdx];
        if (prev.matchPhase == ev.matchPhase &&
            prev.type == ev.type &&
            prev.offenderSlot == ev.offenderSlot &&
            prev.matchTick + 30 >= ev.matchTick &&
            ev.matchTick >= prev.matchTick) {
            return prev.id;
        }
    }

    VarEvent stored = ev;
    stored.id = nextEventId_++;
    events_[eventHead_] = stored;
    eventHead_ = (eventHead_ + 1) % kMaxEvents;
    if (eventCount_ < kMaxEvents) {
        ++eventCount_;
    }

    if (stored.type == VarEventType::Penalty) {
        ++stats_.totalPenaltiesRecorded;
        ++stats_.totalFoulsRecorded;
    } else if (stored.type == VarEventType::Foul) {
        ++stats_.totalFoulsRecorded;
    }
    ++dataRevision_;
    return stored.id;
}

void VarState::PushSnapshot(const PlaySnapshot& snap) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshots_.size() != kMaxSnapshots) {
        snapshots_.resize(kMaxSnapshots);
    }
    if (snapKeys_.size() != kMaxSnapshots) {
        snapKeys_.resize(kMaxSnapshots);
    }
    snapshots_[snapHead_] = snap;
    snapKeys_[snapHead_] = SnapKey{ snap.matchPhase, snap.time60Hz, snap.simFrame };
    snapHead_ = (snapHead_ + 1) % kMaxSnapshots;
    if (snapCount_ < kMaxSnapshots) {
        ++snapCount_;
    }
    ++snapRevision_;
}

void VarState::PushDetectedIncident(const DetectedIncident& inc) {
    if (inc.kind == IncidentKind::None || !IsValidSlot(inc.offenderSlot)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    // Temporal clustering (~1.2s = 72 ticks at 60Hz after the duel was last seen): frames of the same
    // tackle/fall merge into one incident even when another duel nearby alternates with it frame by frame
    const size_t lookBack = std::min<size_t>(incCount_, 8);
    for (size_t k = 0; k < lookBack; ++k) {
        const size_t pIdx = (incHead_ + kMaxIncidents - 1 - k) % kMaxIncidents;
        auto& prev = incidents_[pIdx];
        const bool sameHalf = IsPhaseCompatible(prev.matchPhase, inc.matchPhase);
        const float lastSeen = std::max(prev.time60Hz, prev.lastSeenTime60Hz);
        const float dt = std::fabs(inc.time60Hz - lastSeen);
        if (sameHalf && dt <= 72.0f) {
            // Same play only when it is the same pair of players: a different duel 3 m away is another incident
            const bool samePair =
                (prev.offenderSlot == inc.offenderSlot && prev.victimSlot == inc.victimSlot) ||
                (prev.offenderSlot == inc.victimSlot && prev.victimSlot == inc.offenderSlot);
            if (samePair) {
                prev.lastSeenTime60Hz = std::max(lastSeen, inc.time60Hz);
                const bool upgrade = (inc.priority > prev.priority) ||
                                     (inc.priority == prev.priority && inc.contactDistM < prev.contactDistM);
                if (upgrade) {
                    const int32_t oldPriority = prev.priority;
                    prev.kind         = inc.kind;
                    prev.priority     = inc.priority;
                    prev.offenderSlot = inc.offenderSlot;
                    prev.victimSlot   = inc.victimSlot;
                    prev.pos          = inc.pos;
                    prev.contactDistM = inc.contactDistM;
                    prev.insideBox    = prev.insideBox || inc.insideBox;
                    prev.nearFoul     = prev.nearFoul || inc.nearFoul;
                    if (inc.boxDistValid) { prev.boxDistM = inc.boxDistM; prev.boxDistValid = true; }
                    // Keep first high-severity impact timestamp unless upgrading from a weak contact
                    if (oldPriority < 55 || inc.priority >= oldPriority + 8) {
                        prev.time60Hz   = inc.time60Hz;
                        prev.matchTick  = inc.matchTick;
                        prev.matchPhase = inc.matchPhase;
                    }
                    ++dataRevision_;
                } else if ((!prev.insideBox && inc.insideBox) || (!prev.nearFoul && inc.nearFoul)) {
                    prev.insideBox = prev.insideBox || inc.insideBox; // marker label/colour changes -> invalidate HUD cache
                    prev.nearFoul  = prev.nearFoul || inc.nearFoul;
                    ++dataRevision_;
                }
                return;
            }
        }
    }

    incidents_[incHead_] = inc;
    incidents_[incHead_].lastSeenTime60Hz = inc.time60Hz;
    incHead_ = (incHead_ + 1) % kMaxIncidents;
    if (incCount_ < kMaxIncidents) {
        ++incCount_;
    }
    ++dataRevision_;
}

void VarState::UpdatePlayers(const std::array<PlayerSlotInfo, 22>& players) {
    std::lock_guard<std::mutex> lock(mutex_);
    players_ = players;
    ++rosterRevision_;
    uint32_t hy = 0, hr = 0, ay = 0, ar = 0;
    for (int i = 0; i < 11; ++i) {
        hy += players_[i].yellows;
        hr += players_[i].reds;
    }
    for (int i = 11; i < 22; ++i) {
        ay += players_[i].yellows;
        ar += players_[i].reds;
    }
    stats_.homeYellows = hy;
    stats_.homeReds = hr;
    stats_.awayYellows = ay;
    stats_.awayReds = ar;
}

void VarState::UpdateTeams(const std::array<TeamDisplayInfo, 2>& teams) {
    std::lock_guard<std::mutex> lock(mutex_);
    teams_ = teams;
    ++teamsRevision_;
}

void VarState::UpdateTelemetry(const LiveTelemetry& tel) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t bannerStart = telemetry_.lastBannerStartMs;
    const uint64_t bannerUntil = telemetry_.lastBannerUntilMs;
    const BannerStyle bannerStyle = telemetry_.lastBannerStyle;
    char bannerBuf[128];
    std::memcpy(bannerBuf, telemetry_.lastBannerText, sizeof(bannerBuf));

    const uint64_t alertStart = telemetry_.alertBannerStartMs;
    const uint64_t alertUntil = telemetry_.alertBannerUntilMs;
    const BannerStyle alertStyle = telemetry_.alertBannerStyle;
    char alertBuf[128];
    std::memcpy(alertBuf, telemetry_.alertBannerText, sizeof(alertBuf));

    telemetry_ = tel;
    telemetry_.enabled = enabled_.load(std::memory_order_relaxed);
    telemetry_.offlineAllowed = offlineAllowed_.load(std::memory_order_relaxed);
    telemetry_.siderOverlayOpen = siderOverlayOpen_.load(std::memory_order_relaxed);
    telemetry_.hooksInstalled = hooksInstalled_.load(std::memory_order_relaxed);
    telemetry_.replaySeekReady = replaySeekReady_.load(std::memory_order_relaxed);

    telemetry_.lastBannerStartMs = bannerStart;
    telemetry_.lastBannerUntilMs = bannerUntil;
    telemetry_.lastBannerStyle = bannerStyle;
    std::memcpy(telemetry_.lastBannerText, bannerBuf, sizeof(bannerBuf));

    telemetry_.alertBannerStartMs = alertStart;
    telemetry_.alertBannerUntilMs = alertUntil;
    telemetry_.alertBannerStyle = alertStyle;
    std::memcpy(telemetry_.alertBannerText, alertBuf, sizeof(alertBuf));

    const bool manualNow = telemetry_.matchActive &&
                           (telemetry_.specialModeKind == 0) &&
                           (telemetry_.matchPhase >= 0 && telemetry_.matchPhase < 8) &&
                           (telemetry_.replayMode >= 1 && telemetry_.replayMode <= 3) &&
                           (telemetry_.replayManualFlag != 0);
    liveManualReplaySignal_.store(manualNow, std::memory_order_release);
}

void VarState::PatchLiveReplayTelemetry(bool matchActive, uintptr_t gmatchPtr, uintptr_t matchInfoPtr,
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
                                        int32_t activeVictimSlot) {
    const bool manualNow = matchActive &&
                           (specialModeKind == 0) &&
                           (matchPhase >= 0 && matchPhase < 8) &&
                           (replayMode >= 1 && replayMode <= 3) &&
                           (replayManualFlag != 0);
    liveManualReplaySignal_.store(manualNow, std::memory_order_release);
    if (!matchActive) {
        panelFocused_.store(false, std::memory_order_release);
        inManualReplay_.store(false, std::memory_order_release);
        mouseOverPanel_.store(false, std::memory_order_relaxed);
        inputBlockGraceUntilMs_.store(0, std::memory_order_release);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    telemetry_.matchActive = matchActive;
    telemetry_.gmatchPtr = gmatchPtr;
    telemetry_.matchInfoPtr = matchInfoPtr;
    telemetry_.replayMgrPtr = replayMgrPtr;
    if (!matchActive) {
        telemetry_.replayMode = 0;
        telemetry_.replayManualFlag = 0;
        telemetry_.replayIsPlaying = 0;
        telemetry_.replayUiHidden = 0;
        return;
    }
    telemetry_.matchPhase = matchPhase;
    telemetry_.matchStatus = matchStatus;
    if (fsmState >= 0) {
        telemetry_.fsmState = fsmState;
    }
    telemetry_.specialModeKind = specialModeKind;
    telemetry_.replayMode = replayMode;
    telemetry_.replayManualFlag = replayManualFlag;
    telemetry_.replayIsPlaying = replayIsPlaying;
    telemetry_.replayUiHidden = replayUiHidden;
    telemetry_.replayCursorTime = replayCursorTime;
    telemetry_.replayCursorPhase = replayCursorPhase;
    telemetry_.replayWindowStartTime = replayWinStartTime;
    telemetry_.replayWindowStartPhase = replayWinStartPhase;
    telemetry_.replayWindowEndTime = replayWinEndTime;
    telemetry_.replayWindowEndPhase = replayWinEndPhase;
    telemetry_.replayFocusSlot = replayFocusSlot;
    telemetry_.replayTimelineNorm = replayTimelineNorm;
    telemetry_.activeSetPlayKind = activeSetPlayKind;
    telemetry_.activeBenefitedTeam = activeBenefitedTeam;
    telemetry_.activeFoulValid = activeFoulValid;
    telemetry_.activeFoulKind = activeFoulKind;
    telemetry_.activeFoulCard = activeFoulCard;
    telemetry_.activeOffenderSlot = activeOffenderSlot;
    telemetry_.activeVictimSlot = activeVictimSlot;
}

LiveTelemetry VarState::GetTelemetry() const {
    LiveTelemetry copy{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = telemetry_;
    }
    copy.siderOverlayOpen = IsSiderOverlayOpen();
    return copy;
}

std::array<PlayerSlotInfo, 22> VarState::GetPlayers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return players_;
}

std::array<TeamDisplayInfo, 2> VarState::GetTeams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return teams_;
}

std::vector<VarEvent> VarState::GetRecentEvents(size_t maxCount) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t n = std::min(maxCount, eventCount_);
    std::vector<VarEvent> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t idx = (eventHead_ + kMaxEvents - 1 - i) % kMaxEvents;
        out.push_back(events_[idx]);
    }
    return out;
}

MatchVarStats VarState::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// Positions between the two consecutive frames around the cursor (slow motion and frames between ticks).
// Only positions move: action ids, contact and ownership stay those of the nearest frame, so everything the
// panel derives from the snapshot is unchanged. No blending across a phase change, a stopped clock, a gap
// or a teleport (restart, substitution), where the nearest frame is the faithful one.
void VarState::InterpolateSnapshotLocked(size_t idx, size_t age, float cursorTime60Hz, PlaySnapshot& out) const {
    const SnapKey& k = snapKeys_[idx];
    size_t olderIdx = 0;
    size_t newerIdx = 0;
    if (k.time60Hz < cursorTime60Hz) {
        if (age == 0) return; // newest frame, nothing after it
        olderIdx = idx;
        newerIdx = (idx + 1) % kMaxSnapshots;
    } else if (k.time60Hz > cursorTime60Hz) {
        if (age + 1 >= snapCount_) return; // oldest frame, nothing before it
        olderIdx = (idx + kMaxSnapshots - 1) % kMaxSnapshots;
        newerIdx = idx;
    } else {
        return; // exact frame
    }
    const SnapKey& ka = snapKeys_[olderIdx];
    const SnapKey& kb = snapKeys_[newerIdx];
    if (ka.phase != kb.phase) return;
    if (kb.tick <= ka.tick || kb.tick - ka.tick > kMaxInterpolationFrameGap) return; // not consecutive frames
    const float span = kb.time60Hz - ka.time60Hz;
    if (!(span > 0.0f)) return; // clock stopped between them
    const float alpha = std::clamp((cursorTime60Hz - ka.time60Hz) / span, 0.0f, 1.0f);
    const PlaySnapshot& a = snapshots_[olderIdx];
    const PlaySnapshot& b = snapshots_[newerIdx];
    auto blend = [alpha](const Vec3& p, const Vec3& q, Vec3& dst) {
        const float dx = q.x - p.x;
        const float dz = q.z - p.z;
        if (!(dx * dx + dz * dz <= kMaxInterpolationStepSq)) return; // teleport or NaN: keep the nearest frame
        dst.x = p.x + dx * alpha;
        dst.y = p.y + (q.y - p.y) * alpha;
        dst.z = p.z + dz * alpha;
    };
    blend(a.ballPos, b.ballPos, out.ballPos);
    for (int s = 0; s < 22; ++s) {
        if (!a.playerOnPitch[s] || !b.playerOnPitch[s] || !out.playerOnPitch[s]) continue;
        blend(a.playerPos[s], b.playerPos[s], out.playerPos[s]);
    }
}

ReplaySyncResult VarState::SyncToReplayCursorLocked(int32_t cursorPhase, float cursorTime60Hz, float eventToleranceTicks) const {
    ReplaySyncResult res{};

    float bestEvDelta = eventToleranceTicks + 1.0f;
    for (size_t i = 0; i < eventCount_; ++i) {
        const size_t idx = (eventHead_ + kMaxEvents - 1 - i) % kMaxEvents;
        const auto& ev = events_[idx];
        if (!IsPhaseCompatible(ev.matchPhase, cursorPhase)) continue;
        const float dt = std::fabs(ev.time60Hz - cursorTime60Hz);
        if (dt <= eventToleranceTicks && dt < bestEvDelta) {
            bestEvDelta = dt;
            res.hasEvent = true;
            res.event = ev;
            res.deltaTicks = dt;
        }
    }

    // Snapshots are stored chronologically: walk backward from newest and exit early once the walk is
    // older than the cursor and the distance can only keep growing. Result is identical to a full
    // scan (newest snapshot wins ties), verified against brute force in TestPerfOverhaulEquivalence.
    float bestSnapDelta = 999999.0f;
    size_t bestSnapIdx = kMaxSnapshots;
    size_t bestSnapAge = 0;
    for (size_t i = 0; i < snapCount_; ++i) {
        const size_t idx = (snapHead_ + kMaxSnapshots - 1 - i) % kMaxSnapshots;
        const SnapKey& key = snapKeys_[idx];
        if (!IsPhaseCompatible(key.phase, cursorPhase)) {
            if (bestSnapIdx != kMaxSnapshots) break;
            continue;
        }
        const float dt = std::fabs(key.time60Hz - cursorTime60Hz);
        if (dt < bestSnapDelta) {
            bestSnapDelta = dt;
            bestSnapIdx = idx;
            bestSnapAge = i;
        } else if (bestSnapIdx != kMaxSnapshots && key.time60Hz < cursorTime60Hz && dt > bestSnapDelta + 12.0f) {
            break; // Monotonically moving further into the past -> early exit
        }
    }
    if (bestSnapIdx != kMaxSnapshots) {
        res.hasSnapshot = true;
        res.snapshot = snapshots_[bestSnapIdx]; // single copy instead of one per improvement
        InterpolateSnapshotLocked(bestSnapIdx, bestSnapAge, cursorTime60Hz, res.snapshot);
    }

    return res;
}

void VarState::SampleMotionHistory(int32_t phase, float time60Hz, MotionSample (&out)[kMotionSamples]) const {
    for (auto& m : out) m = MotionSample{};
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapCount_ == 0 || snapKeys_.size() != kMaxSnapshots) return;
    // Frame nearest to the cursor (same search as the replay sync: newest first, early exit once past it)
    float best = 999999.0f;
    size_t bestAge = snapCount_;
    for (size_t i = 0; i < snapCount_; ++i) {
        const SnapKey& key = snapKeys_[(snapHead_ + kMaxSnapshots - 1 - i) % kMaxSnapshots];
        if (key.phase != phase) {
            if (bestAge != snapCount_) break;
            continue;
        }
        const float dt = std::fabs(key.time60Hz - time60Hz);
        if (dt < best) { best = dt; bestAge = i; }
        else if (bestAge != snapCount_ && key.time60Hz < time60Hz && dt > best + 12.0f) break;
    }
    if (bestAge == snapCount_) return;
    const uint32_t baseTick = snapKeys_[(snapHead_ + kMaxSnapshots - 1 - bestAge) % kMaxSnapshots].tick;
    // Walk older frames once; every target is met in order. A gap (stoppage, other phase) ends the history.
    int k = 0;
    for (size_t i = bestAge + 1; i < snapCount_ && k < kMotionSamples; ++i) {
        const size_t idx = (snapHead_ + kMaxSnapshots - 1 - i) % kMaxSnapshots;
        const SnapKey& key = snapKeys_[idx];
        if (key.phase != phase || key.tick > baseTick) break;
        const uint32_t back = baseTick - key.tick;
        const uint32_t want = static_cast<uint32_t>(k + 1) * kMotionStepFrames;
        if (back < want) continue;
        if (back > want + 3) break; // frames missing around the target: no reliable history beyond this point
        const PlaySnapshot& sn = snapshots_[idx];
        MotionSample& m = out[k];
        m.valid = true;
        m.simFrame = key.tick;
        m.ball = sn.ballPos;
        for (int s = 0; s < 22; ++s) {
            m.pos[s] = sn.playerPos[s];
            m.onPitch[s] = sn.playerOnPitch[s];
        }
        ++k;
    }
}

ReplaySyncResult VarState::SyncToReplayCursor(int32_t cursorPhase, float cursorTime60Hz, float eventToleranceTicks) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SyncToReplayCursorLocked(cursorPhase, cursorTime60Hz, eventToleranceTicks);
}

void VarState::FormatIncidentLabel(IncidentKind kind, bool insideBox, char* outBuf, size_t outSize) {
    if (!outBuf || outSize == 0) return;
    switch (kind) {
        case IncidentKind::Penalty:      std::snprintf(outBuf, outSize, "PÊNALTI MARCADO"); break;
        case IncidentKind::RedCard:      std::snprintf(outBuf, outSize, "CARTÃO VERMELHO"); break;
        case IncidentKind::Foul:         std::snprintf(outBuf, outSize, insideBox ? "FALTA NA ÁREA" : "FALTA MARCADA"); break;
        case IncidentKind::YellowCard:   std::snprintf(outBuf, outSize, "CARTÃO AMARELO"); break;
        case IncidentKind::Advantage:    std::snprintf(outBuf, outSize, "VANTAGEM"); break;
        case IncidentKind::SlideTackle:  std::snprintf(outBuf, outSize, insideBox ? "CARRINHO NA ÁREA" : "CARRINHO"); break;
        case IncidentKind::VictimFall:   std::snprintf(outBuf, outSize, insideBox ? "QUEDA NA ÁREA" : "QUEDA"); break;
        case IncidentKind::StandTackle:  std::snprintf(outBuf, outSize, insideBox ? "DIVIDIDA NA ÁREA" : "DIVIDIDA"); break;
        case IncidentKind::BodyCharge:   std::snprintf(outBuf, outSize, insideBox ? "TRANCO NA ÁREA" : "TRANCO"); break;
        case IncidentKind::CloseContact: std::snprintf(outBuf, outSize, insideBox ? "DISPUTA NA ÁREA" : "DISPUTA DE BOLA"); break;
        case IncidentKind::Dive:         std::snprintf(outBuf, outSize, insideBox ? "SIMULAÇÃO NA ÁREA?" : "SIMULAÇÃO?"); break;
        case IncidentKind::None:
        default:                         std::snprintf(outBuf, outSize, "LANCE"); break;
    }
}

void VarState::BuildReplayWindowMarkersLocked(int32_t winPhase, float winStartTime, float winEndTime,
                                              std::vector<ReplayMarker>& markers) const {
    markers.clear();
    const float span = winEndTime - winStartTime;
    if (span <= 1.0f) return;

    auto storedOr = [](bool valid, float stored, float live) { return valid ? stored : live; };
    auto computeBoxDist = [this](int32_t offSlot, int32_t benefitedTeam, const Vec3& pos) -> float {
        int32_t offTeam = IsValidSlot(offSlot) ? SlotToTeam(offSlot) : (1 - benefitedTeam);
        if (offTeam != 0 && offTeam != 1) offTeam = 0;
        return PitchGeometry::SignedDistanceToDefendingBox(telemetry_.pitchDims, pos, telemetry_.teamAttackDir[offTeam]);
    };

    // 1. Official referee events inside the replay window (with +/- 15 tick edge tolerance)
    for (size_t i = 0; i < eventCount_; ++i) {
        const size_t idx = (eventHead_ + kMaxEvents - 1 - i) % kMaxEvents;
        const auto& ev = events_[idx];
        if (!IsPhaseCompatible(ev.matchPhase, winPhase)) continue;
        if (ev.time60Hz < winStartTime - 15.0f || ev.time60Hz > winEndTime + 15.0f) continue;

        ReplayMarker m{};
        m.time60Hz        = std::clamp(ev.time60Hz, winStartTime, winEndTime);
        m.matchPhase      = ev.matchPhase;
        m.timelineNorm    = std::clamp((m.time60Hz - winStartTime) / span, 0.0f, 1.0f);
        m.isOfficialEvent = true;
        m.officialEventId = ev.id;
        m.eventType       = ev.type;
        m.cardKind        = ev.cardKind;
        m.offenderSlot    = ev.offenderSlot;
        m.victimSlot      = ev.victimSlot;
        m.pos             = ev.pos;
        m.contactDistM    = 0.0f;
        m.rolesConfirmed  = ev.type != VarEventType::NoFoul; // a call the VAR annulled names no offender any more
        m.signedBoxDistM  = storedOr(ev.boxDistValid, ev.boxDistM, computeBoxDist(ev.offenderSlot, ev.benefitedTeam, ev.pos));

        if (ev.type == VarEventType::NoFoul && ev.reviewedByVar) {
            m.kind = IncidentKind::None;
            m.priority = 54;
            std::snprintf(m.label, sizeof(m.label), "%s", ev.varNote[0] ? ev.varNote : "ANULADO PELO VAR");
        } else if (ev.type == VarEventType::Penalty) {
            m.kind = IncidentKind::Penalty;
            m.priority = 100;
            FormatIncidentLabel(m.kind, m.signedBoxDistM < 0.0f, m.label, sizeof(m.label));
        } else if (ev.cardKind == CardKind::Red || ev.offenderRedsAfter > 0) {
            m.kind = IncidentKind::RedCard;
            m.priority = 96;
            FormatIncidentLabel(m.kind, m.signedBoxDistM < 0.0f, m.label, sizeof(m.label));
        } else if (ev.type == VarEventType::Advantage) {
            m.kind = IncidentKind::Advantage;
            m.priority = 84;
            FormatIncidentLabel(m.kind, m.signedBoxDistM < 0.0f, m.label, sizeof(m.label));
        } else if (ev.cardKind == CardKind::Yellow) {
            m.kind = IncidentKind::YellowCard;
            m.priority = 88;
            FormatIncidentLabel(m.kind, m.signedBoxDistM < 0.0f, m.label, sizeof(m.label));
        } else {
            m.kind = IncidentKind::Foul;
            m.priority = 86;
            FormatIncidentLabel(m.kind, m.signedBoxDistM < 0.0f, m.label, sizeof(m.label));
        }
        markers.push_back(m);
    }

    // 1b. Official markers take the strongest physical incident of their play and move to its contact moment
    // (the event is recorded at the whistle; a foul whistled after an advantage comes seconds after the contact)
    std::vector<uint8_t> consumedInc(incCount_, 0);
    {
        size_t evMarker = 0;
        for (size_t i = 0; i < eventCount_ && evMarker < markers.size(); ++i) {
            const size_t eIdx = (eventHead_ + kMaxEvents - 1 - i) % kMaxEvents;
            const auto& ev = events_[eIdx];
            if (!IsPhaseCompatible(ev.matchPhase, winPhase)) continue;
            if (ev.time60Hz < winStartTime - 15.0f || ev.time60Hz > winEndTime + 15.0f) continue;
            ReplayMarker& om = markers[evMarker++];
            const float lookBack = ev.wasAdvantage ? 600.0f : 90.0f;
            int bestK = -1;
            int32_t bestPrio = -1;
            for (size_t k = 0; k < incCount_; ++k) {
                if (consumedInc[k]) continue;
                const auto& inc = incidents_[(incHead_ + kMaxIncidents - incCount_ + k) % kMaxIncidents];
                if (!IsPhaseCompatible(inc.matchPhase, winPhase) || !IsValidSlot(inc.offenderSlot)) continue;
                const float d = inc.time60Hz - ev.time60Hz;
                if (d < -lookBack || d > 15.0f) continue;
                const bool samePlayers = (!IsValidSlot(om.offenderSlot) && !IsValidSlot(om.victimSlot)) ||
                                         om.offenderSlot == inc.offenderSlot || om.victimSlot == inc.victimSlot ||
                                         om.offenderSlot == inc.victimSlot || om.victimSlot == inc.offenderSlot;
                if (!samePlayers) continue;
                if (inc.priority > bestPrio) { bestPrio = inc.priority; bestK = static_cast<int>(k); }
            }
            if (bestK < 0) continue;
            consumedInc[static_cast<size_t>(bestK)] = 1;
            const auto& inc = incidents_[(incHead_ + kMaxIncidents - incCount_ + static_cast<size_t>(bestK)) % kMaxIncidents];
            if (!IsValidSlot(om.victimSlot) && IsValidSlot(inc.victimSlot) && inc.victimSlot != om.offenderSlot) {
                om.victimSlot = inc.victimSlot;
            }
            if (!IsValidSlot(om.offenderSlot) && IsValidSlot(inc.offenderSlot)) {
                om.offenderSlot = inc.offenderSlot;
            }
            if (inc.time60Hz < om.time60Hz && inc.time60Hz >= winStartTime) {
                om.time60Hz = inc.time60Hz;
                om.timelineNorm = std::clamp((om.time60Hz - winStartTime) / span, 0.0f, 1.0f);
            }
        }
    }

    // A play the referee already called is never shown again as a separate (or "missed") incident: a physical
    // incident or duel sharing a player with a called foul / penalty / card / advantage from kCallLookBack60Hz
    // before the whistle (10 s after an advantage) up to just after it belongs to that call. The foul the match is
    // stopped for right now counts as a call even before it reaches the event history.
    constexpr float kCallLookBack60Hz = 240.0f;
    constexpr float kAdvantageLookBack60Hz = 600.0f;
    auto sharesPlayer = [](int32_t a1, int32_t a2, int32_t b1, int32_t b2) {
        auto in = [&](int32_t x) { return IsValidSlot(x) && (x == b1 || x == b2); };
        return in(a1) || in(a2);
    };
    const bool liveCall = telemetry_.activeFoulValid != 0 &&
                          telemetry_.matchStatus != static_cast<int32_t>(MatchStatus::InPlay) &&
                          (IsValidSlot(telemetry_.activeOffenderSlot) || IsValidSlot(telemetry_.activeVictimSlot));
    auto coveredByCall = [&](int32_t phase, float t, int32_t off, int32_t vic) -> bool {
        for (size_t e = 0; e < eventCount_; ++e) {
            const auto& ev = events_[(eventHead_ + kMaxEvents - 1 - e) % kMaxEvents];
            if (!IsPhaseCompatible(ev.matchPhase, phase)) continue;
            const float back = ev.wasAdvantage ? kAdvantageLookBack60Hz : kCallLookBack60Hz;
            const float d = t - ev.time60Hz;
            if (d < -back || d > 30.0f) continue;
            if (sharesPlayer(off, vic, ev.offenderSlot, ev.victimSlot)) return true;
        }
        if (liveCall && IsPhaseCompatible(telemetry_.matchPhase, phase)) {
            const float d = t - telemetry_.time60Hz; // the clock is stopped at the whistle during the stoppage
            if (d >= -kAdvantageLookBack60Hz && d <= 30.0f &&
                sharesPlayer(off, vic, telemetry_.activeOffenderSlot, telemetry_.activeVictimSlot)) return true;
        }
        return false;
    };

    // 2. Physical engine incidents detected at 60Hz (Slide Tackle, Victim Fall, Stand Tackle, Body Charge)
    std::vector<ReplayMarker> detected;
    for (size_t i = 0; i < incCount_; ++i) {
        const size_t idx = (incHead_ + kMaxIncidents - incCount_ + i) % kMaxIncidents;
        const auto& inc = incidents_[idx];
        if (!IsPhaseCompatible(inc.matchPhase, winPhase)) continue;
        if (inc.time60Hz < winStartTime || inc.time60Hz > winEndTime) continue;
        if (!IsValidSlot(inc.offenderSlot)) continue;

        // Folded into an official marker above, or another frame of the same play as one (same players, close)
        if (consumedInc[i]) continue;
        bool mergedIntoOfficial = false;
        for (const auto& om : markers) {
            const bool samePlayers = om.offenderSlot == inc.offenderSlot || om.victimSlot == inc.victimSlot ||
                                     om.offenderSlot == inc.victimSlot || om.victimSlot == inc.offenderSlot;
            if (std::fabs(om.time60Hz - inc.time60Hz) <= 90.0f && samePlayers) { mergedIntoOfficial = true; break; }
        }
        if (mergedIntoOfficial || coveredByCall(inc.matchPhase, inc.time60Hz, inc.offenderSlot, inc.victimSlot)) continue;

        const float boxDist = storedOr(inc.boxDistValid, inc.boxDistM,
                                       computeBoxDist(inc.offenderSlot, 1 - SlotToTeam(inc.offenderSlot), inc.pos));
        ReplayMarker m{};
        m.time60Hz        = inc.time60Hz;
        m.matchPhase      = inc.matchPhase;
        m.timelineNorm    = std::clamp((inc.time60Hz - winStartTime) / span, 0.0f, 1.0f);
        m.isOfficialEvent = false;
        m.kind            = inc.kind;
        m.priority        = inc.priority;
        m.eventType       = VarEventType::NoFoul;
        m.cardKind        = CardKind::None;
        m.offenderSlot    = inc.offenderSlot;
        m.victimSlot      = inc.victimSlot;
        m.pos             = inc.pos;
        m.contactDistM    = inc.contactDistM;
        m.signedBoxDistM  = boxDist;
        m.nearFoul        = inc.nearFoul;
        m.rolesConfirmed  = inc.rolesConfirmed || inc.nearFoul;
        FormatIncidentLabel(m.kind, boxDist < 0.0f, m.label, sizeof(m.label));

        const bool samePairAsLast = !detected.empty() &&
            ((detected.back().offenderSlot == m.offenderSlot && detected.back().victimSlot == m.victimSlot) ||
             (detected.back().offenderSlot == m.victimSlot && detected.back().victimSlot == m.offenderSlot));
        if (samePairAsLast && std::fabs(m.time60Hz - detected.back().time60Hz) <= 72.0f) {
            if (m.priority > detected.back().priority ||
                (m.priority == detected.back().priority && m.contactDistM < detected.back().contactDistM)) {
                detected.back() = m;
            }
        } else {
            detected.push_back(m);
        }
    }

    // 3. Fallback from the snapshots only for body-to-body duels for the ball not already covered by official or
    // physical markers: the pair is within touching distance, one of them has (or just had) the ball, and it lasts
    // kFallbackMinFrames consecutive snapshots. A duel has no roles.
    constexpr float kFallbackContactDistSq = kTouchDistM * kTouchDistM;
    constexpr int   kFallbackMinFrames = 4;
    int32_t runA = -1, runB = -1, runLen = 0;
    float runLastTime = -1000.0f;
    for (size_t i = 0; i < snapCount_; ++i) {
        const size_t idx = (snapHead_ + kMaxSnapshots - snapCount_ + i) % kMaxSnapshots;
        const SnapKey& key = snapKeys_[idx]; // filter on the 8-byte key before touching the snapshot
        if (!IsPhaseCompatible(key.phase, winPhase)) continue;
        if (key.time60Hz < winStartTime || key.time60Hz > winEndTime) continue;
        const auto& sn = snapshots_[idx];
        if (!IsValidSlot(sn.closestA) || !IsValidSlot(sn.closestB)) continue;
        const bool withBall = sn.ballOwnerSlot == sn.closestA || sn.ballOwnerSlot == sn.closestB ||
                              sn.recentBallOwnerSlot == sn.closestA || sn.recentBallOwnerSlot == sn.closestB;
        if (sn.closestDistSq > kFallbackContactDistSq || !withBall) { runLen = 0; continue; }
        if (sn.closestA == runA && sn.closestB == runB && sn.time60Hz - runLastTime <= 3.5f) {
            ++runLen;
        } else {
            runA = sn.closestA;
            runB = sn.closestB;
            runLen = 1;
        }
        runLastTime = sn.time60Hz;
        if (runLen != kFallbackMinFrames) continue; // one marker per sustained duel
        if (coveredByCall(sn.matchPhase, sn.time60Hz, sn.closestA, sn.closestB)) continue;

        bool covered = false;
        for (const auto& om : markers) {
            if (std::fabs(om.time60Hz - sn.time60Hz) <= 90.0f) { covered = true; break; }
        }
        if (!covered) {
            for (const auto& dm : detected) {
                if (std::fabs(dm.time60Hz - sn.time60Hz) <= 75.0f) { covered = true; break; }
            }
        }
        if (covered) continue;

        int32_t vic = sn.closestA;
        int32_t off = sn.closestB;
        if (sn.ballOwnerSlot == sn.closestB || sn.recentBallOwnerSlot == sn.closestB) {
            vic = sn.closestB;
            off = sn.closestA;
        } else if (sn.ballOwnerSlot == sn.closestA || sn.recentBallOwnerSlot == sn.closestA) {
            vic = sn.closestA;
            off = sn.closestB;
        } else if (DistSq2D(sn.playerPos[sn.closestB], sn.ballPos) < DistSq2D(sn.playerPos[sn.closestA], sn.ballPos)) {
            // No owner known: the player nearer the ball is the one being challenged (never "always home")
            vic = sn.closestB;
            off = sn.closestA;
        }

        const Vec3 mid{
            0.5f * (sn.playerPos[off].x + sn.playerPos[vic].x),
            0.0f,
            0.5f * (sn.playerPos[off].z + sn.playerPos[vic].z)
        };
        const float boxDist = computeBoxDist(off, 1 - SlotToTeam(off), mid);
        ReplayMarker cm{};
        cm.time60Hz        = sn.time60Hz;
        cm.matchPhase      = sn.matchPhase;
        cm.timelineNorm    = std::clamp((sn.time60Hz - winStartTime) / span, 0.0f, 1.0f);
        cm.isOfficialEvent = false;
        cm.kind            = IncidentKind::CloseContact;
        cm.priority        = (boxDist < 0.0f) ? 44 : 34;
        cm.eventType       = VarEventType::NoFoul;
        cm.cardKind        = CardKind::None;
        cm.offenderSlot    = off;
        cm.victimSlot      = vic;
        cm.pos             = mid;
        cm.contactDistM    = std::sqrt(std::max(0.0f, sn.closestDistSq));
        cm.signedBoxDistM  = boxDist;
        FormatIncidentLabel(cm.kind, boxDist < 0.0f, cm.label, sizeof(cm.label));
        detected.push_back(cm);
    }

    // Keep timeline clean: all official events + up to 8 highest-priority physical incidents
    if (detected.size() > 8) {
        std::sort(detected.begin(), detected.end(), [](const ReplayMarker& a, const ReplayMarker& b) {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.contactDistM < b.contactDistM;
        });
        detected.resize(8);
    }

    markers.insert(markers.end(), detected.begin(), detected.end());
    std::sort(markers.begin(), markers.end(), [](const ReplayMarker& a, const ReplayMarker& b) {
        return a.time60Hz < b.time60Hz;
    });
    FlagTopSuspect(markers);
}

// A missed foul is a contact the referee let go: official events (he already decided) never qualify, and
// physical markers folded into an official one never reach this list. The engine's own contact model weighs
// most: a challenge that made the opponent go down (nearFoul: contact reaction caused by that player) is the
// classic unpunished foul. Then the challenge type, the zone (in the box it is a penalty) and how tight the
// contact was. A dive scores low on purpose: it can deserve a card, but it is not a foul against the diver.
int32_t VarState::SuspicionScore(const ReplayMarker& m) {
    if (m.isOfficialEvent) return 0;
    int32_t s = 0;
    switch (m.kind) {
        case IncidentKind::SlideTackle:  s = 46; break;
        case IncidentKind::StandTackle:  s = 38; break;
        case IncidentKind::VictimFall:   s = 34; break;
        case IncidentKind::BodyCharge:   s = 30; break;
        case IncidentKind::Dive:         s = 22; break;
        case IncidentKind::CloseContact: s = 12; break;
        default:                         return 0;
    }
    if (m.kind == IncidentKind::CloseContact) return 0; // proximity alone is never a missed foul
    if (m.nearFoul && m.kind != IncidentKind::Dive) s += 22;
    if (m.signedBoxDistM < 0.0f) s += 24;
    else if (m.signedBoxDistM < 6.0f) s += 8;
    if (m.contactDistM < 1.0f) s += 10;
    else if (m.contactDistM < 1.5f) s += 5;
    // Without a contact reaction (nobody staggered or went down from that opponent) it stays below the
    // threshold: a clean tackle in the box is not a likely missed foul
    if (!m.nearFoul) s = std::min(s, kSuspectThreshold - 15);
    return std::clamp(s, 0, 100);
}

void VarState::FlagTopSuspect(std::vector<ReplayMarker>& markers) {
    int best = -1;
    for (int i = 0; i < static_cast<int>(markers.size()); ++i) {
        ReplayMarker& m = markers[static_cast<size_t>(i)];
        m.suspicion = SuspicionScore(m);
        m.topSuspect = false;
        if (m.suspicion < kSuspectThreshold) continue;
        if (best < 0) { best = i; continue; }
        const ReplayMarker& b = markers[static_cast<size_t>(best)];
        // Strongest suspicion, then tighter contact, then the latest (closest to the stoppage being reviewed)
        if (m.suspicion > b.suspicion ||
            (m.suspicion == b.suspicion && m.contactDistM < b.contactDistM) ||
            (m.suspicion == b.suspicion && m.contactDistM == b.contactDistM && m.time60Hz > b.time60Hz)) {
            best = i;
        }
    }
    if (best >= 0) markers[static_cast<size_t>(best)].topSuspect = true;
}

std::vector<ReplayMarker> VarState::GetReplayWindowMarkers(int32_t winPhase, float winStartTime, float winEndTime) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ReplayMarker> markers;
    BuildReplayWindowMarkersLocked(winPhase, winStartTime, winEndTime, markers);
    return markers;
}

void VarState::CaptureHudRenderSnapshot(HudRenderSnapshot& io) const {
    const bool siderOpen = IsSiderOverlayOpen();
    std::lock_guard<std::mutex> lock(mutex_);

    io.tel = telemetry_;
    io.tel.siderOverlayOpen = siderOpen;
    io.pool = verdictPool_;
    if (verdictPool_.Empty()) {
        io.pending = VarVerdict{};
    } else if (verdictPool_.Count() == 1) {
        if (verdictPool_.hasPlayDecision) {
            io.pending = verdictPool_.playDecision;
        } else {
            for (const auto& e : verdictPool_.cardEntries) {
                if (e.active) { io.pending = e.verdict; break; }
            }
        }
    } else {
        if (verdictPool_.hasPlayDecision) {
            io.pending = verdictPool_.playDecision;
        } else {
            uint32_t bestOrder = 0xFFFFFFFFU;
            for (const auto& e : verdictPool_.cardEntries) {
                if (e.active && e.order < bestOrder) { bestOrder = e.order; io.pending = e.verdict; }
            }
        }
        io.pending.armed = true;
        io.pending.sequence = verdictPool_.sequence;
        verdictPool_.FormatCombinedSummary(io.pending.summary, sizeof(io.pending.summary));
    }
    io.stats = stats_;
    io.cfg = hudConfig_;

    if (io.rosterRev != rosterRevision_) {
        io.players = players_;
        io.rosterRev = rosterRevision_;
    }
    if (io.teamsRev != teamsRevision_) {
        io.teams = teams_;
        io.teamsRev = teamsRevision_;
    }
    io.snapRev = snapRevision_;
    if (io.historyRev != dataRevision_) {
        const size_t n = std::min<size_t>(16, eventCount_);
        io.history.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = (eventHead_ + kMaxEvents - 1 - i) % kMaxEvents;
            io.history[i] = events_[idx];
        }
        io.historyRev = dataRevision_;
    }

    if (io.tel.matchActive && io.tel.replayMode >= 1 && io.tel.replayMode <= 3) {
        if (io.syncDataRev != dataRevision_ || io.syncSnapRev != snapRevision_ ||
            io.syncPhase != io.tel.replayCursorPhase ||
            std::fabs(io.syncCursorTime - io.tel.replayCursorTime) > 0.05f) {
            io.sync = SyncToReplayCursorLocked(io.tel.replayCursorPhase, io.tel.replayCursorTime, 210.0f);
            io.syncDataRev    = dataRevision_;
            io.syncSnapRev    = snapRevision_;
            io.syncPhase      = io.tel.replayCursorPhase;
            io.syncCursorTime = io.tel.replayCursorTime;
        }

        if (io.markersDataRev != dataRevision_ || io.markersSnapRev != snapRevision_ ||
            io.markersPhase != io.tel.replayWindowStartPhase ||
            std::fabs(io.markersWinStart - io.tel.replayWindowStartTime) > 0.5f ||
            std::fabs(io.markersWinEnd - io.tel.replayWindowEndTime) > 0.5f) {
            BuildReplayWindowMarkersLocked(io.tel.replayWindowStartPhase,
                                           io.tel.replayWindowStartTime,
                                           io.tel.replayWindowEndTime,
                                           io.markers);
            io.markersDataRev  = dataRevision_;
            io.markersSnapRev  = snapRevision_;
            io.markersPhase    = io.tel.replayWindowStartPhase;
            io.markersWinStart = io.tel.replayWindowStartTime;
            io.markersWinEnd   = io.tel.replayWindowEndTime;
        }
    } else {
        io.sync = ReplaySyncResult{};
        io.markers.clear();
        io.syncDataRev = 0;
        io.markersDataRev = 0;
    }
}

void VarState::RequestReplaySeekToTime(int32_t phase, float time60Hz, float fallbackNorm, bool pauseAfterSeek) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasPendingReplayCmd_.load(std::memory_order_acquire)) pendingReplayCmd_ = ReplayControlCommand{};
    pendingReplayCmd_.hasSeek        = true;
    pendingReplayCmd_.seekByTime     = true;
    pendingReplayCmd_.targetPhase    = phase;
    pendingReplayCmd_.targetTime60Hz = time60Hz;
    pendingReplayCmd_.targetNorm     = std::clamp(fallbackNorm, 0.0f, 1.0f);
    pendingReplayCmd_.pauseAfterSeek = pauseAfterSeek;
    if (pauseAfterSeek) pendingReplayCmd_.hasPlayToggle = false; // the latest intent wins: jump and stay paused
    hasPendingReplayCmd_.store(true, std::memory_order_release);
}

void VarState::RequestReplaySeekToNorm(float norm, bool pauseAfterSeek) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasPendingReplayCmd_.load(std::memory_order_acquire)) pendingReplayCmd_ = ReplayControlCommand{};
    pendingReplayCmd_.hasSeek        = true;
    pendingReplayCmd_.seekByTime     = false;
    pendingReplayCmd_.targetNorm     = std::clamp(norm, 0.0f, 1.0f);
    pendingReplayCmd_.pauseAfterSeek = pauseAfterSeek;
    if (pauseAfterSeek) pendingReplayCmd_.hasPlayToggle = false; // the latest intent wins: jump and stay paused
    hasPendingReplayCmd_.store(true, std::memory_order_release);
}

void VarState::RequestReplaySetPlaying(bool playing) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasPendingReplayCmd_.load(std::memory_order_acquire)) pendingReplayCmd_ = ReplayControlCommand{};
    pendingReplayCmd_.hasPlayToggle = true;
    pendingReplayCmd_.targetPlaying = playing;
    hasPendingReplayCmd_.store(true, std::memory_order_release);
}

void VarState::RequestReplayCameraFocus(int32_t focusSlot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasPendingReplayCmd_.load(std::memory_order_acquire)) pendingReplayCmd_ = ReplayControlCommand{};
    pendingReplayCmd_.hasFocusTarget = true;
    pendingReplayCmd_.focusSlot      = std::clamp(focusSlot, 0, 22);
    hasPendingReplayCmd_.store(true, std::memory_order_release);
}

bool VarState::ConsumeReplayCommand(ReplayControlCommand& outCmd) {
    if (!hasPendingReplayCmd_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasPendingReplayCmd_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    outCmd = pendingReplayCmd_;
    pendingReplayCmd_ = ReplayControlCommand{};
    return outCmd.hasSeek || outCmd.hasPlayToggle || outCmd.hasFocusTarget;
}

void VarState::ArmVerdict(const VarVerdict& verdict) {
    AddToVerdictPool(verdict, nullptr);
}

void VarState::AddToVerdictPool(const VarVerdict& verdict, bool* outReplacedPlay) {
    AddToVerdictPoolEx(verdict, outReplacedPlay, nullptr);
}

uint32_t VarState::FindLatestOfficialFoulEventId(int32_t offenderSlot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < eventCount_; ++i) {
        const VarEvent& e = events_[(eventHead_ + kMaxEvents - 1 - i) % kMaxEvents];
        if (e.reviewedByVar || e.offenderSlot != offenderSlot) continue;
        if (e.type == VarEventType::Foul || e.type == VarEventType::Penalty || e.type == VarEventType::CardOnly) {
            return e.id;
        }
    }
    return 0;
}

void VarState::RemoveCardEntriesForEvent(uint32_t eventId) {
    if (eventId == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : verdictPool_.cardEntries) {
        if (e.active && !e.verdict.directCardEdit && e.verdict.sourceEventId == eventId) e = CardPoolEntry{};
    }
    hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
}

bool VarState::AddToVerdictPoolEx(const VarVerdict& verdict, bool* outReplacedPlay, bool* outEvictedCard) {
    if (outReplacedPlay) *outReplacedPlay = false;
    if (outEvictedCard) *outEvictedCard = false;

    // A "None" decision has nothing to execute and would keep the pool armed forever
    if (verdict.action == VarAction::None) return false;
    if (verdict.action == VarAction::EditCard && !IsValidSlot(verdict.offenderSlot)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // Under the lock: SetEnabled(false) stores the flag and then cancels the pool under this same lock
    if (!enabled_.load(std::memory_order_acquire) || !offlineAllowed_.load(std::memory_order_acquire)) return false;
    VarVerdict stored = verdict;
    stored.armed = true;
    stored.sequence = verdictSeq_++;
    verdictPool_.sequence = stored.sequence;

    const VarAction pa = verdictPool_.playDecision.action;
    const bool playCarriesFoul = verdictPool_.hasPlayDecision &&
        (pa == VarAction::ConvertToPenalty || pa == VarAction::ConvertToFreeKick ||
         pa == VarAction::RetroactivePenalty || pa == VarAction::RetroactiveFreeKick);
    if (stored.action == VarAction::EditCard && !stored.directCardEdit && stored.sourceEventId != 0 &&
        playCarriesFoul && verdictPool_.playDecision.sourceEventId == stored.sourceEventId) {
        // The play decision on this event already reverts its original card: the edit only changes the card
        // that decision gives, so the original card is never reverted twice.
        verdictPool_.playDecision.targetCard = stored.targetCard;
        if (IsValidSlot(stored.offenderSlot)) {
            verdictPool_.playDecision.offenderSlot = stored.offenderSlot;
            verdictPool_.playDecision.offenderRosterIdx = stored.offenderRosterIdx;
            // The kick always goes to the opponent of the offender
            verdictPool_.playDecision.benefitedTeam = 1 - SlotToTeam(stored.offenderSlot);
        }
        // playOrder is kept: the revocation of the original card must stay before later direct edits
    } else if (stored.action == VarAction::EditCard) {
        // Replace the entry with the same key (same player for direct edits, same official event for
        // event edits), so one event edited twice with different players never applies both.
        for (auto& e : verdictPool_.cardEntries) {
            if (e.active && SameCardPoolKey(e.verdict, stored)) e = CardPoolEntry{};
        }
        CardPoolEntry* slotFor = nullptr;
        for (auto& e : verdictPool_.cardEntries) {
            if (!e.active) { slotFor = &e; break; }
        }
        if (!slotFor) {
            // Pool full (32 card edits): drop the oldest card entry and tell the HUD
            for (auto& e : verdictPool_.cardEntries) {
                if (!slotFor || e.order < slotFor->order) slotFor = &e;
            }
            if (outEvictedCard) *outEvictedCard = true;
        }
        slotFor->active = true;
        slotFor->order = stored.sequence;
        slotFor->verdict = stored;
    } else {
        auto isLiveCancel = [](const VarVerdict& v) {
            return v.action == VarAction::OverturnCancel && v.targetsLiveSetPlay;
        };
        auto carriesFoul = [](const VarVerdict& v) {
            return v.action == VarAction::ConvertToPenalty || v.action == VarAction::ConvertToFreeKick ||
                   v.action == VarAction::RetroactivePenalty || v.action == VarAction::RetroactiveFreeKick;
        };
        // "Mark the earlier foul and cancel the current call": both are play decisions, but the new foul's
        // set play already replaces the current one. The cancel keeps only its card / ledger part.
        if (verdictPool_.hasPlayDecision &&
            ((isLiveCancel(verdictPool_.playDecision) && carriesFoul(stored)) ||
             (carriesFoul(verdictPool_.playDecision) && isLiveCancel(stored)) ) &&
            verdictPool_.playDecision.sourceEventId != stored.sourceEventId) {
            const bool cancelIsNew = isLiveCancel(stored);
            VarVerdict cancel = cancelIsNew ? stored : verdictPool_.playDecision;
            cancel.targetsLiveSetPlay = false;
            if (!cancelIsNew) {
                verdictPool_.playDecision = stored;
                verdictPool_.playOrder = stored.sequence;
                // Same rule as any play decision: earlier card-only edits of its event are folded into it,
                // so the event's original card is never reverted twice
                if (stored.sourceEventId != 0) {
                    for (auto& e : verdictPool_.cardEntries) {
                        if (e.active && !e.verdict.directCardEdit && e.verdict.sourceEventId == stored.sourceEventId) {
                            e = CardPoolEntry{};
                        }
                    }
                }
            }
            for (auto& e : verdictPool_.cardEntries) {
                if (e.active && SameCardPoolKey(e.verdict, cancel)) e = CardPoolEntry{};
            }
            CardPoolEntry* slotFor = nullptr;
            for (auto& e : verdictPool_.cardEntries) {
                if (!e.active) { slotFor = &e; break; }
            }
            if (!slotFor) {
                for (auto& e : verdictPool_.cardEntries) {
                    if (!slotFor || e.order < slotFor->order) slotFor = &e;
                }
                if (outEvictedCard) *outEvictedCard = true;
            }
            slotFor->active = true;
            slotFor->order = cancel.sequence;
            slotFor->verdict = cancel;
            hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
            return true;
        }
        if (outReplacedPlay &&
            verdictPool_.hasPlayDecision &&
            verdictPool_.playDecision.action != VarAction::None &&
            stored.action != VarAction::None) {
            *outReplacedPlay = true;
        }
        verdictPool_.hasPlayDecision = true;
        verdictPool_.playOrder = stored.sequence;
        verdictPool_.playDecision = stored;

        // If this play decision acts on an official incident, drop any earlier non-direct card-only
        // tweak targeting the exact same official event so its original card is never reverted twice.
        if (stored.sourceEventId != 0) {
            for (auto& e : verdictPool_.cardEntries) {
                if (e.active && !e.verdict.directCardEdit && e.verdict.sourceEventId == stored.sourceEventId) {
                    e = CardPoolEntry{};
                }
            }
        }
    }

    hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
    return true;
}

void VarState::SetPlayDecisionSummary(const char* summary) {
    if (!summary) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!verdictPool_.hasPlayDecision) return;
    std::snprintf(verdictPool_.playDecision.summary, sizeof(verdictPool_.playDecision.summary), "%s", summary);
    Utf8TrimIncompleteTail(verdictPool_.playDecision.summary);
}

void VarState::RemovePlayFromPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    verdictPool_.hasPlayDecision = false;
    verdictPool_.playOrder = 0;
    verdictPool_.playDecision = VarVerdict{};
    hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
}

void VarState::RemoveCardFromPool(int32_t slot) {
    if (!IsValidSlot(slot)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : verdictPool_.cardEntries) {
        if (e.active && e.verdict.offenderSlot == slot) e = CardPoolEntry{};
    }
    hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
}

void VarState::RemoveCardPoolEntry(uint32_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : verdictPool_.cardEntries) {
        if (e.active && e.verdict.sequence == sequence) e = CardPoolEntry{};
    }
    hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
}

void VarState::CancelVerdict() {
    std::lock_guard<std::mutex> lock(mutex_);
    verdictPool_ = VerdictPool{};
    hasArmedVerdict_.store(false, std::memory_order_release);
}

VarVerdict VarState::PeekVerdict() const {
    if (!hasArmedVerdict_.load(std::memory_order_acquire)) {
        return VarVerdict{};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (verdictPool_.Empty()) {
        return VarVerdict{};
    }
    if (verdictPool_.Count() == 1) {
        if (verdictPool_.hasPlayDecision) return verdictPool_.playDecision;
        for (const auto& e : verdictPool_.cardEntries) {
            if (e.active) return e.verdict;
        }
        return VarVerdict{};
    }
    VarVerdict combined{};
    if (verdictPool_.hasPlayDecision) {
        combined = verdictPool_.playDecision;
    } else {
        uint32_t bestOrder = 0xFFFFFFFFU;
        for (const auto& e : verdictPool_.cardEntries) {
            if (e.active && e.order < bestOrder) {
                bestOrder = e.order;
                combined = e.verdict;
            }
        }
    }
    combined.armed = true;
    combined.sequence = verdictPool_.sequence;
    verdictPool_.FormatCombinedSummary(combined.summary, sizeof(combined.summary));
    return combined;
}

VerdictPool VarState::PeekVerdictPool() const {
    if (!hasArmedVerdict_.load(std::memory_order_acquire)) {
        return VerdictPool{};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return verdictPool_;
}

bool VarState::ConsumeVerdict(VarVerdict& outVerdict) {
    if (!hasArmedVerdict_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!verdictPool_.HasExecutableItems()) {
        hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
        return false;
    }

    // Pop the earliest executable decision in chronological order
    bool pickPlay = (verdictPool_.hasPlayDecision &&
                     verdictPool_.playDecision.armed &&
                     verdictPool_.playDecision.action != VarAction::None);
    int bestCardSlot = -1;
    uint32_t bestCardOrder = 0xFFFFFFFFU;
    for (int s = 0; s < static_cast<int>(kMaxCardPoolEntries); ++s) {
        const auto& e = verdictPool_.cardEntries[static_cast<size_t>(s)];
        if (e.active && e.verdict.armed && e.verdict.action != VarAction::None && e.order < bestCardOrder) {
            bestCardOrder = e.order;
            bestCardSlot = s;
        }
    }

    if (pickPlay && (bestCardSlot < 0 || verdictPool_.playOrder <= bestCardOrder)) {
        outVerdict = verdictPool_.playDecision;
        verdictPool_.hasPlayDecision = false;
        verdictPool_.playOrder = 0;
        verdictPool_.playDecision = VarVerdict{};
    } else if (bestCardSlot >= 0) {
        outVerdict = verdictPool_.cardEntries[static_cast<size_t>(bestCardSlot)].verdict;
        verdictPool_.cardEntries[static_cast<size_t>(bestCardSlot)] = CardPoolEntry{};
    } else {
        hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
        return false;
    }

    hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
    return true;
}

int32_t VarState::RecentIncidentOffender(int32_t matchPhase, float time60Hz, float window60Hz) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t best = -1;
    int32_t bestPrio = -1;
    for (size_t k = 0; k < incCount_; ++k) {
        const auto& inc = incidents_[(incHead_ + kMaxIncidents - 1 - k) % kMaxIncidents];
        if (!IsPhaseCompatible(inc.matchPhase, matchPhase)) continue;
        const float d = time60Hz - std::max(inc.time60Hz, inc.lastSeenTime60Hz);
        if (d < 0.0f || d > window60Hz) continue;
        if (IsValidSlot(inc.offenderSlot) && inc.priority > bestPrio && inc.kind != IncidentKind::Dive) {
            bestPrio = inc.priority;
            best = inc.offenderSlot;
        }
    }
    return best;
}

bool VarState::DropStalePlayDecision(uint32_t currentStoppage) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!verdictPool_.hasPlayDecision) return false;
    const VarVerdict& pv = verdictPool_.playDecision;
    if (pv.armStoppage == currentStoppage) return false;
    if (pv.action == VarAction::OverturnCancel && !pv.targetsLiveSetPlay) return false;
    if (pv.action == VarAction::OverturnCancel) {
        // The live set play it cancelled is gone: keep only its card revocation
        verdictPool_.playDecision.targetsLiveSetPlay = false;
        return true;
    }
    verdictPool_.hasPlayDecision = false;
    verdictPool_.playOrder = 0;
    verdictPool_.playDecision = VarVerdict{};
    hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
    return true;
}

bool VarState::RequeueVerdictPool(const VerdictPool& pool, uint32_t matchGeneration) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (matchGeneration != matchGeneration_ || pool.Empty()) return false;
    if (!enabled_.load(std::memory_order_acquire) || !offlineAllowed_.load(std::memory_order_acquire)) return false;
    if (verdictPool_.Empty()) {
        verdictPool_ = pool;
        hasArmedVerdict_.store(true, std::memory_order_release);
        return true;
    }
    // The user armed something meanwhile: merge, the newer items win on the same key / the play slot
    if (!verdictPool_.hasPlayDecision && pool.hasPlayDecision) {
        verdictPool_.hasPlayDecision = true;
        verdictPool_.playDecision = pool.playDecision;
        verdictPool_.playOrder = pool.playOrder;
    }
    for (const auto& old : pool.cardEntries) {
        if (!old.active) continue;
        bool taken = false;
        for (const auto& e : verdictPool_.cardEntries) {
            if (e.active && SameCardPoolKey(e.verdict, old.verdict)) { taken = true; break; }
        }
        if (taken) continue;
        for (auto& e : verdictPool_.cardEntries) {
            if (!e.active) { e = old; break; }
        }
    }
    hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
    return true;
}

bool VarState::ConsumeVerdictPoolIf(bool (*gate)(const VerdictPool&, void*), void* user, VerdictPool& outPool) {
    if (!hasArmedVerdict_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!verdictPool_.HasExecutableItems()) {
        hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
        return false;
    }
    if (gate && !gate(verdictPool_, user)) {
        return false;
    }
    outPool = verdictPool_;
    if (outPool.hasPlayDecision && outPool.playDecision.action == VarAction::None) {
        outPool.hasPlayDecision = false;
        outPool.playOrder = 0;
        outPool.playDecision = VarVerdict{};
    }
    verdictPool_ = VerdictPool{};
    hasArmedVerdict_.store(false, std::memory_order_release);
    return true;
}

bool VarState::ConsumeVerdictPool(VerdictPool& outPool) {
    if (!hasArmedVerdict_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!verdictPool_.HasExecutableItems()) {
        hasArmedVerdict_.store(!verdictPool_.Empty(), std::memory_order_release);
        return false;
    }

    outPool = verdictPool_;
    if (outPool.hasPlayDecision && outPool.playDecision.action == VarAction::None) {
        outPool.hasPlayDecision = false;
        outPool.playOrder = 0;
        outPool.playDecision = VarVerdict{};
    }
    verdictPool_ = VerdictPool{};
    hasArmedVerdict_.store(false, std::memory_order_release);
    return true;
}

void VarState::IncrementInterventions() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.totalVarInterventions;
}

void VarState::UpdateEventAfterVerdict(const VarVerdict& applied) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (applied.sourceEventId == 0) return;

    for (size_t i = 0; i < eventCount_; ++i) {
        const size_t idx = (eventHead_ + kMaxEvents - 1 - i) % kMaxEvents;
        if (events_[idx].id == applied.sourceEventId) {
            auto& ev = events_[idx];
            const CardKind cardBefore = ev.cardKind;
            const int32_t offenderBefore = ev.offenderSlot;
            ev.reviewedByVar = true;
            if (applied.action == VarAction::OverturnCancel) {
                ev.type = VarEventType::NoFoul;
                ev.cardKind = CardKind::None;
                ev.setPlayKind = SetPlayKind::None;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Anulado pelo VAR");
            } else if (applied.action == VarAction::RestartCorner) {
                ev.type = VarEventType::NoFoul;
                ev.cardKind = CardKind::None;
                ev.setPlayKind = SetPlayKind::CornerKick;
                ev.benefitedTeam = applied.benefitedTeam;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Reinício: escanteio");
            } else if (applied.action == VarAction::RestartGoalKick) {
                ev.type = VarEventType::NoFoul;
                ev.cardKind = CardKind::None;
                ev.setPlayKind = SetPlayKind::GoalKick;
                ev.benefitedTeam = applied.benefitedTeam;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Reinício: tiro de meta");
            } else if (applied.action == VarAction::RestartThrowIn) {
                ev.type = VarEventType::NoFoul;
                ev.cardKind = CardKind::None;
                ev.setPlayKind = SetPlayKind::ThrowIn;
                ev.benefitedTeam = applied.benefitedTeam;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Reinício: lateral");
            } else if (applied.action == VarAction::EditCard) {
                if (IsValidSlot(applied.offenderSlot)) {
                    ev.offenderSlot = applied.offenderSlot;
                    ev.offenderRosterIdx = applied.offenderRosterIdx;
                }
                ev.cardKind = applied.targetCard;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Cartão ajustado");
            } else if (applied.action == VarAction::ConvertToPenalty || applied.action == VarAction::RetroactivePenalty) {
                ev.type = VarEventType::Penalty;
                ev.setPlayKind = SetPlayKind::PenaltyKick;
                ev.benefitedTeam = applied.benefitedTeam;
                if (IsValidSlot(applied.offenderSlot)) {
                    ev.offenderSlot = applied.offenderSlot;
                    ev.offenderRosterIdx = applied.offenderRosterIdx;
                }
                if (IsValidSlot(applied.victimSlot)) {
                    ev.victimSlot = applied.victimSlot;
                    ev.victimRosterIdx = applied.victimRosterIdx;
                }
                ev.cardKind = applied.targetCard;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Pênalti pelo VAR");
            } else if (applied.action == VarAction::ConvertToFreeKick || applied.action == VarAction::RetroactiveFreeKick) {
                ev.type = VarEventType::Foul;
                ev.setPlayKind = SetPlayKind::FreeKick;
                ev.benefitedTeam = applied.benefitedTeam;
                if (IsValidSlot(applied.offenderSlot)) {
                    ev.offenderSlot = applied.offenderSlot;
                    ev.offenderRosterIdx = applied.offenderRosterIdx;
                }
                if (IsValidSlot(applied.victimSlot)) {
                    ev.victimSlot = applied.victimSlot;
                    ev.victimRosterIdx = applied.victimRosterIdx;
                }
                ev.cardKind = applied.targetCard;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Falta pelo VAR");
            }
            // Keep the "after" card counts in line with the new card, otherwise the timeline keeps showing
            // CARTÃO VERMELHO for a red the VAR removed.
            if (applied.ledgerYellows >= 0 && applied.ledgerReds >= 0) {
                ev.offenderYellowsAfter = static_cast<uint8_t>(applied.ledgerYellows);
                ev.offenderRedsAfter = static_cast<uint8_t>(applied.ledgerReds);
            } else if (ev.cardKind != cardBefore || ev.offenderSlot != offenderBefore) {
                int yellows = ev.offenderYellowsAfter;
                if (ev.offenderSlot != offenderBefore) {
                    yellows = (ev.cardKind == CardKind::Yellow) ? 1 : 0; // other player: history unknown here
                } else {
                    if (cardBefore == CardKind::Yellow && yellows > 0) --yellows;
                    if (ev.cardKind == CardKind::Yellow) ++yellows;
                }
                ev.offenderYellowsAfter = static_cast<uint8_t>(std::clamp(yellows, 0, 2));
                ev.offenderRedsAfter = (ev.cardKind == CardKind::Red ||
                                        (ev.cardKind == CardKind::Yellow && ev.offenderYellowsAfter >= 2)) ? 1 : 0;
            }
            ++dataRevision_;
            break;
        }
    }
}

void VarState::SetPostApplyBanner(const char* text, uint64_t durationMs, BannerStyle style) {
    const uint64_t now = GetTickCount64();
    const uint64_t until = (text && text[0]) ? (now + durationMs) : 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (text && text[0]) {
            std::snprintf(telemetry_.lastBannerText, sizeof(telemetry_.lastBannerText), "%s", text);
        } else {
            telemetry_.lastBannerText[0] = '\0';
        }
        telemetry_.lastBannerStartMs = now;
        telemetry_.lastBannerUntilMs = until;
        telemetry_.lastBannerStyle = style;
    }
    activeBannerUntilMs_.store(until, std::memory_order_release);
}

void VarState::SetBroadcastAlert(const char* text, uint64_t durationMs, BannerStyle style) {
    if (!broadcastAlerts_.load(std::memory_order_relaxed)) return;
    const uint64_t now = GetTickCount64();
    const uint64_t until = (text && text[0]) ? (now + durationMs) : 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (text && text[0]) {
            std::snprintf(telemetry_.alertBannerText, sizeof(telemetry_.alertBannerText), "%s", text);
        } else {
            telemetry_.alertBannerText[0] = '\0';
        }
        telemetry_.alertBannerStartMs = now;
        telemetry_.alertBannerUntilMs = until;
        telemetry_.alertBannerStyle = style;
    }
    activeAlertUntilMs_.store(until, std::memory_order_release);
}

void VarState::ComputeClosestContact(PlaySnapshot& snap) {
    snap.closestA = -1;
    snap.closestB = -1;
    snap.closestDistSq = 999999.0f;

    // Preserve real BallState owner if already populated by Fox Engine; otherwise find nearest player
    if (!IsValidSlot(snap.ballOwnerSlot)) {
        snap.ballOwnerSlot = -1;
        float bestBallDistSq = 999999.0f;
        for (int i = 0; i < 22; ++i) {
            if (!snap.playerOnPitch[i]) continue;
            const float d2 = DistSq2D(snap.playerPos[i], snap.ballPos);
            if (d2 < bestBallDistSq) {
                bestBallDistSq = d2;
                snap.ballOwnerSlot = i;
            }
        }
    }

    float bestScore = 999999.0f;
    for (int h = 0; h < 11; ++h) {
        if (!snap.playerOnPitch[h]) continue;
        for (int a = 11; a < 22; ++a) {
            if (!snap.playerOnPitch[a]) continue;
            const float pairDistSq = DistSq2D(snap.playerPos[h], snap.playerPos[a]);
            const Vec3 mid{
                0.5f * (snap.playerPos[h].x + snap.playerPos[a].x),
                0.0f,
                0.5f * (snap.playerPos[h].z + snap.playerPos[a].z)
            };
            const float ballDistSq = DistSq2D(mid, snap.ballPos);
            const float score = pairDistSq + 0.12f * ballDistSq;
            if (score < bestScore) {
                bestScore = score;
                snap.closestDistSq = pairDistSq;
                snap.closestA = h;
                snap.closestB = a;
            }
        }
    }
}

bool VarState::DetectPhysicalIncident(const PlaySnapshot& snap, const PitchDims& dims,
                                      const float teamAttackDir[2], DetectedIncident& outInc) {
    // Mirrors the engine's own contact observer (ObserverFallDownOrStagger 0x140A1AF50): an incident is a player whose
    // action is a contact reaction (16 stagger / 17 fall, or flag bit13), and the opponent the engine recorded as the
    // cause at PlayerAction + 0x3D. The causer's action names the incident. A dive (15) or the simulation flag is a
    // simulation, never a foul. A tackle with nobody going down is only a challenge (low priority, neutral colour).
    outInc = DetectedIncident{};
    int32_t bestPriority = 0;

    auto opponents = [](int32_t a, int32_t b) {
        return IsValidSlot(a) && IsValidSlot(b) && SlotToTeam(a) != SlotToTeam(b);
    };
    auto considerCandidate = [&](IncidentKind kind, int32_t prio, int32_t offSlot, int32_t vicSlot, bool nearFoul,
                                 bool rolesConfirmed) {
        if (!opponents(offSlot, vicSlot)) return;
        const Vec3 spot{
            0.35f * snap.playerPos[offSlot].x + 0.65f * snap.playerPos[vicSlot].x,
            0.0f,
            0.35f * snap.playerPos[offSlot].z + 0.65f * snap.playerPos[vicSlot].z
        };
        // The box that matters is the one the offender defends, except for a simulation: the diver goes down in
        // the box his opponents defend
        const int32_t boxTeam = (kind == IncidentKind::Dive) ? SlotToTeam(vicSlot) : SlotToTeam(offSlot);
        const bool inBox = PitchGeometry::IsInsideDefendingBox(dims, spot, teamAttackDir[boxTeam]);
        const int32_t adjPrio = prio + (inBox ? 6 : 0);
        const float distM = std::sqrt(std::max(0.0f, DistSq2D(snap.playerPos[offSlot], snap.playerPos[vicSlot])));
        if (adjPrio > bestPriority || (adjPrio == bestPriority && distM < outInc.contactDistM)) {
            bestPriority        = adjPrio;
            outInc.matchPhase   = snap.matchPhase;
            outInc.time60Hz     = snap.time60Hz;
            outInc.matchTick    = snap.matchTick;
            outInc.kind         = kind;
            outInc.priority     = adjPrio;
            outInc.offenderSlot = offSlot;
            outInc.victimSlot   = vicSlot;
            outInc.pos          = spot;
            outInc.contactDistM = distM;
            outInc.insideBox    = inBox;
            outInc.nearFoul     = nearFoul;
            outInc.rolesConfirmed = rolesConfirmed;
            outInc.boxDistM     = PitchGeometry::SignedDistanceToDefendingBox(dims, spot, teamAttackDir[boxTeam]);
            outInc.boxDistValid = true;
        }
    };
    auto nearestOpponent = [&](int32_t actor, float maxDist, bool requireChallenger) -> int32_t {
        const int32_t oppStart = (SlotToTeam(actor) == 0) ? 11 : 0;
        int32_t best = -1;
        float bestD2 = maxDist * maxDist;
        for (int s = oppStart; s < oppStart + 11; ++s) {
            if (!snap.playerOnPitch[s]) continue;
            if (requireChallenger) {
                const int8_t a = snap.playerActionId[s];
                if (!PlayerActionId::IsSlide(a) && a != PlayerActionId::Tackle && a != PlayerActionId::Jostle) continue;
            }
            const float d2 = DistSq2D(snap.playerPos[actor], snap.playerPos[s]);
            if (d2 <= bestD2) { bestD2 = d2; best = s; }
        }
        return best;
    };
    auto kindFromCauser = [&](int32_t causer, IncidentKind& kind, int32_t& prio) {
        const int8_t a = snap.playerActionId[causer];
        if (PlayerActionId::IsSlide(a))        { kind = IncidentKind::SlideTackle; prio = 82; }
        else if (a == PlayerActionId::Tackle)  { kind = IncidentKind::StandTackle; prio = 74; }
        else if (a == PlayerActionId::Jostle)  { kind = IncidentKind::BodyCharge;  prio = 66; }
        else                                   { kind = IncidentKind::VictimFall;  prio = 64; }
    };

    for (int v = 0; v < 22; ++v) {
        if (!snap.playerOnPitch[v]) continue;
        const int8_t aid = snap.playerActionId[v];
        const uint32_t flags = snap.playerActionFlags[v];

        // Simulation: the player went down by himself
        if (aid == PlayerActionId::Dive || (flags & PlayerActionId::FlagSimulation) != 0) {
            int32_t opp = snap.playerCauser[v];
            if (!opponents(v, opp) || !snap.playerOnPitch[opp] ||
                DistSq2D(snap.playerPos[v], snap.playerPos[opp]) > kCauserMaxDistM * kCauserMaxDistM) {
                opp = nearestOpponent(v, 2.5f, false); // the opponent he went down next to (no contact by definition)
            }
            // The diver is the one to punish (card, indirect restart for the opponents): he is the offender
            if (opponents(v, opp)) considerCandidate(IncidentKind::Dive, 48, v, opp, false, true);
            continue;
        }

        // Contact reaction: the engine tells who caused it. A recorded causer far away is a stale record of an
        // earlier contact, not this one.
        if (PlayerActionId::IsContactReaction(aid, flags)) {
            int32_t causer = snap.playerCauser[v];
            int32_t penalty = 0;
            if (!opponents(v, causer) || !snap.playerOnPitch[causer] ||
                DistSq2D(snap.playerPos[v], snap.playerPos[causer]) > kCauserMaxDistM * kCauserMaxDistM) {
                // No usable causer: only an opponent actively challenging within body reach counts
                causer = nearestOpponent(v, kAdjacentChallengerM, true);
                penalty = 8;
            }
            if (opponents(v, causer)) {
                IncidentKind kind{};
                int32_t prio = 0;
                kindFromCauser(causer, kind, prio);
                considerCandidate(kind, prio - penalty, causer, v, true, true);
            }
        }
    }

    // Challenges with nobody going down: a slide or standing tackle on the ball carrier within the reach of that
    // challenge (never a goalkeeper action, never a block). Low priority and no roles: a duel the user can still
    // jump to, never an offender and a victim.
    if (bestPriority == 0) {
        const int32_t carrier = IsValidSlot(snap.ballOwnerSlot) ? snap.ballOwnerSlot : snap.recentBallOwnerSlot;
        if (IsValidSlot(carrier) && snap.playerOnPitch[carrier]) {
            for (int s = 0; s < 22; ++s) {
                if (!snap.playerOnPitch[s] || !opponents(s, carrier)) continue;
                const int8_t a = snap.playerActionId[s];
                if (!PlayerActionId::IsSlide(a) && a != PlayerActionId::Tackle) continue;
                const float reach = PlayerActionId::IsSlide(a) ? kSlideReachM : kStandReachM;
                if (DistSq2D(snap.playerPos[s], snap.playerPos[carrier]) > reach * reach) continue;
                considerCandidate(PlayerActionId::IsSlide(a) ? IncidentKind::SlideTackle : IncidentKind::StandTackle,
                                  PlayerActionId::IsSlide(a) ? 40 : 34, s, carrier, false, false);
            }
        }
    }

    return bestPriority > 0;
}

} // namespace var_mod
