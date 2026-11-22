#include "port/Engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <libultraship/libultraship.h>
#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>

#include "FrameInterpolation.h"
#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/Nametag/Nametag.h"
#include "port/OS/OS.h"
#include "port/Patches/Patches.h"
#include "port/UI/cvar_prefixes.h"

#define gVIsPerFrame 2 // 30 Hz

extern "C" bool prevAltAssets;
extern "C" void port_releaseRcpTask(void);
#ifdef LUS_XBOX
extern "C" void ResourceMgr_XboxReadAheadHeld(void);
#endif

namespace {
long long sLastSubFrameNs = 0;
long long sPrevSubFrameNs = 0;
long long sLastPresentNs = 0;
long long sFrameLatchNs = 0;
unsigned sFrameViSerial = 0;
bool sFrameTimed = false;

using Clock = std::chrono::steady_clock;
inline long long NsSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
}

void SyncAltAssets() {
#ifdef LUS_XBOX
    bool curAltAssets = CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 0); // (off by default: Engine.cpp)
#else
    bool curAltAssets = CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 1);
#endif
    if (prevAltAssets != curAltAssets) {
        prevAltAssets = curAltAssets;
        Ship::Context::GetRawInstance()->GetResourceManager()->SetAltAssetsEnabled(curAltAssets);
        gfx_texture_cache_clear();
#ifdef LUS_XBOX
        if (curAltAssets) {
            ResourceMgr_XboxReadAheadHeld(); // ResourceHelpers.cpp: this map's HD textures, ahead of the renderer
        }
#endif
    }
}

// Draws one sub-frame without presenting it.
void DrawSubframe(Fast::Interpreter* interpreter, const std::shared_ptr<Ship::Window>& wndBase, Gfx* commands,
                  const std::unordered_map<Mtx*, MtxF>& replacements) {
    auto runT0 = Clock::now();
    auto gui = wndBase->GetGui();
    wndBase->GetMouseStateManager()->StartFrame();
    gui->StartDraw();
    interpreter->StartFrame();
    interpreter->Run(commands, replacements);
    if (OS_ViBlackActive()) {
        interpreter->mGfxFrameBuffer = 0;
        auto rapi = interpreter->GetCurrentRenderingAPI();
        rapi->StartDrawToFramebuffer(0, 1.0f);
        rapi->ClearFramebuffer(true, false);
    }
    gui->EndDraw();
    sPrevSubFrameNs = sLastSubFrameNs;
    sLastSubFrameNs = NsSince(runT0);
}

void PresentSubframe(Fast::Interpreter* interpreter) {
    interpreter->EndFrame();
    sLastPresentNs = OS_SteadyNs();
    CALL_EVENT(FrameDrawEnd);
    interpreter->mInterpolationIndex++;
}
} // namespace

#ifdef LUS_XBOX
// Banjo-Kazooie-X: the frame as the Xbox draws it. The list runs once, into the renderer's recording, and the frame is
// a replay of it (libultraship's Xbox Fast3dWindow), as in the other Lus-X ports: the port's menu (Back) then draws over
// the same frame, again and again, while it's open, with the game held behind it.
#include "lusx_debug.h"
#include "lusx_menu.h"
#include "port/ResourceHelpers.h"
#include "port/DevTools/ThreadWatchdog.h"

namespace {
int EffectiveLogicFps();

// The port's own drawing (the menu, the Test overlay: lusx_overlay.cpp's Ovl_Begin) sets the renderer's state behind
// the interpreter's back, which keeps what it last set so as not to set it again: told here what's set now (depth test
// and mask off, no decal, alpha on, the overlay's shader and texture, the screen), the next tick's recording sets its
// own again instead of drawing its first triangles with the overlay's.
void XboxAfterPortDrawing(Fast::Interpreter* interpreter) {
    interpreter->ForgetBoundTextures();
    interpreter->mRenderingState.depth_test_and_mask = 0;
    interpreter->mRenderingState.decal_mode = false;
    interpreter->mRenderingState.alpha_blend = true;
    interpreter->mRenderingState.mShaderProgram = nullptr;
    interpreter->mGfxFrameBuffer = 0;
}

void XboxRunMenu(Fast::Fast3dWindow* wnd) {
    // the game's threads wait on this one while the menu is open: no stall reports (the watchdog's own bracket)
    Lighthouse::ExpectedStall paused("port menu open");
    if (LusxMenu_Open(wnd->GetRenderingApi())) {
        wnd->SetTargetFps(60);
        while (LusxMenu_Update()) {
            wnd->ReplayUnder(LusxMenu_Draw);
        }
        LusxMenu_Close();
    }
    wnd->ReplayGraphicsCommands(nullptr, true);
    wnd->AfterMenu();
    XboxAfterPortDrawing(wnd->GetInterpreterWeak().lock().get());
}

int MsOf(long long ns) {
    return (int)((ns + 500000) / 1000000);
}
} // namespace

