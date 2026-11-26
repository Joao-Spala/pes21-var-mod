#include "var_hud.h"

#include "imgui.h"
#include "logger.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace var_mod {

namespace {

bool IsManualRestartAction(VarAction a) {
    return a == VarAction::RestartCorner ||
           a == VarAction::RestartGoalKick ||
           a == VarAction::RestartThrowIn;
}

bool IsNonZeroVec(const Vec3& v) {
    return (v.x * v.x + v.z * v.z) > 0.01f;
}

} // namespace

// ============================================================================
// PitchRadarViewport (Full Pitch + Auto Penalty-Box Zoom, 1:1 metric mapping)
// ============================================================================

PitchRadarViewport PitchRadarViewport::Compute(const PitchDims& dims, const Vec3& focusSpot,
                                               bool autoZoomBox, int zoomOverride, float aspectRatio) {
    PitchRadarViewport vp{};
    const float safeAspect = std::clamp(aspectRatio, 1.20f, 3.20f);

    const bool nearBox = (std::fabs(focusSpot.x) >= (dims.box18X - 11.5f));
    const bool wantZoom = (zoomOverride == 1) ? true
                        : (zoomOverride == 0) ? false
                        : (autoZoomBox && nearBox);

    if (!wantZoom) {
        vp.isZoomed = false;
        vp.zoomSide = 0;
        // Strict 1:1 metric aspect ratio so circles and boxes are never stretched
        const float minHalfX = dims.halfLength + 2.6f;
        const float minHalfZ = dims.halfWidth + 2.6f;
        float halfZ = minHalfZ;
        float halfX = halfZ * safeAspect;
        if (halfX < minHalfX) {
            halfX = minHalfX;
            halfZ = halfX / safeAspect;
        }
        vp.minX = -halfX;
        vp.maxX =  halfX;
        vp.minZ = -halfZ;
        vp.maxZ =  halfZ;
        return vp;
    }

    vp.isZoomed = true;
    vp.zoomSide = (focusSpot.x >= 0.0f) ? 1 : -1;

    // 1:1 true metric penalty-box zoom (~2.3x linear / ~5.2x area zoom), centered vertically on incident
    const float spanZ = std::min(dims.halfWidth * 2.0f + 4.0f, 32.0f);
    const float halfZ = spanZ * 0.5f;
    const float spanX = std::max(36.0f, spanZ * safeAspect);

    if (vp.zoomSide > 0) {
        vp.maxX = dims.halfLength + 3.0f;
        vp.minX = vp.maxX - spanX;
    } else {
        vp.minX = -(dims.halfLength + 3.0f);
        vp.maxX = vp.minX + spanX;
    }

    const float maxCenterZ = std::max(0.0f, (dims.halfWidth + 2.2f) - halfZ);
    const float centerZ = std::clamp(focusSpot.z, -maxCenterZ, maxCenterZ);
    vp.minZ = centerZ - halfZ;
    vp.maxZ = centerZ + halfZ;
    return vp;
}

void PitchRadarViewport::WorldToNorm(float wx, float wz, float& outU, float& outV) const {
    const float dx = std::max(1.0f, maxX - minX);
    const float dz = std::max(1.0f, maxZ - minZ);
    outU = (wx - minX) / dx;
    outV = (wz - minZ) / dz;
}

Vec3 PitchRadarViewport::NormToWorld(float u, float v, const PitchDims& dims) const {
    const float cu = std::clamp(u, 0.0f, 1.0f);
    const float cv = std::clamp(v, 0.0f, 1.0f);
    const Vec3 raw{
        minX + cu * (maxX - minX),
        0.0f,
        minZ + cv * (maxZ - minZ)
    };
    return PitchGeometry::ClampToPitch(dims, raw, 0.5f);
}

// ============================================================================
// VarHudController v4.0
// ============================================================================

const char* VarHudController::CardLabel(CardKind c) {
    switch (c) {
        case CardKind::Yellow: return Tr("Amarelo");
        case CardKind::Red:    return Tr("Vermelho");
        case CardKind::Simple:
        case CardKind::None:
        case CardKind::Warning:
        default:               return Tr("Sem cartão");
    }
}

const char* VarHudController::DirectCardPresetLabel(int preset) {
    switch (preset) {
        case 1:  return Tr("1 amarelo");
        case 2:  return Tr("2º amarelo (expulso)");
        case 3:  return Tr("Vermelho direto");
        case 0:
        default: return Tr("Sem cartões");
    }
}

const char* VarHudController::SpotPresetLabel(int preset) {
    switch (preset) {
        case 1:  return Tr("Entrada da área");
        case 2:  return Tr("Centro do campo");
        case 3:  return Tr("Personalizado");
        case 0:
        default: return Tr("Local do lance");
    }
}

const char* VarHudController::EventTypeLabel(VarEventType t) {
    switch (t) {
        case VarEventType::Foul:      return Tr("FALTA MARCADA");
        case VarEventType::Penalty:   return Tr("PÊNALTI MARCADO");
        case VarEventType::Advantage: return Tr("VANTAGEM");
        case VarEventType::CardOnly:  return Tr("CARTÃO APLICADO");
        case VarEventType::NoFoul:
        default:                      return Tr("JOGO SEGUE");
    }
}

const char* VarHudController::ActionLabel(VarAction a) {
    switch (a) {
        case VarAction::EditCard:            return Tr("Alterar cartão");
        case VarAction::ConvertToPenalty:    return Tr("Converter em pênalti");
        case VarAction::ConvertToFreeKick:   return Tr("Falta fora da área");
        case VarAction::RetroactiveFreeKick: return Tr("Marcar falta");
        case VarAction::RetroactivePenalty:  return Tr("Marcar pênalti");
        case VarAction::OverturnCancel:      return Tr("Anular marcação");
        case VarAction::RestartCorner:       return Tr("Escanteio");
        case VarAction::RestartGoalKick:     return Tr("Tiro de meta");
        case VarAction::RestartThrowIn:      return Tr("Lateral");
        case VarAction::None:
        default:                             return Tr("Manter decisão");
    }
}

const char* VarHudController::ScalePresetLabel(int preset) {
    switch (preset) {
        case 0:  return Tr("Compacto");
        case 2:  return Tr("Grande");
        case 1:
        default: return Tr("Padrão");
    }
}

void VarHudController::FormatPlayerBroadcastBuf(int32_t slot, char* outBuf, size_t outSize) const {
    if (!outBuf || outSize == 0) return;
    if (!IsValidSlot(slot)) {
        std::snprintf(outBuf, outSize, "%s", Tr("Jogador"));
        return;
    }
    const PlayerSlotInfo& p = players_[slot];
    if (p.name[0] == '\0' && p.shirtName[0] == '\0' && p.shirtNumber == 0) {
        // Name not found in the game roster: localized generic fallback ("Casa 4" / "Away 4")
        std::snprintf(outBuf, outSize, "%s %d", Tr(p.team == 1 ? "Fora" : "Casa"), (slot % 11) + 1);
        return;
    }
    p.FormatBroadcastName(outBuf, outSize);
}

std::string VarHudController::FormatPlayerBroadcast(int32_t slot) const {
    char buf[80];
    FormatPlayerBroadcastBuf(slot, buf, sizeof(buf));
    return std::string(buf);
}

const char* VarHudController::TeamFullName(int32_t team) const {
    const int idx = (team == 1) ? 1 : 0;
    if (teams_[idx].fullName[0] != '\0') return teams_[idx].fullName;
    return Tr(idx == 0 ? "CASA" : "FORA"); // club name not found in the game roster
}

const char* VarHudController::TeamShortName(int32_t team) const {
    const int idx = (team == 1) ? 1 : 0;
    if (teams_[idx].shortName[0] != '\0') return teams_[idx].shortName;
    if (teams_[idx].fullName[0] != '\0') return teams_[idx].fullName; // the renderer clips the full name
    return Tr(idx == 0 ? "CAS" : "FOR");
}

bool VarHudController::ComputeSecondLastDefenderX(const PlaySnapshot& snap, int32_t defendingTeam,
                                                  float defenderAttackDir, float& outX) {
    if (defendingTeam != 0 && defendingTeam != 1) return false;
    const int start = defendingTeam * 11;
    const float dirSign = (defenderAttackDir >= 0.0f) ? 1.0f : -1.0f;

    // Own defending goal lies on the (+dirSign * halfLength) side (verified via Pitch_ClassifyZone 0x140A73470).
    // Sort active defenders by descending (x * dirSign) so index 0 is closest to own goal (GK), index 1 is 2nd-last!
    float scored[11];
    float rawX[11];
    int count = 0;
    for (int i = 0; i < 11; ++i) {
        const int s = start + i;
        if (!snap.playerOnPitch[s]) continue;
        scored[count] = snap.playerPos[s].x * dirSign;
        rawX[count] = snap.playerPos[s].x;
        ++count;
    }
    if (count < 2) return false;

    int idx1 = 0, idx2 = 1;
    if (scored[idx2] > scored[idx1]) std::swap(idx1, idx2);
    for (int i = 2; i < count; ++i) {
        if (scored[i] > scored[idx1]) {
            idx2 = idx1;
            idx1 = i;
        } else if (scored[i] > scored[idx2]) {
            idx2 = i;
        }
    }
    outX = rawX[idx2];
    return true;
}

namespace {
// A recorded foul and the live FoulInfo describe the same stoppage only when they are close in match time
// (the clock barely moves between the whistle and the set piece). 2 game minutes at 60 Hz.
constexpr float kSameStoppageWindow60Hz = 7200.0f;
} // namespace

bool VarHudController::IsSameStoppage(const VarEvent& a, const VarEvent& b) {
    return a.matchPhase == b.matchPhase && std::fabs(a.time60Hz - b.time60Hz) <= kSameStoppageWindow60Hz;
}

void VarHudController::ApplySelectedMarkerToContext(const ReplayMarker& m) {
    hasDetectedContact_   = IsValidSlot(m.offenderSlot);
    detectedOffenderSlot_ = m.offenderSlot;
    detectedOffenderUnknown_ = false;
    detectedVictimSlot_   = m.victimSlot;
    if (IsNonZeroVec(m.pos)) {
        detectedSpotPos_ = m.pos;
    }
    detectedKind_ = m.kind;
    std::snprintf(detectedLabel_, sizeof(detectedLabel_), "%s", m.label[0] ? m.label : "LANCE DETECTADO"); // stored in PT-BR, translated at display

    if (m.isOfficialEvent) {
        bool foundInHistory = false;
        for (const auto& ev : history_) {
            if (m.officialEventId != 0 && ev.id == m.officialEventId) {
                hasActiveEvent_ = true;
                activeEvent_ = ev;
                if (!IsValidSlot(activeEvent_.victimSlot) && IsValidSlot(m.victimSlot)) {
                    activeEvent_.victimSlot = m.victimSlot;
                }
                foundInHistory = true;
                break;
            }
        }
        if (!foundInHistory) {
            hasActiveEvent_ = true;
            activeEvent_ = VarEvent{};
            activeEvent_.id = m.officialEventId;
            activeEvent_.type = m.eventType;
            activeEvent_.matchPhase = m.matchPhase;
            activeEvent_.time60Hz = m.time60Hz;
            activeEvent_.offenderSlot = m.offenderSlot;
            activeEvent_.victimSlot = m.victimSlot;
            activeEvent_.benefitedTeam = IsValidSlot(m.offenderSlot) ? (1 - SlotToTeam(m.offenderSlot)) : 0;
            activeEvent_.cardKind = m.cardKind;
            activeEvent_.pos = m.pos;
            activeEvent_.setPlayKind = (m.eventType == VarEventType::Penalty) ? SetPlayKind::PenaltyKick : SetPlayKind::FreeKick;
        }
    } else {
        hasActiveEvent_ = false;
        activeEvent_ = VarEvent{};
    }

    RebuildMainOptions();
}

