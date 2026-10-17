// ui/UIEngine.hpp
#pragma once

#include <switch.h>
#include <EGL/egl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <memory>
#include <string>

#include "OledBurnInGuard.hpp"
#include "Osd.hpp"
#include "Renderer2D.hpp"
#include "Types.hpp"
#include "../player/MpvPlayer.hpp"

namespace SwitchCast {

// Dono do contexto EGL/OpenGL, do mpv_render_context e do OSD. Tudo roda na thread principal.
class UIEngine {
public:
    explicit UIEngine(std::shared_ptr<MpvPlayer> player);
    ~UIEngine();

    bool Initialize();
    void Update(u64 kDown, PadState& pad);
    void Render();
    void Shutdown();

    // Pode ser chamado de dentro do appletHook; a troca real ocorre no próximo Render().
    void SetDisplayMode(DisplayMode mode);
    void OnFocusChanged(bool inFocus);

private:
    enum class GestureMode { None, Ignored, Brightness, Volume, Scrub };

    struct Touch {
        bool active = false;
        GestureMode mode = GestureMode::None;
        float startX = 0, startY = 0, lastX = 0, lastY = 0;
        double startTime = 0;
        int baseVolume = 0;
        float baseBrightness = 0;
        int64_t basePositionMs = 0;
    };

    bool InitEgl();
    bool CreateSurface(int width, int height);
    void DestroySurface();
    void ShutdownEgl();
    bool CreateRenderContext();

    void DrainNetworkCommands();
    void HandleButtons(u64 kDown);
    void HandleTouch();
    void ShowOsd(double seconds);
    void ShowToast(const std::string& text, double seconds = 1.5);
    void ApplyPendingDisplayMode();
    void SetBrightness(float value);
    void RenderDashboard(const PlayerSnapshot& snap, bool hasWallpaper = false);
    void RenderVideoOverlay(const PlayerSnapshot& snap);

    std::shared_ptr<MpvPlayer> mPlayer;
    OledBurnInGuard mBurnIn;
    Renderer2D mRenderer;
    float mPulseTimer = 0.0f;

    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    EGLConfig mConfig = nullptr;
    EGLSurface mSurface = EGL_NO_SURFACE;
    EGLContext mContext = EGL_NO_CONTEXT;
    mpv_render_context* mRender = nullptr;

    DisplayMode mMode = DisplayMode::OLED_720P;
    DisplayMode mPendingMode = DisplayMode::OLED_720P;
    int mWidth = 1280, mHeight = 720;
    int mSwapInterval = -1;

    double mLastTime = 0;
    float mDelta = 0;
    double mOsdVisibleUntil = 0;
    std::string mToast;
    double mToastUntil = 0;
    std::string mAddressLine;
    double mAddressRefreshAt = 0;
    std::string mLastOverlay;
    bool mOverlayDirty = true;

    Touch mTouch;
    bool mScrubbing = false;
    int64_t mScrubPositionMs = 0;
    bool mBrightnessAvailable = false;
    float mBrightness = 0.5f;
    bool mInitialized = false;
};

}  // namespace SwitchCast
