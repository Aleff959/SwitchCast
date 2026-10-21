# 🛠️ SwitchCast — Roadmap Cirúrgico de Consertos e Estabilização do Core

> **Objetivo deste documento**: Servir como especificação técnica exata (com arquivos, linhas e trechos de código) para corrigir todos os bugs ocultos do Core atual (`v2.0.0`) e implementar as entregas da **`v2.1.0`** e **`v2.2.0` (Core Gold Freeze)** sem regressões.
> **Regra de Versionamento**: 1 versão por dia (`v2.1.0` ➔ Dia 1 / `v2.2.0` ➔ Dia 2), atualizando o número de versão de forma unificada.

---

## 🚨 PARTE 1: Bugs Críticos Encontrados no Core Atual (Consertar na `v2.1.0`)

Uma auditoria completa no código-fonte de `C:\Users\Alpha\OneDrive\Documentos\SwitchCast` revelou **4 bugs silenciosos** que impedem o funcionamento pleno do OTA, do Rollback e do controle de versão:

### 1.1. Rollback Anti-Brick Quebrado na Arquitetura de 2 Estágios
* **Problema**:
  1. Em `launcher/main.cpp` (linha 280), quando o Launcher instala o `update.nro`, ele executa `std::remove(kCorePath)` direto, **sem criar o arquivo `SwitchCastCore.nro.bak`**.
  2. Em `src/updater/OtaUpdater.cpp` (`RollbackToBackup()`, linhas 481–500), o próprio Core tenta apagar e sobrescrever seu executável em execução no FAT32 (causando falha de permissão no Horizon OS) e ainda busca o caminho em `GetTargetNroPath()`, que aponta para o Launcher (`SwitchCast.nro`) em vez do Core (`SwitchCastCore.nro`).
* **Como Consertar**:
  * **No `launcher/main.cpp`**:
    * Definir `constexpr const char* kBackupPath = "sdmc:/switch/SwitchCast/modules/SwitchCastCore.nro.bak";`.
    * Na **Etapa 2 (linha 279)**, antes de gravar o novo Core, fazer backup do Core antigo se ele for válido:
      ```cpp
      if (hasCore && ValidateNro(kCorePath)) {
          std::remove(kBackupPath);
          std::rename(kCorePath, kBackupPath);
      } else {
          std::remove(kCorePath);
      }
      ```
    * No início do `main()` do Launcher, ler os botões `HidNpadButton_L | HidNpadButton_R` (ou checar se existe flag de rollback em `pending_update.json`). Se o combo `L + R` estiver pressionado e `ValidateNro(kBackupPath)` for `true`, restaurar `kBackupPath` para `kCorePath` no próprio Launcher (Zero-Lock!).
  * **No `src/updater/OtaUpdater.cpp` (`RollbackToBackup()`, linha 481)**:
    * Em vez de tentar sobrescrever o NRO com o Core aberto, gravar `"boot_pending": true` no `kStateFile` (`pending_update.json`) e chamar `ApplyUpdateAndScheduleRestart()` + `RequestRestart()` para que o **Launcher** execute a restauração limpa do `.bak`.

---

### 1.2. Barra de Download OTA na Tela Não Atualiza no Download Automático via GitHub
* **Problema**:
  * Em `src/updater/OtaUpdater.cpp` (`CheckRemoteManifestAndDownload()`, linhas 190–193), o `CURLOPT_WRITEFUNCTION` usa uma lambda que apenas faz `std::fwrite(...)`, **sem chamar `SetDownloadProgress(downloaded, total)` nem `FinishDownload()`**.
  * Consequência: O `ESTADO 2: DOWNLOAD EM ANDAMENTO` da `UIEngine::RenderUpdateCenter()` fica invisível/zerado durante downloads vindos do GitHub.
* **Como Consertar em `src/updater/OtaUpdater.cpp` (linhas 174–205)**:
  * Antes do `curl_easy_perform`, chamar `SetDownloadProgress(0, info.sizeBytes);`.
  * Usar o `CurlWriteContext` (já existente na linha 20 do mesmo arquivo) passando um callback que chama `this->SetDownloadProgress(dl, tot);`.
  * Ao finalizar (com sucesso ou erro), chamar `FinishDownload();`.

---

### 1.3. Hash SHA-256 Inválido no `update_manifest.json`
* **Problema**:
  * Em `update_manifest.json` (linha 9), o campo `"sha256"` está com `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` (hash SHA-256 de um arquivo de **0 bytes**).
  * Na linha 207 de `OtaUpdater.cpp`, `VerifySha256(kTempNro, info.sha256)` sempre retorna `false` e apaga o arquivo baixado.
