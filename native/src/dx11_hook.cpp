#include "dx11_hook.h"

#include "aob_scanner.h"
#include "game_structs.h"
#include "heap_guard.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "inline_hook.h"
#include "input_manager.h"
#include "logger.h"
#include "match_hooks.h"
#include "var_hud.h"
#include "var_state.h"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace var_mod {

namespace {

// Native Fox Engine GrSwapChain::Present0(void* grSwapChain, UINT syncInterval) at 0x141FF46A0.
// Offset +0x20 inside GrSwapChain holds the game's real IDXGISwapChain* (verified in PES2021.exe
// 0x141FF46A0, 0x141FF4690, 0x141FF4752, 0x141FF4996, 0x141FF4B0D).
constexpr uintptr_t kGrSwapChain_DxgiSwapChainOffset = 0x20;

using FnGrSwapChainPresent = HRESULT(__fastcall*)(void* grSwapChain, UINT syncInterval);

InlineHook g_hookGrPresent;
thread_local int g_presentDepth = 0;

// Mod time per presented frame (QPC ticks), game Present excluded
std::atomic<uint64_t> g_presentFrames{ 0 };
std::atomic<uint64_t> g_presentTicks{ 0 };
std::atomic<uint64_t> g_presentMaxTicks{ 0 };

uint64_t PresentQpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// While the engine is releasing heap segments (post-match loading, menus), report the cost every 10 s instead
// of only at the next match start, so a slow loading shows up in the log at the time it happens
void MaybeLogPeriodicPerf() {
    static uint64_t s_lastMs = 0;
    const uint64_t now = GetTickCount64();
    if (s_lastMs == 0) s_lastMs = now;
    if (now - s_lastMs < 10000) return;
    s_lastMs = now;
    static HeapGuard::Stats s_prev{};
    const HeapGuard::Stats totals = HeapGuard::Totals();
    const HeapGuard::Stats hs = HeapGuard::Delta(totals, s_prev);
    s_prev = totals;
    if (hs.calls == 0) return;
    VAR_LOG_INF("Perf last 10 s: heap release %llu call(s) (%llu released, %llu refused, %llu refused from cache) in %llu us",
                static_cast<unsigned long long>(hs.calls), static_cast<unsigned long long>(hs.released),
                static_cast<unsigned long long>(hs.refused), static_cast<unsigned long long>(hs.cacheHits),
                static_cast<unsigned long long>(hs.microseconds));
}

void AccountPresentWork(uint64_t t0) {
    const uint64_t dt = PresentQpc() - t0;
    g_presentFrames.fetch_add(1, std::memory_order_relaxed);
    g_presentTicks.fetch_add(dt, std::memory_order_relaxed);
    uint64_t prev = g_presentMaxTicks.load(std::memory_order_relaxed);
    while (dt > prev && !g_presentMaxTicks.compare_exchange_weak(prev, dt, std::memory_order_relaxed)) {
    }
}

std::mutex g_renderMutex;
std::atomic<bool> g_stopRequested{ false };
std::atomic<bool> g_hooked{ false };
std::atomic<bool> g_initFinished{ false };
// Heap-owned on purpose: a joinable std::thread with static storage calls std::terminate
// from its destructor at process exit (ucrtbase 0xC0000409), when DllMain skips var_shutdown.
std::thread* g_initThread = nullptr;

IDXGISwapChain*      g_swapChain = nullptr;
IDXGISwapChain*      g_trackedSwapChain = nullptr;
ID3D11Device*        g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
HWND                 g_hwnd = nullptr;
bool                 g_imguiInitialized = false;
bool                 g_loggedFirstPresent = false;
int64_t              g_qpcFreq = 0;
int64_t              g_lastQpc = 0;

void ShutdownImGuiLocked() {
    VarHudRenderer::ResetFonts();
    if (g_imguiInitialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_imguiInitialized = false;
    }
    g_lastQpc = 0;
    InputManager::DetachWindow();
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
    g_swapChain = nullptr;
    g_trackedSwapChain = nullptr;
    g_hwnd = nullptr;
}

void EnsureWindowTrackedLocked(IDXGISwapChain* swapChain) {
    if (g_hwnd && swapChain == g_trackedSwapChain) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
        g_trackedSwapChain = swapChain;
        if (desc.OutputWindow != g_hwnd) {
            g_hwnd = desc.OutputWindow;
            InputManager::AttachWindow(g_hwnd);
        }
        if (!g_loggedFirstPresent) {
            g_loggedFirstPresent = true;
            VAR_LOG_INF("Native GrSwapChain::Present0 active: SwapChain=0x%p HWND=0x%p (%ux%u)",
                        static_cast<void*>(swapChain), static_cast<void*>(g_hwnd),
                        static_cast<unsigned>(desc.BufferDesc.Width),
                        static_cast<unsigned>(desc.BufferDesc.Height));
        }
    }
}

