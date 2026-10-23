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

    mPlayer->PumpEvents();
    DrainNetworkCommands();
    HandleButtons(kDown);
    HandleTouch();

    // Interpolação suave e cadenciada (0.68s = ~41 quadros a 60 FPS)
    // Limita o passo por frame a 1/60s para nunca pular quadros ao sair de 15 FPS para 60 FPS
    const float frameStep = std::min(mDelta, 1.0f / 60.0f) / 0.68f;
    if (mMenuOpen) {
        mMenuAnimProgress = std::min(1.0f, mMenuAnimProgress + frameStep);
    } else {
        mMenuAnimProgress = std::max(0.0f, mMenuAnimProgress - frameStep);
    }

    if (mMenuAnimProgress > 0.05f) {
        UpdateDvdPhysics(std::min(mDelta, 1.0f / 30.0f));
    }

    auto& updater = Updater::OtaUpdater::Instance();
    if (updater.IsDownloading() || updater.IsInstalling()) {
        mCurrentScreen = ScreenView::UpdateCenter;
    } else if (updater.HasUpdateReady() && !mUpdateDialogDismissed) {
        mCurrentScreen = ScreenView::UpdateCenter;
    }

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

    // Abas Sobre ou Como Usar (Retorno para Dashboard)
    if (mCurrentScreen == ScreenView::About || mCurrentScreen == ScreenView::HowToUse) {
        if (kDown & (HidNpadButton_B | HidNpadButton_Y | HidNpadButton_X | HidNpadButton_A)) {
            mCurrentScreen = ScreenView::Dashboard;
        }
        return;
    }

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

    const PlayerSnapshot snap = mPlayer->Snapshot();
    const bool hasMedia = snap.state != PlaybackState::Idle;

    // Se estiver no Dashboard sem mídia
    if (!hasMedia) {
        // (-) abre ou fecha a gaveta lateral
        if (kDown & HidNpadButton_Minus) {
            mMenuOpen = !mMenuOpen;
            return;
        }

        if (mMenuOpen) {
            if (kDown & HidNpadButton_L) {
                mActiveDrawerTab = static_cast<DrawerTab>((static_cast<int>(mActiveDrawerTab) + 2) % 3);
                return;
            }
            if (kDown & HidNpadButton_R) {
                mActiveDrawerTab = static_cast<DrawerTab>((static_cast<int>(mActiveDrawerTab) + 1) % 3);
                return;
            }
            if (kDown & HidNpadButton_B) {
                mMenuOpen = false;
                return;
            }
            if (kDown & HidNpadButton_X) {
                if (updater.HasUpdateReady() || updater.IsDownloading()) {
                    mCurrentScreen = ScreenView::UpdateCenter;
                    mUpdateDialogDismissed = false;
                    return;
                }
            }
            return;
        }

        // Quando o menu está fechado:
        // [X] abre Central de Atualização se houver update pendente; senão abre gaveta em Como Usar
        if (kDown & HidNpadButton_X) {
            if (updater.HasUpdateReady() || updater.IsDownloading()) {
                mCurrentScreen = ScreenView::UpdateCenter;
                mUpdateDialogDismissed = false;
            } else {
                mMenuOpen = true;
                mActiveDrawerTab = DrawerTab::HowToUse;
            }
            return;
        }

        // [Y] abre gaveta na aba Sobre
        if (kDown & HidNpadButton_Y) {
            mMenuOpen = true;
            mActiveDrawerTab = DrawerTab::About;
            return;
        }
    }

    if (kDown & (HidNpadButton_L | HidNpadButton_ZL)) {
        mPlayer->AdjustVolume(-kVolumeStep);
        ShowToast("Volume " + std::to_string(std::max(0, snap.volume - kVolumeStep)) + "%");
    }
    if (kDown & (HidNpadButton_R | HidNpadButton_ZR)) {
        mPlayer->AdjustVolume(kVolumeStep);
        ShowToast("Volume " + std::to_string(std::min(150, snap.volume + kVolumeStep)) + "%");
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
                const int level = std::clamp(mTouch.baseVolume - static_cast<int>(dy / kTouchHeight * 100.0f), 0, 150);
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

        if (mCurrentScreen == ScreenView::About || mCurrentScreen == ScreenView::HowToUse) {
            mCurrentScreen = ScreenView::Dashboard;
            mTouch = Touch{};
            return;
        }

        if (mCurrentScreen == ScreenView::Dashboard && !mPlayer->HasMedia()) {
            const float s = (mWidth >= 1920) ? 1.5f : 1.0f;

            // 1. Toque no Botão Morph (canto superior esquerdo)
            if (mTouch.startX < 88.0f * s && mTouch.startY < 76.0f * s) {
                mMenuOpen = !mMenuOpen;
                mTouch = Touch{};
                return;
            }

            // 2. Se a Gaveta Lateral estiver aberta
            if (mMenuOpen) {
                const float drawerW = 480.0f * s;
                // Toque fora da gaveta fecha o menu
                if (mTouch.startX > drawerW) {
                    mMenuOpen = false;
                    mTouch = Touch{};
                    return;
                }

                // Toque no cabeçalho da gaveta (abas ou botão fechar X)
                if (mTouch.startY >= 14.0f * s && mTouch.startY <= 72.0f * s) {
                    // Botão fechar [X] no canto direito do cabeçalho da gaveta
                    if (mTouch.startX >= drawerW - 58.0f * s) {
                        mMenuOpen = false;
                        mTouch = Touch{};
                        return;
                    }

                    const float tabStartX = 84.0f * s;
                    const float tabAreaW = drawerW - tabStartX - 62.0f * s;
                    const float tabColW = tabAreaW / 3.0f;

                    if (mTouch.startX >= tabStartX && mTouch.startX < tabStartX + tabColW) {
                        mActiveDrawerTab = DrawerTab::HowToUse;
                    } else if (mTouch.startX >= tabStartX + tabColW && mTouch.startX < tabStartX + 2.0f * tabColW) {
                        mActiveDrawerTab = DrawerTab::WhatsNew;
                    } else if (mTouch.startX >= tabStartX + 2.0f * tabColW && mTouch.startX < drawerW - 58.0f * s) {
                        mActiveDrawerTab = DrawerTab::About;
                    }
                    mTouch = Touch{};
                    return;
                }

                mTouch = Touch{};
                return;
            }
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

    if (mCurrentScreen == ScreenView::About) {
        // TELA DEDICADA SOBRE E DADOS TÉCNICOS
        eglSwapInterval(mDisplay, 4);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        RenderAbout();

        eglSwapBuffers(mDisplay, mSurface);
        return;
    }

    if (mCurrentScreen == ScreenView::HowToUse) {
        // TELA DEDICADA COMO USAR
        eglSwapInterval(mDisplay, 4);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        RenderHowToUse();

        eglSwapBuffers(mDisplay, mSurface);
        return;
    }

    const PlayerSnapshot snap = mPlayer->Snapshot();
    const bool hasMedia = (snap.state != PlaybackState::Idle);
    const bool isWallpaper = mPlayer->IsWallpaperActive();

    // Mantém 60 FPS no Dashboard para fluidez contínua dos vitrais, pulso de Play, morph e símbolos
    const int interval = 1;
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

// ---------------------------------------------------------------------------
// Funções Auxiliares: Sombra Profunda, Morph Button, DVD Bouncing e Gaveta
// ---------------------------------------------------------------------------
static void DrawShadowedText(Renderer2D& r, float x, float y, const std::string& text,
                             float size, Color color, TextAlign align = TextAlign::Left) {
    r.DrawText(x + 1.5f, y + 2.5f, text, size, Color::Rgba(0, 0, 0, 240), align);
    r.DrawText(x, y, text, size, color, align);
}

void UIEngine::DrawMorphSwitchButton(float x, float y, float rawT, float s, float dim) {
    // Curva SmoothStep (ease-in-out) para que a transformação das 3 linhas no Switch seja bem visível
    const float t = rawT * rawT * (3.0f - 2.0f * rawT);
    auto lerp = [](float a, float b, float f) { return a + (b - a) * f; };
    auto lerpColor = [&](Color c0, Color c1, float f) {
        return Color{
            lerp(c0.r, c1.r, f),
            lerp(c0.g, c1.g, f),
            lerp(c0.b, c1.b, f),
            lerp(c0.a, c1.a, f)
        };
    };

    const float btnW = 56.0f * s;
    const float btnH = 48.0f * s;
    const float cx = x + btnW * 0.5f;
    const float cy = y + btnH * 0.5f;

    // Sombra preta profunda + Fundo do botão + Borda sólida #383D48
    mRenderer.DrawRoundedRect(x + 2.0f * s, y + 4.0f * s, btnW, btnH, 14.0f * s, Color::Rgba(0, 0, 0, static_cast<uint8_t>(230 * dim)));
    mRenderer.DrawRoundedRect(x, y, btnW, btnH, 14.0f * s, Color::Rgba(0, 0, 0, static_cast<uint8_t>(230 * dim)));
    mRenderer.DrawRoundedRectOutline(x, y, btnW, btnH, 14.0f * s, 1.5f * s, Color::Hex(0x383D48, dim));

    const Color grayBar = Color::Hex(0xD4D4D8, dim);
    const Color blueJoy = Color::Hex(0x00C3E3, dim);
    const Color redJoy  = Color::Hex(0xFF3C28, dim);
    const Color oledScr = Color::Hex(0x0B0D11, dim);

    // --- LINHA 1 (Topo -> Joy-Con Esquerdo Azul) ---
    const float b1W = lerp(22.0f, 7.0f, t) * s;
    const float b1H = lerp(2.8f, 22.0f, t) * s;
    const float b1X = cx + lerp(0.0f, -10.5f, t) * s - b1W * 0.5f;
    const float b1Y = cy + lerp(-7.0f, 0.0f, t) * s - b1H * 0.5f;
    const float b1Rad = lerp(1.4f, 3.5f, t) * s;
    mRenderer.DrawRoundedRect(b1X + 1.0f * s, b1Y + 2.0f * s, b1W, b1H, b1Rad, Color::Rgba(0, 0, 0, static_cast<uint8_t>(220 * dim)));
    mRenderer.DrawRoundedRect(b1X, b1Y, b1W, b1H, b1Rad, lerpColor(grayBar, blueJoy, t));
    if (t > 0.25f) {
        mRenderer.DrawCircle(b1X + b1W * 0.5f, b1Y + 5.0f * s, 1.8f * t * s, Color::Hex(0x090B0E, t * dim));
    }

    // --- LINHA 2 (Meio -> Tela Central do Switch) ---
    const float b2W = lerp(22.0f, 12.0f, t) * s;
    const float b2H = lerp(2.8f, 22.0f, t) * s;
    const float b2X = cx - b2W * 0.5f;
    const float b2Y = cy - b2H * 0.5f;
    mRenderer.DrawRoundedRect(b2X + 1.0f * s, b2Y + 2.0f * s, b2W, b2H, 2.0f * s, Color::Rgba(0, 0, 0, static_cast<uint8_t>(220 * dim)));
    mRenderer.DrawRoundedRect(b2X, b2Y, b2W, b2H, 2.0f * s, lerpColor(grayBar, oledScr, t));
    if (t > 0.1f) {
        mRenderer.DrawRoundedRectOutline(b2X, b2Y, b2W, b2H, 2.0f * s, 1.2f * s, Color::Hex(0x383D48, t * dim));
    }

    // --- LINHA 3 (Base -> Joy-Con Direito Vermelho) ---
    const float b3W = lerp(22.0f, 7.0f, t) * s;
    const float b3H = lerp(2.8f, 22.0f, t) * s;
    const float b3X = cx + lerp(0.0f, +10.5f, t) * s - b3W * 0.5f;
    const float b3Y = cy + lerp(+7.0f, 0.0f, t) * s - b3H * 0.5f;
    const float b3Rad = lerp(1.4f, 3.5f, t) * s;
    mRenderer.DrawRoundedRect(b3X + 1.0f * s, b3Y + 2.0f * s, b3W, b3H, b3Rad, Color::Rgba(0, 0, 0, static_cast<uint8_t>(220 * dim)));
    mRenderer.DrawRoundedRect(b3X, b3Y, b3W, b3H, b3Rad, lerpColor(grayBar, redJoy, t));
    if (t > 0.25f) {
        mRenderer.DrawCircle(b3X + b3W * 0.5f, b3Y + b3H - 5.0f * s, 1.8f * t * s, Color::Hex(0x090B0E, t * dim));
    }
}

void UIEngine::UpdateDvdPhysics(float delta) {
    const float s = (mWidth >= 1920) ? 1.5f : 1.0f;
    const float symRadius = 28.0f * s;
    const float minX = 488.0f * s + symRadius;
    const float maxX = static_cast<float>(mWidth) - symRadius - 14.0f * s;
    const float minY = symRadius + 16.0f * s;
    const float maxY = static_cast<float>(mHeight) - symRadius - 16.0f * s;

    for (size_t i = 0; i < mDvdSymbols.size(); ++i) {
        auto& sym = mDvdSymbols[i];
        sym.x += sym.vx * delta;
        sym.y += sym.vy * delta;

        // Amortecimento suave da escala elástica de volta para 1.0
        sym.scale += (1.0f - sym.scale) * std::min(1.0f, delta * 8.0f);

        bool bounced = false;
        if (sym.x <= minX) {
            sym.x = minX;
            sym.vx = std::fabs(sym.vx);
            bounced = true;
        } else if (sym.x >= maxX) {
            sym.x = maxX;
            sym.vx = -std::fabs(sym.vx);
            bounced = true;
        }

        if (sym.y <= minY) {
            sym.y = minY;
            sym.vy = std::fabs(sym.vy);
            bounced = true;
        } else if (sym.y >= maxY) {
            sym.y = maxY;
            sym.vy = -std::fabs(sym.vy);
            bounced = true;
        }

        if (bounced) {
            sym.colorIdx = (sym.colorIdx + 1) % 4;
            sym.scale = 1.25f;
        }
    }

    // Colisão elástica entre símbolos
    for (size_t i = 0; i < mDvdSymbols.size(); ++i) {
        for (size_t j = i + 1; j < mDvdSymbols.size(); ++j) {
            auto& a = mDvdSymbols[i];
            auto& b = mDvdSymbols[j];

            float dx = b.x - a.x;
            float dy = b.y - a.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            float minDist = (symRadius * a.scale + symRadius * b.scale);

            if (dist > 0.001f && dist < minDist) {
                float nx = dx / dist;
                float ny = dy / dist;
                float overlap = (minDist - dist) * 0.5f;

                a.x -= nx * overlap;
                a.y -= ny * overlap;
                b.x += nx * overlap;
                b.y += ny * overlap;

                float dvx = a.vx - b.vx;
                float dvy = a.vy - b.vy;
                float dot = dvx * nx + dvy * ny;

                if (dot > 0.0f) {
                    a.vx -= dot * nx;
                    a.vy -= dot * ny;
                    b.vx += dot * nx;
                    b.vy += dot * ny;

                    a.colorIdx = (a.colorIdx + 1) % 4;
                    b.colorIdx = (b.colorIdx + 1) % 4;
                    a.scale = 1.28f;
                    b.scale = 1.28f;
                }
            }
        }
    }
}

void UIEngine::RenderDvdSymbols(float s, float alpha) {
    if (alpha <= 0.01f) return;

    // 1. Malha de micro-símbolos em movimento diagonal contínuo no fundo (moving-symbols-backdrop)
    {
        const float spacing = 64.0f * s;
        const float driftX = std::fmod(mPulseTimer * 36.0f * s, spacing);
        const float driftY = -std::fmod(mPulseTimer * 36.0f * s, spacing);
        const char gridSyms[4] = { 'D', 'C', 'S', 'T' };

        int rowIdx = 0;
        for (float baseY = -spacing; baseY < static_cast<float>(mHeight) + spacing; baseY += spacing, ++rowIdx) {
            float curY = baseY + driftY;
            if (curY < -20.0f || curY > static_cast<float>(mHeight) + 20.0f) continue;

            float startX = (rowIdx % 2 == 1) ? (spacing * 0.5f) : 0.0f;
            int colIdx = 0;
            for (float baseX = -spacing + startX; baseX < static_cast<float>(mWidth) + spacing; baseX += spacing, ++colIdx) {
                float curX = baseX + driftX;
                if (curX < 460.0f * s || curX > static_cast<float>(mWidth) + 20.0f) continue;

                char sym = gridSyms[(rowIdx * 2 + colIdx) % 4];
                Color gCol = Color::Hex(0xFFFFFF, 0.22f * alpha);
                if ((rowIdx + colIdx) % 3 == 0) {
                    gCol = Color::Hex(0x00C3E3, 0.32f * alpha);
                } else if ((rowIdx + colIdx) % 3 == 1) {
                    gCol = Color::Hex(0xFF3C28, 0.30f * alpha);
                }
                mRenderer.DrawPlayStationSymbol(sym, curX, curY, 18.0f * s, gCol, true);
            }
        }
    }

    // 2. Os 4 símbolos maiores estilo DVD Player quicando nos cantos e entre si
    const Color colors[4] = {
        Color::Hex(0x00C3E3, alpha), // Azul Joy-Con
        Color::Hex(0xFF3C28, alpha), // Vermelho Joy-Con
        Color::Hex(0xE4E4E7, alpha), // Branco / Prata
        Color::Hex(0x38BDF8, alpha)  // Sky Blue
    };

    const char shapeSymbols[4] = { 'D', 'C', 'S', 'T' };

    for (const auto& sym : mDvdSymbols) {
        const Color col = colors[sym.colorIdx % 4];
        const float baseSize = 48.0f * s * sym.scale;

        // Sombra preta obrigatória (#000000)
        mRenderer.DrawPlayStationSymbol(shapeSymbols[sym.shapeType % 4],
                                       sym.x, sym.y, baseSize, col, true);
    }
}

void UIEngine::RenderDrawer(float s, float dim) {
    if (mMenuAnimProgress <= 0.001f) return;

    // Curva Cubic Ease-Out suave para o deslizamento da gaveta
    const float inv = 1.0f - mMenuAnimProgress;
    const float easedSlide = 1.0f - (inv * inv * inv);

    const float drawerW = 480.0f * s;
    const float drawerX = -drawerW * (1.0f - easedSlide);

    // Sombra preta projetada à direita da gaveta + Fundo rgba(10, 12, 16, 0.95)
    mRenderer.DrawRect(drawerX + drawerW, 0.0f, 28.0f * s, static_cast<float>(mHeight),
                       Color::Rgba(0, 0, 0, static_cast<uint8_t>(190 * mMenuAnimProgress * dim)));
    mRenderer.DrawRect(drawerX, 0.0f, drawerW, static_cast<float>(mHeight),
                       Color::Rgba(10, 12, 16, static_cast<uint8_t>(244 * dim)));

    // Borda direita sólida #383D48
    mRenderer.DrawRect(drawerX + drawerW - 1.5f * s, 0.0f, 1.5f * s, static_cast<float>(mHeight),
                       Color::Hex(0x383D48, dim));

    // Container das 3 abas em linha única no topo + Botão Fechar [X]
    const float headerY = 18.0f * s;
    const float tabGroupX = drawerX + 84.0f * s;
    const float closeBtnSize = 38.0f * s;
    const float closeBtnX = drawerX + drawerW - 18.0f * s - closeBtnSize;
    const float tabGroupW = closeBtnX - tabGroupX - 10.0f * s;
    const float tabGroupH = 42.0f * s;

    // Caixa escura envolvente das 3 abas (bg-black/80 border #383D48)
    mRenderer.DrawRoundedRect(tabGroupX, headerY, tabGroupW, tabGroupH, 12.0f * s, Color::Rgba(0, 0, 0, static_cast<uint8_t>(210 * dim)));
    mRenderer.DrawRoundedRectOutline(tabGroupX, headerY, tabGroupW, tabGroupH, 12.0f * s, 1.0f * s, Color::Hex(0x383D48, dim));

    struct TabItem {
        const char* name;
        DrawerTab tab;
    };
    const TabItem tabs[3] = {
        { "Como Usar", DrawerTab::HowToUse },
        { "Novidades", DrawerTab::WhatsNew },
        { "Sobre",     DrawerTab::About }
    };

    const float innerPad = 4.0f * s;
    const float tabGap = 4.0f * s;
    const float tabW = (tabGroupW - 2.0f * innerPad - 2.0f * tabGap) / 3.0f;
    const float tabH = tabGroupH - 2.0f * innerPad;
    const float tabFont = 16.0f * s;

    for (int i = 0; i < 3; ++i) {
        const float tx = tabGroupX + innerPad + i * (tabW + tabGap);
        const float ty = headerY + innerPad;
        const bool isActive = (mActiveDrawerTab == tabs[i].tab);

        if (isActive) {
            mRenderer.DrawRoundedRect(tx, ty, tabW, tabH, 8.0f * s, Color::Hex(0x1F232B, dim));
            mRenderer.DrawRoundedRectOutline(tx, ty, tabW, tabH, 8.0f * s, 1.2f * s, Color::Hex(0x383D48, dim));
            DrawShadowedText(mRenderer, tx + tabW * 0.5f, ty + 8.0f * s, tabs[i].name, tabFont, Color::Hex(0xF4F4F5, dim), TextAlign::Center);
        } else {
            DrawShadowedText(mRenderer, tx + tabW * 0.5f, ty + 8.0f * s, tabs[i].name, tabFont, Color::Hex(0xA1A1AA, dim), TextAlign::Center);
        }
    }

    // Botão Fechar [X] no topo direito da gaveta
    mRenderer.DrawRoundedRect(closeBtnX, headerY + 2.0f * s, closeBtnSize, closeBtnSize, 10.0f * s, Color::Rgba(0, 0, 0, static_cast<uint8_t>(210 * dim)));
    mRenderer.DrawRoundedRectOutline(closeBtnX, headerY + 2.0f * s, closeBtnSize, closeBtnSize, 10.0f * s, 1.0f * s, Color::Hex(0x383D48, dim));
    DrawShadowedText(mRenderer, closeBtnX + closeBtnSize * 0.5f, headerY + 10.0f * s, "X", 16.0f * s, Color::Hex(0xD4D4D8, dim), TextAlign::Center);

    // Linha divisória abaixo do cabeçalho (#2A2E38)
    const float divY = headerY + tabGroupH + 14.0f * s;
    mRenderer.DrawLine(drawerX + 84.0f * s, divY, drawerX + drawerW - 18.0f * s, divY, 1.0f * s, Color::Hex(0x2A2E38, dim));

    // Área de conteúdo (alinhada com pl-16 como no protótipo)
    const float cardX = drawerX + 84.0f * s;
    const float cardW = drawerW - 84.0f * s - 18.0f * s;
    float curY = divY + 18.0f * s;

    auto drawDrawerCard = [&](float y, float h, uint32_t borderHex) {
        mRenderer.DrawRoundedRect(cardX + 2.0f * s, y + 4.0f * s, cardW, h, 14.0f * s, Color::Rgba(0, 0, 0, static_cast<uint8_t>(225 * dim)));
        mRenderer.DrawRoundedRect(cardX, y, cardW, h, 14.0f * s, Color::Rgba(0, 0, 0, static_cast<uint8_t>(165 * dim)));
        mRenderer.DrawRoundedRectOutline(cardX, y, cardW, h, 14.0f * s, 1.2f * s, Color::Hex(borderHex, dim));
    };

    if (mActiveDrawerTab == DrawerTab::HowToUse) {
        const float ch = 92.0f * s;
        const float gap = 14.0f * s;

        // Card 1: No Celular
        drawDrawerCard(curY, ch, 0x383D48);
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 13.0f * s, "No Celular:", 17.5f * s, Color::Hex(0x00C3E3, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 38.0f * s, "Abra o Web Video Caster ou BubbleUPnP", 15.5f * s, Color::Hex(0xE4E4E7, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 60.0f * s, "e selecione o SwitchCast na rede.", 15.5f * s, Color::Hex(0xE4E4E7, dim));
        curY += ch + gap;

        // Card 2: No PC/Navegador
        const std::string ip = Platform::GetLocalIp();
        const std::string pcLine = ip.empty() ? "Digite o IP do Switch na porta :8080" : ("Acesse http://" + ip + ":8080");
        drawDrawerCard(curY, ch, 0x383D48);
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 13.0f * s, "No PC/Navegador:", 17.5f * s, Color::Hex(0xF4F4F5, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 38.0f * s, pcLine, 15.5f * s, Color::Hex(0xE4E4E7, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 60.0f * s, "para o Web Remote com controle total.", 15.5f * s, Color::Hex(0xE4E4E7, dim));
        curY += ch + gap;

        // Card 3: No Console
        drawDrawerCard(curY, ch, 0x383D48);
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 13.0f * s, "No Console:", 17.5f * s, Color::Hex(0xFF3C28, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 38.0f * s, "(A) Pausa  |  (X) Audio  |  (Y) Legendas", 15.5f * s, Color::Hex(0xE4E4E7, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 60.0f * s, "(L / R) Ajuste de Volume ate 150%.", 15.5f * s, Color::Hex(0xE4E4E7, dim));
    } else if (mActiveDrawerTab == DrawerTab::WhatsNew) {
        DrawShadowedText(mRenderer, cardX + 2.0f * s, curY, "COMPILACAO " + std::to_string(SWITCHCAST_BUILD_NUM) + " (HOJE)", 14.5f * s, Color::Hex(0xA1A1AA, dim));
        curY += 26.0f * s;

        const float ch = 84.0f * s;
        const float gap = 12.0f * s;

        // Novidade 1
        drawDrawerCard(curY, ch, 0x383D48);
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 12.0f * s, "Audio Pro 5.1:", 17.0f * s, Color::Hex(0x00C3E3, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 36.0f * s, "Downmix BS.775 eliminando colisao", 15.0f * s, Color::Hex(0xE4E4E7, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 56.0f * s, "acustica surround.", 15.0f * s, Color::Hex(0xE4E4E7, dim));
        curY += ch + gap;

        // Novidade 2
        drawDrawerCard(curY, ch, 0x383D48);
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 12.0f * s, "Volume Boost 150%:", 17.0f * s, Color::Hex(0xFF3C28, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 36.0f * s, "Teto elevado com Soft Limiter", 15.0f * s, Color::Hex(0xE4E4E7, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 56.0f * s, "anti-distorcao.", 15.0f * s, Color::Hex(0xE4E4E7, dim));
        curY += ch + gap;

        // Novidade 3
        drawDrawerCard(curY, ch, 0x383D48);
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 12.0f * s, "Morph Real:", 17.0f * s, Color::Hex(0xF4F4F5, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 36.0f * s, "As 3 linhas do menu se transformam", 15.0f * s, Color::Hex(0xE4E4E7, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 56.0f * s, "fisicamente no Switch.", 15.0f * s, Color::Hex(0xE4E4E7, dim));
    } else if (mActiveDrawerTab == DrawerTab::About) {
        const float ch = 78.0f * s;
        const float gap = 14.0f * s;

        // Card 1: Software • Versão
        drawDrawerCard(curY, ch, 0x2A2E38);
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 13.0f * s, "Software  |  Versao", 14.5f * s, Color::Hex(0xA1A1AA, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 40.0f * s, "SwitchCast Core", 18.0f * s, Color::Hex(0xF4F4F5, dim));
        const float pillW = 76.0f * s;
        const float pillH = 26.0f * s;
        const float pillX = cardX + cardW - pillW - 16.0f * s;
        const float pillY = curY + 37.0f * s;
        mRenderer.DrawPill(pillX, pillY, pillW, pillH, Color::Rgba(0, 0, 0, static_cast<uint8_t>(185 * dim)));
        mRenderer.DrawPillOutline(pillX, pillY, pillW, pillH, 1.0f * s, Color::Hex(0x383D48, dim));
        DrawShadowedText(mRenderer, pillX + pillW * 0.5f, pillY + 4.5f * s, "v" SWITCHCAST_VERSION_STR, 14.5f * s, Color::Hex(0x00C3E3, dim), TextAlign::Center);
        curY += ch + gap;

        // Card 2: Ambiente • Compilação
        drawDrawerCard(curY, ch, 0x2A2E38);
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 13.0f * s, "Ambiente  |  Compilacao", 14.5f * s, Color::Hex(0xA1A1AA, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 40.0f * s, "Atmosph\xC3\xA8re CFW  |  Comp. " + std::to_string(SWITCHCAST_BUILD_NUM), 17.5f * s, Color::Hex(0xE4E4E7, dim));
        curY += ch + gap;

        // Card 3: Autor & Repositório
        drawDrawerCard(curY, ch, 0x2A2E38);
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 13.0f * s, "Autor & Repositorio", 14.5f * s, Color::Hex(0xA1A1AA, dim));
        DrawShadowedText(mRenderer, cardX + 16.0f * s, curY + 40.0f * s, "Aleff959  |  GitHub Oficial", 17.5f * s, Color::Hex(0xE4E4E7, dim));
    }

    // Rodapé da Gaveta (y = mHeight - 46.0f * s)
    const float footY = static_cast<float>(mHeight) - 46.0f * s;
    mRenderer.DrawLine(drawerX + 84.0f * s, footY - 10.0f * s,
                       drawerX + drawerW - 18.0f * s, footY - 10.0f * s,
                       1.0f * s, Color::Hex(0x2A2E38, dim));

    DrawShadowedText(mRenderer, drawerX + 84.0f * s, footY + 4.0f * s,
                     "v" SWITCHCAST_VERSION_STR, 16.0f * s, Color::Hex(0xA1A1AA, dim), TextAlign::Left);

    DrawShadowedText(mRenderer, drawerX + drawerW - 18.0f * s, footY + 4.0f * s,
                     "Comp. " + std::to_string(SWITCHCAST_BUILD_NUM), 16.0f * s, Color::Hex(0x00C3E3, dim), TextAlign::Right);
}

void UIEngine::RenderDashboard(const PlayerSnapshot&, bool hasWallpaper) {
    mRenderer.BeginFrame(mWidth, mHeight);

    const float ox = mBurnIn.OffsetX();
    const float oy = mBurnIn.OffsetY();
    const float dim = mBurnIn.DimmingFactor();
    const float s = (mWidth >= 1920) ? 1.5f : 1.0f;

    // Quando o menu abre, os elementos nítidos da cena traseira dão lugar a passes desfocados (Gaussian Blur simulado)
    const float blurAmount = mMenuAnimProgress;
    const float bgBlurDim = dim * (1.0f - 0.25f * blurAmount);

    // =========================================================================
    // PLANO 1 (Traseiro): Tela Base 90% Preto / 10% Cinza (#0D0E12)
    // =========================================================================
    if (hasWallpaper) {
        mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight),
                           Color::Rgba(13, 14, 18, 175));
    } else {
        mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight),
                           Color::Hex(0x0D0E12, 1.0f));
    }
    mRenderer.DrawRectOutline(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight),
                              1.0f, Color::Hex(0x242730, bgBlurDim));

    // =========================================================================
    // CAMADA INTERMEDIÁRIA 1: Vitrais Facetados (Azul Esq., Vermelho Dir., Centro)
    // =========================================================================
    const float floatWave = std::sin(mPulseTimer * 0.65f) * 12.0f * s;
    mRenderer.DrawDiamondGlow(mWidth * 0.14f + ox, mHeight * 0.22f + floatWave + oy,
                              mWidth * 0.36f, mHeight * 0.46f,
                              Color::Hex(0x00C3E3, 0.34f * bgBlurDim), Color::Hex(0x00607A, 0.0f));
    mRenderer.DrawDiamondGlow(mWidth * 0.86f + ox, mHeight * 0.24f - floatWave + oy,
                              mWidth * 0.36f, mHeight * 0.46f,
                              Color::Hex(0xFF3C28, 0.32f * bgBlurDim), Color::Hex(0x7A140A, 0.0f));
    mRenderer.DrawDiamondGlow(mWidth * 0.48f + ox, mHeight * 0.85f + oy,
                              mWidth * 0.42f, mHeight * 0.40f,
                              Color::Hex(0x3B1D5E, 0.20f * bgBlurDim), Color::Hex(0x000000, 0.0f));

    // =========================================================================
    // CAMADA INTERMEDIÁRIA 2: Micro-símbolos estilo PlayStation com sombra preta
    // =========================================================================
    mRenderer.DrawPlayStationPattern(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 52.0f * s);

    // CANTO SUPERIOR DIREITO DA TELA 1: Pill "v<VERSION> | Comp. <BUILD>" com ponto #00C3E3
    const float badgeW = 204.0f * s;
    const float badgeH = 36.0f * s;
    const float badgeX = static_cast<float>(mWidth) - badgeW - 22.0f * s + ox;
    const float badgeY = 18.0f * s + oy;
    mRenderer.DrawPill(badgeX + 1.5f * s, badgeY + 3.0f * s, badgeW, badgeH, Color::Rgba(0, 0, 0, static_cast<uint8_t>(220 * bgBlurDim)));
    mRenderer.DrawPill(badgeX, badgeY, badgeW, badgeH, Color::Rgba(0, 0, 0, static_cast<uint8_t>(170 * bgBlurDim)));
    mRenderer.DrawPillOutline(badgeX, badgeY, badgeW, badgeH, 1.0f * s, Color::Hex(0x242730, bgBlurDim));
    mRenderer.DrawCircle(badgeX + 18.0f * s, badgeY + badgeH * 0.5f, 4.0f * s, Color::Hex(0x00C3E3, bgBlurDim));
    DrawShadowedText(mRenderer, badgeX + badgeW * 0.5f + 8.0f * s, badgeY + 8.0f * s,
                     "v" SWITCHCAST_VERSION_FULL_STR, 16.5f * s, Color::Hex(0xD4D4D8, bgBlurDim), TextAlign::Center);

    // =========================================================================
    // TELA 2 FRONTAL: Minimalista e Elegante ("SwitchCast" + "Vamos dar o play?")
    // =========================================================================
    const float cardScale = 1.0f - (0.015f * blurAmount);
    const float baseCardW = 780.0f * s;
    const float baseCardH = 420.0f * s;
    const float cardW = baseCardW * cardScale;
    const float cardH = baseCardH * cardScale;
    const float cx = (mWidth - cardW) * 0.5f + ox;
    const float cy = (mHeight - cardH) * 0.5f + oy;

    // Sombra preta profunda + Corpo rgba(14, 16, 21, 0.88) + Borda sólida #383D48 + Borda interna #1A1D24
    mRenderer.DrawRoundedRectWithShadow(
        cx, cy, cardW, cardH, 24.0f * s,
        Color::Rgba(14, 16, 21, static_cast<uint8_t>(225 * bgBlurDim)),
        Color::Hex(0x383D48, bgBlurDim * (1.0f - 0.5f * blurAmount)),
        1.5f * s, 18.0f * s, 12.0f * s,
        Color::Hex(0x000000, 0.96f * bgBlurDim)
    );
    mRenderer.DrawRoundedRectOutline(cx + 1.5f * s, cy + 1.5f * s, cardW - 3.0f * s, cardH - 3.0f * s,
                                     22.5f * s, 1.0f * s, Color::Hex(0x1A1D24, bgBlurDim));

    // Função lambda para desenhar os elementos internos da Tela 2 (permite multi-pass Gaussian Blur quando o menu abre!)
    const float pulseScale = 1.0f + 0.035f * (0.5f + 0.5f * std::sin(mPulseTimer * 2.1f));
    const float playBoxSize = 72.0f * s * pulseScale;
    const float playBoxX = cx + (cardW - playBoxSize) * 0.5f;
    const float playBoxY = cy + cardH * 0.31f - (playBoxSize - 72.0f * s) * 0.5f;
    const float textBaseY = cy + cardH * 0.31f + 92.0f * s;

    auto drawCardInner = [&](float dx, float dy, float passAlpha, bool withShadow) {
        // Topo: SwitchCast
        if (withShadow) {
            DrawShadowedText(mRenderer, cx + cardW * 0.5f + dx, cy + 32.0f * s + dy,
                             "SwitchCast", 31.0f * s, Color::Hex(0xF4F4F5, passAlpha), TextAlign::Center);
        } else {
            mRenderer.DrawText(cx + cardW * 0.5f + dx, cy + 32.0f * s + dy,
                               "SwitchCast", 31.0f * s, Color::Hex(0xF4F4F5, passAlpha), TextAlign::Center);
        }

        // Botão de Play central
        if (withShadow) {
            mRenderer.DrawRoundedRect(playBoxX + 2.0f * s + dx, playBoxY + 6.0f * s + dy, playBoxSize, playBoxSize, 18.0f * s,
                                      Color::Rgba(0, 0, 0, static_cast<uint8_t>(240 * passAlpha)));
        }
        mRenderer.DrawRoundedRect(playBoxX + dx, playBoxY + dy, playBoxSize, playBoxSize, 18.0f * s,
                                  Color::Rgba(10, 14, 20, static_cast<uint8_t>(235 * passAlpha)));
        mRenderer.DrawRoundedRectOutline(playBoxX + dx, playBoxY + dy, playBoxSize, playBoxSize, 18.0f * s, 1.5f * s,
                                         Color::Hex(0x383D48, passAlpha));
        mRenderer.DrawPlayStationSymbol('P',
                                        playBoxX + playBoxSize * 0.5f + 1.5f * s + dx,
                                        playBoxY + playBoxSize * 0.5f + dy,
                                        28.0f * s * pulseScale,
                                        Color::Hex(0xF4F4F5, passAlpha),
                                        withShadow);

        // Título Central: "Vamos dar o play?"
        if (withShadow) {
            DrawShadowedText(mRenderer, cx + cardW * 0.5f + dx, textBaseY + dy,
                             "Vamos dar o play?", 27.0f * s, Color::Hex(0xF4F4F5, passAlpha), TextAlign::Center);
            DrawShadowedText(mRenderer, cx + cardW * 0.5f + dx, textBaseY + 38.0f * s + dy,
                             "Envie videos ou musicas do seu celular ou computador", 18.5f * s, Color::Hex(0xA1A1AA, passAlpha), TextAlign::Center);
        } else {
            mRenderer.DrawText(cx + cardW * 0.5f + dx, textBaseY + dy,
                               "Vamos dar o play?", 27.0f * s, Color::Hex(0xF4F4F5, passAlpha), TextAlign::Center);
            mRenderer.DrawText(cx + cardW * 0.5f + dx, textBaseY + 38.0f * s + dy,
                               "Envie videos ou musicas do seu celular ou computador", 18.5f * s, Color::Hex(0xA1A1AA, passAlpha), TextAlign::Center);
        }
    };

    if (blurAmount < 0.05f) {
        drawCardInner(0.0f, 0.0f, bgBlurDim, true);
    } else {
        // Efeito real de Gaussian Blur multi-pass na Tela 2 quando o menu está abrindo/aberto
        const float sharpWeight = std::max(0.18f, 1.0f - blurAmount * 0.82f) * bgBlurDim;
        drawCardInner(0.0f, 0.0f, sharpWeight, false);

        const float blurRad = 3.2f * s * blurAmount;
        const float ringWeight = 0.14f * blurAmount * bgBlurDim;
        const float offsets[6][2] = {
            { -blurRad,  0.0f }, {  blurRad,  0.0f },
            {  0.0f, -blurRad }, {  0.0f,  blurRad },
            { -blurRad * 0.7f, -blurRad * 0.7f }, { blurRad * 0.7f, blurRad * 0.7f }
        };
        for (int k = 0; k < 6; ++k) {
            drawCardInner(offsets[k][0], offsets[k][1], ringWeight, false);
        }
    }

    // =========================================================================
    // OVERLAY DE BLUR LEVE + SÍMBOLOS EM MOVIMENTO QUANDO O MENU ESTÁ ABERTO
    // =========================================================================
    if (mMenuAnimProgress > 0.01f) {
        // Véu acrílico translúcido + brilho difuso de fundo
        mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight),
                           Color::Rgba(5, 8, 14, static_cast<uint8_t>(115.0f * mMenuAnimProgress)));
    }

    // Após abrir o menu, a malha de símbolos em movimento e os 4 símbolos DVD surgem no fundo
    if (mMenuAnimProgress > 0.15f) {
        const float symAlpha = std::clamp((mMenuAnimProgress - 0.15f) / 0.85f, 0.0f, 0.88f);
        RenderDvdSymbols(s, symAlpha * dim);
    }

    // =========================================================================
    // GAVETA DESLIZANTE DA ESQUERDA PARA A DIREITA (z-50)
    // =========================================================================
    RenderDrawer(s, dim);

    // =========================================================================
    // BOTÃO MORPH REAL NA EXTREMA ESQUERDA SUPERIOR (z-60, sempre no topo)
    // =========================================================================
    DrawMorphSwitchButton(16.0f * s + ox, 16.0f * s + oy, mMenuAnimProgress, s, dim);

    // -------------------------------------------------------------------------
    // Toast Flutuante de Notificação (se houver)
    // -------------------------------------------------------------------------
    const double now = Platform::NowSeconds();
    if (now < mToastUntil && !mToast.empty()) {
        const float toastFontSize = 16.0f * s;
        const float tw = mRenderer.MeasureTextWidth(mToast, toastFontSize) + 40.0f * s;
        const float th = 38.0f * s;
        const float tx = (mWidth - tw) * 0.5f;
        const float ty = 40.0f * s;
        mRenderer.DrawPill(tx, ty, tw, th, Color::Hex(0x11141C, 0.98f));
        mRenderer.DrawPillOutline(tx, ty, tw, th, 1.5f * s, Color::Hex(0x383D48, 0.95f));
        DrawShadowedText(mRenderer, mWidth * 0.5f, ty + 9.5f * s, mToast, toastFontSize, Color::Hex(0xF4F4F5, 1.0f), TextAlign::Center);
    }

    mRenderer.EndFrame();
}

