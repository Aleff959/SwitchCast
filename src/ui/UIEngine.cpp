// ui/UIEngine.cpp
#include "UIEngine.hpp"

#include <EGL/eglext.h>
#include <glad/glad.h>

#include <algorithm>
#include <cmath>

#include "../network/ServerBridge.hpp"
#include "updater/OtaUpdater.hpp"
#include "../utils/Log.hpp"
#include "../utils/Platform.hpp"
#include "../utils/Text.hpp"

namespace SwitchCast {

namespace {

constexpr float kTouchSlopPx = 24.0f;
constexpr double kTapMaxSeconds = 0.45;
constexpr float kScrubSecondsPerScreenWidth = 180.0f;  // arrastar a tela inteira = 3 min
constexpr int kVolumeStep = 5;
constexpr int64_t kShortSeekMs = 10'000;
constexpr int64_t kLongSeekMs = 60'000;
constexpr double kOsdTimeoutSec = 4.0;
constexpr int kTouchWidth = 1280;   // coordenadas do touch são sempre 1280x720
constexpr int kTouchHeight = 720;

DisplayMode CurrentSystemMode() {
    return appletGetOperationMode() == AppletOperationMode_Handheld ? DisplayMode::OLED_720P
                                                                    : DisplayMode::DOCKED_1080P;
}

void* GetProc(void*, const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); }

}  // namespace

UIEngine::UIEngine(std::shared_ptr<MpvPlayer> player) : mPlayer(std::move(player)) {}

UIEngine::~UIEngine() { Shutdown(); }

bool UIEngine::Initialize() {
    mMode = mPendingMode = CurrentSystemMode();
    mWidth = mMode == DisplayMode::DOCKED_1080P ? 1920 : 1280;
    mHeight = mMode == DisplayMode::DOCKED_1080P ? 1080 : 720;

    hidInitializeTouchScreen();
    if (R_SUCCEEDED(lblInitialize())) {
        mBrightnessAvailable = R_SUCCEEDED(lblGetCurrentBrightnessSetting(&mBrightness));
    }

    if (!InitEgl()) return false;
    if (!CreateRenderContext()) return false;
    mRenderer.Initialize(mWidth, mHeight);

    mLastTime = Platform::NowSeconds();
    mInitialized = true;
    return true;
}

bool UIEngine::InitEgl() {
    mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (mDisplay == EGL_NO_DISPLAY || !eglInitialize(mDisplay, nullptr, nullptr)) return false;
    if (!eglBindAPI(EGL_OPENGL_API)) return false;

    const EGLint configAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,  EGL_GREEN_SIZE, 8,
                                  EGL_BLUE_SIZE,       8,              EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                  EGL_STENCIL_SIZE,    8,              EGL_NONE};
    EGLint numConfigs = 0;
    eglChooseConfig(mDisplay, configAttrs, &mConfig, 1, &numConfigs);
    if (numConfigs == 0) return false;

    if (!CreateSurface(mWidth, mHeight)) return false;

    const EGLint ctxAttrs[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                               EGL_CONTEXT_MAJOR_VERSION_KHR,       4,
                               EGL_CONTEXT_MINOR_VERSION_KHR,       3,
                               EGL_NONE};
    mContext = eglCreateContext(mDisplay, mConfig, EGL_NO_CONTEXT, ctxAttrs);
    if (mContext == EGL_NO_CONTEXT) return false;
    if (!eglMakeCurrent(mDisplay, mSurface, mSurface, mContext)) return false;

    gladLoadGL();
    return true;
}

bool UIEngine::CreateSurface(int width, int height) {
    NWindow* window = nwindowGetDefault();
    nwindowSetDimensions(window, static_cast<u32>(width), static_cast<u32>(height));
    mSurface = eglCreateWindowSurface(mDisplay, mConfig, window, nullptr);
    mSwapInterval = -1;  // força reaplicar eglSwapInterval
    return mSurface != EGL_NO_SURFACE;
}

void UIEngine::DestroySurface() {
    if (mSurface == EGL_NO_SURFACE) return;
    eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, mContext);
    eglDestroySurface(mDisplay, mSurface);
    mSurface = EGL_NO_SURFACE;
}

void UIEngine::ShutdownEgl() {
    if (mDisplay == EGL_NO_DISPLAY) return;
    eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (mContext != EGL_NO_CONTEXT) eglDestroyContext(mDisplay, mContext);
    if (mSurface != EGL_NO_SURFACE) eglDestroySurface(mDisplay, mSurface);
    eglTerminate(mDisplay);
    mContext = EGL_NO_CONTEXT;
    mSurface = EGL_NO_SURFACE;
    mDisplay = EGL_NO_DISPLAY;
}