bool InitImGuiLocked(IDXGISwapChain* swapChain) {
    if (!swapChain) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow) {
        return false;
    }

    ID3D11Device* dev = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) {
        return false;
    }

    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) {
        dev->Release();
        return false;
    }

    g_swapChain = swapChain;
    g_device = dev;
    g_context = ctx;
    g_hwnd = desc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // Fonts are built by VarHudRenderer::EnsureFonts at the exact UI scale of the backbuffer (crisp 720p..4K)
    VarHudRenderer::EnsureFonts(static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height));

    ImGui::StyleColorsDark();
    if (!ImGui_ImplDX11_Init(g_device, g_context)) {
        ImGui::DestroyContext();
        ShutdownImGuiLocked();
        return false;
    }

    LARGE_INTEGER freq{}, nowQpc{};
    if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
        g_qpcFreq = freq.QuadPart;
    }
    if (QueryPerformanceCounter(&nowQpc)) {
        g_lastQpc = nowQpc.QuadPart;
    }

    g_imguiInitialized = true;
    InputManager::AttachWindow(g_hwnd);
    VAR_LOG_INF("ImGui DX11 initialized (lazy, precompiled DXBC) on SwapChain=0x%p HWND=0x%p",
                static_cast<void*>(swapChain), static_cast<void*>(g_hwnd));
    return true;
}

void RenderOverlayOnSwapChainLocked(IDXGISwapChain* swapChain, bool alreadyPolled) {
    if (!swapChain) return;

    EnsureWindowTrackedLocked(swapChain);

    // Poll ReplayManager live on render thread so cursor & ManualFlag update at 60fps
    // even when simulation (MatchFSM::MainTick) is paused during Replay.
    if (!alreadyPolled) {
        MatchHooks::PollLiveReplayState();
    }

    auto& st = VarState::Instance();
    const uint64_t nowMs = GetTickCount64();

    // Fast-path: during boot, menus, and 98% of live gameplay, skip ImGui init/draw completely.
    // TickIdle still polls F10 so the debug overlay can be opened on demand at any moment.
    if (!st.ShouldRenderAnyHud(nowMs)) {
        VarHudRenderer::TickIdle();
        return;
    }

    // Any frame that cannot be drawn still runs the idle tick, which clears panel focus / input blocking
    // and the manual replay flag, so a skipped HUD never keeps the user's input blocked.
    if (!g_imguiInitialized) {
        if (!InitImGuiLocked(swapChain)) {
            VarHudRenderer::TickIdle();
            return;
        }
    } else if (swapChain != g_swapChain) {
        ShutdownImGuiLocked();
        if (!InitImGuiLocked(swapChain)) {
            VarHudRenderer::TickIdle();
            return;
        }
    }

    if (!g_imguiInitialized || !g_device || !g_context) {
        VarHudRenderer::TickIdle();
        return;
    }

    // Acquire backbuffer & transient RTV ONLY for this frame, then release immediately.
    // Holding zero persistent backbuffer references means ResizeBuffers / Alt+Tab / fullscreen
    // transitions always succeed natively with zero need to hook dxgi!ResizeBuffers!
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer) {
        VarHudRenderer::TickIdle();
        return;
    }

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);
    const float vw = static_cast<float>(bbDesc.Width);
    const float vh = static_cast<float>(bbDesc.Height);

    ID3D11RenderTargetView* rtv = nullptr;
    const HRESULT rtvHr = g_device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();
    if (FAILED(rtvHr) || !rtv) {
        VarHudRenderer::TickIdle();
        return;
    }

    // Every view taken for this frame is released (and the game's targets restored once ours was bound) on any
    // exit, including an exception from font rebuilding or ImGui::Render: a leaked back buffer view would make
    // the next ResizeBuffers fail.
    struct FrameViews {
        ID3D11DeviceContext* ctx;
        ID3D11RenderTargetView* own;
        ID3D11RenderTargetView* prevRtv0 = nullptr;
        ID3D11DepthStencilView* prevDsv = nullptr;
        bool bound = false;
        ~FrameViews() {
            if (bound) ctx->OMSetRenderTargets(1, &prevRtv0, prevDsv);
            if (prevRtv0) prevRtv0->Release();
            if (prevDsv) prevDsv->Release();
            own->Release();
        }
    } views{ g_context, rtv };

    if (vw < 320.0f || vh < 240.0f) {
        VarHudRenderer::TickIdle();
    } else {
        g_context->OMGetRenderTargets(1, &views.prevRtv0, &views.prevDsv);

        // Resolution or HUD size preset changed: re-rasterize fonts at the new scale (outside the ImGui frame)
        if (VarHudRenderer::EnsureFonts(vw, vh)) {
            ImGui_ImplDX11_InvalidateDeviceObjects(); // NewFrame recreates the font texture
        }
        ImGui_ImplDX11_NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        // The atlas pixels live on the GPU once uploaded: drop the CPU copy (2.5 MB at 1080p, 10 MB at 4K). The
        // font input stays, so a texture recreation (device reset, new resolution) rebuilds the pixels by itself.
        if (io.Fonts->TexID && (io.Fonts->TexPixelsAlpha8 || io.Fonts->TexPixelsRGBA32)) {
            io.Fonts->ClearTexData();
        }
        if (!io.Fonts->IsBuilt()) {
            // Device objects could not be created this frame (device removed / out of video memory):
            // ImGui::NewFrame would assert on the missing font atlas. Skip and retry next frame.
            VarHudRenderer::TickIdle();
            return;
        }
        io.DisplaySize = ImVec2(vw, vh);

        LARGE_INTEGER curQpc{};
        if (g_qpcFreq > 0 && QueryPerformanceCounter(&curQpc)) {
            const double dt = (g_lastQpc > 0)
                ? static_cast<double>(curQpc.QuadPart - g_lastQpc) / static_cast<double>(g_qpcFreq)
                : (1.0 / 60.0);
            g_lastQpc = curQpc.QuadPart;
            io.DeltaTime = static_cast<float>(std::clamp(dt, 0.0001, 0.25));
        } else {
            io.DeltaTime = 1.0f / 60.0f;
        }

        const bool mouseActive = st.IsPanelFocused() && st.IsMouseAllowed() &&
                                 !st.IsSiderOverlayOpen() && (GetForegroundWindow() == g_hwnd);
        io.MouseDrawCursor = mouseActive;
        ImGui::GetStyle().MouseCursorScale = std::clamp(vh / 1080.0f, 1.0f, 1.6f);
        if (mouseActive) {
            POINT pt{};
            RECT rc{};
            if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) && GetClientRect(g_hwnd, &rc)) {
                const float cw = static_cast<float>(rc.right - rc.left);
                const float ch = static_cast<float>(rc.bottom - rc.top);
                if (cw > 1.0f && ch > 1.0f) {
                    const float mx = static_cast<float>(pt.x) * (vw / cw);
                    const float my = static_cast<float>(pt.y) * (vh / ch);
                    io.AddMousePosEvent(mx, my);
                }
            }
            io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        } else {
            io.AddMousePosEvent(-1e30f, -1e30f);
            io.AddMouseButtonEvent(0, false);
        }

        ImGui::NewFrame();

        ImGuiErrorRecoveryState recoverTo;
        ImGui::ErrorRecoveryStoreState(&recoverTo);
        try {
            VarHudRenderer::RenderFrame(vw, vh);
        } catch (...) {
            // Out of memory while building HUD text: unwind ImGui's stacks to this point and finish the frame
            // normally, so the back buffer views below are always released
            ImGui::ErrorRecoveryTryToRecoverState(&recoverTo);
            VarHudRenderer::TickIdle();
        }

        ImGui::Render();
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        views.bound = true;
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

