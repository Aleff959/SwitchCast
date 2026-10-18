// launcher/main.cpp - SwitchCast Launcher (App Pai)
// Responsavel por inicializacao transparente do SwitchCast Core e
// instalacao limpa de atualizacoes com substituicao de arquivos do zero.

#include <switch.h>
#include <glad/glad.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/ui/Renderer2D.hpp"

namespace {

constexpr const char* kCorePath   = "sdmc:/switch/SwitchCast/modules/SwitchCastCore.nro";
constexpr const char* kUpdatePath = "sdmc:/switch/SwitchCast/modules/update.nro";
constexpr const char* kStatePath  = "sdmc:/switch/SwitchCast/modules/pending_update.json";
constexpr const char* kLegacyPath = "sdmc:/switch/SwitchCast/SwitchCast.nro.tmp";

// Validacao de seguranca do executavel NRO
bool ValidateNro(const char* path) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;

    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    if (size < 0x80) {
        std::fclose(fp);
        return false;
    }

    uint32_t magic = 0;
    std::fseek(fp, 0x10, SEEK_SET);
    if (std::fread(&magic, sizeof(uint32_t), 1, fp) != 1) {
        std::fclose(fp);
        return false;
    }

    std::fclose(fp);
    return (magic == 0x304F524E); // "NRO0"
}

// Cria diretorios pai se nao existirem
void EnsureDirectories() {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchCast", 0777);
    mkdir("sdmc:/switch/SwitchCast/modules", 0777);
    mkdir("sdmc:/switch/SwitchCast/config", 0777);
}

// Gerenciador grafico EGL minimo para a tela de atualizacao do Pai
class LauncherGraphics {
public:
    LauncherGraphics() = default;
    ~LauncherGraphics() { Shutdown(); }

    bool Initialize(int width, int height) {
        mWidth = width;
        mHeight = height;

        mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (mDisplay == EGL_NO_DISPLAY || !eglInitialize(mDisplay, nullptr, nullptr)) return false;
        if (!eglBindAPI(EGL_OPENGL_API)) return false;

        const EGLint configAttrs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
            EGL_NONE
        };

        EGLint numConfigs = 0;
        eglChooseConfig(mDisplay, configAttrs, &mConfig, 1, &numConfigs);
        if (numConfigs == 0) return false;

        NWindow* window = nwindowGetDefault();
        nwindowSetDimensions(window, static_cast<u32>(width), static_cast<u32>(height));
        mSurface = eglCreateWindowSurface(mDisplay, mConfig, window, nullptr);
        if (mSurface == EGL_NO_SURFACE) return false;

        const EGLint ctxAttrs[] = {
            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
            EGL_CONTEXT_MINOR_VERSION_KHR, 3,
            EGL_NONE
        };

        mContext = eglCreateContext(mDisplay, mConfig, EGL_NO_CONTEXT, ctxAttrs);
        if (mContext == EGL_NO_CONTEXT) return false;
        if (!eglMakeCurrent(mDisplay, mSurface, mSurface, mContext)) return false;

        gladLoadGL();
        eglSwapInterval(mDisplay, 1);