void VarHudController::UpdateContext(const LiveTelemetry& tel,
                                     const ReplaySyncResult& sync,
                                     const std::array<PlayerSlotInfo, 22>& players,
                                     const std::array<TeamDisplayInfo, 2>& teams,
                                     const std::vector<VarEvent>& history,
                                     const MatchVarStats& stats,
                                     const HudConfig& cfg,
                                     uint32_t rosterRev,
                                     uint32_t teamsRev,
                                     uint32_t historyRev) {
    tel_ = tel;
    sync_ = sync;
    if (rosterRev == 0 || rosterRev != cachedRosterRev_) {
        players_ = players;
        cachedRosterRev_ = rosterRev;
    }
    if (teamsRev == 0 || teamsRev != cachedTeamsRev_) {
        teams_ = teams;
        cachedTeamsRev_ = teamsRev;
    }
    if (historyRev == 0 || historyRev != cachedHistoryRev_) {
        history_ = history;
        cachedHistoryRev_ = historyRev;
    }
    stats_ = stats;
    config_ = cfg;

    const bool manualNow = tel.matchActive &&
                           tel.enabled &&
                           tel.offlineAllowed &&
                           (tel.specialModeKind == 0) &&
                           (tel.matchPhase >= 0 && tel.matchPhase < 8) &&
                           (tel.replayMode >= 1 && tel.replayMode <= 3) &&
                           (tel.replayManualFlag != 0);

    if (!manualNow) {
        if (inManualReplay_) {
            focused_ = false;
            page_ = HudPage::Main;
            returnPage_ = HudPage::Main;
            selectedIndex_ = 0;
            selectedMarkerIndex_ = -1;
            manualMarkerLock_ = false;
            radarZoomOverride_ = -1;
        }
        inManualReplay_ = false;
        return;
    }

    const bool justEnteredReplay = !inManualReplay_;
    inManualReplay_ = true;

    const uint64_t nowMs = GetTickCount64();
    const bool cursorMoved = (lastTrackedCursorTime_ > -500.0f) &&
                             (std::fabs(tel.replayCursorTime - lastTrackedCursorTime_) > 10.0f);
    if (cursorMoved && nowMs >= seekLockUntilMs_) {
        manualMarkerLock_ = false;
    }
    lastTrackedCursorTime_ = tel.replayCursorTime;

    // Official foul / penalty the match is stopped for, whatever the replay cursor shows
    hasLiveFoul_ = tel.activeFoulValid &&
                   tel.activeFoulKind >= static_cast<int32_t>(FoulKind::StandardFoul) &&
                   (tel.matchStatus == static_cast<int32_t>(MatchStatus::OutOfPlay) ||
                    tel.matchStatus == static_cast<int32_t>(MatchStatus::SetPlayReady));
    if (hasLiveFoul_) {
        liveFoulEvent_ = VarEvent{};
        liveFoulEvent_.matchPhase = tel.matchPhase;
        liveFoulEvent_.time60Hz = tel.time60Hz;
        liveFoulEvent_.setPlayKind = static_cast<SetPlayKind>(tel.activeSetPlayKind);
        liveFoulEvent_.benefitedTeam = tel.activeBenefitedTeam;
        liveFoulEvent_.offenderSlot = tel.activeOffenderSlot;
        liveFoulEvent_.victimSlot = tel.activeVictimSlot;
        liveFoulEvent_.cardKind = (tel.activeFoulCard == 3) ? CardKind::Yellow
                                : (tel.activeFoulCard == 4) ? CardKind::Red
                                : CardKind::Simple;
        liveFoulEvent_.type = (liveFoulEvent_.setPlayKind == SetPlayKind::PenaltyKick) ? VarEventType::Penalty : VarEventType::Foul;
        liveFoulEvent_.pos = tel.liveBallPos;
        // Link to the recorded official event of that offender (newest first) so cards / ledger match
        for (const VarEvent& ev : history_) {
            if (!ev.reviewedByVar && ev.offenderSlot == liveFoulEvent_.offenderSlot &&
                ev.type == liveFoulEvent_.type && IsSameStoppage(ev, liveFoulEvent_)) {
                liveFoulEvent_.id = ev.id;
                liveFoulEvent_.pos = ev.pos;
                liveFoulEvent_.offenderRosterIdx = ev.offenderRosterIdx;
                liveFoulEvent_.victimRosterIdx = (ev.victimSlot == liveFoulEvent_.victimSlot || !IsValidSlot(liveFoulEvent_.victimSlot))
                    ? ev.victimRosterIdx : static_cast<int8_t>(-1);
                liveFoulEvent_.victimSlot = IsValidSlot(ev.victimSlot) ? ev.victimSlot : liveFoulEvent_.victimSlot;
                break;
            }
        }
    }

    // Update active official event & live contact when not manually locked to a chosen marker
    if (!manualMarkerLock_ && page_ != HudPage::Configure) {
        if (sync.hasEvent) {
            hasActiveEvent_ = true;
            activeEvent_ = sync.event;
        } else if (tel.activeFoulValid &&
                   tel.activeFoulKind >= static_cast<int32_t>(FoulKind::StandardFoul) &&
                   (tel.matchStatus == static_cast<int32_t>(MatchStatus::OutOfPlay) ||
                    tel.matchStatus == static_cast<int32_t>(MatchStatus::SetPlayReady))) {
            hasActiveEvent_ = true;
            activeEvent_ = VarEvent{};
            activeEvent_.matchPhase = tel.matchPhase;
            activeEvent_.time60Hz = tel.time60Hz;
            activeEvent_.setPlayKind = static_cast<SetPlayKind>(tel.activeSetPlayKind);
            activeEvent_.benefitedTeam = tel.activeBenefitedTeam;
            activeEvent_.offenderSlot = tel.activeOffenderSlot;
            activeEvent_.victimSlot = tel.activeVictimSlot;
            activeEvent_.cardKind = (tel.activeFoulCard == 3) ? CardKind::Yellow
                                  : (tel.activeFoulCard == 4) ? CardKind::Red
                                  : CardKind::Simple;
            activeEvent_.type = (activeEvent_.setPlayKind == SetPlayKind::PenaltyKick)
                ? VarEventType::Penalty
                : VarEventType::Foul;
            activeEvent_.pos = sync.hasSnapshot ? sync.snapshot.ballPos : tel.liveBallPos;
        } else {
            hasActiveEvent_ = false;
            activeEvent_ = VarEvent{};
        }

        if (hasActiveEvent_ && IsValidSlot(activeEvent_.offenderSlot)) {
            hasDetectedContact_ = true;
            detectedOffenderSlot_ = activeEvent_.offenderSlot;
            detectedOffenderUnknown_ = false;
            detectedVictimSlot_ = IsValidSlot(activeEvent_.victimSlot) ? activeEvent_.victimSlot : -1;
            detectedSpotPos_ = IsNonZeroVec(activeEvent_.pos) ? activeEvent_.pos
                             : (sync.hasSnapshot ? sync.snapshot.ballPos : tel.liveBallPos);
            detectedKind_ = (activeEvent_.type == VarEventType::Penalty) ? IncidentKind::Penalty
                          : (activeEvent_.cardKind == CardKind::Red) ? IncidentKind::RedCard
                          : (activeEvent_.cardKind == CardKind::Yellow) ? IncidentKind::YellowCard
                          : (activeEvent_.type == VarEventType::Advantage) ? IncidentKind::Advantage
                          : IncidentKind::Foul;
            VarState::FormatIncidentLabel(detectedKind_, ActiveSignedBoxDistance() < 0.0f, detectedLabel_, sizeof(detectedLabel_));
        } else if (sync.hasSnapshot && IsValidSlot(sync.snapshot.closestA) && IsValidSlot(sync.snapshot.closestB)) {
            int32_t vic = sync.snapshot.closestA;
            int32_t off = sync.snapshot.closestB;
            if (sync.snapshot.ballOwnerSlot == sync.snapshot.closestB ||
                sync.snapshot.recentBallOwnerSlot == sync.snapshot.closestB) {
                vic = sync.snapshot.closestB;
                off = sync.snapshot.closestA;
            }
            hasDetectedContact_ = (sync.snapshot.closestDistSq <= 6.25f);
            detectedOffenderSlot_ = off;
            detectedOffenderUnknown_ = false;
            detectedVictimSlot_ = vic;
            detectedSpotPos_ = Vec3{
                0.5f * (sync.snapshot.playerPos[off].x + sync.snapshot.playerPos[vic].x),
                0.0f,
                0.5f * (sync.snapshot.playerPos[off].z + sync.snapshot.playerPos[vic].z)
            };
            detectedKind_ = hasDetectedContact_ ? IncidentKind::CloseContact : IncidentKind::None;
            std::snprintf(detectedLabel_, sizeof(detectedLabel_), "%s",
                          hasDetectedContact_ ? "CONTATO" : "JOGO SEGUE");
        } else if (tel.replayFocusSlot >= 1 && tel.replayFocusSlot <= 22) {
            hasDetectedContact_ = false;
            detectedOffenderSlot_ = tel.replayFocusSlot - 1;
            detectedOffenderUnknown_ = false;
            detectedVictimSlot_ = -1;
            detectedSpotPos_ = tel.liveBallPos;
            detectedKind_ = IncidentKind::None;
            std::snprintf(detectedLabel_, sizeof(detectedLabel_), "%s", "JOGO SEGUE");
        } else {
            hasDetectedContact_ = false;
            detectedOffenderSlot_ = -1;
            detectedOffenderUnknown_ = false;
            detectedVictimSlot_ = -1;
            detectedSpotPos_ = tel.liveBallPos;
            detectedKind_ = IncidentKind::None;
            std::snprintf(detectedLabel_, sizeof(detectedLabel_), "%s", "JOGO SEGUE");
        }
    }

    if (justEnteredReplay) {
        selectedMarkerIndex_ = -1;
        manualMarkerLock_ = false;
        radarZoomOverride_ = -1;
        if (config_.autoOpenPanel) {
            focused_ = true;
            page_ = HudPage::Main;
            returnPage_ = HudPage::Main;
            selectedIndex_ = 0;
        }
    }

    // Back on the main page after acting on the stopped-for foul: return to the play the user had chosen
    if (restoreMarkerAfterLive_ && page_ != HudPage::Configure) {
        restoreMarkerAfterLive_ = false;
        if (manualMarkerLock_ && selectedMarkerIndex_ >= 0 &&
            selectedMarkerIndex_ < static_cast<int>(replayMarkers_.size())) {
            ApplySelectedMarkerToContext(replayMarkers_[selectedMarkerIndex_]);
        }
        RebuildMainOptions();
    }

    if (!focused_ || mainOptions_.empty() || (!manualMarkerLock_ && page_ == HudPage::Main)) {
        RebuildMainOptions();
    }
}

