// Wrappers finos sobre libnx (threads, rede, identidade, fontes do sistema).
#pragma once

#include <string>

#include "Types.hpp"

namespace SwitchCast::Platform {

inline constexpr const char* kDataDir = "sdmc:/switch/SwitchCast";

// Fixa a thread atual em um núcleo (apps usam os núcleos 0..2 do Cortex-A57).
void PinCurrentThreadToCore(int core);

// IP atual do Wi-Fi via nifm ("0.0.0.0" se desconectado).
std::string GetLocalIp();

// Lê (ou cria e persiste) UUID e nome em sdmc:/switch/SwitchCast.
DeviceIdentity LoadDeviceIdentity(int httpPort, int wsPort);

// Exporta as fontes compartilhadas do Horizon para o SD e devolve o diretório.
// O libass do mpv precisa de arquivos de fonte reais para desenhar o OSD.
// Retorna string vazia em caso de falha.
std::string ExportSystemFonts();

// Tempo monotônico em segundos.
double NowSeconds();

}  // namespace SwitchCast::Platform