* **Como Consertar**:
  * No script de build (`scripts/build_win.ps1`), adicionar ao final da compilação do `SwitchCastCore.nro` a atualização automática do `sha256` e `size_bytes` no `update_manifest.json` usando `(Get-FileHash .\SwitchCastCore.nro -Algorithm SHA256).Hash.ToLower()`.

---

### 1.4. Versão `"2.0.0"` Hardcoded em 7 Arquivos Diferentes
* **Problema**:
  * A versão está repetida manualmente em 7 lugares, gerando inconsistência nas trocas diárias de versão (`v2.1.0`, `v2.2.0`).
* **Como Consertar**:
  * Definir em `src/common/Types.hpp`:
    ```cpp
    #define SWITCHCAST_VERSION_STR  "2.1.0"
    #define SWITCHCAST_VERSION_CODE 20100
    ```
  * Substituir as strings fixas nos seguintes arquivos para usar `SWITCHCAST_VERSION_STR`:
    1. `include/updater/OtaUpdater.hpp` (linhas 45–46)
    2. `src/ui/UIEngine.cpp` (linhas 517 e 780)
    3. `src/network/ServerBridge.cpp` (linhas 94 e 342)
    4. `src/network/Upnp.cpp` (linha 211)
    5. `src/player/MpvPlayer.cpp` (linha 136)
    6. `Makefile` (linha 23: `APP_VERSION := 2.1.0`) e `launcher/Makefile` (linha 20: `APP_VERSION := 2.1.0`).

---

## 🚀 PARTE 2: Implementação Completa da `v2.1.0` (NetInstaller, Áudio Pro & Core Leve)

### 2.1. Core Leve (~25 MB): Desacoplamento do Vídeo RomFS e Símbolos de Debug
* **Por que funciona sem quebrar nada**:
  * `MpvPlayer::StartWallpaperLoop()` (`src/player/MpvPlayer.cpp`, linhas 390–410) e `ServeWallpaper()` (`src/network/ServerBridge.cpp`, linhas 502–519) **já procuram primeiro** em `"sdmc:/switch/SwitchCast/wallpaper.mp4"` e fazem fallback limpo se o arquivo não existir.
* **Arquivos para Editar**:
  1. **`Makefile` (linhas 30–36)**:
     * Remover `-g` de `CFLAGS`, `ASFLAGS` e `LDFLAGS` no modo Release (manter `-g` apenas dentro do bloco `ifeq ($(DEBUG),1)`).
  2. **`Makefile` (linhas 74–77)**:
     * Remover/comentar o embutimento do diretório `romfs`:
       ```makefile
       # RomFS desacoplado na v2.1.0 para reduzir o Core de ~38MB para ~25MB
       # ifneq ($(wildcard $(CURDIR)/romfs),)
       # export ROMFS_DIR := $(CURDIR)/romfs
       # export NROFLAGS += --romfsdir=$(CURDIR)/romfs
       # endif
       ```

---

### 2.2. Áudio Pro (Downmix 5.1 ITU-R BS.775) + Volume Boost 150% com Soft Limiter
Para que o volume chegue a **150%** sem ser cortado em 100%, é obrigatório atualizar **todos os 6 pontos de clamp** no código:

* **Passo 1 — Configurar Downmix 5.1 e Limiter no `src/player/MpvPlayer.cpp` (`Initialize`, linhas 80–83)**:
  ```cpp
  // --- Áudio Pro v2.1.0: Downmix ITU-R BS.775 + Volume Boost 150% ---
  SetFirstSupportedOption("ao", {"hos", "sdl"});
  opt("audio-channels", "stereo");
  opt("audio-samplerate", "48000");
  opt("ad-lavc-downmix", "yes");
  opt("audio-normalize-downmix", "yes");
  opt("volume-max", "150");
  // Soft limiter para impedir distorção (clipping) acima de 100% nos alto-falantes do Switch
  opt("af", "lavfi=[alimiter=limit=0.95:attack=5:release=50]");
  ```