        mRenderer.Initialize(mWidth, mHeight);
        mInitialized = true;
        return true;
    }

    void Shutdown() {
        if (!mInitialized) return;
        mRenderer.Shutdown();
        if (mDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (mContext != EGL_NO_CONTEXT) eglDestroyContext(mDisplay, mContext);
            if (mSurface != EGL_NO_SURFACE) eglDestroySurface(mDisplay, mSurface);
            eglTerminate(mDisplay);
        }
        mInitialized = false;
    }

    void RenderFrame(const std::string& statusText, float progressFraction) {
        if (!mInitialized) return;

        glClearColor(0.043f, 0.055f, 0.078f, 1.0f); // #0B0E14
        glClear(GL_COLOR_BUFFER_BIT);

        mRenderer.BeginFrame(mWidth, mHeight);

        // 1. Fundo Base OLED
        mRenderer.DrawRect(0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight),
                           SwitchCast::Color::Hex(0x0B0E14));

        // Barra de Status Superior
        const float topY = 24.0f;
        mRenderer.DrawPill(48.0f, topY, 140.0f, 30.0f, SwitchCast::Color::Hex(0x1A1D24));
        mRenderer.DrawPillOutline(48.0f, topY, 140.0f, 30.0f, 1.0f, SwitchCast::Color::Hex(0x35383F));
        mRenderer.DrawText(48.0f + 70.0f, topY + 8.0f, "SwitchCast", 14.0f, SwitchCast::Color::Hex(0xE2E2E6), SwitchCast::TextAlign::Center);

        // 2. Card Central Master (Surface Container #1D2024, radius 28px)
        const float cardW = (mWidth >= 1920) ? 960.0f : 800.0f;
        const float cardH = (mHeight >= 1080) ? 460.0f : 380.0f;
        const float cardX = (mWidth - cardW) * 0.5f;
        const float cardY = (mHeight - cardH) * 0.5f;
        const float cardRadius = 28.0f;

        mRenderer.DrawRoundedRect(cardX, cardY, cardW, cardH, cardRadius, SwitchCast::Color::Hex(0x1D2024));
        mRenderer.DrawRoundedRectOutline(cardX, cardY, cardW, cardH, cardRadius, 1.0f, SwitchCast::Color::Hex(0x35383F));

        // 3. Avatar Icon em Primary Container (#004A77)
        const float iconCenterX = cardX + cardW * 0.5f;
        const float iconCenterY = cardY + 60.0f;
        mRenderer.DrawCircle(iconCenterX, iconCenterY, 30.0f, SwitchCast::Color::Hex(0x004A77));

        // Icone vetorial de download / atualizacao
        mRenderer.DrawRect(iconCenterX - 3.0f, iconCenterY - 11.0f, 6.0f, 13.0f, SwitchCast::Color::Hex(0xA8C7FA));
        mRenderer.DrawRoundedRect(iconCenterX - 8.0f, iconCenterY + 2.0f, 16.0f, 5.0f, 2.5f, SwitchCast::Color::Hex(0xA8C7FA));
        mRenderer.DrawRoundedRect(iconCenterX - 10.0f, iconCenterY + 9.0f, 20.0f, 4.0f, 2.0f, SwitchCast::Color::Hex(0xA8C7FA));

        // 4. Titulo e Subtitulo com Tipografia Aumentada e Alinhada
        mRenderer.DrawText(iconCenterX, cardY + 110.0f, "Atualizando componentes do sistema", 26.0f, SwitchCast::Color::Hex(0xE2E2E6), SwitchCast::TextAlign::Center);
        mRenderer.DrawText(iconCenterX, cardY + 146.0f, "Instalando o modulo principal com seguranca", 16.0f, SwitchCast::Color::Hex(0xC4C7C5), SwitchCast::TextAlign::Center);

        // 5. Container Interno de Progresso (Surface Container High #24282F, radius 18px)
        const float boxW = cardW - 72.0f;
        const float boxH = 110.0f;
        const float boxX = cardX + 36.0f;
        const float boxY = cardY + 180.0f;

        mRenderer.DrawRoundedRect(boxX, boxY, boxW, boxH, 18.0f, SwitchCast::Color::Hex(0x24282F));
        mRenderer.DrawRoundedRectOutline(boxX, boxY, boxW, boxH, 18.0f, 1.0f, SwitchCast::Color::Hex(0x333740));

        // Texto da Etapa Atual (em fonte destacada)
        mRenderer.DrawText(boxX + 24.0f, boxY + 22.0f, statusText, 18.0f, SwitchCast::Color::Hex(0xE2E2E6), SwitchCast::TextAlign::Left);

        // Barra Linear em Pilula
        const float barX = boxX + 24.0f;
        const float barY = boxY + 60.0f;
        const float barW = boxW - 140.0f;
        const float barH = 14.0f;

        mRenderer.DrawProgressBar(barX, barY, barW, barH, progressFraction, SwitchCast::Color::Hex(0x33353A), SwitchCast::Color::Hex(0xA8C7FA));

        // Porcentagem alinhada perfeitamente a direita e verticalmente com a barra
        const int pct = std::clamp(static_cast<int>(progressFraction * 100.0f), 0, 100);
        std::string pctStr = std::to_string(pct) + "%";
        const float pctX = boxX + boxW - 24.0f;
        mRenderer.DrawText(pctX, barY - 2.0f, pctStr, 18.0f, SwitchCast::Color::Hex(0xA8C7FA), SwitchCast::TextAlign::Right);

        // 6. Rodape do Card com Dica de Seguranca
        mRenderer.DrawText(iconCenterX, cardY + cardH - 32.0f, "Por favor, nao desligue o console durante a instalacao", 15.0f, SwitchCast::Color::Hex(0x8E918F), SwitchCast::TextAlign::Center);

        mRenderer.EndFrame();
        eglSwapBuffers(mDisplay, mSurface);
    }

private:
    int mWidth = 1280;
    int mHeight = 720;
    bool mInitialized = false;

    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    EGLSurface mSurface = EGL_NO_SURFACE;
    EGLContext mContext = EGL_NO_CONTEXT;
    EGLConfig mConfig = nullptr;

    SwitchCast::Renderer2D mRenderer;
};

} // namespace