bool UIEngine::CreateRenderContext() {
    mpv_opengl_init_params glInit{GetProc, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    return mpv_render_context_create(&mRender, mPlayer->Handle(), params) >= 0;
}

void UIEngine::Shutdown() {
    if (!mInitialized && mDisplay == EGL_NO_DISPLAY) return;
    mRenderer.Shutdown();
    if (mRender) {
        // O contexto GL precisa estar corrente para liberar texturas/FBOs do mpv.
        if (mSurface != EGL_NO_SURFACE) eglMakeCurrent(mDisplay, mSurface, mSurface, mContext);
        mpv_render_context_free(mRender);
        mRender = nullptr;
    }
    ShutdownEgl();
    if (mBrightnessAvailable) lblExit();
    mBrightnessAvailable = false;
    mInitialized = false;
}

void UIEngine::SetDisplayMode(DisplayMode mode) { mPendingMode = mode; }

void UIEngine::OnFocusChanged(bool inFocus) {
    if (!inFocus && mPlayer->Snapshot().state == PlaybackState::Playing) mPlayer->Pause();
    mOverlayDirty = true;
}

void UIEngine::ApplyPendingDisplayMode() {
    if (mPendingMode == mMode) return;
    mMode = mPendingMode;
    mWidth = mMode == DisplayMode::DOCKED_1080P ? 1920 : 1280;
    mHeight = mMode == DisplayMode::DOCKED_1080P ? 1080 : 720;

    DestroySurface();
    if (CreateSurface(mWidth, mHeight)) eglMakeCurrent(mDisplay, mSurface, mSurface, mContext);
    mOverlayDirty = true;
    ShowToast(mMode == DisplayMode::DOCKED_1080P ? "Modo Dock: 1080p" : "Modo port\xC3\xA1til: 720p");
}

// ---------------------------------------------------------------------------
// Entrada e comandos
// ---------------------------------------------------------------------------
void UIEngine::Update(u64 kDown, PadState&) {
    const double now = Platform::NowSeconds();
    mDelta = static_cast<float>(std::clamp(now - mLastTime, 0.0, 0.25));
    mLastTime = now;
    mPulseTimer += mDelta;

    auto& updater = Updater::OtaUpdater::Instance();
    if (updater.IsDownloading() || updater.IsInstalling()) {
        mCurrentScreen = ScreenView::UpdateCenter;
    } else if (updater.HasUpdateReady() && !mUpdateDialogDismissed) {
        mCurrentScreen = ScreenView::UpdateCenter;
    }

    mPlayer->PumpEvents();
    DrainNetworkCommands();
    HandleButtons(kDown);
    HandleTouch();

    const PlayerSnapshot snap = mPlayer->Snapshot();
    mBurnIn.Update(mDelta, snap.state != PlaybackState::Idle);

    if (now >= mAddressRefreshAt) {
        mAddressRefreshAt = now + 2.0;
        const DeviceIdentity id = ServerBridge::Instance().Identity();
        const std::string ip = Platform::GetLocalIp();
        mAddressLine = ip + " | HTTP " + std::to_string(id.httpPort) + " | WS " + std::to_string(id.wsPort);
    }
}

void UIEngine::DrainNetworkCommands() {
    NetworkCommand cmd;
    while (ServerBridge::Instance().PopCommand(cmd)) {
        mBurnIn.NotifyActivity();
        switch (cmd.type) {
            case NetworkCommand::Type::LoadMedia:
                mPlayer->LoadMedia(cmd.media);
                ShowToast(cmd.media.title.empty() ? std::string("Transmiss\xC3\xA3o recebida") : cmd.media.title, 3.0);
                ShowOsd(kOsdTimeoutSec);
                break;
            case NetworkCommand::Type::Play:         mPlayer->Play(); break;
            case NetworkCommand::Type::Pause:        mPlayer->Pause(); break;
            case NetworkCommand::Type::Stop:         mPlayer->Stop(); break;
            case NetworkCommand::Type::Seek:         mPlayer->SeekAbsoluteMs(cmd.positionMs); break;
            case NetworkCommand::Type::SeekRelative: mPlayer->SeekRelativeMs(cmd.positionMs); break;
            case NetworkCommand::Type::SetVolume:
                mPlayer->SetVolume(cmd.volume);
                ShowToast("Volume " + std::to_string(cmd.volume) + "%");
                break;
            case NetworkCommand::Type::SetMute:      mPlayer->SetMute(cmd.flag); break;
            case NetworkCommand::Type::AddSubtitle:  mPlayer->AddExternalSubtitle(cmd.text); break;
            case NetworkCommand::Type::CycleAudio:   ShowToast(mPlayer->CycleAudioTrack()); break;
            case NetworkCommand::Type::CycleSubtitle: ShowToast(mPlayer->CycleSubtitleTrack()); break;
        }
    }
}

void UIEngine::HandleButtons(u64 kDown) {
    if (!kDown) return;
    mBurnIn.NotifyActivity();

    auto& updater = Updater::OtaUpdater::Instance();

    // Central de Atualização (Aba dedicada em tela preta)
    if (mCurrentScreen == ScreenView::UpdateCenter) {
        if (updater.IsInstalling()) {
            return; // Bloqueia botões durante a gravação para proteger o console
        }
        if (kDown & (HidNpadButton_Y | HidNpadButton_A)) {
            if (updater.HasUpdateReady()) {
                if (mPlayer) {
                    mPlayer->StopWallpaperLoop();
                    mPlayer->Stop();
                }
                updater.StartInstallation();
            }
            return;
        }
        if (kDown & HidNpadButton_B) {
            mCurrentScreen = ScreenView::Dashboard;
            mUpdateDialogDismissed = true;
            ShowToast("Atualizacao adiada. Pressione [X] para reabrir.", 2.5);
            return;
        }
        return;
    }

    // Se estiver no Dashboard e houver atualização pronta/baixando, [X] reabre a Central
    if ((updater.HasUpdateReady() || updater.IsDownloading()) && (kDown & HidNpadButton_X)) {
        mCurrentScreen = ScreenView::UpdateCenter;
        mUpdateDialogDismissed = false;
        return;
    }

    const PlayerSnapshot snap = mPlayer->Snapshot();
    const bool hasMedia = snap.state != PlaybackState::Idle;

    if (kDown & (HidNpadButton_L | HidNpadButton_ZL)) {
        mPlayer->AdjustVolume(-kVolumeStep);
        ShowToast("Volume " + std::to_string(std::max(0, snap.volume - kVolumeStep)) + "%");
    }
    if (kDown & (HidNpadButton_R | HidNpadButton_ZR)) {
        mPlayer->AdjustVolume(kVolumeStep);
        ShowToast("Volume " + std::to_string(std::min(100, snap.volume + kVolumeStep)) + "%");
    }
    if (kDown & HidNpadButton_Minus) {
        mPlayer->SetMute(!snap.muted);
        ShowToast(snap.muted ? "Som ativado" : "Mudo");
    }

    if (!hasMedia) return;

    if (kDown & HidNpadButton_A) {
        mPlayer->TogglePause();
        ShowOsd(kOsdTimeoutSec);
    }
    if (kDown & HidNpadButton_B) mOsdVisibleUntil = 0;  // esconde o OSD
    if (kDown & HidNpadButton_X) ShowToast(mPlayer->CycleAudioTrack());
    if (kDown & HidNpadButton_Y) ShowToast(mPlayer->CycleSubtitleTrack());
    if (kDown & HidNpadButton_Up)    { mPlayer->SeekRelativeMs(kLongSeekMs);   ShowOsd(kOsdTimeoutSec); }
    if (kDown & HidNpadButton_Down)  { mPlayer->SeekRelativeMs(-kLongSeekMs);  ShowOsd(kOsdTimeoutSec); }
    if (kDown & HidNpadButton_Right) { mPlayer->SeekRelativeMs(kShortSeekMs);  ShowOsd(kOsdTimeoutSec); }
    if (kDown & HidNpadButton_Left)  { mPlayer->SeekRelativeMs(-kShortSeekMs); ShowOsd(kOsdTimeoutSec); }
}

void UIEngine::SetBrightness(float value) {
    mBrightness = std::clamp(value, 0.02f, 1.0f);
    if (mBrightnessAvailable) lblSetCurrentBrightnessSetting(mBrightness);
}

void UIEngine::HandleTouch() {
    HidTouchScreenState ts{};
    const bool pressed = hidGetTouchScreenStates(&ts, 1) > 0 && ts.count > 0;
    const double now = Platform::NowSeconds();

    if (pressed) {
        mBurnIn.NotifyActivity();
        const float x = static_cast<float>(ts.touches[0].x);
        const float y = static_cast<float>(ts.touches[0].y);
        const PlayerSnapshot snap = mPlayer->Snapshot();

        if (!mTouch.active) {
            mTouch = Touch{};
            mTouch.active = true;
            mTouch.startX = mTouch.lastX = x;
            mTouch.startY = mTouch.lastY = y;
            mTouch.startTime = now;
            mTouch.baseVolume = snap.volume;
            mTouch.baseBrightness = mBrightness;
            mTouch.basePositionMs = snap.positionMs;
            return;
        }

        const float dx = x - mTouch.startX;
        const float dy = y - mTouch.startY;
        mTouch.lastX = x;
        mTouch.lastY = y;

        if (mTouch.mode == GestureMode::None && (std::fabs(dx) > kTouchSlopPx || std::fabs(dy) > kTouchSlopPx)) {
            const bool vertical = std::fabs(dy) > std::fabs(dx);
            if (vertical && mTouch.startX < kTouchWidth / 3.0f)             mTouch.mode = GestureMode::Brightness;
            else if (vertical && mTouch.startX > kTouchWidth * 2.0f / 3.0f) mTouch.mode = GestureMode::Volume;
            else if (!vertical && snap.state != PlaybackState::Idle && snap.durationMs > 0)
                mTouch.mode = GestureMode::Scrub;
            else mTouch.mode = GestureMode::Ignored;
        }

        switch (mTouch.mode) {
            case GestureMode::Brightness: {
                SetBrightness(mTouch.baseBrightness - dy / kTouchHeight);
                ShowToast("Brilho " + std::to_string(static_cast<int>(mBrightness * 100 + 0.5f)) + "%", 0.8);
                break;
            }
            case GestureMode::Volume: {
                const int level = std::clamp(mTouch.baseVolume - static_cast<int>(dy / kTouchHeight * 100.0f), 0, 100);
                if (level != snap.volume) mPlayer->SetVolume(level);
                ShowToast("Volume " + std::to_string(level) + "%", 0.8);
                break;
            }
            case GestureMode::Scrub: {
                const int64_t deltaMs = static_cast<int64_t>(dx / kTouchWidth * kScrubSecondsPerScreenWidth * 1000.0f);
                mScrubPositionMs = std::clamp<int64_t>(mTouch.basePositionMs + deltaMs, 0, snap.durationMs);
                mScrubbing = true;
                break;
            }
            default: break;
        }
        return;
    }

    if (!mTouch.active) return;

    if (mTouch.mode == GestureMode::None && now - mTouch.startTime < kTapMaxSeconds) {
        auto& updater = Updater::OtaUpdater::Instance();
        if (mCurrentScreen == ScreenView::UpdateCenter) {
            if (updater.IsInstalling()) {
                mTouch = Touch{};
                return;
            }
            const float cardW = (mWidth >= 1920) ? 1040.0f : 860.0f;
            const float cardH = (mHeight >= 1080) ? 660.0f : 530.0f;
            const float cardX = (mWidth - cardW) * 0.5f;
            const float cardY = (mHeight - cardH) * 0.5f + 15.0f;
            const float padX = cardX + 32.0f;
            const float btnY = cardY + cardH - 64.0f;
            const float btn1W = 240.0f;
            const float btnH = 46.0f;
            const float btn2W = 160.0f;
            const float btn2X = cardX + cardW - 32.0f - btn2W;

            if (mTouch.startX >= padX && mTouch.startX <= padX + btn1W &&
                mTouch.startY >= btnY && mTouch.startY <= btnY + btnH) {
                if (updater.HasUpdateReady()) {
                    if (mPlayer) {
                        mPlayer->StopWallpaperLoop();
                        mPlayer->Stop();
                    }
                    updater.StartInstallation();
                }
                mTouch = Touch{};
                return;
            }
            if (mTouch.startX >= btn2X && mTouch.startX <= btn2X + btn2W &&
                mTouch.startY >= btnY && mTouch.startY <= btnY + btnH) {
                mCurrentScreen = ScreenView::Dashboard;
                mUpdateDialogDismissed = true;
                ShowToast("Atualizacao adiada. Pressione [X] para reabrir.", 2.5);
                mTouch = Touch{};
                return;
            }
            mTouch = Touch{};
            return;
        }

        // Toque único: alterna a barra de controle.
        if (mPlayer->HasMedia()) mOsdVisibleUntil = (mOsdVisibleUntil > now) ? 0 : now + kOsdTimeoutSec;
    } else if (mTouch.mode == GestureMode::Scrub && mScrubbing) {
        mPlayer->SeekAbsoluteMs(mScrubPositionMs);  // aplica só ao soltar: evita rajada de seeks pela rede
        ShowOsd(kOsdTimeoutSec);
    }
    mScrubbing = false;
    mTouch = Touch{};
}

void UIEngine::ShowOsd(double seconds) { mOsdVisibleUntil = Platform::NowSeconds() + seconds; }

void UIEngine::ShowToast(const std::string& text, double seconds) {
    mToast = text;
    mToastUntil = Platform::NowSeconds() + seconds;
}

// ---------------------------------------------------------------------------
// Renderização nativa OpenGL 4.3 + libmpv
// ---------------------------------------------------------------------------
void UIEngine::Render() {
    if (!mInitialized) return;
    ApplyPendingDisplayMode();

    if (mCurrentScreen == ScreenView::UpdateCenter) {
        // TELA DEDICADA PRETA DA CENTRAL DE ATUALIZAÇÃO (Sem dashboard nem vídeo no fundo)
        eglSwapInterval(mDisplay, 1);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        RenderUpdateCenter();

        eglSwapBuffers(mDisplay, mSurface);
        return;
    }

    const PlayerSnapshot snap = mPlayer->Snapshot();
    const bool hasMedia = (snap.state != PlaybackState::Idle);
    const bool isWallpaper = mPlayer->IsWallpaperActive();

    // Com mídia ou wallpaper animado em segundo plano: vsync a 60 fps.
    // Sem mídia ou wallpaper: 15 fps bastam e economizam bateria.
    const int interval = (hasMedia || isWallpaper) ? 1 : 4;
    if (interval != mSwapInterval) {
        eglSwapInterval(mDisplay, interval);
        mSwapInterval = interval;
    }

    // Preto absoluto (#000000) nos subpixels OLED do Switch
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (hasMedia) {
        mpv_render_context_update(mRender);
        mpv_opengl_fbo fbo{0, mWidth, mHeight, 0};
        int flipY = 1;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(mRender, params);

        RenderVideoOverlay(snap);

        eglSwapBuffers(mDisplay, mSurface);
        mpv_render_context_report_swap(mRender);
    } else {
        if (isWallpaper) {
            mpv_render_context_update(mRender);
            mpv_opengl_fbo fbo{0, mWidth, mHeight, 0};
            int flipY = 1;
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                {MPV_RENDER_PARAM_FLIP_Y, &flipY},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            mpv_render_context_render(mRender, params);
        }

        RenderDashboard(snap, isWallpaper);
        eglSwapBuffers(mDisplay, mSurface);
        if (isWallpaper) {
            mpv_render_context_report_swap(mRender);
        }
    }
}

void UIEngine::RenderDashboard(const PlayerSnapshot&, bool hasWallpaper) {
    mRenderer.BeginFrame(mWidth, mHeight);

    // Se houver wallpaper animado em segundo plano, aplica filtro escuro suave
    // para contraste e legibilidade impecaveis
    if (hasWallpaper) {
        mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight),
                           Color::Rgba(11, 14, 20, 160));
    } else {
        // Fundo OLED escuro limpo (#0B0E14)
        mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight),
                           Color::Hex(0x0B0E14, 1.0f));
    }

    const float ox = mBurnIn.OffsetX();
    const float oy = mBurnIn.OffsetY();
    const float dim = mBurnIn.DimmingFactor();

    // 1. Top App Bar (Discreta, elegante e perfeitamente alinhada)
    const float topY = 24.0f + oy;

    // Avatar Icon + Titulo SwitchCast
    const float brandIconX = 48.0f + ox + 14.0f;
    const float brandIconY = topY + 12.0f;
    mRenderer.DrawCircle(brandIconX, brandIconY, 14.0f, Color::Hex(0x004A77, dim));
    // Ponto vetorial de transmissao
    mRenderer.DrawCircle(brandIconX, brandIconY, 4.0f, Color::Hex(0xA8C7FA, dim));

    const float brandTextX = 48.0f + ox + 36.0f;
    mRenderer.DrawText(brandTextX, topY + 2.0f, "SwitchCast", 22.0f, Color::Hex(0xE2E2E6, dim), TextAlign::Left);

    // Chip de Versao alinhado ao lado do nome
    const float verChipX = brandTextX + mRenderer.MeasureTextWidth("SwitchCast", 22.0f) + 12.0f;
    mRenderer.DrawPill(verChipX, topY + 2.0f, 62.0f, 22.0f, Color::Hex(0x222630, dim));
    mRenderer.DrawPillOutline(verChipX, topY + 2.0f, 62.0f, 22.0f, 1.0f, Color::Hex(0x35383F, dim));
    mRenderer.DrawText(verChipX + 31.0f, topY + 7.0f, "v2.0.0", 11.0f, Color::Hex(0xA8C7FA, dim), TextAlign::Center);

    // Pill de resolucao / modo (Dock 1080p / Portatil 720p)
    const std::string modeStr = (mMode == DisplayMode::DOCKED_1080P) ? "Dock 1080p" : "Portatil 720p";
    const float badgeW = 124.0f;
    const float badgeX = mWidth - badgeW - 48.0f + ox;
    mRenderer.DrawPill(badgeX, topY, badgeW, 26.0f, Color::Hex(0x1D2024, dim));
    mRenderer.DrawPillOutline(badgeX, topY, badgeW, 26.0f, 1.0f, Color::Hex(0x35383F, dim));
    mRenderer.DrawText(badgeX + badgeW * 0.5f, topY + 7.0f, modeStr, 12.0f, Color::Hex(0xC4C7C5, dim), TextAlign::Center);

    // Chip de Notificacao de Atualizacao (se houver update disponivel ou em download)
    auto& updater = Updater::OtaUpdater::Instance();
    if (updater.HasUpdateReady() || updater.IsDownloading()) {
        const std::string otaNotice = updater.IsDownloading()
            ? "[ (X) Baixando atualizacao... ]"
            : "[ (X) Atualizacao disponivel ]";
        const float otaW = mRenderer.MeasureTextWidth(otaNotice, 12.0f) + 24.0f;
        const float otaX = badgeX - otaW - 12.0f;
        mRenderer.DrawPill(otaX, topY, otaW, 26.0f, Color::Hex(0x004A77, dim));
        mRenderer.DrawPillOutline(otaX, topY, otaW, 26.0f, 1.0f, Color::Hex(0x2D73A8, dim));
        mRenderer.DrawText(otaX + otaW * 0.5f, topY + 7.0f, otaNotice, 12.0f, Color::Hex(0xC2E7FF, dim), TextAlign::Center);
    } else {
        const AppletType at = appletGetAppletType();
        if (at != AppletType_Application && at != AppletType_SystemApplication) {
            const std::string appletNotice = "Modo Album (16MB) - Segure R ao abrir um jogo para 128MB";
            const float noticeW = mRenderer.MeasureTextWidth(appletNotice, 12.0f) + 24.0f;
            const float noticeX = badgeX - noticeW - 12.0f;
            mRenderer.DrawPill(noticeX, topY, noticeW, 26.0f, Color::Hex(0x2D2518, dim));
            mRenderer.DrawPillOutline(noticeX, topY, noticeW, 26.0f, 1.0f, Color::Hex(0x524329, dim));
            mRenderer.DrawText(noticeX + noticeW * 0.5f, topY + 7.0f, appletNotice, 12.0f, Color::Hex(0xF5CF8E, dim), TextAlign::Center);
        }
    }

    // 2. Card Central Master (Surface Container #1D2024, radius 28px)
    const float cardW = (mWidth >= 1920) ? 1000.0f : 840.0f;
    const float cardH = (mHeight >= 1080) ? 450.0f : 370.0f;
    const float cx = (mWidth - cardW) * 0.5f + ox;
    const float cy = (mHeight - cardH) * 0.5f - 8.0f + oy;
    const float cardRadius = 28.0f;

    // Fundo do card e contorno sutil
    mRenderer.DrawRoundedRect(cx, cy, cardW, cardH, cardRadius, Color::Hex(0x1D2024, dim));
    mRenderer.DrawRoundedRectOutline(cx, cy, cardW, cardH, cardRadius, 1.0f, Color::Hex(0x35383F, dim));

    // Avatar Central com Pulso Suave (Primary Container #004A77 + Acento #A8C7FA)
    const float pulse = (std::sin(mPulseTimer * 2.2f) + 1.0f) * 0.5f;
    const float iconCenterX = cx + cardW * 0.5f;
    const float iconCenterY = cy + 54.0f;

    // Onda externa tonal discreta
    mRenderer.DrawCircle(iconCenterX, iconCenterY, 32.0f + pulse * 6.0f, Color::Hex(0x004A77, (0.20f + 0.15f * pulse) * dim));
    // Circulo base do avatar
    mRenderer.DrawCircle(iconCenterX, iconCenterY, 26.0f, Color::Hex(0x004A77, dim));
    // Ícone de Transmissao vetorial no centro
    mRenderer.DrawRoundedRect(iconCenterX - 11.0f, iconCenterY - 8.0f, 22.0f, 16.0f, 3.0f, Color::Hex(0xA8C7FA, dim));
    mRenderer.DrawRoundedRect(iconCenterX - 9.0f, iconCenterY - 6.0f, 18.0f, 12.0f, 2.0f, Color::Hex(0x004A77, dim));
    mRenderer.DrawCircle(iconCenterX - 5.0f, iconCenterY + 1.0f, 3.0f, Color::Hex(0xA8C7FA, dim));

    // Titulo e Subtitulo com tipografia aumentada e perfeitamente centralizada
    mRenderer.DrawText(iconCenterX, cy + 96.0f, "Pronto para transmitir", 24.0f, Color::Hex(0xE2E2E6, dim), TextAlign::Center);
    mRenderer.DrawText(iconCenterX, cy + 126.0f, "Envie videos ou musicas do celular, computador ou navegador", 15.0f, Color::Hex(0xC4C7C5, dim), TextAlign::Center);

    // Box interno para IP e Conectividade (Surface Container High #24282F, radius 16px)
    const float boxW = cardW - 64.0f;
    const float boxH = 92.0f;
    const float boxX = cx + 32.0f;
    const float boxY = cy + 154.0f;

    mRenderer.DrawRoundedRect(boxX, boxY, boxW, boxH, 16.0f, Color::Hex(0x24282F, dim));
    mRenderer.DrawRoundedRectOutline(boxX, boxY, boxW, boxH, 16.0f, 1.0f, Color::Hex(0x333740, dim));

    const std::string ipText = mAddressLine.empty() ? "Obtendo endereco de rede local..." : mAddressLine;
    mRenderer.DrawText(boxX + boxW * 0.5f, boxY + 20.0f, ipText, 21.0f, Color::Hex(0x6DD58C, dim), TextAlign::Center);

    const std::string subNet = "Renderizador DLNA/UPnP Ativo - Descoberta SSDP - Servidor REST e WebSocket";
    mRenderer.DrawText(boxX + boxW * 0.5f, boxY + 54.0f, subNet, 13.0f, Color::Hex(0x8E918F, dim), TextAlign::Center);

    // 3 Status Chips informativos na base do card com espacamento harmonioso
    const float chipRowY = cy + 264.0f;
    const float chipGap = 12.0f;
    const float chipW = (boxW - 2.0f * chipGap) / 3.0f;

    // Chip 1: Motor NVDEC
    mRenderer.DrawRoundedRect(boxX, chipRowY, chipW, 40.0f, 12.0f, Color::Hex(0x20232A, dim));
    mRenderer.DrawRoundedRectOutline(boxX, chipRowY, chipW, 40.0f, 12.0f, 1.0f, Color::Hex(0x2F333B, dim));
    mRenderer.DrawText(boxX + 12.0f, chipRowY + 14.0f, "NVDEC HW", 12.0f, Color::Hex(0xA8C7FA, dim), TextAlign::Left);
    mRenderer.DrawText(boxX + chipW - 12.0f, chipRowY + 14.0f, "H.264 / HEVC", 12.0f, Color::Hex(0xC4C7C5, dim), TextAlign::Right);

    // Chip 2: Servidor FTP
    const float chip2X = boxX + chipW + chipGap;
    mRenderer.DrawRoundedRect(chip2X, chipRowY, chipW, 40.0f, 12.0f, Color::Hex(0x20232A, dim));
    mRenderer.DrawRoundedRectOutline(chip2X, chipRowY, chipW, 40.0f, 12.0f, 1.0f, Color::Hex(0x2F333B, dim));
    mRenderer.DrawText(chip2X + 12.0f, chipRowY + 14.0f, "Servidor FTP", 12.0f, Color::Hex(0x6DD58C, dim), TextAlign::Left);
    mRenderer.DrawText(chip2X + chipW - 12.0f, chipRowY + 14.0f, "Porta 5000/21", 12.0f, Color::Hex(0xC4C7C5, dim), TextAlign::Right);

    // Chip 3: Sistema
    const float chip3X = boxX + 2.0f * (chipW + chipGap);
    mRenderer.DrawRoundedRect(chip3X, chipRowY, chipW, 40.0f, 12.0f, Color::Hex(0x20232A, dim));
    mRenderer.DrawRoundedRectOutline(chip3X, chipRowY, chipW, 40.0f, 12.0f, 1.0f, Color::Hex(0x2F333B, dim));
    mRenderer.DrawText(chip3X + 12.0f, chipRowY + 14.0f, "Renderizador", 12.0f, Color::Hex(0xA8C7FA, dim), TextAlign::Left);
    mRenderer.DrawText(chip3X + chipW - 12.0f, chipRowY + 14.0f, "OpenGL 4.3", 12.0f, Color::Hex(0xC4C7C5, dim), TextAlign::Right);

    // Dica de compatibilidade elegante
    const std::string compText = "Compativel com BubbleUPnP, Web Video Caster, VLC, mupnp, AirPlay e HTTP";
    mRenderer.DrawText(iconCenterX, cy + cardH - 26.0f, compText, 12.0f, Color::Hex(0x8E918F, dim), TextAlign::Center);

    // 3. Barra Inferior (Legenda de Acoes em Pilula Centralizada)
    const float botY = mHeight - 44.0f + oy;
    const std::string buttonsLegend = (updater.HasUpdateReady() || updater.IsDownloading())
        ? "(A) Pausar   (X) Central de Atualizacoes   (L/R) Volume   (-) Mudo   (+) Sair"
        : "(A) Pausar   (L/R) Volume   (-) Mudo   (Toque) Brilho e Volume   (+) Sair";
    const float legendW = mRenderer.MeasureTextWidth(buttonsLegend, 13.0f) + 36.0f;
    const float legendX = (mWidth - legendW) * 0.5f + ox;

    mRenderer.DrawPill(legendX, botY - 3.0f, legendW, 28.0f, Color::Hex(0x1A1D24, dim));
    mRenderer.DrawPillOutline(legendX, botY - 3.0f, legendW, 28.0f, 1.0f, Color::Hex(0x2E333E, dim));
    mRenderer.DrawText(mWidth * 0.5f + ox, botY + 4.5f, buttonsLegend, 13.0f, Color::Hex(0xC4C7C5, dim), TextAlign::Center);

    // 4. Toast flutuante de notificacao se houver
    const double now = Platform::NowSeconds();
    if (now < mToastUntil && !mToast.empty()) {
        const float tw = mRenderer.MeasureTextWidth(mToast, 15.0f) + 44.0f;
        const float th = 38.0f;
        const float tx = (mWidth - tw) * 0.5f;
        const float ty = 50.0f;
        mRenderer.DrawPill(tx, ty, tw, th, Color::Hex(0x1D2024, 0.96f));
        mRenderer.DrawPillOutline(tx, ty, tw, th, 1.0f, Color::Hex(0x44474E, 0.9f));
        mRenderer.DrawText(mWidth * 0.5f, ty + 10.0f, mToast, 15.0f, Color::Hex(0xE2E2E6, 1.0f), TextAlign::Center);
    }

    mRenderer.EndFrame();
}