* **Passo 2 — Liberar o Teto de 150% nos 6 Pontos de Controle**:
  1. `src/player/MpvPlayer.cpp` (linha 223, em `MpvPlayer::SetVolume`):
     * Trocar `std::clamp(percent, 0, 100)` por `std::clamp(percent, 0, 150)`.
  2. `src/player/MpvPlayer.cpp` (linhas 561 e 587, em `MpvPlayer::HandlePropertyChange`):
     * Trocar ambos os `std::clamp(..., 0, 100)` por `std::clamp(..., 0, 150)`.
  3. `src/ui/UIEngine.cpp` (linha 263, botão `R`/`ZR` em `HandleButtons`):
     * Trocar `std::min(100, snap.volume + kVolumeStep)` por `std::min(150, snap.volume + kVolumeStep)`.
  4. `src/ui/UIEngine.cpp` (linha 334, gesto de toque vertical em `HandleTouch`):
     * Trocar `std::clamp(..., 0, 100)` por `std::clamp(..., 0, 150)`.
  5. `src/network/ServerBridge.cpp` (linhas 138–140, comando JSON `"volume"`):
     * Trocar `std::min<int64_t>(100, num("level", 100))` por `std::min<int64_t>(150, num("level", 100))`.
  6. `src/network/UpnpService.cpp` (linha 187, SOAP `SetVolume`) e `src/network/WebRemote.hpp` (slider de volume HTML):
     * Trocar o clamp `100` por `150` e no HTML alterar `max="100"` para `max="150"` no slider de volume.

---

### 2.3. Bootstrap NetInstaller & Auto-Cura no App Pai (`launcher/`)
Se o usuário colocar apenas `SwitchCast.nro` no SD (ou se `SwitchCastCore.nro` estiver corrompido), o Launcher baixa o Core automaticamente do GitHub com barra de progresso gráfica.

* **Passo 1 — `launcher/Makefile` (linha 29)**:
  * Adicionar `libcurl` e `mbedtls` na linha `LIBS`:
    ```makefile
    LIBS := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lEGL -lglapi -ldrm_nouveau -lglad -lfreetype -lharfbuzz -lfribidi -lpng16 -lbz2 -lz -lnx -lm
    ```
* **Passo 2 — `launcher/main.cpp`**:
  * Incluir `<curl/curl.h>`.
  * Alterar a verificação inicial (linhas 220–234) para validar a integridade do Core existente:
    ```cpp
    const bool hasUpdate = ValidateNro(kUpdatePath);
    const bool hasCore   = ValidateNro(kCorePath);
    ```
  * Se `!hasUpdate && !hasCore` (Modo **NetInstaller / Auto-Cura**):
    1. Inicializar `gfx.Initialize(screenW, screenH)` e exibir `"Conectando a rede para baixar o SwitchCast Core..."`.
    2. Chamar `nifmInitialize(NifmServiceType_User)` e `socketInitializeDefault()`.
    3. Baixar `https://github.com/Aleff959/SwitchCast/releases/latest/download/SwitchCastCore.nro` para `kUpdatePath` via `libcurl`.
    4. No `CURLOPT_XFERINFOFUNCTION` (ou `CURLOPT_WRITEFUNCTION`), atualizar `gfx.RenderFrame("Baixando SwitchCast Core (" + mbStr + " MB)...", progressFraction)` a cada ~64 KB recebidos.
    5. Fechar `socketExit()` e `nifmExit()`, validar `ValidateNro(kUpdatePath)` e prosseguir para a **Etapa 3 (Gravação em `kCorePath`)** que já existe no `launcher/main.cpp`.

---

## 🏪 PARTE 3: Implementação da `v2.2.0` (Vitrine da Lojinha + Motor de Módulos para o Core Freeze)

Para que o Core possa ser **congelado na `v2.2.0`** sem precisar ser recompilado quando novos módulos forem lançados, a `v2.2.0` deve entregar a **Interface da Lojinha** + o **Motor Declarativo de Módulos**.

### 3.1. Resolução de Conflito de Botões (`L`/`R` vs `ZL`/`ZR`) na `UIEngine`
* **Atenção**: Hoje, em `src/ui/UIEngine.cpp` (linhas 257–264), tanto `L`/`R` quanto `ZL`/`ZR` alteram o volume mesmo no Dashboard.
* **Regra Clara para a `v2.2.0`**:
  * **Quando NÃO há vídeo tocando (`!hasMedia`)**:
    * `HidNpadButton_L` ➔ Muda para a aba **`ScreenView::Dashboard`**.
    * `HidNpadButton_R` ➔ Muda para a aba **`ScreenView::ModuleStore`** (Lojinha de Módulos).
    * `HidNpadButton_ZL` / `HidNpadButton_ZR` ➔ Diminui / Aumenta o volume.
  * **Quando HÁ vídeo tocando (`hasMedia`)**:
    * `L`/`ZL` e `R`/`ZR` continuam controlando o volume normalmente.

### 3.2. Catálogo Remoto e Download de Módulos no `ModuleManager`
Expandir `include/updater/ModuleManager.hpp` e `src/updater/ModuleManager.cpp` com:
1. **`FetchRemoteCatalog()`**:
   * Lê o array `"modules"` do `update_manifest.json` remoto (via `libcurl` em thread separada para não travar a UI) e mescla com os módulos já instalados em `sdmc:/switch/SwitchCast/modules/manifest.json`.
   * Adiciona campos visuais ao `ModuleDescriptor`: `description`, `category` (`protocol`, `audio`, `theme`, `util`), `downloadUrl`, `installed` (`bool`).