// A tick's frames: the list runs once, into the recording, and each frame is a replay of it. With `frames` > 1 (60 fps:
// a frame a blank) the frames before the tick's own are its matrices blended between the last tick and this one
// (Lighthouse's recorded pairs, FrameInterpolation_InterpolateInto, into libultraship's MtxBlends table): the list
// isn't run again for them, as Lighthouse's own timed pass does on a PC, where the CPU can afford it.
static MtxBlends sXboxBlended;

// Banjo-Kazooie-X: where a tick's CPU goes, a line in the log each 100 ticks drawn: the game's logic, the game making
// its display list, the audio's synthesis (port_xboxCost, from the game's and the audio's threads), and this thread's
// run of the list and replays. All on one CPU: together they're what has to fit in a tick (33 ms at 30 a second). A
// map's load (a tick's logic over 100 ms) is counted apart, not in the averages.
extern "C" double gLusxCyclesPerMs; // xbox_main.cpp
static std::atomic<uint64_t> sXboxCost[3];
static std::atomic<uint32_t> sXboxLogicTicks, sXboxLoads;
static std::atomic<uint64_t> sXboxLogicLongest;

extern "C" void port_xboxCost(int which, unsigned long long cycles) {
    if (which < 0 || which > 2) {
        return;
    }
    if (which == 0) {
        if (cycles > (uint64_t)(gLusxCyclesPerMs * 100.0)) {
            sXboxLoads++;
            return;
        }
        sXboxLogicTicks++;
        uint64_t longest = sXboxLogicLongest.load();
        while (cycles > longest && !sXboxLogicLongest.compare_exchange_weak(longest, cycles)) {
        }
    }
    sXboxCost[which] += cycles;
}

static void XboxLogTickCosts(long long listNs, long long drawNs, int frames) {
    static long long sListNs, sDrawNs;
    static uint32_t sTicks, sFrames;
    sListNs += listNs;
    sDrawNs += drawNs;
    sFrames += frames;
    if (++sTicks < 100) {
        return;
    }
    const double perMs = gLusxCyclesPerMs > 0.0 ? gLusxCyclesPerMs : 733333.0;
    const uint32_t logicTicks = sXboxLogicTicks.exchange(0);
    const double n = logicTicks != 0 ? (double)logicTicks : 1.0;
    const double logic = (double)sXboxCost[0].exchange(0) / perMs / n;
    const double build = (double)sXboxCost[1].exchange(0) / perMs / n;
    const double audio = (double)sXboxCost[2].exchange(0) / perMs / sTicks; // (its frames aren't the ticks)
    const double list = (double)sListNs / 1e6 / sTicks, draw = (double)sDrawNs / 1e6 / sTicks;
    SPDLOG_INFO("lus-x: a tick: the game's logic {:.1f} ms (the longest {:.1f}), its display list made {:.1f} ms, "
                "audio {:.1f} ms; the list's run {:.1f} ms, {:.1f} frames replayed {:.1f} ms; together {:.1f} ms; "
                "{} loads",
                logic, (double)sXboxLogicLongest.exchange(0) / perMs, build, audio, list, (double)sFrames / sTicks,
                draw, logic + build + audio + list + draw, sXboxLoads.exchange(0));
    sListNs = sDrawNs = 0;
    sTicks = sFrames = 0;
}