void UIEngine::RenderAbout() {
    mRenderer.BeginFrame(mWidth, mHeight);

    const float ox = mBurnIn.OffsetX();
    const float oy = mBurnIn.OffsetY();
    const float dim = mBurnIn.DimmingFactor();
    const float s = (mWidth >= 1920) ? 1.5f : 1.0f;

    // PLANO 1 (Traseiro): Tela Base 90/10 (#0D0E12)
    mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), Color::Hex(0x0D0E12, 1.0f));
    mRenderer.DrawRectOutline(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 1.0f, Color::Hex(0x242730, dim));

    // PLANO 2 (Intermediário): Vitrais Neon Dual + Símbolos PlayStation
    const float vitralL_X = mWidth * 0.16f + ox;
    const float vitralL_Y = mHeight * 0.32f + oy;
    mRenderer.DrawDiamondGlow(vitralL_X, vitralL_Y, mWidth * 0.36f, mHeight * 0.44f,
                              Color::Hex(0x00C3E3, 0.32f * dim), Color::Hex(0x004557, 0.0f));

    const float vitralR_X = mWidth * 0.84f + ox;
    const float vitralR_Y = mHeight * 0.68f + oy;
    mRenderer.DrawDiamondGlow(vitralR_X, vitralR_Y, mWidth * 0.36f, mHeight * 0.44f,
                              Color::Hex(0xFF3C28, 0.32f * dim), Color::Hex(0x5E1007, 0.0f));

    mRenderer.DrawPlayStationPattern(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 68.0f);

    // PLANO 3 (Frontal): Card Master com Borda Sólida #383D48
    const float cardW = (mWidth >= 1920) ? 1640.0f : 1100.0f;
    const float cardH = (mHeight >= 1080) ? 920.0f : 616.0f;
    const float cx = (mWidth - cardW) * 0.5f + ox;
    const float cy = (mHeight - cardH) * 0.5f - 8.0f + oy;
    const float cardRadius = 24.0f * s;

    mRenderer.DrawRoundedRectWithShadow(
        cx, cy, cardW, cardH, cardRadius,
        Color::Rgba(14, 16, 21, static_cast<uint8_t>(225 * dim)),
        Color::Hex(0x383D48, dim),
        1.5f * s, 16.0f * s, 10.0f * s,
        Color::Hex(0x000000, 0.95f * dim)
    );

    mRenderer.DrawRoundedRectOutline(cx + 1.5f * s, cy + 1.5f * s, cardW - 3.0f * s, cardH - 3.0f * s,
                                     cardRadius - 1.5f * s, 1.0f * s, Color::Hex(0x1A1D24, dim));

    const float padX = 32.0f * s;

    // Cabeçalho da Aba Sobre
    const float headY = cy + 22.0f * s;
    const float iconSize = 36.0f * s;
    const float iconX = cx + padX;
    const float iconY = headY;

    mRenderer.DrawRoundedRect(iconX, iconY, iconSize, iconSize, 10.0f * s, Color::Hex(0x000000, dim));
    mRenderer.DrawRoundedRectOutline(iconX, iconY, iconSize, iconSize, 10.0f * s, 1.0f * s, Color::Hex(0x383D48, dim));
    mRenderer.DrawRoundedRect(iconX + 3.0f * s, iconY + 3.0f * s, iconSize * 0.5f - 4.0f * s, iconSize - 6.0f * s, 4.0f * s, Color::Hex(0x00C3E3, dim));
    mRenderer.DrawRoundedRect(iconX + iconSize * 0.5f + 1.0f * s, iconY + 3.0f * s, iconSize * 0.5f - 4.0f * s, iconSize - 6.0f * s, 4.0f * s, Color::Hex(0xFF3C28, dim));

    const float brandTextX = iconX + iconSize + 14.0f * s;
    const float brandFontSize = 24.0f * s;
    mRenderer.DrawTextWithShadow(brandTextX, headY + 5.0f * s, "SwitchCast - Sobre e Dados Técnicos", brandFontSize, Color::Hex(0xF4F4F5, dim));

    const float chipFontSize = 13.5f * s;
    const std::string verText = SWITCHCAST_VERSION_FULL_STR;
    const float verTextW = mRenderer.MeasureTextWidth(verText, chipFontSize);
    const float verChipW = verTextW + 20.0f * s;
    const float verChipX = cx + cardW - padX - verChipW;
    mRenderer.DrawPill(verChipX, headY + 5.0f * s, verChipW, 26.0f * s, Color::Hex(0x000000, dim));
    mRenderer.DrawPillOutline(verChipX, headY + 5.0f * s, verChipW, 26.0f * s, 1.0f * s, Color::Hex(0x383D48, dim));
    mRenderer.DrawTextWithShadow(verChipX + verChipW * 0.5f, headY + 9.5f * s, verText, chipFontSize, Color::Hex(0xE2E2E6, dim), TextAlign::Center);

    // Grid 2x3 de Especificações Técnicas Detalhadas
    const float gridY = headY + 52.0f * s;
    const float totalW = cardW - 2.0f * padX;
    const float colGap = 16.0f * s;
    const float rowGap = 14.0f * s;
    const float colW = (totalW - colGap) * 0.5f;
    const float rowH = 110.0f * s;

    struct TechItem {
        const char* title;
        const char* value;
        const char* detail;
        uint32_t accent;
        uint32_t border;
    };

    const TechItem specs[6] = {
        {"1. ACELERAÇÃO DE VÍDEO", "Motor NVDEC HW (Tegra X1)", "Decodificação nativa H.264/HEVC a 60 FPS sem aquecimento", 0x00C3E3, 0x0A637A},
        {"2. ÁUDIO PROFISSIONAL", "ITU-R BS.775 + Soft Limiter", "Downmix surround 5.1 equilibrado e boost de volume a 150%", 0xFF3C28, 0x7A1D16},
        {"3. SERVIDOR DE ARQUIVOS", "Servidor FTP (Porta 5000 / 21)", "Acesso e transferências ultra-rápidas para o microSD", 0x6DD58C, 0x1A5930},
        {"4. RENDERIZADOR 2D", "OpenGL 4.3 Core Profile", "Design System Vitral Stealth com sombras pretas profundas", 0xA8C7FA, 0x383D48},
        {"5. PROTOCOLOS DE REDE", "DLNA / UPnP, SSDP, WebSocket", "Descoberta SSDP local e API REST para controle remoto web", 0xE2E2E6, 0x383D48},
        {"6. PROTEÇÃO ZERO-LOCK", "Rollback Automático (L + R)", "Restauração de segurança imediata segurando L e R no boot", 0xFEE48A, 0x5A4300}
    };

    const float specTitleFont = 14.0f * s;
    const float specValFont = 18.5f * s;
    const float specDetailFont = 15.0f * s;

    for (int i = 0; i < 6; ++i) {
        int r = i / 2;
        int c = i % 2;
        float itemX = cx + padX + c * (colW + colGap);
        float itemY = gridY + r * (rowH + rowGap);

        mRenderer.DrawRoundedRectWithShadow(
            itemX, itemY, colW, rowH, 14.0f * s,
            Color::Rgba(0, 0, 0, static_cast<uint8_t>(200 * dim)),
            Color::Hex(specs[i].border, dim), 1.0f * s, 6.0f * s, 4.0f * s,
            Color::Hex(0x000000, 0.90f * dim)
        );

        mRenderer.DrawTextWithShadow(itemX + 16.0f * s, itemY + 12.0f * s, specs[i].title, specTitleFont, Color::Hex(0xA1A1AA, dim), TextAlign::Left);
        mRenderer.DrawTextWithShadow(itemX + 16.0f * s, itemY + 38.0f * s, specs[i].value, specValFont, Color::Hex(specs[i].accent, dim), TextAlign::Left);
        mRenderer.DrawTextWithShadow(itemX + 16.0f * s, itemY + 74.0f * s, specs[i].detail, specDetailFont, Color::Hex(0x8E918F, dim), TextAlign::Left);
    }

    // Rodapé da Aba Sobre com Botão Virtual [Voltar] no Canto Esquerdo
    const float footY = cy + cardH - 52.0f * s;
    mRenderer.DrawLine(cx + padX, footY - 12.0f * s, cx + cardW - padX, footY - 12.0f * s, 1.0f * s, Color::Hex(0x2A2E38, dim));

    const float backBtnW = 160.0f * s;
    const float backBtnH = 34.0f * s;
    const float backBtnX = cx + padX;
    mRenderer.DrawRoundedRect(backBtnX, footY - 2.0f * s, backBtnW, backBtnH, 8.0f * s, Color::Hex(0x000000, dim));
    mRenderer.DrawRoundedRectOutline(backBtnX, footY - 2.0f * s, backBtnW, backBtnH, 8.0f * s, 1.0f * s, Color::Hex(0x383D48, dim));
    mRenderer.DrawTextWithShadow(backBtnX + backBtnW * 0.5f, footY + 6.0f * s, "Voltar", 16.0f * s, Color::Hex(0xE2E2E6, dim), TextAlign::Center);

    // Barra inferior
    const float botY = mHeight - 34.0f * s + oy;
    const std::string backLegend = "(B) Voltar   (L/R) Volume   (-) Mudo   (+) Sair";
    const float legendFontSize = 14.5f * s;
    const float legendH = 28.0f * s;
    const float legendW = mRenderer.MeasureTextWidth(backLegend, legendFontSize) + 38.0f * s;
    const float legendX = (mWidth - legendW) * 0.5f + ox;
    mRenderer.DrawPill(legendX, botY - 3.0f * s, legendW, legendH, Color::Hex(0x000000, dim));
    mRenderer.DrawPillOutline(legendX, botY - 3.0f * s, legendW, legendH, 1.0f * s, Color::Hex(0x2E333E, dim));
    mRenderer.DrawTextWithShadow(mWidth * 0.5f + ox, botY + 5.0f * s, backLegend, legendFontSize, Color::Hex(0xC4C7C5, dim), TextAlign::Center);

    mRenderer.EndFrame();
}

