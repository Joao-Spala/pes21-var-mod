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
    eventHead_ = 0;
    eventCount_ = 0;
    nextEventId_ = 1;
    snapHead_ = 0;
    snapCount_ = 0;
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

    FILE* out = _wfopen(path.c_str(), L"wb");
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
    std::fprintf(out, "language = %s\n", i18n::LanguageCode(cfg.language == 1 ? Language::English : Language::PtBr));
    std::fprintf(out, "# Atalhos: key_* = ate 2 codigos de tecla virtual (hex, 0x00 = vazio), pad_* = botao XInput (hex)\n");
    for (int a = 0; a < kBindActionCount; ++a) {
        const auto& k = cfg.bindings.keys[static_cast<size_t>(a)];
        std::fprintf(out, "key_%s = 0x%02X,0x%02X\n", kBindIniNames[a], k[0], k[1]);
        if (InputBindings::IsPadRemappable(static_cast<BindAction>(a))) {
            std::fprintf(out, "pad_%s = 0x%04X\n", kBindIniNames[a], cfg.bindings.pad[static_cast<size_t>(a)]);
        }
    }
    std::fclose(out);
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
    snapshots_[snapHead_] = snap;
    snapHead_ = (snapHead_ + 1) % kMaxSnapshots;
    if (snapCount_ < kMaxSnapshots) {
        ++snapCount_;
    }
    ++dataRevision_;
}

void VarState::PushDetectedIncident(const DetectedIncident& inc) {
    if (inc.kind == IncidentKind::None || !IsValidSlot(inc.offenderSlot)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    // Temporal clustering (~1.2s = 72 ticks at 60Hz): merge consecutive frames of the same tackle/fall
    if (incCount_ > 0) {
        const size_t lastIdx = (incHead_ + kMaxIncidents - 1) % kMaxIncidents;
        auto& prev = incidents_[lastIdx];
        const bool sameHalf = IsPhaseCompatible(prev.matchPhase, inc.matchPhase);
        const float dt = std::fabs(inc.time60Hz - prev.time60Hz);
        if (sameHalf && dt <= 72.0f) {
            const bool samePair =
                (prev.offenderSlot == inc.offenderSlot && prev.victimSlot == inc.victimSlot) ||
                (prev.offenderSlot == inc.victimSlot && prev.victimSlot == inc.offenderSlot) ||
                (prev.offenderSlot == inc.offenderSlot) ||
                (DistSq2D(prev.pos, inc.pos) <= 20.0f);
            if (samePair) {
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
                    // Keep first high-severity impact timestamp unless upgrading from a weak contact
                    if (oldPriority < 55 || inc.priority >= oldPriority + 8) {
                        prev.time60Hz   = inc.time60Hz;
                        prev.matchTick  = inc.matchTick;
                        prev.matchPhase = inc.matchPhase;
                    }
                    ++dataRevision_;
                } else if (!prev.insideBox && inc.insideBox) {
                    prev.insideBox = true; // marker label/colour changes (box incident) -> invalidate HUD cache
                    ++dataRevision_;
                }
                return;
            }
        }
    }

    incidents_[incHead_] = inc;
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
    for (size_t i = 0; i < snapCount_; ++i) {
        const size_t idx = (snapHead_ + kMaxSnapshots - 1 - i) % kMaxSnapshots;
        const auto& sn = snapshots_[idx];
        if (!IsPhaseCompatible(sn.matchPhase, cursorPhase)) {
            if (bestSnapIdx != kMaxSnapshots) break;
            continue;
        }
        const float dt = std::fabs(sn.time60Hz - cursorTime60Hz);
        if (dt < bestSnapDelta) {
            bestSnapDelta = dt;
            bestSnapIdx = idx;
        } else if (bestSnapIdx != kMaxSnapshots && sn.time60Hz < cursorTime60Hz && dt > bestSnapDelta + 12.0f) {
            break; // Monotonically moving further into the past -> early exit
        }
    }
    if (bestSnapIdx != kMaxSnapshots) {
        res.hasSnapshot = true;
        res.snapshot = snapshots_[bestSnapIdx]; // single 450-byte copy instead of one per improvement
    }

    return res;
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
        case IncidentKind::None:
        default:                         std::snprintf(outBuf, outSize, "LANCE"); break;
    }
}