void VarHudController::UpdateReplayMarkers(const std::vector<ReplayMarker>& markers) {
    replayMarkers_ = markers;
    if (markers.empty()) {
        selectedMarkerIndex_ = -1;
        return;
    }

    if (manualMarkerLock_ && selectedMarkerIndex_ >= 0 && selectedMarkerIndex_ < static_cast<int>(markers.size())) {
        return;
    }

    // Automatically track the incident marker closest to the live Replay cursor!
    int bestIdx = -1;
    float bestDt = 150.0f; // within 2.5s of cursor
    for (int i = 0; i < static_cast<int>(markers.size()); ++i) {
        if ((markers[i].matchPhase / 2) != (tel_.replayCursorPhase / 2)) continue;
        const float dt = std::fabs(markers[i].time60Hz - tel_.replayCursorTime);
        if (dt < bestDt) {
            bestDt = dt;
            bestIdx = i;
        }
    }

    // If no marker within 2.5s of cursor, pick the closest one overall so user always sees the key play
    if (bestIdx < 0) {
        float fallbackDt = 1e9f;
        for (int i = 0; i < static_cast<int>(markers.size()); ++i) {
            const float dt = std::fabs(markers[i].time60Hz - tel_.replayCursorTime);
            if (dt < fallbackDt) {
                fallbackDt = dt;
                bestIdx = i;
            }
        }
    }

    selectedMarkerIndex_ = bestIdx;
    if (bestIdx >= 0 && page_ != HudPage::Configure) {
        // Snap detected offender/victim/label to the active timeline marker if within 1.8s of cursor
        if (bestDt <= 108.0f) {
            ApplySelectedMarkerToContext(markers[bestIdx]);
        }
    }
}

void VarHudController::SelectReplayMarker(int markerIndex, VarState& state, bool triggerSeek) {
    if (markerIndex < 0 || markerIndex >= static_cast<int>(replayMarkers_.size())) return;
    selectedMarkerIndex_ = markerIndex;
    manualMarkerLock_ = true;
    seekLockUntilMs_ = GetTickCount64() + 500ULL;

    const auto& m = replayMarkers_[markerIndex];
    ApplySelectedMarkerToContext(m);
    if (!WasPlayOpened(m)) {
        openedPhase_[openedNext_] = m.matchPhase;
        openedTime_[openedNext_] = m.time60Hz;
        openedNext_ = (openedNext_ + 1) % kOpenedPlays;
        if (openedCount_ < kOpenedPlays) ++openedCount_;
    }

    if (triggerSeek) {
        state.RequestReplaySeekToTime(m.matchPhase, m.time60Hz, m.timelineNorm, true);
    }
}

void VarHudController::StepReplayMarker(int delta, VarState& state) {
    if (replayMarkers_.empty()) return;
    const int n = static_cast<int>(replayMarkers_.size());
    int next = selectedMarkerIndex_;
    if (next < 0 || next >= n) {
        next = (delta >= 0) ? 0 : (n - 1);
    } else {
        next = (next + delta + n) % n;
    }
    SelectReplayMarker(next, state, true);
}

void VarHudController::ScrubReplayToNorm(float norm, VarState& state) {
    manualMarkerLock_ = false;
    seekLockUntilMs_ = 0;
    state.RequestReplaySeekToNorm(std::clamp(norm, 0.0f, 1.0f), true);
}

void VarHudController::ToggleReplayPlayPause(VarState& state) {
    const bool wantPlay = (tel_.replayIsPlaying == 0);
    if (wantPlay) {
        manualMarkerLock_ = false;
    }
    state.RequestReplaySetPlaying(wantPlay);
}

void VarHudController::CycleCameraFocus(VarState& state) {
    const int32_t offTarget = IsValidSlot(detectedOffenderSlot_) ? (detectedOffenderSlot_ + 1) : 0;
    const int32_t vicTarget = IsValidSlot(detectedVictimSlot_)   ? (detectedVictimSlot_ + 1)   : 0;

    int32_t nextFocus = 0;
    if (tel_.replayFocusSlot != offTarget && offTarget > 0) {
        nextFocus = offTarget;
    } else if (tel_.replayFocusSlot != vicTarget && vicTarget > 0) {
        nextFocus = vicTarget;
    } else {
        nextFocus = 0; // Ball
    }
    state.RequestReplayCameraFocus(nextFocus);
}

void VarHudController::FormatNextCameraFocusLabelBuf(char* outBuf, size_t outSize) const {
    if (!outBuf || outSize == 0) return;
    const int32_t offTarget = IsValidSlot(detectedOffenderSlot_) ? (detectedOffenderSlot_ + 1) : 0;
    const int32_t vicTarget = IsValidSlot(detectedVictimSlot_)   ? (detectedVictimSlot_ + 1)   : 0;
    if (tel_.replayFocusSlot != offTarget && offTarget > 0) {
        const uint8_t num = players_[detectedOffenderSlot_].shirtNumber;
        if (num > 0) std::snprintf(outBuf, outSize, Tr("Câmera: #%u"), static_cast<unsigned>(num));
        else std::snprintf(outBuf, outSize, "%s", Tr("Câmera: infrator"));
        return;
    }
    if (tel_.replayFocusSlot != vicTarget && vicTarget > 0) {
        const uint8_t num = players_[detectedVictimSlot_].shirtNumber;
        if (num > 0) std::snprintf(outBuf, outSize, Tr("Câmera: #%u"), static_cast<unsigned>(num));
        else std::snprintf(outBuf, outSize, "%s", Tr("Câmera: vítima"));
        return;
    }
    std::snprintf(outBuf, outSize, "%s", Tr("Câmera: bola"));
}

std::string VarHudController::FormatNextCameraFocusLabel() const {
    char buf[32];
    FormatNextCameraFocusLabelBuf(buf, sizeof(buf));
    return std::string(buf);
}

void VarHudController::InvertDetectedRoles() {
    if (!IsValidSlot(detectedOffenderSlot_) && !IsValidSlot(detectedVictimSlot_)) return;
    // (an unknown opponent after a first invert has only the victim set)
    if (IsValidSlot(detectedOffenderSlot_) && IsValidSlot(detectedVictimSlot_)) {
        std::swap(detectedOffenderSlot_, detectedVictimSlot_);
    } else if (IsValidSlot(detectedOffenderSlot_)) {
        detectedVictimSlot_ = detectedOffenderSlot_;
        detectedOffenderSlot_ = -1;
        detectedOffenderUnknown_ = true; // the opponent is unknown: the decision page asks for him
    } else if (IsValidSlot(detectedVictimSlot_)) {
        // Second invert of an unknown opponent: back to the original roles
        detectedOffenderSlot_ = detectedVictimSlot_;
        detectedVictimSlot_ = -1;
        detectedOffenderUnknown_ = false;
    }
    manualMarkerLock_ = true;
    seekLockUntilMs_ = GetTickCount64() + 500ULL;
    if (page_ == HudPage::Configure) {
        if (IsValidSlot(draftVictimSlot_)) {
            std::swap(draftOffenderSlot_, draftVictimSlot_);
        } else if (IsValidSlot(draftOffenderSlot_)) {
            draftVictimSlot_ = draftOffenderSlot_;
            draftOffenderSlot_ = -1; // the user picks the offender
        }
        if (draftAction_ != VarAction::OverturnCancel && !IsManualRestartAction(draftAction_)) {
            draftBenefitedTeam_ = IsValidSlot(draftOffenderSlot_) ? 1 - SlotToTeam(draftOffenderSlot_)
                                : IsValidSlot(draftVictimSlot_) ? SlotToTeam(draftVictimSlot_) : draftBenefitedTeam_;
        }
    }
    RebuildMainOptions();
}

bool VarHudController::GetLockedMarkerTime(int32_t& outPhase, float& outTime) const {
    if (!manualMarkerLock_) return false;
    if (selectedMarkerIndex_ < 0 || selectedMarkerIndex_ >= static_cast<int>(replayMarkers_.size())) {
        return false;
    }
    const auto& m = replayMarkers_[selectedMarkerIndex_];
    outPhase = m.matchPhase;
    outTime  = m.time60Hz;
    return true;
}

void VarHudController::ToggleRadarZoom() {
    const bool currentlyZoomed = IsEffectiveRadarZoomed();
    radarZoomOverride_ = currentlyZoomed ? 0 : 1;
}

bool VarHudController::IsEffectiveRadarZoomed() const {
    const Vec3 spot = EffectivePreviewSpot();
    const PitchRadarViewport vp = PitchRadarViewport::Compute(
        tel_.pitchDims, spot, config_.autoZoomBox, radarZoomOverride_, 1.65f);
    return vp.isZoomed;
}

