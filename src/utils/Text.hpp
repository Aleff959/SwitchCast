// Utilidades de texto/tempo/XML usadas por rede e UI. Sem dependências do libnx.
#pragma once

#include <cstdint>
#include <string>

namespace SwitchCast::Text {

// "H:MM:SS" (ou "M:SS" se curto). Valores negativos viram 0.
std::string FormatClock(int64_t ms);

// UPnP: "HH:MM:SS" com horas sempre presentes.
std::string FormatUpnpTime(int64_t ms);

// Aceita "HH:MM:SS", "HH:MM:SS.mmm" e "MM:SS". Retorna false se inválido.
bool ParseUpnpTime(const std::string& text, int64_t& outMs);

std::string XmlEscape(const std::string& in);
std::string XmlUnescape(const std::string& in);

// Escapa texto para eventos ASS do mpv (osd-overlay).
std::string AssEscape(const std::string& in);

bool EqualsIgnoreCase(const std::string& a, const std::string& b);
std::string Trim(const std::string& in);

}  // namespace SwitchCast::Text