void VarState::BuildReplayWindowMarkersLocked(int32_t winPhase, float winStartTime, float winEndTime,
                                              std::vector<ReplayMarker>& markers) const {
    markers.clear();
    const float span = winEndTime - winStartTime;
    if (span <= 1.0f) return;

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
        m.signedBoxDistM  = computeBoxDist(ev.offenderSlot, ev.benefitedTeam, ev.pos);

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

    // 2. Physical engine incidents detected at 60Hz (Slide Tackle, Victim Fall, Stand Tackle, Body Charge)
    std::vector<ReplayMarker> detected;
    for (size_t i = 0; i < incCount_; ++i) {
        const size_t idx = (incHead_ + kMaxIncidents - incCount_ + i) % kMaxIncidents;
        const auto& inc = incidents_[idx];
        if (!IsPhaseCompatible(inc.matchPhase, winPhase)) continue;
        if (inc.time60Hz < winStartTime || inc.time60Hz > winEndTime) continue;
        if (!IsValidSlot(inc.offenderSlot)) continue;

        // If near an official event (<= 1.5s), enrich missing victimSlot on the official marker instead of duplicating
        bool mergedIntoOfficial = false;
        for (auto& om : markers) {
            if (std::fabs(om.time60Hz - inc.time60Hz) <= 90.0f) {
                if (!IsValidSlot(om.victimSlot) && IsValidSlot(inc.victimSlot) && inc.victimSlot != om.offenderSlot) {
                    om.victimSlot = inc.victimSlot;
                }
                if (!IsValidSlot(om.offenderSlot) && IsValidSlot(inc.offenderSlot)) {
                    om.offenderSlot = inc.offenderSlot;
                }
                mergedIntoOfficial = true;
                break;
            }
        }
        if (mergedIntoOfficial) continue;

        const float boxDist = computeBoxDist(inc.offenderSlot, 1 - SlotToTeam(inc.offenderSlot), inc.pos);
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
        FormatIncidentLabel(m.kind, boxDist < 0.0f, m.label, sizeof(m.label));

        if (!detected.empty() && std::fabs(m.time60Hz - detected.back().time60Hz) <= 72.0f) {
            if (m.priority > detected.back().priority ||
                (m.priority == detected.back().priority && m.contactDistM < detected.back().contactDistM)) {
                detected.back() = m;
            }
        } else {
            detected.push_back(m);
        }
    }

    // 3. Fallback from 10Hz snapshots only for close contests not already covered by official or physical markers
    constexpr float kFallbackContactDistSq = 2.10f; // <= 1.45m
    for (size_t i = 0; i < snapCount_; ++i) {
        const size_t idx = (snapHead_ + kMaxSnapshots - snapCount_ + i) % kMaxSnapshots;
        const auto& sn = snapshots_[idx];
        if (!IsPhaseCompatible(sn.matchPhase, winPhase)) continue;
        if (sn.time60Hz < winStartTime || sn.time60Hz > winEndTime) continue;
        if (!IsValidSlot(sn.closestA) || !IsValidSlot(sn.closestB)) continue;
        if (sn.closestDistSq > kFallbackContactDistSq) continue;

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
        if (io.syncDataRev != dataRevision_ ||
            io.syncPhase != io.tel.replayCursorPhase ||
            std::fabs(io.syncCursorTime - io.tel.replayCursorTime) > 0.05f) {
            io.sync = SyncToReplayCursorLocked(io.tel.replayCursorPhase, io.tel.replayCursorTime, 210.0f);
            io.syncDataRev    = dataRevision_;
            io.syncPhase      = io.tel.replayCursorPhase;
            io.syncCursorTime = io.tel.replayCursorTime;
        }

        if (io.markersDataRev != dataRevision_ ||
            io.markersPhase != io.tel.replayWindowStartPhase ||
            std::fabs(io.markersWinStart - io.tel.replayWindowStartTime) > 0.5f ||
            std::fabs(io.markersWinEnd - io.tel.replayWindowEndTime) > 0.5f) {
            BuildReplayWindowMarkersLocked(io.tel.replayWindowStartPhase,
                                           io.tel.replayWindowStartTime,
                                           io.tel.replayWindowEndTime,
                                           io.markers);
            io.markersDataRev  = dataRevision_;
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
    pendingReplayCmd_.hasSeek        = true;
    pendingReplayCmd_.seekByTime     = true;
    pendingReplayCmd_.targetPhase    = phase;
    pendingReplayCmd_.targetTime60Hz = time60Hz;
    pendingReplayCmd_.targetNorm     = std::clamp(fallbackNorm, 0.0f, 1.0f);
    pendingReplayCmd_.pauseAfterSeek = pauseAfterSeek;
    hasPendingReplayCmd_.store(true, std::memory_order_release);
}

void VarState::RequestReplaySeekToNorm(float norm, bool pauseAfterSeek) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingReplayCmd_.hasSeek        = true;
    pendingReplayCmd_.seekByTime     = false;
    pendingReplayCmd_.targetNorm     = std::clamp(norm, 0.0f, 1.0f);
    pendingReplayCmd_.pauseAfterSeek = pauseAfterSeek;
    hasPendingReplayCmd_.store(true, std::memory_order_release);
}

void VarState::RequestReplaySetPlaying(bool playing) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingReplayCmd_.hasPlayToggle = true;
    pendingReplayCmd_.targetPlaying = playing;
    hasPendingReplayCmd_.store(true, std::memory_order_release);
}

