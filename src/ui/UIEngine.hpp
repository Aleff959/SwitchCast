// ui/UIEngine.hpp
#pragma once

#include <switch.h>
#include <EGL/egl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <array>
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
    enum class ScreenView {
        Dashboard,
        About,
        HowToUse,
        UpdateCenter
    };

    enum class DrawerTab {
        HowToUse = 0,
        WhatsNew = 1,
        About = 2
    };

    struct DvdSymbol {
        float x, y;
        float vx, vy;
        float scale = 1.0f;
        int colorIdx = 0;
        int shapeType = 0; // 0 = Losango, 1 = Círculo, 2 = Quadrado, 3 = Triângulo
    };

    void RenderDashboard(const PlayerSnapshot& snap, bool hasWallpaper = false);
    void RenderAbout();
    void RenderHowToUse();
    void RenderVideoOverlay(const PlayerSnapshot& snap);
    void RenderUpdateCenter();

    void DrawMorphSwitchButton(float x, float y, float t, float s, float dim);
    void RenderDrawer(float s, float dim);
    void UpdateDvdPhysics(float delta);
    void RenderDvdSymbols(float s, float alpha);

    ScreenView mCurrentScreen = ScreenView::Dashboard;
    bool mUpdateDialogDismissed = false;

    bool mMenuOpen = false;
    float mMenuAnimProgress = 0.0f; // 0.0f = Hambúrguer (Fechado) -> 1.0f = Switch (Aberto)
    DrawerTab mActiveDrawerTab = DrawerTab::HowToUse;

    std::array<DvdSymbol, 4> mDvdSymbols = {{
        { 560.0f, 120.0f,  215.0f,  185.0f, 1.0f, 0, 0 },
        { 940.0f, 180.0f, -195.0f,  210.0f, 1.0f, 1, 1 },
        { 640.0f, 480.0f,  205.0f, -190.0f, 1.0f, 2, 2 },
        { 990.0f, 520.0f, -210.0f, -180.0f, 1.0f, 0, 3 }
    }};

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
