# SwitchCast (Nintendo Switch OLED)

Receptor de mídia e streaming de alto desempenho para o Nintendo Switch (com foco nas particularidades do painel **OLED**), implementado em **C++20** utilizando **libnx**, **libmpv** com aceleração de hardware **Tegra NVDEC** e servidor de rede embutido (**UPnP/DLNA**, **REST** e **WebSocket**).

---

## 1. Visão Geral da Arquitetura

```
┌────────────────────────────────────────────────────────────────────────┐
│                        NINTENDO SWITCH OLED                            │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                    Horizon OS Native Services                    │  │
│  │    [nifm:u]          [bsd:u]          [lbl]         [hid / pad]  │  │
│  │     (Rede)         (Sockets)        (Brilho)        (Controles)  │  │
│  └────────┬─────────────────┬──────────────┬────────────────┬───────┘  │
│           │                 │              │                │          │
│  ┌────────▼─────────────────▼──────────────▼────────────────▼───────┐  │
│  │                   SwitchCast Core (C++20)                        │  │
│  │                                                                  │  │
│  │   ┌───────────────────┐               ┌───────────────────────┐  │  │
│  │   │  Network Engine   │               │   UI & Display Engine │  │  │
│  │   │ - CivetWeb HTTP   │ CommandQueue  │ - OpenGL 4.3 / EGL    │  │  │
│  │   │ - WebSocket (/ws) │ ────────────> │ - OSD (mpv overlay)   │  │  │
│  │   │ - SSDP (UDP 1900) │ (ThreadSafe)  │ - OLED Burn-in Guard  │  │  │
│  │   └─────────┬─────────┘               └───────────┬───────────┘  │  │
│  │             │                                     │              │  │
│  │             │ Media URL / Headers                 │ FBO Swap     │  │
│  │             ▼                                     ▼              │  │
│  │   ┌───────────────────────────────────────────────────────────┐  │  │
│  │   │             Playback Engine (libmpv + Tegra X1)           │  │  │
│  │   │  - Hardware Decoder: Tegra NVDEC via nvtegra              │  │  │
│  │   │  - Audio: Horizon audren nativo (ao=hos)                  │  │  │
│  │   │  - Ring Buffer: 128 MB RAM + 30s readahead                │  │  │
│  │   └───────────────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

### Divisão de Núcleos (Multi-core)
- **Núcleo 0 (Core 0)**: Loop principal do aplicativo, manipulação de eventos do Horizon OS (`appletMainLoop`), leitura de controles/touch, despacho da fila de comandos e renderização gráfica OpenGL/EGL (`UIEngine`).
- **Núcleo 1 (Core 1)**: Servidor HTTP/WebSocket (`CivetWeb`), listener UDP multicast SSDP (`239.255.255.250:1900`) e broadcast de telemetria WebSocket (fixados via `svcSetThreadCoreMask`).
- **Núcleo 2 (Core 2)**: Threads de demuxing, decodificação e streaming de rede gerenciadas internamente pela `libmpv` e `ffmpeg`.

---

## 2. Decisões de Engenharia e Ajustes Técnicos no Horizon OS

Durante o desenvolvimento sobre o toolchain oficial `devkitPro` / `libnx`, foram aplicados refinamentos críticos de engenharia:

1. **Aceleração de Hardware Tegra X1 (`nvtegra`)**:
   - No build de FFmpeg/libmpv do devkitPro para o Switch, o decodificador por hardware do Tegra X1 está registrado com o identificador de driver `nvtegra` (com fallback seguro para `auto-safe`), enquanto `nvdec` é a nomenclatura do backend CUDA para PCs com arquitetura desktop.
2. **Pipeline de Áudio (`ao=hos`)**:
   - O áudio é gerenciado pelo backend nativo do mpv `ao=hos` (Horizon Audio Renderer - `audren`). Inicializar o `audren` separadamente no `main.cpp` causava conflito de posse do canal com a biblioteca. A inicialização agora fica centralizada e isolada no próprio player.
3. **Gerenciamento de Sockets BSD e Wi-Fi**:
   - A pilha BSD do Horizon (`socketInitialize`) foi configurada com buffers TCP expandidos (`tcp_rx_buf_size = 256KB`, `tcp_rx_buf_max_size = 1MB`) e 6 sessões simultâneas (`num_bsd_sessions = 6`), permitindo tráfego paralelo estável entre CivetWeb, SSDP, WebSocket e streaming de mídia sem estouro de sessões.
4. **Alocação de Memória e Applet Mode**:
   - O buffer de rede do SwitchCast requer 128 MB de RAM dedicados. O aplicativo verifica explicitamente `appletGetAppletType()` e exige execução em modo **Application** (Title Takeover - segurar `R` ao abrir qualquer jogo no Horizon OS), prevenindo travamento por esgotamento de memória no modo Applet (Álbum).
5. **Renderização de OSD via ASS / `osd-overlay`**:
   - Em vez de embutir uma engine pesada de fontes ou pipeline complexo de textura, as fontes oficiais do Horizon são exportadas via `plGetSharedFontByType` e fornecidas ao `libass` do mpv. O OSD é composto em tempo real como comandos ASS transmitidos ao `osd-overlay` do mpv, garantindo alinhamento perfeito com o vídeo e baixo consumo de CPU.
6. **Otimizações para Painel OLED**:
   - **Zero-Pixel Powering**: O fundo e barras utilizam `#000000` absoluto, desligando fisicamente os subpixels OLED.
   - **Dynamic Swap Interval**: Em repouso (sem vídeo), o `eglSwapInterval` é ajustado para 15 FPS, reduzindo drasticamente o consumo térmico e a bateria. Em reprodução de vídeo, sincroniza a 60 Hz.
   - **Burn-In Guard**: Após 45s de ociosidade, o brilho da UI decai para 35% e os elementos sofrem micro-deslocamentos aleatórios de até ±4 pixels a cada 20s.