HRESULT __fastcall Detour_GrSwapChainPresent(void* grSwapChain, UINT syncInterval) {
    auto orig = g_hookGrPresent.Original<FnGrSwapChainPresent>();
    if (g_presentDepth > 0) {
        return orig ? orig(grSwapChain, syncInterval) : S_OK;
    }
    ++g_presentDepth;
    const uint64_t workStart = PresentQpc();
    MaybeLogPeriodicPerf();
    MatchHooks::CheckSimStall();

    if (IsValidPtr(grSwapChain) && !g_stopRequested.load(std::memory_order_acquire)) {
        IDXGISwapChain* swapChain = *reinterpret_cast<IDXGISwapChain* const*>(
            reinterpret_cast<uintptr_t>(grSwapChain) + kGrSwapChain_DxgiSwapChainOffset);
        if (IsValidPtr(swapChain)) {
            bool polledThisFrame = false;
            // Ultra-light lock-free fast-path for boot, menus, and 98% of live gameplay
            if (g_hwnd && swapChain == g_trackedSwapChain) {
                MatchHooks::PollLiveReplayState();
                polledThisFrame = true;
                if (!VarState::Instance().ShouldRenderAnyHud(GetTickCount64())) {
                    VarHudRenderer::TickIdle();
                    AccountPresentWork(workStart);
                    const HRESULT hr = orig ? orig(grSwapChain, syncInterval) : S_OK;
                    --g_presentDepth;
                    return hr;
                }
            }
            std::unique_lock<std::mutex> lock(g_renderMutex, std::try_to_lock);
            // Re-check under the lock: Shutdown may have destroyed ImGui between the first test and here
            if (lock.owns_lock() && !g_stopRequested.load(std::memory_order_acquire)) {
                try {
                    RenderOverlayOnSwapChainLocked(swapChain, polledThisFrame);
                } catch (...) {
                    // Out of memory while building HUD strings: skip this frame, never throw into the engine
                    VarHudRenderer::TickIdle();
                }
            }
        }
    }

    AccountPresentWork(workStart);
    const HRESULT hr = orig ? orig(grSwapChain, syncInterval) : S_OK;
    --g_presentDepth;
    return hr;
}

