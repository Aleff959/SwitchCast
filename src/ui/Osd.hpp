// ui/Osd.hpp - monta o OSD como eventos ASS para o comando `osd-overlay` do mpv.
// O texto/shape é desenhado pelo próprio libass do mpv sobre o vídeo; assim a UI
// não precisa de um renderizador de fontes próprio.
#pragma once

#include <cstdint>
#include <string>

#include "Types.hpp"

namespace SwitchCast::Osd {

constexpr int kWidth = 1280;   // resolução lógica do overlay (escalada para 1080p no dock)
constexpr int kHeight = 720;

struct Model {
    PlayerSnapshot snapshot;
    bool idle = true;            // sem mídia: tela de espera
    bool showControls = false;   // barra de controle durante a reprodução
    std::string toast;           // mensagem curta no topo ("Volume 40%")
    bool scrubbing = false;      // gesto de arrastar no meio da tela
    int64_t scrubPositionMs = 0;
    std::string deviceName;
    std::string addressLine;     // "192.168.0.10  |  HTTP 8080  |  WS 8081"
    bool networkOk = true;
    float dim = 1.0f;            // OledBurnInGuard::DimmingFactor
    int offsetX = 0;             // pixel-shifting
    int offsetY = 0;
};

// Retorna os eventos ASS (um por linha) ou string vazia se nada deve ser exibido.
std::string BuildAss(const Model& m);

}  // namespace SwitchCast::Osd
