// SwitchCast - receptor de mídia (UPnP/DLNA, REST, WebSocket) para Nintendo Switch.
#include <switch.h>

#include <cstdio>
#include <memory>

#include "network/ServerBridge.hpp"
#include "player/MpvPlayer.hpp"
#include "ui/UIEngine.hpp"
#include "utils/Log.hpp"
#include "utils/Platform.hpp"
#include "updater/OtaUpdater.hpp"
#include "updater/ModuleManager.hpp"

namespace {

constexpr int kHttpPort = 8080;
constexpr int kWsPort = 8081;

AppletHookCookie g_appletHookCookie;

// Chamado de dentro de appletMainLoop(), portanto na thread principal.
void OnAppletHook(AppletHookType type, void* param) {
    auto* ui = static_cast<SwitchCast::UIEngine*>(param);
    switch (type) {
        case AppletHookType_OnOperationMode:
            ui->SetDisplayMode(appletGetOperationMode() == AppletOperationMode_Handheld
                                   ? SwitchCast::DisplayMode::OLED_720P
                                   : SwitchCast::DisplayMode::DOCKED_1080P);
            break;
        case AppletHookType_OnFocusState: {
            // Home/sleep: o foco vai para Background. Pausa a mídia e avisa a rede.
            const bool inFocus = appletGetFocusState() == AppletFocusState_InFocus;
            ui->OnFocusChanged(inFocus);
            if (!inFocus) SwitchCast::ServerBridge::Instance().OnSleep();
            else          SwitchCast::ServerBridge::Instance().OnResume();
            break;
        }
        case AppletHookType_OnResume:
            SwitchCast::ServerBridge::Instance().OnResume();
            break;
        default:
            break;
    }
}

// Buffers de TCP adaptativos: 4 sessões em modo Álbum, 6 em modo Title Takeover.
bool InitSockets() {
    const AppletType at = appletGetAppletType();
    const bool isApplet = (at != AppletType_Application && at != AppletType_SystemApplication);

    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    if (isApplet) {
        cfg.tcp_rx_buf_size = 0x20000;
        cfg.tcp_rx_buf_max_size = 0x40000;
        cfg.num_bsd_sessions = 4;
    } else {
        cfg.tcp_rx_buf_size = 0x40000;
        cfg.tcp_rx_buf_max_size = 0x100000;
        cfg.num_bsd_sessions = 6;
    }
    if (R_SUCCEEDED(socketInitialize(&cfg))) return true;
    return R_SUCCEEDED(socketInitializeDefault());
}

}  // namespace

int main(int argc, char* argv[]) {
    // 0. Captura o caminho exato do .nro passado pelo hbmenu
    SwitchCast::Updater::OtaUpdater::Instance().InitFromMainArgs(argc, argv);

    // 1. Servicos do Horizon OS
    const bool romfsOk = R_SUCCEEDED(romfsInit());
    nifmInitialize(NifmServiceType_User);  // IP atual do Wi-Fi
    const bool socketsOk = InitSockets();  // BSD sockets (civetweb, SSDP, rede do mpv)
#ifdef SWITCHCAST_DEBUG
    nxlinkStdio();
#endif
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    // 2. BootGuard Anti-Brick: verifica se segurou L + R ou se o boot anterior falhou
    padUpdate(&pad);
    const u64 initialButtons = padGetButtonsDown(&pad) | padGetButtons(&pad);
    const bool forceRollback = (initialButtons & HidNpadButton_L) && (initialButtons & HidNpadButton_R);
    if (!SwitchCast::Updater::OtaUpdater::Instance().VerifyBootIntegrity(forceRollback)) {
        return 0; // Rollback acionado e auto-restart agendado
    }

    // Carrega módulos dinâmicos instalados
    SwitchCast::Updater::ModuleManager::Instance().LoadInstalledModules();

    // 3. Impede o sleep automático enquanto o app roda
    appletSetMediaPlaybackState(true);

    // 4. Módulos centrais (fontes antes do mpv: o libass precisa delas para o OSD)
    const std::string fontsDir = SwitchCast::Platform::ExportSystemFonts();

    auto player = std::make_shared<SwitchCast::MpvPlayer>();
    auto ui = std::make_shared<SwitchCast::UIEngine>(player);
    const bool playerOk = player->Initialize(fontsDir);
    const bool uiOk = playerOk && ui->Initialize();

    if (uiOk) {
        // Marca boot como bem-sucedido (limpa a flag pending do BootGuard)
        SwitchCast::Updater::OtaUpdater::Instance().MarkBootSuccessful();

        player->StartWallpaperLoop();
        appletHook(&g_appletHookCookie, OnAppletHook, ui.get());

        // 5. Servidor de rede (threads próprias, núcleo 1)
        if (socketsOk) {
            SwitchCast::ServerBridge::Instance().Start(player, kHttpPort, kWsPort);
            SwitchCast::Updater::OtaUpdater::Instance().StartBackgroundChecker();
        }

        // 6. Loop principal: entrada, comandos de rede, OSD e render (núcleo 0)
        while (appletMainLoop()) {
            if (SwitchCast::Updater::OtaUpdater::Instance().IsExitRequested()) break;

            padUpdate(&pad);
            const u64 kDown = padGetButtonsDown(&pad);
            if (kDown & HidNpadButton_Plus) break;  // + encerra com segurança

            ui->Update(kDown, pad);
            ui->Render();
        }

        appletUnhook(&g_appletHookCookie);
    }

    // 7. Desligamento ordenado: para threads secundárias antes de manipular arquivos
    SwitchCast::Updater::OtaUpdater::Instance().StopBackgroundChecker();
    SwitchCast::Updater::OtaUpdater::Instance().WaitForInstallThread();
    SwitchCast::ServerBridge::Instance().Stop();
    ui->Shutdown();
    player->Shutdown();

    // 8. Se uma reinicialização de atualização OTA foi solicitada,
    // substitui o .nro e agenda o reload EXCLUSIVAMENTE na Main Thread:
    if (SwitchCast::Updater::OtaUpdater::Instance().IsRestartPending()) {
        if (romfsOk) romfsExit();
        SwitchCast::Updater::OtaUpdater::Instance().ApplyUpdateAndScheduleRestart();
    } else {
        if (romfsOk) romfsExit();
    }

    appletSetMediaPlaybackState(false);
    if (socketsOk) socketExit();
    nifmExit();

    #ifdef __SWITCH__
    // Drenagem de 500ms para liberação total de portas/sockets no bsd:u e sysmodules do CNX
    svcSleepThread(500'000'000ULL);
    #endif

    // Retorna 0 para que o nx-hbloader leia envSetNextLoad e reinicie o novo .nro automaticamente
    return 0;
}