void UIEngine::RenderVideoOverlay(const PlayerSnapshot& snap) {
    const double now = Platform::NowSeconds();
    const bool showControls = (now < mOsdVisibleUntil);
    const bool showToast = (now < mToastUntil && !mToast.empty());

    if (!showControls && !showToast) return;

    mRenderer.BeginFrame(mWidth, mHeight);

    if (showControls) {
        // Barra Superior translúcida flutuante
        const float topH = 72.0f;
        mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), topH, Color::Hex(0x0B0E14, 0.85f));
        mRenderer.DrawRect(0.0f, topH - 1.0f, static_cast<float>(mWidth), 1.0f, Color::Hex(0x2E333E, 0.6f));

        const std::string title = snap.title.empty() ? "Transmissao de Video" : snap.title;
        mRenderer.DrawText(48.0f, 24.0f, title, 20.0f, Color::Hex(0xE2E2E6, 1.0f), TextAlign::Left);

        // Chip de Estado de Reprodução
        const bool isPaused = (snap.state == PlaybackState::Paused);
        const std::string statusStr = isPaused ? "PAUSADO" : "REPRODUZINDO";
        const Color statusBg = isPaused ? Color::Hex(0x3B2526) : Color::Hex(0x163422);
        const Color statusFg = isPaused ? Color::Hex(0xF2B8B5) : Color::Hex(0x6DD58C);

        const float statW = mRenderer.MeasureTextWidth(statusStr, 12.0f) + 24.0f;
        const float statX = mWidth - 48.0f - statW;
        mRenderer.DrawPill(statX, 22.0f, statW, 26.0f, statusBg);
        mRenderer.DrawPillOutline(statX, 22.0f, statW, 26.0f, 1.0f, statusFg);
        mRenderer.DrawText(statX + statW * 0.5f, 29.0f, statusStr, 12.0f, statusFg, TextAlign::Center);

        // Barra Inferior: Controles, Scrubber em Pílula e Metadados
        const float botH = 104.0f;
        const float botY = static_cast<float>(mHeight) - botH;
        mRenderer.DrawRect(0.0f, botY, static_cast<float>(mWidth), botH, Color::Hex(0x0B0E14, 0.90f));
        mRenderer.DrawRect(0.0f, botY, static_cast<float>(mWidth), 1.0f, Color::Hex(0x2E333E, 0.6f));

        // Trilha de progresso em pilula (#33353A + preenchimento tonal #A8C7FA)
        const float barX = 48.0f;
        const float barY = botY + 24.0f;
        const float barW = static_cast<float>(mWidth) - 96.0f;
        const float barH = 8.0f;
        const int64_t curPos = mScrubbing ? mScrubPositionMs : snap.positionMs;
        const float progress = (snap.durationMs > 0) ? (static_cast<float>(curPos) / static_cast<float>(snap.durationMs)) : 0.0f;

        mRenderer.DrawProgressBar(barX, barY, barW, barH, progress, Color::Hex(0x33353A), Color::Hex(0xA8C7FA));

        // Thumb circular no ponto atual do scrubber
        if (progress > 0.005f && progress < 0.995f) {
            const float thumbX = barX + barW * progress;
            const float thumbY = barY + barH * 0.5f;
            mRenderer.DrawCircle(thumbX, thumbY, 7.0f, Color::Hex(0xA8C7FA));
            mRenderer.DrawCircle(thumbX, thumbY, 3.0f, Color::Hex(0x004A77));
        }

        // Tempos formatados (decorrido / total)
        const std::string timeStr = Text::FormatClock(curPos) + " / " + Text::FormatClock(snap.durationMs);
        mRenderer.DrawText(barX, botY + 48.0f, timeStr, 14.0f, Color::Hex(0xC4C7C5), TextAlign::Left);

        // Legenda de controles Joy-Con no centro
        const std::string mediaControls = "(A) Pausar   (D-Pad) Avancar/Voltar   (X) Audio   (Y) Legendas";
        mRenderer.DrawText(static_cast<float>(mWidth) * 0.5f, botY + 48.0f, mediaControls, 13.0f, Color::Hex(0x8E918F), TextAlign::Center);

        // Volume / Mudo em pílula tonal no canto direito
        const std::string volStr = snap.muted ? "MUDO" : ("VOL " + std::to_string(snap.volume) + "%");
        const float volW = mRenderer.MeasureTextWidth(volStr, 12.0f) + 20.0f;
        const float volX = static_cast<float>(mWidth) - 48.0f - volW;
        mRenderer.DrawPill(volX, botY + 44.0f, volW, 24.0f, Color::Hex(0x1D2024));
        mRenderer.DrawPillOutline(volX, botY + 44.0f, volW, 24.0f, 1.0f, Color::Hex(0x35383F));
        mRenderer.DrawText(volX + volW * 0.5f, botY + 50.0f, volStr, 12.0f, Color::Hex(0xC4C7C5), TextAlign::Center);
    }

    // Toast de notificacao
    if (showToast) {
        const float tw = mRenderer.MeasureTextWidth(mToast, 15.0f) + 44.0f;
        const float th = 38.0f;
        const float tx = (static_cast<float>(mWidth) - tw) * 0.5f;
        const float ty = showControls ? 88.0f : 44.0f;
        mRenderer.DrawPill(tx, ty, tw, th, Color::Hex(0x1D2024, 0.96f));
        mRenderer.DrawPillOutline(tx, ty, tw, th, 1.0f, Color::Hex(0x44474E, 0.9f));
        mRenderer.DrawText(static_cast<float>(mWidth) * 0.5f, ty + 10.0f, mToast, 15.0f, Color::Hex(0xE2E2E6, 1.0f), TextAlign::Center);
    }

    mRenderer.EndFrame();
}