2. **`DownloadAndInstallModuleAsync(const std::string& moduleId)`**:
   * Faz download de `desc.downloadUrl` para arquivo `.tmp`, valida `OtaUpdater::VerifySha256` (ou aceita se `sha256` for `"skip"` durante desenvolvimento), move para `sdmc:/switch/SwitchCast/modules/<entryFile>`, marca `installed = true, enabled = true` e chama `ApplyActiveModules(player)`.
3. **`ToggleModule(const std::string& moduleId)`**:
   * Alterna `enabled = !enabled`, salva `manifest.json` local e reaplica os efeitos imediatamente no `MpvPlayer` (`ApplyActiveModules`).

### 3.3. Motor Declarativo de Módulos (Como o Core Congelado Executa Módulos Novos)
Adicionar em `ModuleManager::ApplyActiveModules(std::shared_ptr<MpvPlayer> player)` o suporte nativo aos 4 tipos de módulos sem exigir recompilação futura do C++:

1. **Módulos de Áudio (`audio.*`, ex: `audio.night_mode`, `audio.equalizer_10band`)**:
   * Lê o JSON do módulo em `sdmc:/switch/SwitchCast/modules/<entryFile>`.
   * Se contiver `"mpv_af"` (ex: `"lavfi=[dynaudnorm=f=150:g=15:p=0.95]"`), aplica em tempo real no `MpvPlayer` via `mpv_set_property_string(mMpv, "af", combinedFilters.c_str())`.
2. **Módulos de Vídeo/Shaders (`video.*`, ex: `video.anime_sharpener`)**:
   * Se o módulo apontar para um arquivo `.glsl` no SD, aplica no `libmpv` via `Command({"change-list", "glsl-shaders", "append", shaderFullPath})`.
3. **Módulos de Tema/Wallpaper (`theme.*`, ex: `theme.cyberpunk_neon`, `theme.anime_lofi`)**:
   * Se o módulo fornecer um `.mp4` em `sdmc:/switch/SwitchCast/modules/themes/<id>.mp4`, atualiza o caminho ativo do wallpaper no `MpvPlayer::StartWallpaperLoop()` e recarrega o loop imediatamente.
4. **Módulos de Utilidades e Protocolos (`util.sleep_timer`, `protocol.roku_bridge`, `protocol.youtube_dial`)**:
   * **Sleep Timer**: Contador regressivo simples no `UIEngine::Update` que aplica fade-out de volume nos últimos 30s e chama `mPlayer->Stop()` quando ativo.
   * **Roku ECP / DIAL**: Endpoints HTTP leves (`/keypress/*`, `/launch/*`, `/query/device-info`, `/apps/YouTube`) já registrados no `ServerBridge.cpp` que verificam `ModuleManager::Instance().IsModuleEnabled("protocol.roku_bridge")` ou `"protocol.youtube_dial"` antes de responder.

---

## ✅ Checklist de Execução Passo a Passo (Para Colar no Gemini Flash)

Quando for pedir para o Gemini Flash executar cada etapa, você pode usar prompts diretos referenciando este arquivo:

1. **Prompt Dia 1 (v2.1.0 — Parte A: Versão, Áudio Pro 150% e Core Leve)**:
   > *"Siga a Parte 1.4, Parte 2.1 e Parte 2.2 do `ROADMAP_CORE.md` para atualizar a versão para 2.1.0, ativar o Áudio Pro 5.1 + Volume 150% nos 6 pontos do código e remover o romfs/debug symbols do Makefile do Core."*
2. **Prompt Dia 1 (v2.1.0 — Parte B: Conserto do Rollback, Progresso OTA e NetInstaller)**:
   > *"Siga a Parte 1.1, Parte 1.2, Parte 1.3 e Parte 2.3 do `ROADMAP_CORE.md` para consertar o backup/rollback no Launcher, o progresso de download no OtaUpdater e implementar o NetInstaller com libcurl no `launcher/main.cpp`."*
3. **Prompt Dia 2 (v2.2.0 — Vitrine da Lojinha & Core Gold Freeze)**:
   > *"Siga a Parte 3 do `ROADMAP_CORE.md` para atualizar a versão para 2.2.0, implementar a aba `ScreenView::ModuleStore` navegável por `[L]` e `[R]` na `UIEngine`, o catálogo remoto no `ModuleManager` e o motor declarativo de módulos."*