static void XboxDrawTick(Gfx* Commands, int frames) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    if (wnd == nullptr) {
        return;
    }
    auto interpreter = wnd->GetInterpreterWeak().lock().get();
    wnd->HandleEvents();
    interpreter->mInterpolationIndex = 0;
    Nametag::SetSubframeBlend(1.0f);
    if (Ship::Context::GetRawInstance()->GetWindow()->IsFrameReady()) {
        const bool menu = LusxMenu_Wanted();
        // the Test overlay's numbers (the Debug page): the list's run into the recording and the replays, this thread's
        // work a tick, against the tick's time (the game and its sound run on threads of their own)
        static long long sLastTickNs;
        const long long tickNs = OS_SteadyNs();
        if (ResourceMgr_XboxTakeTexturesStale()) {
            // a map change unloaded assets whose textures the cache keys by address, where new ones may now be
            // (ResourceMgr_XboxFreeUnused): the cache cleared before this list, the first that may draw them, records
            gfx_texture_cache_clear();
        }
        if (frames > 1) {
            FrameInterpolation_XboxBlendAnimVertices(); // (read as the list records)
        }
        wnd->RecordGraphicsCommands(Commands);
        const long long listNs = OS_SteadyNs() - tickNs;
        long long drawNs = 0;
        sXboxBlended.Clear();
        for (int i = 1; i <= frames; i++) {
            const bool last = i == frames;
            const float t = (float)i / (float)frames;
            if (!last) {
                FrameInterpolation_InterpolateInto(t, sXboxBlended);
            }
            interpreter->mInterpolationT = t;
            Nametag::SetSubframeBlend(t);
            const long long replayNs = OS_SteadyNs();
            interpreter->Replay(last ? nullptr : &sXboxBlended, last && !menu);
            drawNs += OS_SteadyNs() - replayNs;
            if (OS_ViBlackActive()) { // (as DrawSubframe: the VI shows black, whatever the list drew)
                interpreter->mGfxFrameBuffer = 0;
                auto rapi = interpreter->GetCurrentRenderingAPI();
                rapi->StartDrawToFramebuffer(0, 1.0f);
                rapi->ClearFramebuffer(true, false);
            }
            if (!menu && LusxMenu_DrawTestOverlay(wnd->GetRenderingApi())) {
                XboxAfterPortDrawing(interpreter);
            }
            if (last && frames > 1) {
                // nothing more of the game's memory is read: the game's next tick can go on while this one's last frame
                // waits for its blank (as Lighthouse's timed pass)
                port_releaseRcpTask();
            }
            interpreter->EndFrame();
            sLastPresentNs = OS_SteadyNs();
            CALL_EVENT(FrameDrawEnd);
            interpreter->mInterpolationIndex++;
        }
        interpreter->mInterpolationT = 1.0f;
        LusxDebug_SetCosts(-1, -1, MsOf(listNs), MsOf(drawNs), sLastTickNs != 0 ? MsOf(tickNs - sLastTickNs) : 0);
        if (!menu) {
            XboxLogTickCosts(listNs, drawNs, frames);
        }
        LusxDebug_SetNeed(MsOf(listNs + drawNs), 1000 / EffectiveLogicFps());
        sLastTickNs = tickNs;
        if (menu) {
            XboxRunMenu(wnd.get());
        }
    }
    SyncAltAssets();
}

void GameEngine::RunCommands(Gfx* Commands) {
    XboxDrawTick(Commands, 1);
}
#else
void GameEngine::RunCommands(Gfx* Commands) {
    static const std::unordered_map<Mtx*, MtxF> kNoReplacements;
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    if (wnd == nullptr) {
        return;
    }
    auto interpreter = wnd->GetInterpreterWeak().lock().get();
    wnd->HandleEvents();
    interpreter->mInterpolationIndex = 0;
    auto wndBase = Ship::Context::GetRawInstance()->GetWindow();
    Nametag::SetSubframeBlend(1.0f);
    if (wndBase->IsFrameReady()) {
        DrawSubframe(interpreter, wndBase, Commands, kNoReplacements);
        PresentSubframe(interpreter);
    }
    SyncAltAssets();
}
#endif

int GameEngine::CurrentViPerTick() {
    int viPerTick = FramePacingPatches_GetDemoViCount();
    if (viPerTick <= 0) {
        viPerTick = gVIsPerFrame + FramePacingPatches_GetCutsceneExtraVis();
    }
    if (viPerTick < gVIsPerFrame) {
        viPerTick = gVIsPerFrame;
    }
    // Clamp to 15 for demo playbacks.
    if (viPerTick > 15) {
        viPerTick = 15;
    }
    return viPerTick;
}