void VarHudController::RebuildMainOptions() {
    mainOptions_.clear();

    const VarEventType ctxType = hasActiveEvent_ ? activeEvent_.type : VarEventType::NoFoul;
    const bool inBoxNow = (ActiveSignedBoxDistance() < 0.0f);

    if (ctxType == VarEventType::NoFoul) {
        if (inBoxNow) {
            mainOptions_.push_back({ "Marcar pênalti", "Infração dentro da área", VarAction::RetroactivePenalty, CardKind::None, false, false, false });
            mainOptions_.push_back({ "Marcar falta", "Tiro livre fora da área", VarAction::RetroactiveFreeKick, CardKind::None, false, false, false });
        } else {
            mainOptions_.push_back({ "Marcar falta", "Infrator e local já preenchidos", VarAction::RetroactiveFreeKick, CardKind::None, false, false, false });
            mainOptions_.push_back({ "Marcar pênalti", "Cobrança da marca penal", VarAction::RetroactivePenalty, CardKind::None, false, false, false });
        }
        mainOptions_.push_back({ "Aplicar cartão", "Qualquer jogador, com ou sem falta", VarAction::EditCard, CardKind::Yellow, false, true, false });
        mainOptions_.push_back({ "Bola parada", "Escanteio, tiro de meta ou lateral", VarAction::RestartCorner, CardKind::None, false, false, false });
    } else if (ctxType == VarEventType::Foul) {
        mainOptions_.push_back({ "Anular falta", "Remove o cartão e devolve a posse", VarAction::OverturnCancel, CardKind::None, false, false, false });
        mainOptions_.push_back({ "Converter em pênalti", "Leva a cobrança para a marca penal", VarAction::ConvertToPenalty, activeEvent_.cardKind, false, false, false });
        mainOptions_.push_back({ "Alterar cartão", "Sem cartão, amarelo ou vermelho", VarAction::EditCard, activeEvent_.cardKind, false, false, false });
        mainOptions_.push_back({ "Inverter falta", "A falta passa para o outro time", VarAction::ConvertToFreeKick, CardKind::None, false, false, true });
    } else if (ctxType == VarEventType::Penalty) {
        mainOptions_.push_back({ "Anular pênalti", "Remove o cartão e reinicia com tiro de meta", VarAction::OverturnCancel, CardKind::None, false, false, false });
        mainOptions_.push_back({ "Falta fora da área", "Recua a cobrança para fora da área", VarAction::ConvertToFreeKick, activeEvent_.cardKind, false, false, false });
        mainOptions_.push_back({ "Alterar cartão", "Sem cartão, amarelo ou vermelho", VarAction::EditCard, activeEvent_.cardKind, false, false, false });
        mainOptions_.push_back({ "Manter decisão", "Mantém a decisão do árbitro neste lance", VarAction::None, CardKind::None, true, false, false });
    } else {
        mainOptions_.push_back({ "Alterar cartão", "Sem cartão, amarelo ou vermelho", VarAction::EditCard, activeEvent_.cardKind, false, false, false });
        mainOptions_.push_back({ "Marcar falta", "Falta no lance selecionado", VarAction::RetroactiveFreeKick, activeEvent_.cardKind, false, false, false });
        mainOptions_.push_back({ "Marcar pênalti", "Pênalti no lance selecionado", VarAction::RetroactivePenalty, activeEvent_.cardKind, false, false, false });
        mainOptions_.push_back({ "Anular marcação", "Retira o cartão e devolve a posse", VarAction::OverturnCancel, CardKind::None, false, false, false });
    }

    // The foul / penalty the match is stopped for can always be overturned, even while the cursor is on an
    // earlier play (review an earlier incident, mark it, and cancel the current call in the same pool)
    if (hasLiveFoul_ && !IsActiveEventTheLiveFoul()) {
        const bool livePen = (liveFoulEvent_.type == VarEventType::Penalty);
        HudMenuOption o{ livePen ? "Anular pênalti marcado" : "Anular falta marcada",
                         livePen ? "Cancela o pênalti da parada atual" : "Cancela a falta da parada atual",
                         VarAction::OverturnCancel, CardKind::None, false, false, false };
        o.useLiveFoul = true;
        mainOptions_.push_back(o);
    }

    if (page_ == HudPage::Main && selectedIndex_ >= static_cast<int>(mainOptions_.size())) {
        selectedIndex_ = static_cast<int>(mainOptions_.size()) - 1;
    }
    if (selectedIndex_ < 0) selectedIndex_ = 0;
}

bool VarHudController::IsActiveEventTheLiveFoul() const {
    if (!hasLiveFoul_ || !hasActiveEvent_) return false;
    if (activeEvent_.type != liveFoulEvent_.type || activeEvent_.offenderSlot != liveFoulEvent_.offenderSlot) return false;
    // Same recorded event, or (when one side has no recorded id yet) the same stoppage in time
    if (activeEvent_.id != 0 && liveFoulEvent_.id != 0) return activeEvent_.id == liveFoulEvent_.id;
    return IsSameStoppage(activeEvent_, liveFoulEvent_);
}

void VarHudController::RebuildConfigFields() {
    configFields_.clear();

    if (draftAction_ == VarAction::OverturnCancel) {
        configFields_.push_back(ConfigField::Team);
    } else if (IsManualRestartAction(draftAction_)) {
        configFields_.push_back(ConfigField::RestartType);
        configFields_.push_back(ConfigField::Team);
        configFields_.push_back(ConfigField::Spot);
    } else if (draftAction_ == VarAction::EditCard) {
        configFields_.push_back(ConfigField::Card);
        configFields_.push_back(ConfigField::Offender);
    } else if (draftAction_ == VarAction::RetroactivePenalty || draftAction_ == VarAction::ConvertToPenalty) {
        configFields_.push_back(ConfigField::Card);
        configFields_.push_back(ConfigField::Team);
        configFields_.push_back(ConfigField::Offender);
    } else {
        configFields_.push_back(ConfigField::Card);
        configFields_.push_back(ConfigField::Team);
        configFields_.push_back(ConfigField::Offender);
        configFields_.push_back(ConfigField::Spot);
    }

    configFields_.push_back(ConfigField::Confirm);
    configFields_.push_back(ConfigField::Back);
}

void VarHudController::SyncDirectPresetFromPlayer(int32_t slot) {
    if (!IsValidSlot(slot)) {
        draftDirectCardPreset_ = 0;
        draftCard_ = CardKind::None;
        return;
    }
    const auto& p = players_[slot];
    if (p.reds > 0 && p.yellows >= 2) {
        draftDirectCardPreset_ = 2;
        draftCard_ = CardKind::Red;
    } else if (p.reds > 0) {
        draftDirectCardPreset_ = 3;
        draftCard_ = CardKind::Red;
    } else if (p.yellows > 0) {
        draftDirectCardPreset_ = 1;
        draftCard_ = CardKind::Yellow;
    } else {
        draftDirectCardPreset_ = 0;
        draftCard_ = CardKind::None;
    }
}

void VarHudController::SetDraftCustomSpot(const Vec3& worldPos) {
    if (page_ == HudPage::Configure &&
        std::find(configFields_.begin(), configFields_.end(), ConfigField::Spot) == configFields_.end()) {
        return; // cancel / card decisions have no spot: a click on the radar must not change the restart
    }
    const Vec3 clamped = PitchGeometry::ClampToPitch(tel_.pitchDims, worldPos, 0.5f);
    draftSpotPos_ = clamped;
    draftSpotPreset_ = 3;
    draftCustomSpot_ = true;
    detectedSpotPos_ = clamped;
    manualMarkerLock_ = true;
    seekLockUntilMs_ = GetTickCount64() + 500ULL;
    if (page_ == HudPage::Main) {
        RebuildMainOptions();
    }
}

Vec3 VarHudController::EffectivePreviewSpot() const {
    if (page_ == HudPage::Configure) {
        if (draftAction_ == VarAction::ConvertToPenalty || draftAction_ == VarAction::RetroactivePenalty) {
            const int32_t offTeam = IsValidSlot(draftOffenderSlot_) ? SlotToTeam(draftOffenderSlot_) : (1 - draftBenefitedTeam_);
            return PitchGeometry::CalcPenaltySpot(tel_.pitchDims, tel_.teamAttackDir[offTeam == 1 ? 1 : 0]);
        }
        return draftSpotPos_;
    }
    if (IsNonZeroVec(detectedSpotPos_)) return detectedSpotPos_;
    if (hasActiveEvent_ && IsNonZeroVec(activeEvent_.pos)) return activeEvent_.pos;
    if (sync_.hasSnapshot && IsNonZeroVec(sync_.snapshot.ballPos)) return sync_.snapshot.ballPos;
    return tel_.liveBallPos;
}

float VarHudController::ActiveSignedBoxDistance() const {
    const Vec3 spot = EffectivePreviewSpot();
    int32_t offTeam = 0;
    if (page_ == HudPage::Configure) {
        offTeam = IsValidSlot(draftOffenderSlot_) ? SlotToTeam(draftOffenderSlot_) : (1 - draftBenefitedTeam_);
    } else if (IsValidSlot(detectedOffenderSlot_)) {
        offTeam = SlotToTeam(detectedOffenderSlot_);
    } else if (detectedOffenderUnknown_ && IsValidSlot(detectedVictimSlot_)) {
        offTeam = 1 - SlotToTeam(detectedVictimSlot_); // roles inverted: the offender is the victim's opponent
    } else if (hasActiveEvent_) {
        offTeam = 1 - activeEvent_.benefitedTeam;
    } else {
        const float d0 = PitchGeometry::SignedDistanceToDefendingBox(tel_.pitchDims, spot, tel_.teamAttackDir[0]);
        const float d1 = PitchGeometry::SignedDistanceToDefendingBox(tel_.pitchDims, spot, tel_.teamAttackDir[1]);
        return (std::fabs(d0) <= std::fabs(d1)) ? d0 : d1;
    }
    return PitchGeometry::SignedDistanceToDefendingBox(tel_.pitchDims, spot, tel_.teamAttackDir[offTeam == 1 ? 1 : 0]);
}

void VarHudController::ApplySpotPreset(int preset) {
    draftSpotPreset_ = preset;
    draftCustomSpot_ = (preset != 0);

    const int32_t offTeam = IsValidSlot(draftOffenderSlot_) ? SlotToTeam(draftOffenderSlot_) : (1 - draftBenefitedTeam_);
    const float offDir = (offTeam == 0 || offTeam == 1) ? tel_.teamAttackDir[offTeam] : -1.0f;
    const float dirSign = (offDir >= 0.0f) ? 1.0f : -1.0f;

    if (preset == 0) {
        if (IsNonZeroVec(detectedSpotPos_)) {
            draftSpotPos_ = detectedSpotPos_;
        } else if (hasActiveEvent_ && IsNonZeroVec(activeEvent_.pos)) {
            draftSpotPos_ = activeEvent_.pos;
        } else if (sync_.hasSnapshot && IsValidSlot(draftOffenderSlot_) && IsNonZeroVec(sync_.snapshot.playerPos[draftOffenderSlot_])) {
            draftSpotPos_ = sync_.snapshot.playerPos[draftOffenderSlot_];
        } else if (sync_.hasSnapshot && IsNonZeroVec(sync_.snapshot.ballPos)) {
            draftSpotPos_ = sync_.snapshot.ballPos;
        } else if (IsNonZeroVec(tel_.liveBallPos)) {
            draftSpotPos_ = tel_.liveBallPos;
        } else {
            draftSpotPos_ = Vec3{};
        }
    } else if (preset == 1) {
        draftSpotPos_ = Vec3{ dirSign * (tel_.pitchDims.box18X - 1.2f), 0.0f, 0.0f };
    } else {
        draftSpotPos_ = Vec3{ 0.0f, 0.0f, 0.0f };
    }
}