---

## 3. Protocolos de Rede e API

O SwitchCast expõe três interfaces de controle simultâneas na porta HTTP **8080** e WebSocket **8081**:

### 3.1. UPnP / DLNA (MediaRenderer:1)
- Suporta controladores UPnP como **BubbleUPnP**, **VLC Cast**, smart TVs e apps móveis.
- **SSDP**: Responde a buscas `M-SEARCH` (`ssdp:all`, `upnp:rootdevice`, etc.) e emite `NOTIFY ssdp:alive` e `ssdp:byebye`.
- **Rotas**:
  - `GET /upnp/description.xml`: Descritor do dispositivo com UUID único e persistente em SD.
  - `GET /upnp/scpd/AVTransport.xml`, `RenderingControl.xml`, `ConnectionManager.xml`: Especificações de serviço.
  - `POST /upnp/control/AVTransport`: Ações `SetAVTransportURI`, `Play`, `Pause`, `Stop`, `Seek`, `GetTransportInfo`, `GetPositionInfo`, `GetMediaInfo`.
  - `POST /upnp/control/RenderingControl`: `GetVolume`, `SetVolume`, `GetMute`, `SetMute`.

### 3.2. REST API (`/api/v1`)
Permite integração direta com aplicações web, scripts ou automações:

| Método | Endpoint | Payload / Descrição |
|---|---|---|
| `GET` | `/api/v1/info` | Retorna metadados do Switch (UUID, IP, portas, versão). |
| `GET` | `/api/v1/status` | Retorna estado atual (`idle`, `playing`, `paused`, posição, duração, volume). |
| `POST` | `/api/v1/play` | `{"url": "https://...", "title": "Vídeo", "headers": {"Referer": "..."}, "subtitles": ["https://..."]}` |
| `POST` | `/api/v1/pause` | Pausa a reprodução. |
| `POST` | `/api/v1/play` | Retoma a reprodução (quando sem campo `url`). |
| `POST` | `/api/v1/stop` | Para a reprodução e retorna ao estado de espera. |
| `POST` | `/api/v1/seek` | `{"positionMs": 45000}` ou `{"offsetMs": 10000}`. |
| `POST` | `/api/v1/volume` | `{"level": 80}` (0 a 100). |
| `POST` | `/api/v1/mute` | `{"mute": true}`. |
| `POST` | `/api/v1/subtitle`| `{"url": "https://..."}` (adiciona legenda externa). |

### 3.3. WebSocket (`/ws`)
Conexão bidirecional em tempo real para controle e telemetria:
- Emite eventos `{"event": "status", ...}` a cada segundo ou sob alteração de estado.
- Aceita comandos em JSON: `{"cmd": "play", "url": "..."}`, `{"cmd": "pause"}`, `{"cmd": "volume", "level": 70}`, etc.

---

## 4. Mapeamento de Controles e Gestos