bool InstallNativePresentHook(bool quietOnMiss) {
    const AobMatchResult r = AobScanner::ScanModuleTextWithHint(
        nullptr, Aobs::GrSwapChain_Present0, AobHints::GrSwapChain_Present0);
    if (!r.unique()) {
        if (!quietOnMiss) {
            VAR_LOG_ERR("AOB 'GrSwapChain::Present0' failed: matchCount=%zu addr=0x%llX",
                        r.matchCount, static_cast<unsigned long long>(r.address));
        }
        return false;
    }

    uintptr_t presentFn = r.address;
    const size_t stolen = InlineHook::MeasureSafeStolenLen(presentFn);
    if (stolen == 0) {
        VAR_LOG_ERR("Unsupported prologue at GrSwapChain::Present0=0x%llX",
                    static_cast<unsigned long long>(presentFn));
        return false;
    }

    if (!g_hookGrPresent.Install(presentFn, reinterpret_cast<void*>(&Detour_GrSwapChainPresent), stolen)) {
        VAR_LOG_ERR("Failed to install native GrSwapChain::Present0 hook at 0x%llX",
                    static_cast<unsigned long long>(presentFn));
        return false;
    }

    VAR_LOG_INF("Native GrSwapChain::Present0 hook installed at 0x%llX (stolen=%zu, zero dummy DX11)",
                static_cast<unsigned long long>(presentFn), stolen);
    return true;
}

void InitWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (g_stopRequested.load(std::memory_order_acquire)) return;

    // Install MatchHooks + native Fox Engine Present & DirectInput blocker hooks (< 0.8 ms via shared 4KB slab)
    bool presentOk = false;
    bool inputBlockOk = false;
    for (int attempt = 0; attempt < 60 && !g_stopRequested.load(std::memory_order_acquire); ++attempt) {
        const bool quiet = (attempt < 59);
        if (!MatchHooks::IsInstalled()) {
            MatchHooks::Install(quiet);
        }
        if (!presentOk) {
            presentOk = InstallNativePresentHook(quiet);
            if (presentOk) {
                g_hooked.store(true, std::memory_order_release);
            }
        }
        if (!inputBlockOk) {
            inputBlockOk = InputManager::InstallNativeInputBlockHooks(quiet);
        }
        if (MatchHooks::IsInstalled() && presentOk && inputBlockOk) {
            break;
        }
        Sleep(100);
    }

    Logger::Flush();
    g_initFinished.store(true, std::memory_order_release);
}

} // namespace

bool Dx11Hook::StartAsync() {
    if (g_hooked.load(std::memory_order_acquire) || (g_initThread && g_initThread->joinable())) {
        return true;
    }
    g_stopRequested.store(false, std::memory_order_release);
    g_initFinished.store(false, std::memory_order_release);
    g_initThread = new std::thread(InitWorker);
    return true;
}

void Dx11Hook::Shutdown() {
    g_stopRequested.store(true, std::memory_order_release);
    if (g_initThread) {
        if (g_initThread->joinable()) {
            g_initThread->join();
        }
        delete g_initThread;
        g_initThread = nullptr;
    }

    g_hookGrPresent.Uninstall();
    g_hooked.store(false, std::memory_order_release);
    g_initFinished.store(false, std::memory_order_release); // a later var_on_match_start must not re-hook

    std::lock_guard<std::mutex> lock(g_renderMutex);
    ShutdownImGuiLocked();
    g_loggedFirstPresent = false;
}

bool Dx11Hook::IsHooked() {
    return g_hooked.load(std::memory_order_acquire);
}

bool Dx11Hook::IsInitFinished() {
    return g_initFinished.load(std::memory_order_acquire);
}

Dx11Hook::PresentStats Dx11Hook::TakePresentStats() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const uint64_t freq = (f.QuadPart > 0) ? static_cast<uint64_t>(f.QuadPart) : 1;
    PresentStats s{};
    s.frames = g_presentFrames.exchange(0, std::memory_order_relaxed);
    s.microseconds = g_presentTicks.exchange(0, std::memory_order_relaxed) * 1000000ULL / freq;
    s.maxMicroseconds = g_presentMaxTicks.exchange(0, std::memory_order_relaxed) * 1000000ULL / freq;
    return s;
}

} // namespace var_mod