void VarHudController::PreFillDraft(VarAction action, CardKind presetCard, bool invertRoles) {
    draftAction_ = action;
    draftCard_ = presetCard;
    draftOrigCard_ = hasActiveEvent_ ? activeEvent_.cardKind : CardKind::None;
    draftOrigOffenderSlot_ = (hasActiveEvent_ && IsValidSlot(activeEvent_.offenderSlot))
        ? activeEvent_.offenderSlot
        : -1;
    draftSourceEventId_ = hasActiveEvent_ ? activeEvent_.id : 0;
    draftEventOffSlot_   = hasActiveEvent_ ? activeEvent_.offenderSlot : -1;
    draftEventVicSlot_   = hasActiveEvent_ ? activeEvent_.victimSlot : -1;
    draftEventOffRoster_ = hasActiveEvent_ ? activeEvent_.offenderRosterIdx : -1;
    draftEventVicRoster_ = hasActiveEvent_ ? activeEvent_.victimRosterIdx : -1;
    draftDirectCardMode_ = (action == VarAction::EditCard && !hasActiveEvent_);
    draftSpotPreset_ = 0;
    draftCustomSpot_ = false;

    int32_t offSlot = -1;
    int32_t vicSlot = -1;

    if (detectedOffenderUnknown_ && IsValidSlot(detectedVictimSlot_)) {
        offSlot = -1;
        vicSlot = detectedVictimSlot_;
    } else if (IsValidSlot(detectedOffenderSlot_)) {
        offSlot = detectedOffenderSlot_;
        vicSlot = IsValidSlot(detectedVictimSlot_) ? detectedVictimSlot_ : -1;
    } else if (hasActiveEvent_ && IsValidSlot(activeEvent_.offenderSlot)) {
        offSlot = activeEvent_.offenderSlot;
        vicSlot = IsValidSlot(activeEvent_.victimSlot) ? activeEvent_.victimSlot : -1;
    } else if (tel_.replayFocusSlot >= 1 && tel_.replayFocusSlot <= 22) {
        offSlot = tel_.replayFocusSlot - 1;
        if (sync_.hasSnapshot) {
            if (sync_.snapshot.closestA == offSlot) vicSlot = sync_.snapshot.closestB;
            else if (sync_.snapshot.closestB == offSlot) vicSlot = sync_.snapshot.closestA;
        }
    } else if (sync_.hasSnapshot && IsValidSlot(sync_.snapshot.closestA) && IsValidSlot(sync_.snapshot.closestB)) {
        if (sync_.snapshot.ballOwnerSlot == sync_.snapshot.closestA ||
            sync_.snapshot.recentBallOwnerSlot == sync_.snapshot.closestA) {
            vicSlot = sync_.snapshot.closestA;
            offSlot = sync_.snapshot.closestB;
        } else {
            vicSlot = sync_.snapshot.closestB;
            offSlot = sync_.snapshot.closestA;
        }
    }

    // No placeholder offender (a goalkeeper would be carded): when nobody was identified the decision page
    // shows "Jogador" and the user must pick him before confirming. No placeholder victim either: a filler slot would be written to FoulInfo and the history as the fouled player
    if (!IsValidSlot(vicSlot)) vicSlot = -1;

    if (invertRoles) {
        if (IsValidSlot(vicSlot)) {
            std::swap(offSlot, vicSlot);
        } else if (IsValidSlot(offSlot)) {
            vicSlot = offSlot;
            offSlot = -1; // unknown player of the other team: the user picks him
        }
        draftCard_ = CardKind::None;
    }

    draftOffenderSlot_ = offSlot;
    draftVictimSlot_ = vicSlot;

    if (draftDirectCardMode_) {
        SyncDirectPresetFromPlayer(draftOffenderSlot_);
    }

    if (action == VarAction::OverturnCancel || IsManualRestartAction(action)) {
        if (hasActiveEvent_) {
            draftBenefitedTeam_ = 1 - activeEvent_.benefitedTeam;
        } else {
            draftBenefitedTeam_ = IsValidSlot(offSlot) ? SlotToTeam(offSlot) : 0;
        }
        draftCard_ = CardKind::None;
    } else {
        // The side that gets the kick is always the opponent of the FINAL offender. Deriving it from the
        // recorded event broke after Tab (roles swapped) or "invert": offender and beneficiary on one side.
        draftBenefitedTeam_ = IsValidSlot(offSlot) ? 1 - SlotToTeam(offSlot)
                            : IsValidSlot(vicSlot) ? SlotToTeam(vicSlot)
                            : hasActiveEvent_ ? activeEvent_.benefitedTeam : 0;
    }

    ApplySpotPreset(0);
    RebuildConfigFields();
}

void VarHudController::PreFillDirectCardForSlot(int32_t slot) {
    draftAction_ = VarAction::EditCard;
    draftOrigCard_ = CardKind::None;
    draftOrigOffenderSlot_ = -1;
    draftEventOffSlot_ = draftEventVicSlot_ = -1;
    draftEventOffRoster_ = draftEventVicRoster_ = -1;
    draftFromLiveFoul_ = false;
    draftSourceEventId_ = 0;
    draftDirectCardMode_ = true;
    draftOffenderSlot_ = std::clamp(slot, 0, 21);
    draftVictimSlot_ = (draftOffenderSlot_ < 11) ? 11 : 0;
    draftBenefitedTeam_ = 1 - SlotToTeam(draftOffenderSlot_);
    draftSpotPreset_ = 0;
    draftCustomSpot_ = false;
    SyncDirectPresetFromPlayer(draftOffenderSlot_);
    // A pending direct edit for this player wins over the live cards, so reopening the player shows
    // (and re-confirms) what will actually be applied.
    {
        const VerdictPool pool = VarState::Instance().PeekVerdictPool();
        VarVerdict probe{};
        probe.action = VarAction::EditCard;
        probe.directCardEdit = true;
        probe.offenderSlot = draftOffenderSlot_;
        probe.offenderRosterIdx = players_[static_cast<size_t>(draftOffenderSlot_)].rosterIdx;
        if (const CardPoolEntry* pend = pool.FindSameKey(probe)) {
            const uint8_t y = pend->verdict.directYellows;
            const uint8_t r = pend->verdict.directReds;
            draftDirectCardPreset_ = (r > 0 && y >= 2) ? 2 : (r > 0) ? 3 : (y > 0) ? 1 : 0;
            draftCard_ = (r > 0) ? CardKind::Red : (y > 0) ? CardKind::Yellow : CardKind::None;
        }
    }
    ApplySpotPreset(0);
    RebuildConfigFields();
}

void VarHudController::SetDraftCardDirect(CardKind card) {
    draftCard_ = card;
}

void VarHudController::SetDraftDirectCardPreset(int preset) {
    draftDirectCardPreset_ = std::clamp(preset, 0, 3);
    draftCard_ = (draftDirectCardPreset_ == 0) ? CardKind::None
               : (draftDirectCardPreset_ == 1) ? CardKind::Yellow
               : CardKind::Red;
}

void VarHudController::SetDraftRestartAction(VarAction restartAction) {
    if (IsManualRestartAction(restartAction)) {
        draftAction_ = restartAction;
        draftCard_ = CardKind::None;
    }
}

void VarHudController::SetDraftBenefitedTeam(int32_t team) {
    draftBenefitedTeam_ = (team == 1) ? 1 : 0;
}

void VarHudController::CollapseToReplay() {
    focused_ = false;
    page_ = HudPage::Main;
    returnPage_ = HudPage::Main;
    selectedIndex_ = 0;
}

void VarHudController::SetSelectedIndex(int idx) {
    int maxCount = 0;
    if (page_ == HudPage::Main) {
        maxCount = static_cast<int>(mainOptions_.size());
    } else if (page_ == HudPage::Squad) {
        maxCount = 13;
    } else if (page_ == HudPage::Config) {
        maxCount = kSettingsRowCount;
    } else if (page_ == HudPage::Controls) {
        maxCount = kControlsRowCount;
    } else if (page_ == HudPage::Configure) {
        maxCount = static_cast<int>(configFields_.size());
    }
    if (maxCount > 0 && idx >= 0 && idx < maxCount) {
        selectedIndex_ = idx;
    }
}

void VarHudController::AdjustField(ConfigField field, int delta, VarState& state) {
    switch (field) {
        case ConfigField::Card: {
            if (draftDirectCardMode_) {
                SetDraftDirectCardPreset((draftDirectCardPreset_ + delta + 4) % 4);
            } else {
                static constexpr CardKind kCycle[3] = { CardKind::None, CardKind::Yellow, CardKind::Red };
                int idx = (draftCard_ == CardKind::Yellow) ? 1 : (draftCard_ == CardKind::Red) ? 2 : 0;
                idx = (idx + delta + 3) % 3;
                draftCard_ = kCycle[idx];
            }
            break;
        }
        case ConfigField::RestartType: {
            static constexpr VarAction kRestarts[3] = {
                VarAction::RestartCorner, VarAction::RestartGoalKick, VarAction::RestartThrowIn
            };
            int idx = (draftAction_ == VarAction::RestartGoalKick) ? 1
                    : (draftAction_ == VarAction::RestartThrowIn)  ? 2 : 0;
            idx = (idx + delta + 3) % 3;
            draftAction_ = kRestarts[idx];
            draftCard_ = CardKind::None;
            break;
        }
        case ConfigField::Team: {
            draftBenefitedTeam_ = (draftBenefitedTeam_ == 0) ? 1 : 0;
            if (draftAction_ == VarAction::RetroactiveFreeKick ||
                draftAction_ == VarAction::RetroactivePenalty ||
                draftAction_ == VarAction::ConvertToFreeKick ||
                draftAction_ == VarAction::ConvertToPenalty) {
                const int32_t desiredOffTeam = 1 - draftBenefitedTeam_;
                if (IsValidSlot(draftOffenderSlot_) && SlotToTeam(draftOffenderSlot_) != desiredOffTeam) {
                    draftOffenderSlot_ = (desiredOffTeam == 0) ? (draftOffenderSlot_ % 11) : (11 + (draftOffenderSlot_ % 11));
                    if (IsValidSlot(draftVictimSlot_) && SlotToTeam(draftVictimSlot_) == desiredOffTeam) {
                        draftVictimSlot_ = -1; // the victim cannot be on the offender's team
                    }
                }
            }
            break;
        }
        case ConfigField::Offender: {
            if (!IsValidSlot(draftOffenderSlot_)) {
                const int32_t offTeam = IsValidSlot(draftVictimSlot_) ? 1 - SlotToTeam(draftVictimSlot_)
                                                                      : 1 - draftBenefitedTeam_;
                const int32_t base = (offTeam == 1) ? 11 : 0;
                draftOffenderSlot_ = (delta >= 0) ? base : base + 10;
            } else {
                draftOffenderSlot_ = (draftOffenderSlot_ + delta + 22) % 22;
            }
            if (draftDirectCardMode_) {
                SyncDirectPresetFromPlayer(draftOffenderSlot_);
            }
            if (draftAction_ != VarAction::OverturnCancel && !IsManualRestartAction(draftAction_)) {
                draftBenefitedTeam_ = 1 - SlotToTeam(draftOffenderSlot_);
                if (IsValidSlot(draftVictimSlot_) && SlotToTeam(draftVictimSlot_) == SlotToTeam(draftOffenderSlot_)) {
                    draftVictimSlot_ = -1;
                }
            }
            if (draftSpotPreset_ == 0) {
                ApplySpotPreset(0);
            }
            break;
        }
        case ConfigField::Spot: {
            if (draftAction_ == VarAction::RestartCorner || draftAction_ == VarAction::RestartGoalKick) {
                draftSpotPos_.z = (draftSpotPos_.z >= 0.0f) ? -12.0f : 12.0f;
                draftCustomSpot_ = true;
            } else {
                const int nextPreset = (draftSpotPreset_ + delta + 3) % 3;
                ApplySpotPreset(nextPreset);
            }
            break;
        }
        case ConfigField::Confirm: {
            ArmCurrentDraft(state);
            break;
        }
        case ConfigField::Back: {
            page_ = returnPage_;
            selectedIndex_ = 0;
            break;
        }
    }
}