void UIEngine::RenderUpdateCenter() {
    auto& updater = Updater::OtaUpdater::Instance();
    const bool isDownloading = updater.IsDownloading();
    const bool isInstalling = updater.IsInstalling();
    auto info = updater.GetPendingUpdateInfo();

    mRenderer.BeginFrame(mWidth, mHeight);

    // 1. Fundo Base Surface (#101318)
    mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight),
                       Color::Hex(0x101318, 1.0f));

    // Barra de Status Superior (Discreta e Elegante)
    const float topY = 18.0f;
    mRenderer.DrawPill(40.0f, topY, 140.0f, 28.0f, Color::Hex(0x191C20));
    mRenderer.DrawPillOutline(40.0f, topY, 140.0f, 28.0f, 1.0f, Color::Hex(0x2E333E));
    mRenderer.DrawText(40.0f + 70.0f, topY + 7.5f, "SwitchCast", 13.0f, Color::Hex(0xC4C7C5), TextAlign::Center);
    mRenderer.DrawText(mWidth - 40.0f, topY + 7.5f, "Central de Atualizacoes", 13.0f, Color::Hex(0x8E918F), TextAlign::Right);

    // 2. Card Central Master (#1D2024, radius 28px)
    const float cardW = (mWidth >= 1920) ? 1040.0f : 860.0f;
    const float cardH = (mHeight >= 1080) ? 620.0f : 496.0f;
    const float cardX = (mWidth - cardW) * 0.5f;
    const float cardY = (mHeight - cardH) * 0.5f + 10.0f;
    const float cardRadius = 28.0f;

    // Superficie do Container
    mRenderer.DrawRoundedRect(cardX, cardY, cardW, cardH, cardRadius, Color::Hex(0x1D2024));
    mRenderer.DrawRoundedRectOutline(cardX, cardY, cardW, cardH, cardRadius, 1.0f, Color::Hex(0x35383F));

    const float padX = cardX + 32.0f;

    // 3. Cabecalho de Atualizacao do Sistema
    // Avatar Icon Container
    const float iconX = padX + 24.0f;
    const float iconY = cardY + 48.0f;
    mRenderer.DrawCircle(iconX, iconY, 24.0f, Color::Hex(0x004A77));

    // Ícone de Download / Atualizacao
    mRenderer.DrawRect(iconX - 2.0f, iconY - 8.0f, 4.0f, 10.0f, Color::Hex(0xA8C7FA));
    mRenderer.DrawRoundedRect(iconX - 6.0f, iconY + 1.0f, 12.0f, 4.0f, 2.0f, Color::Hex(0xA8C7FA));
    mRenderer.DrawRoundedRect(iconX - 8.0f, iconY + 7.0f, 16.0f, 3.0f, 1.5f, Color::Hex(0xA8C7FA));

    // Titulo e Subtitulo com tipografia aumentada e alinhamento consistente
    mRenderer.DrawText(padX + 60.0f, cardY + 26.0f, "Atualizacao de sistema disponivel", 22.0f, Color::Hex(0xE2E2E6), TextAlign::Left);
    std::string subTitle = "SwitchCast v" + (info.version.empty() ? "2.0.0" : info.version) + " - Pronto para instalar";
    mRenderer.DrawText(padX + 60.0f, cardY + 54.0f, subTitle, 14.0f, Color::Hex(0xC4C7C5), TextAlign::Left);

    // Chip de Categoria no Topo Direito
    const float chipW = 100.0f;
    const float chipH = 28.0f;
    const float chipX = cardX + cardW - 32.0f - chipW;
    const float chipY = cardY + 34.0f;
    mRenderer.DrawPill(chipX, chipY, chipW, chipH, Color::Hex(0x282A2F));
    mRenderer.DrawPillOutline(chipX, chipY, chipW, chipH, 1.0f, Color::Hex(0x444746));
    mRenderer.DrawText(chipX + chipW * 0.5f, chipY + 8.0f, "Oficial", 12.0f, Color::Hex(0xA8C7FA), TextAlign::Center);

    // Divisor Suave
    mRenderer.DrawRect(padX, cardY + 86.0f, cardW - 64.0f, 1.0f, Color::Hex(0x2E3136));

    // =========================================================================
    // ESTADO 1: INSTALACAO EM ANDAMENTO
    // =========================================================================
    if (isInstalling) {
        float curY = cardY + 104.0f;
        mRenderer.DrawText(padX, curY, "Instalando componentes do sistema...", 22.0f, Color::Hex(0xE2E2E6), TextAlign::Left);
        curY += 28.0f;

        std::string statusText = updater.GetInstallStatus();
        if (statusText.empty()) statusText = "Processando gravacao no armazenamento microSD...";
        mRenderer.DrawText(padX, curY, statusText, 14.0f, Color::Hex(0xC4C7C5), TextAlign::Left);
        curY += 24.0f;

        const float frac = updater.GetInstallFraction();
        const int pct = std::clamp(static_cast<int>(frac * 100.0f), 0, 100);

        // Card do Indicador de Progresso
        const float boxW = cardW - 64.0f;
        const float boxH = 96.0f;
        mRenderer.DrawRoundedRect(padX, curY, boxW, boxH, 18.0f, Color::Hex(0x24282F));
        mRenderer.DrawRoundedRectOutline(padX, curY, boxW, boxH, 18.0f, 1.0f, Color::Hex(0x333740));

        const float barX = padX + 24.0f;
        const float barY = curY + 28.0f;
        const float barW = boxW - 130.0f;
        const float barH = 12.0f;

        mRenderer.DrawProgressBar(barX, barY, barW, barH, frac, Color::Hex(0x33353A), Color::Hex(0xA8C7FA));

        char pctBuf[32];
        std::snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
        mRenderer.DrawText(padX + boxW - 24.0f, barY - 4.0f, pctBuf, 22.0f, Color::Hex(0xA8C7FA), TextAlign::Right);
        mRenderer.DrawText(barX, curY + 58.0f, "Sincronizando dados no cartao microSD...", 13.0f, Color::Hex(0x8E918F), TextAlign::Left);

        curY += boxH + 20.0f;

        // Chips de Etapas Horizontais
        const float chipGroupW = cardW - 64.0f;
        const float stepChipW = (chipGroupW - 30.0f) / 4.0f;
        const float stepChipH = 34.0f;

        struct StepItem { const char* name; float threshold; };
        const StepItem steps[4] = {
            {"SHA-256", 0.35f},
            {"Backup", 0.65f},
            {"Gravacao", 0.95f},
            {"Reinicio", 1.0f}
        };

        for (int i = 0; i < 4; ++i) {
            const float sx = padX + i * (stepChipW + 10.0f);
            const bool isDone = (frac >= steps[i].threshold);
            const bool isActive = (!isDone && (i == 0 || frac >= steps[i-1].threshold));

            Color bg = isDone ? Color::Hex(0x133E26) : (isActive ? Color::Hex(0x1F3854) : Color::Hex(0x282A2F));
            Color border = isDone ? Color::Hex(0x2B5C3D) : (isActive ? Color::Hex(0x3E5E85) : Color::Hex(0x383B40));
            Color text = isDone ? Color::Hex(0xA8EAB4) : (isActive ? Color::Hex(0xA8C7FA) : Color::Hex(0x8E918F));

            mRenderer.DrawPill(sx, curY, stepChipW, stepChipH, bg);
            mRenderer.DrawPillOutline(sx, curY, stepChipW, stepChipH, 1.0f, border);

            std::string label = (isDone ? "OK " : (isActive ? "> " : "  ")) + std::string(steps[i].name);
            mRenderer.DrawText(sx + stepChipW * 0.5f, curY + 11.0f, label, 12.0f, text, TextAlign::Center);
        }
        curY += stepChipH + 20.0f;

        // Banner de Alerta
        const float warnW = cardW - 64.0f;
        const float warnH = 40.0f;
        mRenderer.DrawRoundedRect(padX, curY, warnW, warnH, 14.0f, Color::Hex(0x2A2214));
        mRenderer.DrawRoundedRectOutline(padX, curY, warnW, warnH, 14.0f, 1.0f, Color::Hex(0x5A4300));
        mRenderer.DrawText(padX + warnW * 0.5f, curY + 13.0f, "Mantenha o console ligado ate a conclusao da atualizacao.", 13.0f, Color::Hex(0xFEE48A), TextAlign::Center);

        mRenderer.EndFrame();
        return;
    }

    // =========================================================================
    // ESTADO 2: DOWNLOAD EM ANDAMENTO
    // =========================================================================
    if (isDownloading) {
        float curY = cardY + 104.0f;
        mRenderer.DrawText(padX, curY, "Baixando pacote do sistema...", 22.0f, Color::Hex(0xE2E2E6), TextAlign::Left);
        curY += 26.0f;
        mRenderer.DrawText(padX, curY, "A transferencia e realizada em segundo plano sem interromper o uso.", 14.0f, Color::Hex(0xC4C7C5), TextAlign::Left);
        curY += 24.0f;

        const float boxW = cardW - 64.0f;
        const float boxH = 100.0f;
        mRenderer.DrawRoundedRect(padX, curY, boxW, boxH, 18.0f, Color::Hex(0x24282F));
        mRenderer.DrawRoundedRectOutline(padX, curY, boxW, boxH, 18.0f, 1.0f, Color::Hex(0x333740));

        const float frac = updater.GetDownloadFraction();
        const size_t downBytes = updater.GetDownloadedBytes();
        const size_t totBytes = updater.GetTotalBytes();
        const int pct = std::clamp(static_cast<int>(frac * 100.0f), 0, 100);

        const float barX = padX + 24.0f;
        const float barY = curY + 30.0f;
        const float barW = boxW - 48.0f;
        const float barH = 12.0f;

        mRenderer.DrawProgressBar(barX, barY, barW, barH, frac, Color::Hex(0x33353A), Color::Hex(0xA8C7FA));

        char infoBuf[64];
        const float downMb = static_cast<float>(downBytes) / (1024.0f * 1024.0f);
        const float totMb = static_cast<float>(totBytes) / (1024.0f * 1024.0f);
        if (totBytes > 0) {
            std::snprintf(infoBuf, sizeof(infoBuf), "%.1f MB de %.1f MB (%d%%)", downMb, totMb, pct);
        } else {
            std::snprintf(infoBuf, sizeof(infoBuf), "%.1f MB transferidos", downMb);
        }
        mRenderer.DrawText(barX, curY + 58.0f, infoBuf, 14.0f, Color::Hex(0xA8C7FA), TextAlign::Left);
        mRenderer.DrawText(padX + boxW - 24.0f, curY + 58.0f, "Buffer SDMMC 60 FPS", 12.0f, Color::Hex(0x8E918F), TextAlign::Right);

        mRenderer.EndFrame();
        return;
    }

    // =========================================================================
    // ESTADO 3: ATUALIZACAO PRONTA (CARDS DE DESTAQUE + BOTOES DE ACAO)
    // =========================================================================
    float curY = cardY + 102.0f;
    mRenderer.DrawText(padX, curY, "NOVIDADES DA VERSAO", 12.0f, Color::Hex(0x8E918F), TextAlign::Left);
    curY += 20.0f;

    const float tileW = cardW - 64.0f;
    const float tileH = (mHeight >= 1080) ? 68.0f : 54.0f;
    const float tileSpacing = (mHeight >= 1080) ? 12.0f : 8.0f;

    struct FeatureItem {
        const char* code;
        Color avatarBg;
        Color avatarText;
        const char* title;
        const char* desc;
    };

    const FeatureItem items[4] = {
        {"UI", Color::Hex(0x1F3854), Color::Hex(0xA8C7FA), "Interface Renovada e Fluida", "Navegacao suave, superficies limpas e tipografia nitida."},
        {"60", Color::Hex(0x133E26), Color::Hex(0xA8EAB4), "Reproducao Fluida a 60 FPS", "Streaming de video em tempo real com aceleracao por hardware."},
        {"SD", Color::Hex(0x3B2456), Color::Hex(0xD0BCFF), "Estabilidade de Armazenamento", "Gravacao direta e sincronizacao aprimorada no cartao microSD."},
        {"OK", Color::Hex(0x453300), Color::Hex(0xFEE48A), "Protecao e Seguranca", "Backup automatico e restauracao de emergencia segurando L + R."}
    };

    for (int i = 0; i < 4; ++i) {
        const float ty = curY + i * (tileH + tileSpacing);

        // Container do Card
        mRenderer.DrawRoundedRect(padX, ty, tileW, tileH, 16.0f, Color::Hex(0x24282F));
        mRenderer.DrawRoundedRectOutline(padX, ty, tileW, tileH, 16.0f, 1.0f, Color::Hex(0x2E3238));

        // Avatar Circular (32px diametro)
        const float avRadius = 16.0f;
        const float avX = padX + 24.0f;
        const float avY = ty + tileH * 0.5f;
        mRenderer.DrawCircle(avX, avY, avRadius, items[i].avatarBg);
        mRenderer.DrawText(avX, avY - 5.5f, items[i].code, 11.0f, items[i].avatarText, TextAlign::Center);

        // Titulo e Descricao com alinhamento e espacamento harmonioso
        const float textX = padX + 52.0f;
        mRenderer.DrawText(textX, ty + 9.0f, items[i].title, 14.0f, Color::Hex(0xE2E2E6), TextAlign::Left);
        mRenderer.DrawText(textX, ty + 29.0f, items[i].desc, 12.0f, Color::Hex(0x8E918F), TextAlign::Left);
    }

    // Botoes de Acao na base do Card
    const float btnY = cardY + cardH - 64.0f;
    const float btnH = 44.0f;

    // Botao 1: Filled Button (Pilula Primaria #A8C7FA)
    const float btn1W = 240.0f;
    mRenderer.DrawPill(padX, btnY, btn1W, btnH, Color::Hex(0xA8C7FA));

    // Joy-Con Y Icon Badge
    const float circleX = padX + 22.0f;
    const float circleY = btnY + btnH * 0.5f;
    mRenderer.DrawCircle(circleX, circleY, 11.0f, Color::Hex(0x004A77));
    mRenderer.DrawText(circleX, circleY - 6.0f, "Y", 12.0f, Color::Hex(0xFFD600), TextAlign::Center);
    mRenderer.DrawText(padX + 42.0f + (btn1W - 42.0f) * 0.5f, btnY + 15.0f, "Reiniciar agora", 14.0f, Color::Hex(0x062E6F), TextAlign::Center);

    // Botao 2: Outlined Button (Pilula Secundaria #282A2F + Contorno #444746)
    const float btn2W = 160.0f;
    const float btn2X = cardX + cardW - 32.0f - btn2W;
    mRenderer.DrawPill(btn2X, btnY, btn2W, btnH, Color::Hex(0x282A2F));
    mRenderer.DrawPillOutline(btn2X, btnY, btn2W, btnH, 1.0f, Color::Hex(0x444746));

    const float circle2X = btn2X + 22.0f;
    mRenderer.DrawCircle(circle2X, circleY, 11.0f, Color::Hex(0x35383F));
    mRenderer.DrawText(circle2X, circleY - 6.0f, "B", 12.0f, Color::Hex(0xFF6B8B), TextAlign::Center);
    mRenderer.DrawText(btn2X + 40.0f + (btn2W - 40.0f) * 0.5f, btnY + 15.0f, "Mais tarde", 14.0f, Color::Hex(0xE2E2E6), TextAlign::Center);

    mRenderer.EndFrame();
}

}  // namespace SwitchCast