void VarState::RequestReplayCameraFocus(int32_t focusSlot) {
    std::lock_guard<std::mutex> lock(mutex_);
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

void VarState::AddToVerdictPoolEx(const VarVerdict& verdict, bool* outReplacedPlay, bool* outEvictedCard) {
    if (outReplacedPlay) *outReplacedPlay = false;
    if (outEvictedCard) *outEvictedCard = false;

    // A "None" decision has nothing to execute and would keep the pool armed forever
    if (verdict.action == VarAction::None) return;
    if (verdict.action == VarAction::EditCard && !IsValidSlot(verdict.offenderSlot)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    VarVerdict stored = verdict;
    stored.armed = true;
    stored.sequence = verdictSeq_++;
    verdictPool_.sequence = stored.sequence;

    if (stored.action == VarAction::EditCard) {
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
                if (IsValidSlot(applied.offenderSlot)) ev.offenderSlot = applied.offenderSlot;
                ev.cardKind = applied.targetCard;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Cartão ajustado");
            } else if (applied.action == VarAction::ConvertToPenalty || applied.action == VarAction::RetroactivePenalty) {
                ev.type = VarEventType::Penalty;
                ev.setPlayKind = SetPlayKind::PenaltyKick;
                ev.benefitedTeam = applied.benefitedTeam;
                if (IsValidSlot(applied.offenderSlot)) ev.offenderSlot = applied.offenderSlot;
                if (IsValidSlot(applied.victimSlot)) ev.victimSlot = applied.victimSlot;
                ev.cardKind = applied.targetCard;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Pênalti pelo VAR");
            } else if (applied.action == VarAction::ConvertToFreeKick || applied.action == VarAction::RetroactiveFreeKick) {
                ev.type = VarEventType::Foul;
                ev.setPlayKind = SetPlayKind::FreeKick;
                ev.benefitedTeam = applied.benefitedTeam;
                if (IsValidSlot(applied.offenderSlot)) ev.offenderSlot = applied.offenderSlot;
                if (IsValidSlot(applied.victimSlot)) ev.victimSlot = applied.victimSlot;
                ev.cardKind = applied.targetCard;
                std::snprintf(ev.varNote, sizeof(ev.varNote), "Falta pelo VAR");
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
    outInc = DetectedIncident{};
    int32_t bestPriority = 0;

    auto considerCandidate = [&](IncidentKind kind, int32_t prio, int32_t offSlot, int32_t vicSlot, float distSq) {
        if (!IsValidSlot(offSlot) || !IsValidSlot(vicSlot) || SlotToTeam(offSlot) == SlotToTeam(vicSlot)) return;
        const Vec3 spot{
            0.35f * snap.playerPos[offSlot].x + 0.65f * snap.playerPos[vicSlot].x,
            0.0f,
            0.35f * snap.playerPos[offSlot].z + 0.65f * snap.playerPos[vicSlot].z
        };
        const int32_t offTeam = SlotToTeam(offSlot);
        const bool inBox = PitchGeometry::IsInsideDefendingBox(dims, spot, teamAttackDir[offTeam]);
        const int32_t adjPrio = prio + (inBox ? 6 : 0);
        const float distM = std::sqrt(std::max(0.0f, distSq));
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
        }
    };

    auto findBestOpponent = [&](int32_t actorSlot, float maxDistSq, bool preferAggressor) -> std::pair<int32_t, float> {
        const int32_t actorTeam = SlotToTeam(actorSlot);
        const int32_t oppStart = (actorTeam == 0) ? 11 : 0;
        const int32_t oppEnd   = oppStart + 11;
        int32_t bestSlot = -1;
        float bestScore = 999999.0f;
        float bestRealDistSq = 999999.0f;

        for (int s = oppStart; s < oppEnd; ++s) {
            if (!snap.playerOnPitch[s]) continue;
            const float d2 = DistSq2D(snap.playerPos[actorSlot], snap.playerPos[s]);
            if (d2 > maxDistSq) continue;

            float score = d2;
            const uint8_t aid = snap.playerActionId[s];
            if (preferAggressor) {
                if (aid == PlayerActionId::SlideTackle) score -= 2.2f;
                else if (aid == PlayerActionId::StandTackleA || aid == PlayerActionId::StandTackleB ||
                         aid == PlayerActionId::BodyChargeA  || aid == PlayerActionId::BodyChargeB) score -= 1.2f;
                if (snap.playerActionHit[s] != 0) score -= 1.0f;
            } else {
                if (aid == PlayerActionId::VictimFall) score -= 2.4f;
                if (s == snap.ballOwnerSlot || s == snap.recentBallOwnerSlot) score -= 1.5f;
            }
            if (score < bestScore) {
                bestScore = score;
                bestSlot = s;
                bestRealDistSq = d2;
            }
        }
        return { bestSlot, bestRealDistSq };
    };

    for (int s = 0; s < 22; ++s) {
        if (!snap.playerOnPitch[s]) continue;
        const uint8_t aid = snap.playerActionId[s];
        const bool hasHit = (snap.playerActionHit[s] != 0);

        if (aid == PlayerActionId::SlideTackle) {
            auto [vic, d2] = findBestOpponent(s, 3.2f * 3.2f, false);
            if (IsValidSlot(vic)) {
                const bool vicFell = (snap.playerActionId[vic] == PlayerActionId::VictimFall);
                if (d2 <= 2.75f * 2.75f || vicFell || hasHit) {
                    const int32_t prio = (vicFell || hasHit) ? 80 : 74;
                    considerCandidate(IncidentKind::SlideTackle, prio, s, vic, d2);
                }
            }
        } else if (aid == PlayerActionId::VictimFall) {
            auto [off, d2] = findBestOpponent(s, 3.0f * 3.0f, true);
            if (IsValidSlot(off)) {
                const uint8_t oAid = snap.playerActionId[off];
                const bool aggAct = (oAid == PlayerActionId::SlideTackle ||
                                     oAid == PlayerActionId::StandTackleA || oAid == PlayerActionId::StandTackleB ||
                                     oAid == PlayerActionId::BodyChargeA  || oAid == PlayerActionId::BodyChargeB ||
                                     snap.playerActionHit[off] != 0);
                if (d2 <= 2.65f * 2.65f || aggAct) {
                    const IncidentKind k = (oAid == PlayerActionId::SlideTackle)
                        ? IncidentKind::SlideTackle : IncidentKind::VictimFall;
                    const int32_t prio = aggAct ? 78 : 70;
                    considerCandidate(k, prio, off, s, d2);
                }
            }
        } else if (aid == PlayerActionId::StandTackleA || aid == PlayerActionId::StandTackleB) {
            const float lim = hasHit ? (2.45f * 2.45f) : (2.05f * 2.05f);
            auto [vic, d2] = findBestOpponent(s, lim, false);
            if (IsValidSlot(vic)) {
                const int32_t prio = hasHit ? 65 : 57;
                considerCandidate(IncidentKind::StandTackle, prio, s, vic, d2);
            }
        } else if (aid == PlayerActionId::BodyChargeA || aid == PlayerActionId::BodyChargeB || hasHit) {
            const float lim = hasHit ? (2.15f * 2.15f) : (1.85f * 1.85f);
            auto [opp, d2] = findBestOpponent(s, lim, false);
            if (IsValidSlot(opp)) {
                int32_t off = s;
                int32_t vic = opp;
                // If the actor was actually the ball carrier who took a hit, swap so carrier is victim
                if ((s == snap.ballOwnerSlot || s == snap.recentBallOwnerSlot) &&
                    opp != snap.ballOwnerSlot && opp != snap.recentBallOwnerSlot) {
                    off = opp;
                    vic = s;
                }
                const int32_t prio = hasHit ? 60 : 52;
                considerCandidate(IncidentKind::BodyCharge, prio, off, vic, d2);
            }
        }
    }

    return bestPriority > 0;
}

} // namespace var_mod