void VarHudController::TriggerConfigField(ConfigField field, int delta, VarState& state) {
    AdjustField(field, delta, state);
}

void VarHudController::TriggerSettingsRow(int row, int delta, VarState& state) {
    if (row == kSettingsRowBack) {
        page_ = HudPage::Main;
        selectedIndex_ = 0;
        return;
    }
    if (row == kSettingsRowControls) {
        OpenControlsPage();
        return;
    }
    HudConfig cfg = config_;
    if (row == 0) {
        cfg.autoOpenPanel = !cfg.autoOpenPanel;
    } else if (row == 1) {
        cfg.showRadar = !cfg.showRadar;
    } else if (row == 2) {
        cfg.autoZoomBox = !cfg.autoZoomBox;
        radarZoomOverride_ = -1;
    } else if (row == 3) {
        cfg.dockLeft = !cfg.dockLeft;
    } else if (row == 4) {
        cfg.scalePreset = (cfg.scalePreset + delta + 3) % 3;
    } else if (row == 5) {
        cfg.broadcastAlerts = !cfg.broadcastAlerts;
    } else if (row == kSettingsRowReplayAfterDecision) {
        cfg.replayAfterDecision = !cfg.replayAfterDecision;
    } else if (row == kSettingsRowLanguage) {
        cfg.language = (cfg.language == 1) ? 0 : 1; // applied instantly to the whole HUD
    }
    config_ = cfg;
    state.SetHudConfig(cfg, true);
}

// ---------------------------------------------------------------------------
// Controls (remap) page
// ---------------------------------------------------------------------------
const char* VarHudController::BindActionLabel(int action) {
    static constexpr const char* kLabels[kBindActionCount] = {
        "Abrir / recolher painel", "Confirmar", "Voltar", "Limpar pool", "Aba anterior",
        "Próxima aba", "Inverter / trocar time", "Lance anterior", "Próximo lance", "Reproduzir / pausar",
        "Cima", "Baixo", "Esquerda", "Direita", "Telemetria",
    };
    if (action < 0 || action >= kBindActionCount) return "";
    return Tr(kLabels[action]);
}

void VarHudController::SetControlsMessage(const char* ptMsg, bool isError) {
    controlsMsg_ = ptMsg;
    controlsMsgError_ = isError;
    controlsMsgUntilMs_ = GetTickCount64() + 2600ULL;
}

const char* VarHudController::ControlsMessage() const {
    if (!controlsMsg_ || GetTickCount64() >= controlsMsgUntilMs_) return nullptr;
    return Tr(controlsMsg_);
}

const char* VarHudController::PoolNoticeMessage() const {
    if (!poolNoticeMsg_ || GetTickCount64() >= poolNoticeUntilMs_) return nullptr;
    return Tr(poolNoticeMsg_);
}

const char* VarHudController::ConfirmToast(uint64_t nowMs, uint64_t& outAgeMs) const {
    outAgeMs = 0;
    if (!toastMsg_ || nowMs < toastStartMs_ || nowMs - toastStartMs_ >= kConfirmToastMs) return nullptr;
    outAgeMs = nowMs - toastStartMs_;
    return Tr(toastMsg_);
}

void VarHudController::OpenControlsPage() {
    CancelBindingCapture();
    page_ = HudPage::Controls;
    selectedIndex_ = 0;
    controlsColumn_ = 0;
}

void VarHudController::SetControlsColumn(int col) {
    controlsColumn_ = (col + 3) % 3;
}

void VarHudController::BeginBindingCapture(int row, int col, VarState& state) {
    (void)state;
    if (row < 0 || row >= kBindActionCount || col < 0 || col > 2) return;
    selectedIndex_ = row;
    controlsColumn_ = col;
    if (col == 2 && !InputBindings::IsPadRemappable(static_cast<BindAction>(row))) {
        SetControlsMessage("O controle só abre e recolhe o painel", true);
        return; // directions use the fixed D-Pad / stick, debug has no gamepad shortcut
    }
    capturing_ = true;
    captureRow_ = row;
    captureCol_ = col;
    controlsMsg_ = nullptr;
    InputManager::BeginCapture(col);
}

void VarHudController::CancelBindingCapture() {
    if (!capturing_) return;
    capturing_ = false;
    captureRow_ = -1;
    InputManager::CancelCapture();
}

void VarHudController::ApplyCapturedBinding(uint16_t code, VarState& state) {
    if (!capturing_ && captureRow_ < 0) return;
    const int row = captureRow_;
    const int col = captureCol_;
    capturing_ = false;
    captureRow_ = -1;
    InputManager::CancelCapture(); // no-op when PollCapture already finished it
    if (row < 0 || row >= kBindActionCount) return;

    HudConfig cfg = config_;
    const BindEditResult res = AssignBinding(cfg.bindings, static_cast<BindAction>(row), col, code);
    if (res == BindEditResult::Rejected) {
        SetControlsMessage("Esta ação precisa de pelo menos um atalho", true);
        return;
    }
    config_ = cfg;
    state.SetHudConfig(cfg, true);
    SetControlsMessage(res == BindEditResult::Moved ? "Atalho trocado com outra ação" : "Atalho salvo", false);
}

void VarHudController::ClearBindingCell(int row, int col, VarState& state) {
    if (row < 0 || row >= kBindActionCount) return;
    HudConfig cfg = config_;
    if (ClearBinding(cfg.bindings, static_cast<BindAction>(row), col) == BindEditResult::Rejected) {
        SetControlsMessage("Esta ação precisa de pelo menos um atalho", true);
        return;
    }
    config_ = cfg;
    state.SetHudConfig(cfg, true);
    SetControlsMessage("Atalho removido", false);
}

void VarHudController::RestoreDefaultBindings(VarState& state) {
    CancelBindingCapture();
    HudConfig cfg = config_;
    cfg.bindings = InputBindings::Defaults();
    config_ = cfg;
    state.SetHudConfig(cfg, true);
    SetControlsMessage("Atalhos padrão restaurados", false);
}

void VarHudController::CycleTopLevelTab(int dir) {
    static constexpr HudPage kTabs[3] = { HudPage::Main, HudPage::Squad, HudPage::Config };
    CancelBindingCapture();
    const HudPage cur = (page_ == HudPage::Controls) ? HudPage::Config : page_; // Controls lives in OPCOES
    int idx = 0;
    for (int i = 0; i < 3; ++i) {
        if (kTabs[i] == cur) { idx = i; break; }
    }
    idx = (idx + dir + 3) % 3;
    page_ = kTabs[idx];
    selectedIndex_ = 0;
}

void VarHudController::SelectTab(HudPage tab) {
    if (tab == HudPage::Main || tab == HudPage::Squad || tab == HudPage::Config) {
        CancelBindingCapture();
        focused_ = true;
        page_ = tab;
        selectedIndex_ = 0;
    }
}

void VarHudController::SelectMainOption(int index, VarState& state) {
    if (index < 0 || index >= static_cast<int>(mainOptions_.size())) return;
    selectedIndex_ = index;
    const auto& opt = mainOptions_[index];
    if (opt.keepDecision) {
        // Keep the referee's call on THIS event: drop only a pending play decision about this event.
        // Card edits and decisions about other events stay in the pool.
        if (hasActiveEvent_) {
            const VerdictPool pool = state.PeekVerdictPool();
            // Id 0 is shared by every retroactive decision and by the live FoulInfo context: match it only
            // when both refer to the stopped-for foul, never a decision about another play.
            const bool samePlay = pool.hasPlayDecision &&
                ((activeEvent_.id != 0 && pool.playDecision.sourceEventId == activeEvent_.id) ||
                 (activeEvent_.id == 0 && pool.playDecision.sourceEventId == 0 &&
                  pool.playDecision.fromLiveFoul && IsActiveEventTheLiveFoul()));
            if (samePlay) {
                state.RemovePlayFromPool();
            }
            state.RemoveCardEntriesForEvent(activeEvent_.id);
        }
        CollapseToReplay();
        return;
    }
    if (opt.openSquad) {
        page_ = HudPage::Squad;
        returnPage_ = HudPage::Main;
        selectedIndex_ = 1;
        return;
    }
    if (opt.useLiveFoul && hasLiveFoul_) {
        // Act on the stopped-for foul, not on the play under the replay cursor
        hasActiveEvent_ = true;
        activeEvent_ = liveFoulEvent_;
        hasDetectedContact_ = IsValidSlot(liveFoulEvent_.offenderSlot);
        detectedOffenderSlot_ = liveFoulEvent_.offenderSlot;
        detectedOffenderUnknown_ = false;
        detectedVictimSlot_ = IsValidSlot(liveFoulEvent_.victimSlot) ? liveFoulEvent_.victimSlot : -1;
        detectedSpotPos_ = liveFoulEvent_.pos;
        restoreMarkerAfterLive_ = true;
    }
    const bool targetsLive = (opt.action == VarAction::OverturnCancel) &&
                             ((opt.useLiveFoul && hasLiveFoul_) || IsActiveEventTheLiveFoul());
    PreFillDraft(opt.action, opt.presetCard, opt.invertRoles);
    if (!opt.invertRoles && draftSourceEventId_ != 0 &&
        (opt.action == VarAction::ConvertToPenalty || opt.action == VarAction::ConvertToFreeKick ||
         opt.action == VarAction::RetroactivePenalty || opt.action == VarAction::RetroactiveFreeKick)) {
        VarVerdict probe{};
        probe.action = VarAction::EditCard;
        probe.sourceEventId = draftSourceEventId_;
        const VerdictPool pool = state.PeekVerdictPool();
        const CardPoolEntry* pend = pool.FindSameKey(probe);
        if (pend && pend->verdict.action == VarAction::EditCard) {
            draftCard_ = pend->verdict.targetCard;
            if (IsValidSlot(pend->verdict.offenderSlot)) {
                draftOffenderSlot_ = pend->verdict.offenderSlot;
                draftBenefitedTeam_ = 1 - SlotToTeam(draftOffenderSlot_);
                if (draftSpotPreset_ == 0) ApplySpotPreset(0);
            }
            RebuildConfigFields();
        }
    }
    draftTargetsLive_ = targetsLive;
    draftFromLiveFoul_ = (opt.useLiveFoul && hasLiveFoul_) || IsActiveEventTheLiveFoul();
    returnPage_ = HudPage::Main;
    page_ = HudPage::Configure;
    selectedIndex_ = (opt.action == VarAction::OverturnCancel)
        ? static_cast<int>(configFields_.size()) - 2
        : 0;
}