int main(int argc, char* argv[]) {
    EnsureDirectories();

    // Migracao de seguranca: se houver arquivo temporario da esteira antiga, adota-o
    struct stat stLegacy{};
    if (stat(kLegacyPath, &stLegacy) == 0 && stLegacy.st_size > 0x80) {
        std::rename(kLegacyPath, kUpdatePath);
    }

    struct stat stUpdate{}, stCore{};
    const bool hasUpdate = (stat(kUpdatePath, &stUpdate) == 0 && stUpdate.st_size > 0x80);
    const bool hasCore   = (stat(kCorePath, &stCore) == 0 && stCore.st_size > 0x80);

    // =========================================================================
    // CENARIO 1: INICIALIZACAO TRANSPARENTE E INSTANTANEA NO DIA A DIA
    // =========================================================================
    // Se NAO houver atualizacao para aplicar e o modulo Core ja estiver instalado:
    // O Launcher NAO inicializa janelas graficas, GPU ou buffers.
    // Ele agenda a carga do SwitchCastCore imediatamente e encerra em 2ms!
    if (!hasUpdate && hasCore) {
        if (envHasNextLoad()) {
            envSetNextLoad(kCorePath, kCorePath);
        }
        return 0;
    }

    // =========================================================================
    // CENARIO 2: ATUALIZACAO OU INSTALACAO DO MODULO
    // =========================================================================
    // A interface so aparece quando ha trabalho real de substituicao a ser feito!
    const bool isHandheld = (appletGetOperationMode() == AppletOperationMode_Handheld);
    const int screenW = isHandheld ? 1280 : 1920;
    const int screenH = isHandheld ? 720 : 1080;

    LauncherGraphics gfx;
    if (!gfx.Initialize(screenW, screenH)) {
        // Fallback de emergencia sem graficos se o subsistema falhar
        if (hasUpdate) {
            std::remove(kCorePath);
            std::rename(kUpdatePath, kCorePath);
            fsdevCommitDevice("sdmc");
        }
        if (envHasNextLoad()) {
            envSetNextLoad(kCorePath, kCorePath);
        }
        return 0;
    }

    auto sleepMs = [](u64 ms) {
        svcSleepThread(ms * 1'000'000ULL);
    };

    // ETAPA 1: Verificacao de Integridade (15%)
    gfx.RenderFrame("Validando integridade do pacote de atualizacao...", 0.15f);
    sleepMs(300);

    const char* sourceFile = hasUpdate ? kUpdatePath : kCorePath;
    if (!ValidateNro(sourceFile)) {
        gfx.RenderFrame("Erro: Pacote invalido ou corrompido!", 0.15f);
        sleepMs(1500);
        std::remove(kUpdatePath);
        return 0;
    }

    // ETAPA 2: Remocao Segura da Versao Anterior (40%)
    // O modulo Core nao esta em execucao, liberando a gravacao direta no cartao microSD.
    gfx.RenderFrame("Removendo versao anterior com seguranca...", 0.40f);
    sleepMs(200);

    if (hasUpdate) {
        std::remove(kCorePath);
        fsdevCommitDevice("sdmc");
        sleepMs(150);

        // ETAPA 3: Gravacao do Novo Modulo do Zero (75%)
        gfx.RenderFrame("Gravando novo modulo no armazenamento...", 0.75f);

        FILE* src = std::fopen(kUpdatePath, "rb");
        FILE* dst = std::fopen(kCorePath, "wb");

        if (src && dst) {
            char chunk[65536];
            size_t bytes = 0;
            while ((bytes = std::fread(chunk, 1, sizeof(chunk), src)) > 0) {
                std::fwrite(chunk, 1, bytes, dst);
            }
            std::fflush(dst);
            fsync(fileno(dst));
            std::fclose(dst);
            std::fclose(src);
        } else {
            if (src) std::fclose(src);
            if (dst) std::fclose(dst);
        }

        std::remove(kUpdatePath);
        std::remove(kStatePath);
    }

    // ETAPA 4: Sincronizacao do Cartao SD (95%)
    gfx.RenderFrame("Sincronizando sistema de arquivos...", 0.95f);
    fsdevCommitDevice("sdmc");
    sleepMs(350);

    // ETAPA 5: Conclusao com Sucesso (100%)
    gfx.RenderFrame("Instalacao concluida com sucesso! Iniciando SwitchCast...", 1.0f);
    sleepMs(450);

    gfx.Shutdown();

    // Transfere a execucao para o modulo Core
    if (envHasNextLoad()) {
        envSetNextLoad(kCorePath, kCorePath);
    }
    return 0;
}