namespace {
int EffectiveLogicFps() {
    int fps = 60 / GameEngine::CurrentViPerTick();
    return (fps < 1) ? 1 : fps;
}
} // namespace

bool GameEngine::IsInterpolationEnabled() {
    return (int)GetInterpolationFPS() > EffectiveLogicFps();
}

// Decided once per list at submit, so the pickup in ServiceRcp and the pass that draws it agree.
bool GameEngine::WantsTimedPass(bool recorded, int viPerTick) {
    return recorded && !GfxDebuggerIsDebugging() && (int)GetInterpolationFPS() > 60 / viPerTick;
}

void GameEngine::SetFrameTiming(long long latchNs, unsigned viSerial, bool timed) {
    sFrameViSerial = viSerial;
    sFrameLatchNs = latchNs;
    sFrameTimed = timed;
}

namespace {
// Room left before the next swap has to latch, for the last draw and the game's swap behind it.
constexpr long long kReleaseMarginNs = 4000000;
constexpr int kMaxTimedSubframes = 32;
// How far a prefetched blend may be from the sub-frame's own before it is redone, as a share of the tick.
constexpr float kPrefetchSlack = 0.03f;

void BuildReplacements(float t, std::unordered_map<Mtx*, MtxF>& replacements) {
    if (t < 1.0f) {
        FrameInterpolation_Interpolate(t, replacements);
    } else {
        replacements.clear();
    }
}

class Prefetcher {
public:
    void Start(float t, std::unordered_map<Mtx*, MtxF>* out) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mThread.joinable()) {
            mThread = std::thread([this] { Run(); });
        }
        mT = t;
        mOut = out;
        mBusy = true;
        mCv.notify_all();
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(mMutex);
        mCv.wait(lock, [this] { return !mBusy; });
    }

private:
    void Run() {
        std::unique_lock<std::mutex> lock(mMutex);
        for (;;) {
            mCv.wait(lock, [this] { return mOut != nullptr; });
            auto* out = mOut;
            const float t = mT;
            mOut = nullptr;
            lock.unlock();
            BuildReplacements(t, *out);
            lock.lock();
            mBusy = false;
            mCv.notify_all();
        }
    }

    std::thread mThread;
    std::mutex mMutex;
    std::condition_variable mCv;
    std::unordered_map<Mtx*, MtxF>* mOut = nullptr;
    float mT = 0.0f;
    bool mBusy = false;
};

Prefetcher& GetPrefetcher() {
    static Prefetcher* sPrefetcher = new Prefetcher();
    return *sPrefetcher;
}