void VarHudController::SelectSquadTeam(int teamIdx) {
    squadTeamFilter_ = (teamIdx == 1) ? 1 : 0;
}

void VarHudController::SelectSquadPlayer(int slot) {
    PreFillDirectCardForSlot(slot);
    returnPage_ = HudPage::Squad;
    page_ = HudPage::Configure;
    selectedIndex_ = 0;
}

void VarHudController::ArmCurrentDraft(VarState& state) {
    const bool needsOffender = draftAction_ == VarAction::ConvertToPenalty || draftAction_ == VarAction::ConvertToFreeKick ||
                               draftAction_ == VarAction::RetroactivePenalty || draftAction_ == VarAction::RetroactiveFreeKick ||
                               draftAction_ == VarAction::EditCard;
    if (needsOffender && !IsValidSlot(draftOffenderSlot_)) {
        poolNoticeMsg_ = "Escolha o jogador antes de confirmar";
        poolNoticeUntilMs_ = GetTickCount64() + 3600ULL;
        for (size_t i = 0; i < configFields_.size(); ++i) {
            if (configFields_[i] == ConfigField::Offender) { selectedIndex_ = static_cast<int>(i); break; }
        }
        return;
    }
    VarVerdict v{};
    v.action = draftAction_;
    v.sourceEventId = draftSourceEventId_;
    v.origOffenderSlot = draftOrigOffenderSlot_;
    v.offenderSlot = draftOffenderSlot_;
    v.victimSlot = draftVictimSlot_;
    v.benefitedTeam = draftBenefitedTeam_;
    v.origCard = draftOrigCard_;
    v.targetCard = draftCard_;
    v.directCardEdit = draftDirectCardMode_;
    if (draftDirectCardMode_) {
        v.directYellows = (draftDirectCardPreset_ == 1) ? 1 : (draftDirectCardPreset_ == 2) ? 2 : 0;
        v.directReds    = (draftDirectCardPreset_ >= 2) ? 1 : 0;
        // Straight red keeps a yellow the player already had (yellow + straight red is legal)
        if (draftDirectCardPreset_ == 3 && IsValidSlot(draftOffenderSlot_)) {
            // Base = the pending direct edit of this player if there is one, else the live cards
            uint8_t baseYellows = players_[draftOffenderSlot_].yellows;
            VarVerdict probe{};
            probe.action = VarAction::EditCard;
            probe.directCardEdit = true;
            probe.offenderSlot = draftOffenderSlot_;
            probe.offenderRosterIdx = players_[static_cast<size_t>(draftOffenderSlot_)].rosterIdx;
            if (const CardPoolEntry* pend = state.PeekVerdictPool().FindSameKey(probe)) {
                baseYellows = pend->verdict.directYellows;
            }
            v.directYellows = std::min<uint8_t>(baseYellows, 1);
        }
    }
    v.spotPos = draftSpotPos_;
    v.customSpot = draftCustomSpot_;
    v.targetsLiveSetPlay = (draftAction_ == VarAction::OverturnCancel) && draftTargetsLive_;
    v.fromLiveFoul = draftFromLiveFoul_ && !draftDirectCardMode_;
    v.liveSetPlayKind = v.targetsLiveSetPlay ? static_cast<int8_t>(tel_.activeSetPlayKind) : static_cast<int8_t>(-1);
    v.armStoppage = tel_.stoppageCount;
    // The reviewed moment (replayed after the decision) only exists when the decision was confirmed in the replay
    const bool inReplay = tel_.replayMode >= 1 && tel_.replayMode <= 3;
    v.playPhase = inReplay ? tel_.replayCursorPhase : -1;
    v.playTime60Hz = inReplay ? tel_.replayCursorTime : 0.0f;
    // A slot of the source event means the player who held it then. A slot the user picked means whoever
    // holds it now (the squad lists show the current players).
    auto currentRoster = [&](int32_t slot) -> int8_t {
        return IsValidSlot(slot) ? players_[static_cast<size_t>(slot)].rosterIdx : static_cast<int8_t>(-1);
    };
    if (!draftDirectCardMode_) {
        auto rosterOf = [&](int32_t slot) -> int8_t {
            if (!IsValidSlot(slot)) return -1;
            if (slot == draftEventOffSlot_ && draftEventOffRoster_ >= 0) return draftEventOffRoster_;
            if (slot == draftEventVicSlot_ && draftEventVicRoster_ >= 0) return draftEventVicRoster_;
            // A play under the replay cursor: the player who held the slot in that moment (a substitution
            // since then must not move the card to the substitute)
            if (sync_.hasSnapshot && sync_.snapshot.playerRosterIdx[slot] >= 0 &&
                draftAction_ != VarAction::EditCard) {
                return sync_.snapshot.playerRosterIdx[slot];
            }
            return currentRoster(slot);
        };
        v.origOffenderRosterIdx = rosterOf(v.origOffenderSlot);
        v.offenderRosterIdx = rosterOf(v.offenderSlot);
        v.victimRosterIdx = rosterOf(v.victimSlot);
    } else {
        v.offenderRosterIdx = currentRoster(v.offenderSlot);
    }

    const std::string sum = FormatVerdictSummary();
    std::snprintf(v.summary, sizeof(v.summary), "%s", sum.c_str());

    bool replacedPlay = false;
    const VerdictPool before = state.PeekVerdictPool();
    // Re-confirming the decision of the same play updates it (the button says "Atualizar decisão")
    const bool samePlayAgain = before.hasPlayDecision && v.action != VarAction::EditCard &&
        ((v.sourceEventId != 0 && before.playDecision.sourceEventId == v.sourceEventId) ||
         (v.sourceEventId == 0 && before.playDecision.sourceEventId == 0 &&
          before.playDecision.armStoppage == v.armStoppage &&
          before.playDecision.playPhase == v.playPhase &&
          std::fabs(before.playDecision.playTime60Hz - v.playTime60Hz) <= 180.0f));
    bool evictedCard = false;
    if (!state.AddToVerdictPoolEx(v, &replacedPlay, &evictedCard)) {
        poolNoticeMsg_ = "Decisão não enviada: VAR desligado";
        poolNoticeUntilMs_ = GetTickCount64() + 3600ULL;
        toastMsg_ = nullptr;
        return;
    }
    if (samePlayAgain) replacedPlay = false;
    const VerdictPool after = state.PeekVerdictPool();
    const bool droppedEventCard = (v.action != VarAction::EditCard) && (after.CardCount() < before.CardCount());
    const bool mergedIntoPlay = (v.action == VarAction::EditCard) && !v.directCardEdit && v.sourceEventId != 0 &&
                                after.hasPlayDecision && after.playDecision.sourceEventId == v.sourceEventId &&
                                after.FindSameKey(v) == nullptr;
    if (mergedIntoPlay) {
        const std::string merged = FormatSummaryOf(after.playDecision);
        state.SetPlayDecisionSummary(merged.c_str());
    }
    if (replacedPlay) {
        poolNoticeMsg_ = "Lance substituído no pool (máx. 1 falta ou reinício)";
        poolNoticeUntilMs_ = GetTickCount64() + 3600ULL;
    } else if (evictedCard) {
        poolNoticeMsg_ = "Pool cheio: a edição de cartão mais antiga foi removida";
        poolNoticeUntilMs_ = GetTickCount64() + 3600ULL;
    } else if (droppedEventCard) {
        poolNoticeMsg_ = "Cartão do lance incorporado à decisão";
        poolNoticeUntilMs_ = GetTickCount64() + 3600ULL;
    } else {
        poolNoticeMsg_ = nullptr;
    }
    // Confirmation toast: what happened to the decision, in the user's words
    if (after.Count() > 0) {
        toastMsg_ = mergedIntoPlay ? "Cartão incorporado à decisão do lance"
                  : replacedPlay   ? "Decisão substituída no pool de decisões"
                  : (samePlayAgain || (after.Count() == before.Count() && !droppedEventCard && !evictedCard))
                        ? "Decisão atualizada no pool de decisões"
                  : "Decisão enviada ao pool de decisões";
        toastStartMs_ = GetTickCount64();
    }
    VAR_LOG_INF("HUD added verdict to pool (replacedPlay=%d): %s", replacedPlay ? 1 : 0, v.summary);

    if (returnPage_ == HudPage::Squad && IsValidSlot(draftOffenderSlot_)) {
        page_ = HudPage::Squad;
        squadTeamFilter_ = SlotToTeam(draftOffenderSlot_);
        selectedIndex_ = (draftOffenderSlot_ % 11) + 1;
    } else {
        page_ = HudPage::Main;
        returnPage_ = HudPage::Main;
        selectedIndex_ = 0;
    }
}

bool VarHudController::IsPostState13RedRescindWarning() const {
    if (tel_.fsmState != 0x1A) return false;
    if (draftDirectCardMode_) {
        if (!IsValidSlot(draftOffenderSlot_)) return false;
        const auto& p = players_[draftOffenderSlot_];
        const bool wasExpelled = (p.reds > 0 || p.yellows >= 2);
        const bool willBeExpelled = (draftDirectCardPreset_ >= 2);
        return wasExpelled && !willBeExpelled;
    }
    if (draftAction_ == VarAction::OverturnCancel && draftOrigCard_ == CardKind::Red) return true;
    if (draftAction_ == VarAction::EditCard && draftOrigCard_ == CardKind::Red && draftCard_ != CardKind::Red) return true;
    return false;
}

std::string VarHudController::FormatActiveContextTitle() const {
    if (detectedLabel_[0] != '\0') {
        if (IsValidSlot(detectedOffenderSlot_)) {
            char buf[128];
            const std::string offStr = FormatPlayerBroadcast(detectedOffenderSlot_);
            std::snprintf(buf, sizeof(buf), "%s  |  %s", Tr(detectedLabel_), offStr.c_str());
            return std::string(buf);
        }
        return std::string(Tr(detectedLabel_));
    }
    if (!hasActiveEvent_) return Tr("JOGO SEGUE");
    char buf[128];
    const char* evName = EventTypeLabel(activeEvent_.type);
    if (IsValidSlot(activeEvent_.offenderSlot)) {
        const std::string offStr = FormatPlayerBroadcast(activeEvent_.offenderSlot);
        std::snprintf(buf, sizeof(buf), "%s  |  %s", evName, offStr.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "%s", evName);
    }
    return std::string(buf);
}

