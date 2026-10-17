// ui/UIEngine.cpp
#include "UIEngine.hpp"

#include <EGL/eglext.h>
#include <glad/glad.h>

#include <algorithm>
#include <cmath>

#include "../network/ServerBridge.hpp"
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
        mAddressLine = ip + "   |   HTTP " + std::to_string(id.httpPort) + "   |   WS " + std::to_string(id.wsPort);
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

    // Soltou o dedo.
    if (mTouch.mode == GestureMode::None && now - mTouch.startTime < kTapMaxSeconds) {
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

    // Se houver vídeo animado no fundo, aplica um filtro escuro sutil para garantir
    // legibilidade de nível de cinema e contraste premium em todos os textos e cards
    if (hasWallpaper) {
        mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight),
                           Color::Rgba(0, 0, 0, 115));
    }

    const float ox = mBurnIn.OffsetX();
    const float oy = mBurnIn.OffsetY();
    const float dim = mBurnIn.DimmingFactor();

    // 1. Header Superior com identidade Switch (Ciano + Vermelho Neon)
    const float topY = 28.0f + oy;
    const float switchW = mRenderer.MeasureTextWidth("SWITCH", 26.0f);
    mRenderer.DrawText(48.0f + ox, topY, "SWITCH", 26.0f, Color::Hex(0x00E5FF, dim), TextAlign::Left);
    mRenderer.DrawText(48.0f + ox + switchW, topY, "CAST", 26.0f, Color::Hex(0xFF2A55, dim), TextAlign::Left);

    const float brandW = switchW + mRenderer.MeasureTextWidth("CAST", 26.0f);
    mRenderer.DrawText(48.0f + ox + brandW + 14.0f, topY + 7.0f, "v1.2.0 by Aleff959", 14.0f, Color::Hex(0x8C9FB5, dim), TextAlign::Left);

    // Pill de resolução / modo
    const std::string modeStr = (mMode == DisplayMode::DOCKED_1080P) ? "DOCKED 1080p" : "OLED 720p";
    const float badgeW = 130.0f;
    const float badgeX = mWidth - badgeW - 48.0f + ox;
    mRenderer.DrawRoundedRect(badgeX, topY - 2.0f, badgeW, 28.0f, 14.0f, Color::Rgba(18, 28, 42, 220 * dim));
    mRenderer.DrawRectOutline(badgeX, topY - 2.0f, badgeW, 28.0f, 1.0f, Color::Hex(0x00A8FF, 0.4f * dim));
    mRenderer.DrawText(badgeX + badgeW / 2.0f, topY + 5.0f, modeStr, 14.0f, Color::Hex(0x40C8FF, dim), TextAlign::Center);

    // Se estiver em modo Applet (Álbum), exibe aviso em amarelo discreto
    const AppletType at = appletGetAppletType();
    if (at != AppletType_Application && at != AppletType_SystemApplication) {
        const std::string appletNotice = "MODO ALBUM (16MB CACHE) - Use Title Takeover (R+Jogo) p/ 128MB";
        const float noticeW = mRenderer.MeasureTextWidth(appletNotice, 13.0f) + 24.0f;
        const float noticeX = badgeX - noticeW - 16.0f;
        mRenderer.DrawRoundedRect(noticeX, topY - 2.0f, noticeW, 28.0f, 14.0f, Color::Rgba(45, 35, 10, 220 * dim));
        mRenderer.DrawRectOutline(noticeX, topY - 2.0f, noticeW, 28.0f, 1.0f, Color::Hex(0xFFAA00, 0.5f * dim));
        mRenderer.DrawText(noticeX + noticeW / 2.0f, topY + 5.0f, appletNotice, 13.0f, Color::Hex(0xFFCC00, dim), TextAlign::Center);
    }

    // 2. Card Central
    const float cardW = (mWidth >= 1920) ? 880.0f : 700.0f;
    const float cardH = (mHeight >= 1080) ? 380.0f : 300.0f;
    const float cx = (mWidth - cardW) / 2.0f + ox;
    const float cy = (mHeight - cardH) / 2.0f - 15.0f + oy;

    // Fundo do card com borda sutil Joy-Con
    mRenderer.DrawRoundedRect(cx, cy, cardW, cardH, 20.0f, Color::Rgba(12, 16, 24, 235 * dim));
    mRenderer.DrawRectOutline(cx, cy, cardW, cardH, 1.5f, Color::Hex(0x2E1E34, dim));

    // Círculo com pulso dinâmico (Joy-Con Neon Blue + Neon Red)
    const float pulse = (std::sin(mPulseTimer * 2.5f) + 1.0f) * 0.5f;
    const float iconY = cy + 42.0f;
    const float iconX = cx + cardW / 2.0f;
    // Onda vermelha externa pulsante
    mRenderer.DrawCircle(iconX, iconY, 22.0f + pulse * 6.0f, Color::Hex(0xFF2A55, (0.15f + 0.18f * pulse) * dim));
    // Onda ciano intermediária
    mRenderer.DrawCircle(iconX, iconY, 15.0f + pulse * 2.0f, Color::Hex(0x00E5FF, 0.45f * dim));
    // Centro
    mRenderer.DrawCircle(iconX, iconY, 10.0f, Color::Hex(0x00E5FF, 0.9f * dim));
    mRenderer.DrawCircle(iconX, iconY, 4.0f, Color::Hex(0xFFFFFF, dim));

    // Texto de status
    mRenderer.DrawText(iconX, iconY + 28.0f, "PRONTO PARA TRANSMITIR", 22.0f, Color::Hex(0xFFFFFF, dim), TextAlign::Center);
    mRenderer.DrawText(iconX, iconY + 58.0f, "Envie videos de celulares, PCs ou apps na mesma rede", 15.0f, Color::Hex(0x8A9FB8, dim), TextAlign::Center);

    // Box interno com endereço IP e portas
    const float boxW = cardW - 70.0f;
    const float boxH = 88.0f;
    const float boxX = cx + 35.0f;
    const float boxY = iconY + 86.0f;
    mRenderer.DrawRoundedRect(boxX, boxY, boxW, boxH, 12.0f, Color::Rgba(6, 9, 14, 255 * dim));
    mRenderer.DrawRectOutline(boxX, boxY, boxW, boxH, 1.0f, Color::Hex(0x182638, dim));

    const std::string ipText = mAddressLine.empty() ? "Obtendo endereco IP..." : mAddressLine;
    mRenderer.DrawText(boxX + boxW / 2.0f, boxY + 20.0f, ipText, 20.0f, Color::Hex(0x38EF7D, dim), TextAlign::Center);

    const std::string subNet = "DLNA / UPnP Renderer Ativo  *  SSDP Broadcast  *  REST / WebSocket";
    mRenderer.DrawText(boxX + boxW / 2.0f, boxY + 52.0f, subNet, 14.0f, Color::Hex(0x5A708C, dim), TextAlign::Center);

    // Dica de compatibilidade
    const std::string compText = "Compativel com BubbleUPnP, Web Video Caster, VLC, mupnp e comandos HTTP";
    mRenderer.DrawText(iconX, cy + cardH - 28.0f, compText, 13.0f, Color::Hex(0x4A5C74, dim), TextAlign::Center);

    // 3. Barra Inferior (Legenda de Botões)
    const float botY = mHeight - 44.0f + oy;
    const std::string buttonsLegend = "[A] Play/Pause    [L/ZL - R/ZR] Volume    [-] Mudo    [Toque] Brilho e Volume    [+] Sair";
    mRenderer.DrawText(mWidth / 2.0f + ox, botY, buttonsLegend, 14.0f, Color::Hex(0x60748E, dim), TextAlign::Center);

    // 4. Toast flutuante se houver
    const double now = Platform::NowSeconds();
    if (now < mToastUntil && !mToast.empty()) {
        const float tw = mRenderer.MeasureTextWidth(mToast, 18.0f) + 48.0f;
        const float th = 40.0f;
        const float tx = (mWidth - tw) / 2.0f;
        const float ty = 50.0f;
        mRenderer.DrawRoundedRect(tx, ty, tw, th, 20.0f, Color::Rgba(15, 22, 32, 245));
        mRenderer.DrawRectOutline(tx, ty, tw, th, 1.5f, Color::Hex(0x00E5FF, 0.9f));
        mRenderer.DrawText(mWidth / 2.0f, ty + 10.0f, mToast, 18.0f, Color::Hex(0xFFFFFF, 1.0f), TextAlign::Center);
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
        // Barra Superior: Título da mídia com linha duo-tone Joy-Con
        mRenderer.DrawRect(0, 0, (float)mWidth, 80.0f, Color::Rgba(0, 0, 0, 180));
        mRenderer.DrawRect(0, 79.0f, (float)mWidth / 2.0f, 1.5f, Color::Hex(0x00E5FF, 0.8f));
        mRenderer.DrawRect((float)mWidth / 2.0f, 79.0f, (float)mWidth / 2.0f, 1.5f, Color::Hex(0xFF2A55, 0.8f));

        const std::string title = snap.title.empty() ? "Transmissao de Video" : snap.title;
        mRenderer.DrawText(48.0f, 26.0f, title, 22.0f, Color::Hex(0xFFFFFF, 1.0f), TextAlign::Left);

        const std::string statusStr = (snap.state == PlaybackState::Paused) ? "PAUSADO" : "REPRODUZINDO";
        const Color statusColor = (snap.state == PlaybackState::Paused) ? Color::Hex(0xFF2A55) : Color::Hex(0x38EF7D);
        mRenderer.DrawText((float)mWidth - 48.0f, 28.0f, statusStr, 16.0f, statusColor, TextAlign::Right);

        // Barra Inferior: Progresso e Controles
        const float botH = 100.0f;
        const float botY = (float)mHeight - botH;
        mRenderer.DrawRect(0, botY, (float)mWidth, botH, Color::Rgba(0, 0, 0, 200));
        mRenderer.DrawRect(0, botY, (float)mWidth / 2.0f, 1.5f, Color::Hex(0x00E5FF, 0.8f));
        mRenderer.DrawRect((float)mWidth / 2.0f, botY, (float)mWidth / 2.0f, 1.5f, Color::Hex(0xFF2A55, 0.8f));

        // Barra de progresso
        const float barX = 48.0f;
        const float barY = botY + 28.0f;
        const float barW = (float)mWidth - 96.0f;
        const float barH = 8.0f;
        const int64_t curPos = mScrubbing ? mScrubPositionMs : snap.positionMs;
        const float progress = (snap.durationMs > 0) ? (float)curPos / (float)snap.durationMs : 0.0f;

        mRenderer.DrawProgressBar(barX, barY, barW, barH, progress, Color::Rgba(60, 68, 80, 180), Color::Hex(0x00E5FF));

        // Tempos (decorrido / total)
        const std::string timeStr = Text::FormatClock(curPos) + " / " + Text::FormatClock(snap.durationMs);
        mRenderer.DrawText(barX, botY + 48.0f, timeStr, 16.0f, Color::Hex(0xBAC8DC), TextAlign::Left);

        // Legenda de controles no centro
        const std::string mediaControls = "[A] Pausar    [D-Pad] Avancar/Voltar    [X] Audio    [Y] Legenda";
        mRenderer.DrawText((float)mWidth / 2.0f, botY + 48.0f, mediaControls, 14.0f, Color::Hex(0x6A809C), TextAlign::Center);

        // Volume / Mudo no canto direito
        const std::string volStr = snap.muted ? "MUDO" : ("VOL: " + std::to_string(snap.volume) + "%");
        mRenderer.DrawText((float)mWidth - 48.0f, botY + 48.0f, volStr, 16.0f, Color::Hex(0xBAC8DC), TextAlign::Right);
    }

    // Toast de notificação
    if (showToast) {
        const float tw = mRenderer.MeasureTextWidth(mToast, 18.0f) + 48.0f;
        const float th = 40.0f;
        const float tx = ((float)mWidth - tw) / 2.0f;
        const float ty = showControls ? 95.0f : 50.0f;
        mRenderer.DrawRoundedRect(tx, ty, tw, th, 20.0f, Color::Rgba(15, 22, 32, 245));
        mRenderer.DrawRectOutline(tx, ty, tw, th, 1.5f, Color::Hex(0x00E5FF, 0.9f));
        mRenderer.DrawText((float)mWidth / 2.0f, ty + 10.0f, mToast, 18.0f, Color::Hex(0xFFFFFF, 1.0f), TextAlign::Center);
    }

    mRenderer.EndFrame();
}

}  // namespace SwitchCast