```
[ZL / L]                                                               [ZR / R]
Volume -                                                               Volume +
   ▲                                                                      ▲
   │                                                                      │
┌──┴──────────────────────────────────────────────────────────────────────┴──┐
│  [D-Pad]                                                   (X) Áudio       │
│  ▲ : +60s Seek                                             (Y) Legendas    │
│  ▼ : -60s Seek                                             (A) Play/Pause  │
│  ◄ : -10s Seek                                             (B) Esconder OSD│
│  ► : +10s Seek                                                             │
│  [-] : Mudo / Desmudo                                      (+) Sair        │
│                                                                            │
│                           TELA OLED (Touch Gestures)                       │
│                   Toque Rápido: Alterna Barra de Controle                  │
│                   Deslizar Esquerda (Vertical): Brilho da Tela             │
│                   Deslizar Direita (Vertical): Volume do Áudio             │
│                   Arrastar Horizontal (Meio): Seek Interativo              │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## 5. Estrutura do Código

```
SwitchCast/
├── Makefile                     # Script de build para devkitPro / switch_rules
├── SwitchCast.nro               # Executável final compilado para Horizon OS
├── scripts/
│   ├── build.sh                 # Script bash para compilação nativa no MSYS2/Linux
│   └── build_win.ps1            # Wrapper PowerShell (compila fora do OneDrive e copia o .nro)
├── src/
│   ├── main.cpp                 # Entry point, ciclo de vida do Horizon, applet hooks
│   ├── common/
│   │   ├── Command.hpp          # Fila thread-safe de comandos de rede
│   │   └── Types.hpp            # Estruturas de dados, snapshots e enums
│   ├── network/
│   │   ├── ServerBridge.hpp/.cpp # Servidor CivetWeb (HTTP, REST, WS)
│   │   ├── SsdpServer.hpp/.cpp   # Descoberta UDP multicast UPnP
│   │   ├── Upnp.hpp/.cpp         # Templates XML e serialização SOAP
│   │   └── UpnpService.hpp/.cpp  # Handlers de ações AVTransport / RenderingControl
│   ├── player/
│   │   └── MpvPlayer.hpp/.cpp   # Integração libmpv, decodificação Tegra, eventos
│   ├── ui/
│   │   ├── OledBurnInGuard.hpp/.cpp # Protetor contra retenção de imagem OLED
│   │   ├── Osd.hpp/.cpp         # Gerador de layout OSD em scripts ASS
│   │   └── UIEngine.hpp/.cpp    # Contexto EGL/OpenGL, renderização, touch e input
│   └── utils/
│       ├── Log.hpp              # Logs para debug via nxlink
│       ├── Platform.hpp/.cpp    # Extração de fontes compartilhadas, IP e afinidade de núcleos
│       ├── PosixStubs.c         # Stubs POSIX para o newlib do libnx
│       └── Text.hpp/.cpp        # Utilitários de texto, formatação de tempo e XML
└── third_party/
    ├── civetweb/                # Servidor HTTP/WS ultraleve em C
    └── nlohmann/json.hpp        # Parser JSON para REST e WebSocket
```

---

## 6. Compilação e Instalação

### Pré-requisitos
Ter o [devkitPro](https://devkitpro.org/) instalado com a toolchain do Nintendo Switch (`switch-dev`):
```bash
sudo dkp-pacman -S \
    switch-dev \
    switch-libnx \
    switch-mesa \
    switch-glad \
    switch-libmpv \
    switch-ffmpeg \
    switch-libass \
    switch-freetype \
    switch-mbedtls \
    switch-zlib
```

### Compilar no Windows (PowerShell)
O repositório inclui um script que compila fora de pastas sincronizadas pelo OneDrive para evitar travas de I/O em arquivos de dependência `.d`:
```powershell
.\scripts\build_win.ps1
```
Para uma compilação limpa (clean):
```powershell
.\scripts\build_win.ps1 -Clean
```

### Compilar no Linux / macOS / MSYS2
```bash
make -j$(nproc)
```

### Instalação no Console
1. Copie o executável `SwitchCast.nro` para o cartão SD do Nintendo Switch em:
   ```
   sdmc:/switch/SwitchCast/SwitchCast.nro
   ```
2. Inicie o console em modo CFW (Atmosphère).
3. Abra o Homebrew Menu em **modo de aplicativo** (segure o botão `R` ao abrir qualquer jogo instalado).
4. Selecione o **SwitchCast** e aproveite!