std::string VarHudController::FormatSummaryOf(const VarVerdict& v) const {
    char buf[128];
    const char* teamStr = TeamShortName(v.benefitedTeam);
    if (v.action == VarAction::OverturnCancel) {
        std::snprintf(buf, sizeof(buf), Tr("Anular lance · posse %s"), teamStr);
    } else if (v.action == VarAction::RestartCorner) {
        std::snprintf(buf, sizeof(buf), Tr("Escanteio · %s"), teamStr);
    } else if (v.action == VarAction::RestartGoalKick) {
        std::snprintf(buf, sizeof(buf), Tr("Tiro de meta · %s"), teamStr);
    } else if (v.action == VarAction::RestartThrowIn) {
        std::snprintf(buf, sizeof(buf), Tr("Lateral · %s"), teamStr);
    } else if (v.action == VarAction::ConvertToPenalty || v.action == VarAction::RetroactivePenalty) {
        std::snprintf(buf, sizeof(buf), Tr("Pênalti · %s · %s"), teamStr, CardLabel(v.targetCard));
    } else if (v.action == VarAction::ConvertToFreeKick || v.action == VarAction::RetroactiveFreeKick) {
        std::snprintf(buf, sizeof(buf), Tr("Falta · %s · %s"), teamStr, CardLabel(v.targetCard));
    } else if (v.action == VarAction::EditCard) {
        // No "Cartão ·" prefix: the pool row already carries the CARTÃO tag, so the result stays visible
        const std::string offName = FormatPlayerBroadcast(v.offenderSlot);
        if (v.directCardEdit) {
            const int preset = (v.directReds > 0 && v.directYellows >= 2) ? 2 : (v.directReds > 0) ? 3 : (v.directYellows > 0) ? 1 : 0;
            std::snprintf(buf, sizeof(buf), "%s · %s", offName.c_str(), DirectCardPresetLabel(preset));
        } else {
            std::snprintf(buf, sizeof(buf), "%s · %s", offName.c_str(), CardLabel(v.targetCard));
        }
    } else {
        std::snprintf(buf, sizeof(buf), "%s", Tr("Manter decisão"));
    }
    Utf8TrimIncompleteTail(buf);
    return std::string(buf);
}

std::string VarHudController::FormatPoolSummary(const VerdictPool& pool) const {
    const size_t total = pool.Count();
    if (total == 0) return std::string();
    std::string first;
    std::string second;
    if (pool.hasPlayDecision) first = FormatSummaryOf(pool.playDecision);
    std::array<const CardPoolEntry*, kMaxCardPoolEntries> sorted{};
    size_t sc = 0;
    for (const auto& e : pool.cardEntries) {
        if (e.active) sorted[sc++] = &e;
    }
    std::sort(sorted.begin(), sorted.begin() + sc,
              [](const CardPoolEntry* a, const CardPoolEntry* b) { return a->order < b->order; });
    for (size_t i = 0; i < sc && second.empty(); ++i) {
        if (first.empty()) first = FormatSummaryOf(sorted[i]->verdict);
        else second = FormatSummaryOf(sorted[i]->verdict);
    }
    char buf[160];
    if (total == 1 || second.empty()) {
        std::snprintf(buf, sizeof(buf), "%s", first.c_str());
    } else if (total == 2) {
        std::snprintf(buf, sizeof(buf), "%s  +  %s", first.c_str(), second.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "%s  (+%zu)", first.c_str(), total - 1);
    }
    Utf8TrimIncompleteTail(buf);
    return std::string(buf);
}

std::string VarHudController::FormatVerdictSummary() const {
    VarVerdict v{};
    v.action = draftAction_;
    v.benefitedTeam = draftBenefitedTeam_;
    v.offenderSlot = draftOffenderSlot_;
    v.targetCard = draftCard_;
    v.directCardEdit = draftDirectCardMode_;
    if (draftDirectCardMode_) {
        v.directYellows = (draftDirectCardPreset_ == 1) ? 1 : (draftDirectCardPreset_ == 2) ? 2 : 0;
        v.directReds    = (draftDirectCardPreset_ >= 2) ? 1 : 0;
    }
    return FormatSummaryOf(v);
}
void VarHudController::HandleAction(NavAction action, VarState& state) {
    if (action == NavAction::ToggleDebug) {
        state.SetDebugOverlayOpen(!state.IsDebugOverlayOpen());
        return;
    }

    if (!inManualReplay_) return;

    if (action == NavAction::ToggleFocus) {
        focused_ = !focused_;
        if (focused_) {
            page_ = HudPage::Main;
            returnPage_ = HudPage::Main;
            selectedIndex_ = 0;
            RebuildMainOptions();
        }
        return;
    }

    if (!focused_) return;

    if (page_ == HudPage::Controls && capturing_) return;

    if (action == NavAction::CancelVerdict && page_ != HudPage::Controls) { // on Controls it clears a shortcut
        poolNoticeMsg_ = nullptr;
        state.CancelVerdict();
        return;
    }
    if (action == NavAction::PrevIncident || action == NavAction::NextIncident) {
        // With a decision open the draft belongs to the current play: switching plays would confirm a
        // decision about one play while showing another
        if (page_ == HudPage::Configure) return;
        StepReplayMarker(action == NavAction::PrevIncident ? -1 : +1, state);
        return;
    }
    if (action == NavAction::TogglePlayPause) {
        ToggleReplayPlayPause(state);
        return;
    }

    if (action == NavAction::QuickSwap) {
        if (page_ == HudPage::Main) {
            InvertDetectedRoles();
        } else if (page_ == HudPage::Squad) {
            squadTeamFilter_ = (squadTeamFilter_ == 0) ? 1 : 0;
        } else if (page_ == HudPage::Configure) {
            for (ConfigField f : configFields_) {
                if (f == ConfigField::Team) {
                    AdjustField(ConfigField::Team, +1, state);
                    break;
                }
            }
        }
        return;
    }

    if (action == NavAction::PrevTab && page_ != HudPage::Configure) {
        CycleTopLevelTab(-1);
        return;
    }
    if (action == NavAction::NextTab && page_ != HudPage::Configure) {
        CycleTopLevelTab(+1);
        return;
    }

    if (page_ == HudPage::Main) {
        const int count = static_cast<int>(mainOptions_.size());
        if (count <= 0) return;

        if (action == NavAction::Up) {
            selectedIndex_ = (selectedIndex_ - 1 + count) % count;
        } else if (action == NavAction::Down) {
            selectedIndex_ = (selectedIndex_ + 1) % count;
        } else if (action == NavAction::Left) {
            StepReplayMarker(-1, state);
        } else if (action == NavAction::Right) {
            StepReplayMarker(+1, state);
        } else if (action == NavAction::Back) {
            CollapseToReplay();
        } else if (action == NavAction::Confirm) {
            SelectMainOption(selectedIndex_, state);
        }
    } else if (page_ == HudPage::Squad) {
        constexpr int totalRows = 13;
        if (action == NavAction::Up) {
            selectedIndex_ = (selectedIndex_ - 1 + totalRows) % totalRows;
        } else if (action == NavAction::Down) {
            selectedIndex_ = (selectedIndex_ + 1) % totalRows;
        } else if (action == NavAction::Left || action == NavAction::Right) {
            squadTeamFilter_ = (squadTeamFilter_ == 0) ? 1 : 0;
        } else if (action == NavAction::Back) {
            page_ = HudPage::Main;
            selectedIndex_ = 0;
        } else if (action == NavAction::Confirm) {
            if (selectedIndex_ == 0) {
                squadTeamFilter_ = (squadTeamFilter_ == 0) ? 1 : 0;
            } else if (selectedIndex_ == 12) {
                page_ = HudPage::Main;
                selectedIndex_ = 0;
            } else {
                const int slot = squadTeamFilter_ * 11 + (selectedIndex_ - 1);
                SelectSquadPlayer(slot);
            }
        }
    } else if (page_ == HudPage::Config) {
        constexpr int totalRows = kSettingsRowCount;
        if (action == NavAction::Up) {
            selectedIndex_ = (selectedIndex_ - 1 + totalRows) % totalRows;
        } else if (action == NavAction::Down) {
            selectedIndex_ = (selectedIndex_ + 1) % totalRows;
        } else if (action == NavAction::Left) {
            if (selectedIndex_ <= kSettingsRowLanguage) TriggerSettingsRow(selectedIndex_, -1, state);
        } else if (action == NavAction::Right || action == NavAction::Confirm) {
            if (action == NavAction::Right && selectedIndex_ == kSettingsRowBack) return;
            TriggerSettingsRow(selectedIndex_, +1, state);
        } else if (action == NavAction::Back) {
            page_ = HudPage::Main;
            selectedIndex_ = 0;
        }
    } else if (page_ == HudPage::Controls) {
        if (capturing_) return; // InputManager owns every key/button until the capture ends
        constexpr int totalRows = kControlsRowCount;
        if (action == NavAction::Up) {
            selectedIndex_ = (selectedIndex_ - 1 + totalRows) % totalRows;
        } else if (action == NavAction::Down) {
            selectedIndex_ = (selectedIndex_ + 1) % totalRows;
        } else if (action == NavAction::Left) {
            SetControlsColumn(controlsColumn_ - 1);
        } else if (action == NavAction::Right) {
            SetControlsColumn(controlsColumn_ + 1);
        } else if (action == NavAction::Confirm) {
            if (selectedIndex_ == kControlsRowRestore) {
                RestoreDefaultBindings(state);
            } else if (selectedIndex_ == kControlsRowBack) {
                page_ = HudPage::Config;
                selectedIndex_ = kSettingsRowControls;
            } else {
                BeginBindingCapture(selectedIndex_, controlsColumn_, state);
            }
        } else if (action == NavAction::CancelVerdict) {
            ClearBindingCell(selectedIndex_, controlsColumn_, state);
        } else if (action == NavAction::Back) {
            page_ = HudPage::Config;
            selectedIndex_ = kSettingsRowControls;
        }
    } else if (page_ == HudPage::Configure) {
        const int totalRows = static_cast<int>(configFields_.size());
        if (totalRows <= 0) return;

        if (action == NavAction::Up) {
            selectedIndex_ = (selectedIndex_ - 1 + totalRows) % totalRows;
        } else if (action == NavAction::Down) {
            selectedIndex_ = (selectedIndex_ + 1) % totalRows;
        } else if (action == NavAction::Left) {
            const ConfigField f = configFields_[selectedIndex_];
            if (f != ConfigField::Confirm && f != ConfigField::Back) AdjustField(f, -1, state);
        } else if (action == NavAction::Right) {
            const ConfigField f = configFields_[selectedIndex_];
            if (f != ConfigField::Confirm && f != ConfigField::Back) AdjustField(f, +1, state);
        } else if (action == NavAction::Back) {
            page_ = returnPage_;
            selectedIndex_ = 0;
        } else if (action == NavAction::Confirm) {
            const ConfigField f = configFields_[selectedIndex_];
            if (f == ConfigField::Confirm) ArmCurrentDraft(state);
            else if (f == ConfigField::Back) { page_ = returnPage_; selectedIndex_ = 0; }
            else {
                // Enter on a value row goes to the Confirm button (values change with left / right),
                // so following the "Enter Confirmar" hint never silently changes a value.
                for (size_t i = 0; i < configFields_.size(); ++i) {
                    if (configFields_[i] == ConfigField::Confirm) { selectedIndex_ = static_cast<int>(i); break; }
                }
            }
        }
    }
}

} // namespace var_mod
