// launcher/main.cpp - SwitchCast Launcher (App Pai)
// Responsavel por inicializacao transparente do SwitchCast Core e
// instalacao limpa de atualizacoes com substituicao de arquivos do zero.

#include <switch.h>
#include <glad/glad.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <curl/curl.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

#include "Types.hpp"
#include "../src/ui/Renderer2D.hpp"

namespace {

constexpr const char* kCorePath        = "sdmc:/switch/SwitchCast/modules/SwitchCastCore.nro";
constexpr const char* kBackupPath      = "sdmc:/switch/SwitchCast/modules/SwitchCastCore.nro.bak";
constexpr const char* kUpdatePath      = "sdmc:/switch/SwitchCast/modules/update.nro";
constexpr const char* kStatePath       = "sdmc:/switch/SwitchCast/modules/pending_update.json";
constexpr const char* kLegacyPath      = "sdmc:/switch/SwitchCast/SwitchCast.nro.tmp";
constexpr const char* kCoreDownloadUrl = "https://github.com/Aleff959/SwitchCast/releases/latest/download/SwitchCastCore.nro";

inline void sleepMs(u64 ms) {
    svcSleepThread(ms * 1'000'000ULL);
}

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

        eglBindAPI(EGL_OPENGL_API);

        static const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_DEPTH_SIZE,      0,
            EGL_STENCIL_SIZE,    0,
            EGL_NONE
        };

        EGLint numConfigs = 0;
        if (!eglChooseConfig(mDisplay, attribs, &mConfig, 1, &numConfigs) || numConfigs < 1) return false;

        mSurface = eglCreateWindowSurface(mDisplay, mConfig, nwindowGetDefault(), nullptr);
        if (mSurface == EGL_NO_SURFACE) return false;

        static const EGLint ctxAttribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 4,
            EGL_CONTEXT_MINOR_VERSION, 3,
            EGL_NONE
        };

        mContext = eglCreateContext(mDisplay, mConfig, EGL_NO_CONTEXT, ctxAttribs);
        if (mContext == EGL_NO_CONTEXT) return false;

        if (!eglMakeCurrent(mDisplay, mSurface, mSurface, mContext)) return false;
        if (!gladLoadGL()) return false;

        glViewport(0, 0, mWidth, mHeight);
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
        mDisplay = EGL_NO_DISPLAY;
        mSurface = EGL_NO_SURFACE;
        mContext = EGL_NO_CONTEXT;
        mInitialized = false;
    }

    void RenderFrame(const std::string& statusText, float progressFraction) {
        if (!mInitialized) return;

        glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        mRenderer.BeginFrame(mWidth, mHeight);

        // Fundo com gradiente sutil
        mRenderer.DrawRect(0, 0, mWidth, mHeight, SwitchCast::Color::Hex(0x0C0E14, 1.0f));

        const float cardW = 720.0f;
        const float cardH = 260.0f;
        const float cardX = (mWidth - cardW) * 0.5f;
        const float cardY = (mHeight - cardH) * 0.5f;

        // Card central elegante com borda em vidro
        mRenderer.DrawRoundedRect(cardX, cardY, cardW, cardH, 20.0f, SwitchCast::Color::Hex(0x161922, 0.95f));
        mRenderer.DrawRoundedRectOutline(cardX, cardY, cardW, cardH, 20.0f, 1.5f, SwitchCast::Color::Hex(0x282D3C, 0.8f));

        // Titulo SwitchCast com badge OTA
        mRenderer.DrawText(cardX + 40.0f, cardY + 36.0f, "SwitchCast", 26.0f, SwitchCast::Color::Hex(0xFFFFFF), SwitchCast::TextAlign::Left);

        const float badgeX = cardX + 40.0f + mRenderer.MeasureTextWidth("SwitchCast", 26.0f) + 14.0f;
        mRenderer.DrawPill(badgeX, cardY + 38.0f, 74.0f, 22.0f, SwitchCast::Color::Hex(0x00E5FF, 0.2f));
        mRenderer.DrawPillOutline(badgeX, cardY + 38.0f, 74.0f, 22.0f, 1.0f, SwitchCast::Color::Hex(0x00E5FF, 0.8f));
        mRenderer.DrawText(badgeX + 37.0f, cardY + 43.0f, "UPDATE", 10.0f, SwitchCast::Color::Hex(0x00E5FF), SwitchCast::TextAlign::Center);

        // Descricao da acao atual
        mRenderer.DrawText(cardX + 40.0f, cardY + 84.0f, statusText, 15.0f, SwitchCast::Color::Hex(0x9E9E9E), SwitchCast::TextAlign::Left);

        // Trilho da barra de progresso
        const float barX = cardX + 40.0f;
        const float barY = cardY + 140.0f;
        const float barW = cardW - 80.0f;
        const float barH = 10.0f;

        mRenderer.DrawPill(barX, barY, barW, barH, SwitchCast::Color::Hex(0x252936, 1.0f));

        // Preenchimento gradiente da barra (Cyan para Roxo eletrico)
        const float fillW = std::clamp(barW * progressFraction, 10.0f, barW);
        mRenderer.DrawPill(barX, barY, fillW, barH, SwitchCast::Color::Hex(0x00E5FF, 1.0f));

        // Porcentagem numerica
        int pct = static_cast<int>(progressFraction * 100.0f + 0.5f);
        mRenderer.DrawText(barX + barW, barY - 24.0f, std::to_string(pct) + "%", 15.0f, SwitchCast::Color::Hex(0x00E5FF), SwitchCast::TextAlign::Right);

        // Rodape de instrucoes de seguranca
        mRenderer.DrawText(mWidth * 0.5f, cardY + cardH - 32.0f,
                           "Nao remova o cartao microSD nem desligue o console.",
                           13.0f, SwitchCast::Color::Hex(0x606573), SwitchCast::TextAlign::Center);

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

struct NetDownloadProgress {
    LauncherGraphics* gfx = nullptr;
    curl_off_t lastRenderBytes = 0;
};

inline int OnNetProgress(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<NetDownloadProgress*>(clientp);
    if (!ctx || !ctx->gfx) return 0;

    // Atualiza a cada ~256 KB para evitar gargalo de GPU/vsync e liberar a CPU para o download
    if (dlnow - ctx->lastRenderBytes >= 262144 || (dltotal > 0 && dlnow == dltotal)) {
        ctx->lastRenderBytes = dlnow;
        float fraction = (dltotal > 0) ? std::clamp(static_cast<float>(dlnow) / static_cast<float>(dltotal), 0.05f, 0.95f) : 0.5f;
        char buf[128];
        double nowMb = static_cast<double>(dlnow) / (1024.0 * 1024.0);
        if (dltotal > 0) {
            double totMb = static_cast<double>(dltotal) / (1024.0 * 1024.0);
            std::snprintf(buf, sizeof(buf), "Baixando SwitchCast Core (%.1f / %.1f MB)...", nowMb, totMb);
        } else {
            std::snprintf(buf, sizeof(buf), "Baixando SwitchCast Core (%.1f MB)...", nowMb);
        }
        ctx->gfx->RenderFrame(buf, fraction);
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    EnsureDirectories();

    // Migracao de seguranca: se houver arquivo temporario da esteira antiga, adota-o
    struct stat stLegacy{};
    if (stat(kLegacyPath, &stLegacy) == 0 && stLegacy.st_size > 0x80) {
        std::rename(kLegacyPath, kUpdatePath);
    }

    // Leitura dos controles para deteccao do combo de Rollback (L + R)
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    padUpdate(&pad);
    const u64 initialButtons = padGetButtonsDown(&pad) | padGetButtons(&pad);
    const bool forceRollback = (initialButtons & HidNpadButton_L) && (initialButtons & HidNpadButton_R);

    // Verificacao de flag de rollback registrada pelo Core em pending_update.json
    bool rollbackFlagInState = false;
    {
        FILE* fState = std::fopen(kStatePath, "r");
        if (fState) {
            char sBuf[512];
            size_t r = std::fread(sBuf, 1, sizeof(sBuf) - 1, fState);
            sBuf[r] = '\0';
            std::fclose(fState);
            if (std::strstr(sBuf, "\"rollback_pending\": true")) {
                rollbackFlagInState = true;
            }
        }
    }

    const bool hasBackup = ValidateNro(kBackupPath);
    const bool isHandheld = (appletGetOperationMode() == AppletOperationMode_Handheld);
    const int screenW = isHandheld ? 1280 : 1920;
    const int screenH = isHandheld ? 720 : 1080;

    // =========================================================================
    // CENARIO 0: ROLLBACK ANTI-BRICK RESTAURACAO DE BACKUP (Zero-Lock!)
    // =========================================================================
    if ((forceRollback || rollbackFlagInState) && hasBackup) {
        LauncherGraphics gfx;
        if (gfx.Initialize(screenW, screenH)) {
            gfx.RenderFrame("Restaurando versao anterior de seguranca (Rollback)...", 0.5f);
        }
        std::remove(kCorePath);
        std::rename(kBackupPath, kCorePath);
        std::remove(kStatePath);
        std::remove(kUpdatePath);
        fsdevCommitDevice("sdmc");
        if (gfx.Initialize(screenW, screenH)) {
            gfx.RenderFrame("Rollback concluido com sucesso! Iniciando SwitchCast...", 1.0f);
            sleepMs(400);
            gfx.Shutdown();
        }
        if (envHasNextLoad()) {
            envSetNextLoad(kCorePath, kCorePath);
        }
        return 0;
    }

    bool hasUpdate = ValidateNro(kUpdatePath);
    bool hasCore   = ValidateNro(kCorePath);

    // =========================================================================
    // CENARIO 1: INICIALIZACAO TRANSPARENTE E INSTANTANEA NO DIA A DIA
    // =========================================================================
    // Se NAO houver atualizacao para aplicar e o modulo Core ja estiver instalado e valido:
    // O Launcher NAO inicializa janelas graficas, GPU ou buffers.
    // Ele agenda a carga do SwitchCastCore imediatamente e encerra em 2ms!
    if (!hasUpdate && hasCore) {
        if (envHasNextLoad()) {
            envSetNextLoad(kCorePath, kCorePath);
        }
        return 0;
    }

    // =========================================================================
    // CENARIO 2: ATUALIZACAO OU INSTALACAO DO MODULO (NetInstaller / Update)
    // =========================================================================
    // A interface so aparece quando ha trabalho real de substituicao a ser feito!
    LauncherGraphics gfx;
    if (!gfx.Initialize(screenW, screenH)) {
        // Fallback de emergencia sem graficos se o subsistema falhar
        if (hasUpdate) {
            if (hasCore && ValidateNro(kCorePath)) {
                std::remove(kBackupPath);
                std::rename(kCorePath, kBackupPath);
            } else {
                std::remove(kCorePath);
            }
            std::rename(kUpdatePath, kCorePath);
            std::remove(kStatePath);
            fsdevCommitDevice("sdmc");
        }
        if (envHasNextLoad()) {
            envSetNextLoad(kCorePath, kCorePath);
        }
        return 0;
    }

    // MODO NETINSTALLER / AUTO-CURA: Baixa o Core via rede se o SD estiver sem ele
    if (!hasUpdate && !hasCore) {
        gfx.RenderFrame("Conectando a rede para baixar o SwitchCast Core...", 0.05f);
        nifmInitialize(NifmServiceType_User);

        // Otimizacao TCP: janelas de recepcao estendidas (512 KB rx / 2 MB max)
        SocketInitConfig sockCfg = *socketGetDefaultInitConfig();
        sockCfg.tcp_rx_buf_size = 0x80000;      // 512 KB
        sockCfg.tcp_rx_buf_max_size = 0x200000;  // 2 MB
        sockCfg.num_bsd_sessions = 4;
        if (R_FAILED(socketInitialize(&sockCfg))) {
            socketInitializeDefault();
        }

        FILE* fp = std::fopen(kUpdatePath, "wb");
        if (fp) {
            // Buffer de escrita em RAM de 256 KB para o microSD nao pausar os ACKs TCP
            char ioBuf[262144];
            std::setvbuf(fp, ioBuf, _IOFBF, sizeof(ioBuf));

            CURL* curl = curl_easy_init();
            if (curl) {
                NetDownloadProgress prog;
                prog.gfx = &gfx;
                curl_easy_setopt(curl, CURLOPT_URL, kCoreDownloadUrl);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
                curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchCast-Launcher/" SWITCHCAST_VERSION_STR " (NintendoSwitch)");

                // Buffers otimizados de transferencia do libcurl
                curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512 * 1024L); // 512 KB
                curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
                curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, void* user) -> size_t {
                    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(user));
                });
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnNetProgress);
                curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);

                CURLcode res = curl_easy_perform(curl);
                curl_easy_cleanup(curl);
                std::fflush(fp);
                std::fclose(fp);

                if (res != CURLE_OK) {
                    std::remove(kUpdatePath);
                    gfx.RenderFrame("Erro ao baixar o Core via rede! Verifique o Wi-Fi.", 0.05f);
                    sleepMs(2500);
                }
            } else {
                std::fclose(fp);
            }
        }
        socketExit();
        nifmExit();

        hasUpdate = ValidateNro(kUpdatePath);
    }

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

    // ETAPA 2: Backup e Remocao Segura da Versao Anterior (40%)
    gfx.RenderFrame("Criando backup de seguranca da versao anterior...", 0.40f);
    sleepMs(200);

    if (hasUpdate) {
        if (hasCore && ValidateNro(kCorePath)) {
            std::remove(kBackupPath);
            std::rename(kCorePath, kBackupPath);
        } else {
            std::remove(kCorePath);
        }
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