void UIEngine::RenderHowToUse() {
    mRenderer.BeginFrame(mWidth, mHeight);

    const float ox = mBurnIn.OffsetX();
    const float oy = mBurnIn.OffsetY();
    const float dim = mBurnIn.DimmingFactor();
    const float s = (mWidth >= 1920) ? 1.5f : 1.0f;

    // PLANO 1 (Traseiro): Tela Base 90/10 (#0D0E12)
    mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), Color::Hex(0x0D0E12, 1.0f));
    mRenderer.DrawRectOutline(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 1.0f, Color::Hex(0x242730, dim));

    // PLANO 2 (Intermediário): Vitrais Neon Dual + Símbolos PlayStation
    const float vitralL_X = mWidth * 0.16f + ox;
    const float vitralL_Y = mHeight * 0.32f + oy;
    mRenderer.DrawDiamondGlow(vitralL_X, vitralL_Y, mWidth * 0.36f, mHeight * 0.44f,
                              Color::Hex(0x00C3E3, 0.32f * dim), Color::Hex(0x004557, 0.0f));

    const float vitralR_X = mWidth * 0.84f + ox;
    const float vitralR_Y = mHeight * 0.68f + oy;
    mRenderer.DrawDiamondGlow(vitralR_X, vitralR_Y, mWidth * 0.36f, mHeight * 0.44f,
                              Color::Hex(0xFF3C28, 0.32f * dim), Color::Hex(0x5E1007, 0.0f));

    mRenderer.DrawPlayStationPattern(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 68.0f);

    // PLANO 3 (Frontal): Card Master com Borda Sólida #383D48
    const float cardW = (mWidth >= 1920) ? 1640.0f : 1100.0f;
    const float cardH = (mHeight >= 1080) ? 920.0f : 616.0f;
    const float cx = (mWidth - cardW) * 0.5f + ox;
    const float cy = (mHeight - cardH) * 0.5f - 8.0f + oy;
    const float cardRadius = 24.0f * s;

    mRenderer.DrawRoundedRectWithShadow(
        cx, cy, cardW, cardH, cardRadius,
        Color::Rgba(14, 16, 21, static_cast<uint8_t>(225 * dim)),
        Color::Hex(0x383D48, dim),
        1.5f * s, 16.0f * s, 10.0f * s,
        Color::Hex(0x000000, 0.95f * dim)
    );

    mRenderer.DrawRoundedRectOutline(cx + 1.5f * s, cy + 1.5f * s, cardW - 3.0f * s, cardH - 3.0f * s,
                                     cardRadius - 1.5f * s, 1.0f * s, Color::Hex(0x1A1D24, dim));

    const float padX = 32.0f * s;

    // Cabeçalho da Aba Como Usar
    const float headY = cy + 22.0f * s;
    const float iconSize = 36.0f * s;
    const float iconX = cx + padX;
    const float iconY = headY;

    mRenderer.DrawRoundedRect(iconX, iconY, iconSize, iconSize, 10.0f * s, Color::Hex(0x000000, dim));
    mRenderer.DrawRoundedRectOutline(iconX, iconY, iconSize, iconSize, 10.0f * s, 1.0f * s, Color::Hex(0x383D48, dim));
    mRenderer.DrawRoundedRect(iconX + 3.0f * s, iconY + 3.0f * s, iconSize * 0.5f - 4.0f * s, iconSize - 6.0f * s, 4.0f * s, Color::Hex(0x00C3E3, dim));
    mRenderer.DrawRoundedRect(iconX + iconSize * 0.5f + 1.0f * s, iconY + 3.0f * s, iconSize * 0.5f - 4.0f * s, iconSize - 6.0f * s, 4.0f * s, Color::Hex(0xFF3C28, dim));

    const float brandTextX = iconX + iconSize + 14.0f * s;
    const float brandFontSize = 24.0f * s;
    mRenderer.DrawTextWithShadow(brandTextX, headY + 5.0f * s, "SwitchCast - Como Usar", brandFontSize, Color::Hex(0xF4F4F5, dim));

    const float chipFontSize = 13.5f * s;
    const std::string verText = SWITCHCAST_VERSION_FULL_STR;
    const float verTextW = mRenderer.MeasureTextWidth(verText, chipFontSize);
    const float verChipW = verTextW + 20.0f * s;
    const float verChipX = cx + cardW - padX - verChipW;
    mRenderer.DrawPill(verChipX, headY + 5.0f * s, verChipW, 26.0f * s, Color::Hex(0x000000, dim));
    mRenderer.DrawPillOutline(verChipX, headY + 5.0f * s, verChipW, 26.0f * s, 1.0f * s, Color::Hex(0x383D48, dim));
    mRenderer.DrawTextWithShadow(verChipX + verChipW * 0.5f, headY + 9.5f * s, verText, chipFontSize, Color::Hex(0xE2E2E6, dim), TextAlign::Center);

    // Grid 2x2 de Instruções de Uso
    const float gridY = headY + 52.0f * s;
    const float totalW = cardW - 2.0f * padX;
    const float colGap = 16.0f * s;
    const float rowGap = 14.0f * s;
    const float colW = (totalW - colGap) * 0.5f;
    const float rowH = 175.0f * s;

    struct GuideItem {
        const char* step;
        const char* title;
        const char* desc1;
        const char* desc2;
        uint32_t accent;
        uint32_t border;
    };

    const GuideItem guides[4] = {
        {"1. CONEXÃO WI-FI", "Mesma Rede Local", "Conecte celular, computador ou tablet", "na mesma rede Wi-Fi deste Switch.", 0x00C3E3, 0x0A637A},
        {"2. SELECIONE O VÍDEO", "Seu App Preferido", "Abra qualquer vídeo no Web Video Caster,", "BubbleUPnP, YouTube, VLC ou navegadores.", 0xA8C7FA, 0x383D48},
        {"3. INICIAR TRANSMISSÃO", "Escolha 'SwitchCast'", "Toque no ícone de Transmissão (Cast/DLNA)", "e selecione 'SwitchCast' para reproduzir.", 0x6DD58C, 0x1A5930},
        {"4. CONTROLE REMOTO WEB", "Acesse pelo Navegador", "Abra o IP exibido na tela inicial para pausar,", "ajustar volume e selecionar legendas.", 0xFF3C28, 0x7A1D16}
    };

    for (int i = 0; i < 4; ++i) {
        int r = i / 2;
        int c = i % 2;
        float itemX = cx + padX + c * (colW + colGap);
        float itemY = gridY + r * (rowH + rowGap);

        mRenderer.DrawRoundedRectWithShadow(
            itemX, itemY, colW, rowH, 14.0f * s,
            Color::Rgba(0, 0, 0, static_cast<uint8_t>(200 * dim)),
            Color::Hex(guides[i].border, dim), 1.0f * s, 6.0f * s, 4.0f * s,
            Color::Hex(0x000000, 0.90f * dim)
        );

        mRenderer.DrawTextWithShadow(itemX + 18.0f * s, itemY + 14.0f * s, guides[i].step, 14.0f * s, Color::Hex(guides[i].accent, dim), TextAlign::Left);
        mRenderer.DrawTextWithShadow(itemX + 18.0f * s, itemY + 42.0f * s, guides[i].title, 19.0f * s, Color::Hex(0xF4F4F5, dim), TextAlign::Left);
        mRenderer.DrawTextWithShadow(itemX + 18.0f * s, itemY + 82.0f * s, guides[i].desc1, 15.0f * s, Color::Hex(0xA1A1AA, dim), TextAlign::Left);
        mRenderer.DrawTextWithShadow(itemX + 18.0f * s, itemY + 112.0f * s, guides[i].desc2, 15.0f * s, Color::Hex(0xA1A1AA, dim), TextAlign::Left);
    }

    // Rodapé da Aba Como Usar com Botão Virtual [Voltar] no Canto Esquerdo
    const float footY = cy + cardH - 52.0f * s;
    mRenderer.DrawLine(cx + padX, footY - 12.0f * s, cx + cardW - padX, footY - 12.0f * s, 1.0f * s, Color::Hex(0x2A2E38, dim));

    const float backBtnW = 160.0f * s;
    const float backBtnH = 34.0f * s;
    const float backBtnX = cx + padX;
    mRenderer.DrawRoundedRect(backBtnX, footY - 2.0f * s, backBtnW, backBtnH, 8.0f * s, Color::Hex(0x000000, dim));
    mRenderer.DrawRoundedRectOutline(backBtnX, footY - 2.0f * s, backBtnW, backBtnH, 8.0f * s, 1.0f * s, Color::Hex(0x383D48, dim));
    mRenderer.DrawTextWithShadow(backBtnX + backBtnW * 0.5f, footY + 6.0f * s, "Voltar", 16.0f * s, Color::Hex(0xE2E2E6, dim), TextAlign::Center);

    // Barra inferior
    const float botY = mHeight - 34.0f * s + oy;
    const std::string backLegend = "(B) Voltar   (L/R) Volume   (-) Mudo   (+) Sair";
    const float legendFontSize = 14.5f * s;
    const float legendH = 28.0f * s;
    const float legendW = mRenderer.MeasureTextWidth(backLegend, legendFontSize) + 38.0f * s;
    const float legendX = (mWidth - legendW) * 0.5f + ox;
    mRenderer.DrawPill(legendX, botY - 3.0f * s, legendW, legendH, Color::Hex(0x000000, dim));
    mRenderer.DrawPillOutline(legendX, botY - 3.0f * s, legendW, legendH, 1.0f * s, Color::Hex(0x2E333E, dim));
    mRenderer.DrawTextWithShadow(mWidth * 0.5f + ox, botY + 5.0f * s, backLegend, legendFontSize, Color::Hex(0xC4C7C5, dim), TextAlign::Center);

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
    std::string subTitle = "SwitchCast v" + (info.version.empty() ? SWITCHCAST_VERSION_STR : info.version) + " - Pronto para instalar";
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