// Draws the frame at the window's rate until its successor is due to latch, blending each sub-frame to
// where its present lands between the previous frame and this one.
void RunTimedPass(Gfx* commands, int fps) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    if (wnd == nullptr) {
        port_releaseRcpTask();
        return;
    }
    auto interpreter = wnd->GetInterpreterWeak().lock().get();
    auto wndBase = Ship::Context::GetRawInstance()->GetWindow();
    wnd->HandleEvents();
    interpreter->mInterpolationIndex = 0;
    static std::unordered_map<Mtx*, MtxF> maps[2];
    Prefetcher& prefetcher = GetPrefetcher();
    bool prefetching = false;
    float prefetchT = -1.0f;
    // The tick after this list's sets its VI count when it polls input, right after the submit, and that
    // count decides when this frame latches.
    const long long waitEndNs = OS_SteadyNs() + 3000000;
    while (port_getDemoViSerial() == sFrameViSerial && OS_SteadyNs() < waitEndNs) {
        port_serviceRenderRequests();
        port_waitDemoViSerial(sFrameViSerial, 250);
    }
    const int vis = std::max(FramePacingPatches_GetDemoViCount(), 2);
    const long long tickNs = 1000000000LL * vis / 60;
    const long long presentNs = 1000000000LL / fps;
    const long long passStartNs = OS_SteadyNs();
    long long anchorNs = sFrameLatchNs;
    if (anchorNs == 0 || std::llabs(passStartNs - anchorNs) > tickNs) {
        const long long latchNs = OS_ViLastLatchNs();
        anchorNs = (latchNs > 0 && passStartNs - latchNs < tickNs) ? latchNs : passStartNs;
    }
    const long long releaseBy = anchorNs + tickNs - kReleaseMarginNs;
    int count = 0;

    for (;;) {
        const long long now = OS_SteadyNs();
        const long long drawNs = std::min(sLastSubFrameNs, sPrevSubFrameNs);
        const long long presentAtNs = std::max(now + drawNs, sLastPresentNs + presentNs);
        const long long nextPresentAtNs = presentAtNs + std::max(presentNs, drawNs);
        // Last when the next present would land after the successor latches, or its draw could not be finished by
        // the release deadline.
        const bool last = count + 1 >= kMaxTimedSubframes || nextPresentAtNs >= anchorNs + tickNs ||
                          presentAtNs + sLastSubFrameNs > releaseBy;
        float t = std::clamp((float)(presentAtNs - anchorNs) / (float)tickNs, 0.0f, 1.0f);
        auto& replacements = maps[count & 1];
        bool prefetched = false;
        if (prefetching) {
            prefetcher.Wait();
            prefetching = false;
            if (std::fabs(prefetchT - t) <= kPrefetchSlack) {
                t = prefetchT;
                prefetched = true;
            }
        }
        if (!prefetched) {
            BuildReplacements(t, replacements);
        }
        if (!last) {
            prefetchT = std::clamp((float)(nextPresentAtNs - anchorNs) / (float)tickNs, 0.0f, 1.0f);
            prefetcher.Start(prefetchT, &maps[(count + 1) & 1]);
            prefetching = true;
        }
        FrameInterpolation_ApplyAnimVertices(t);
        Nametag::SetSubframeBlend(t);
        DrawSubframe(interpreter, wndBase, commands, replacements);
        if (last) {
            port_releaseRcpTask();
        }
        PresentSubframe(interpreter);
        count++;
        if (last) {
            break;
        }
    }
    if (prefetching) {
        prefetcher.Wait();
    }
    SyncAltAssets();
}
} // namespace

void GameEngine::ProcessGfxCommands(Gfx* commands) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());

    if (wnd == nullptr) {
        return;
    }

    // if(gEnableGammaBoost) {
    //     wnd->EnableSRGBMode();
    // }
    wnd->SetRendererUCode(UcodeHandlers::ucode_f3dex);

#ifdef LUS_XBOX
    // Banjo-Kazooie-X: a tick recorded for blending (60 fps on, the port menu's Display and sound) is a frame a blank,
    // the tick's own the last of them; otherwise the tick's one frame, at the game's own rate
    wnd->SetMaximumFrameLatency(2);
    if (sFrameTimed) {
        wnd->SetTargetFps(60);
        XboxDrawTick(commands, CurrentViPerTick());
    } else {
        wnd->SetTargetFps(EffectiveLogicFps());
        XboxDrawTick(commands, 1);
    }
    return;
#endif
    if (sFrameTimed) {
        const int fps = (int)GetInterpolationFPS();
        wnd->SetTargetFps(fps);
        wnd->SetMaximumFrameLatency(2);
        RunTimedPass(commands, fps);
        return;
    }

    wnd->SetTargetFps(EffectiveLogicFps());
    wnd->SetMaximumFrameLatency(2);
    RunCommands(commands);
}

uint32_t GameEngine::GetInterpolationFPS() {
#ifdef LUS_XBOX
    // Banjo-Kazooie-X: 60, a frame a blank, with the port menu's 60 fps on (the default); the game's own rate, as the
    // N64 drew it, with it off. The frames between ticks are replays of the tick's recording with blended matrices
    // (XboxDrawTick), not the list run again.
    return CVarGetInteger("gLusx.Fps60", 1) ? 60u : (uint32_t)EffectiveLogicFps();
#endif
    if (CVarGetInteger(CVAR_SETTING("MatchRefreshRate"), 0)) {
        return Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate();

    } else if (CVarGetInteger(CVAR_VSYNC_ENABLED, 1) ||
               !Ship::Context::GetRawInstance()->GetWindow()->CanDisableVerticalSync()) {
        return std::min<uint32_t>(Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate(),
                                  CVarGetInteger(CVAR_SETTING("InterpolationFPS"), 60));
    }

    return CVarGetInteger(CVAR_SETTING("InterpolationFPS"), 30);
}
