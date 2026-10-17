// ui/Osd.cpp
#include "Osd.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "../utils/Text.hpp"

namespace SwitchCast::Osd {

namespace {

constexpr const char* kWhite  = "&HFFFFFF&";
constexpr const char* kGray   = "&HB4B4B4&";
constexpr const char* kAccent = "&H5445FF&";  // #FF4554 (ASS usa BGR)

std::string Fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// 0 = opaco, 255 = transparente (convenção ASS).
std::string AlphaHex(float visibility) {
    const int a = static_cast<int>((1.0f - std::clamp(visibility, 0.0f, 1.0f)) * 255.0f + 0.5f);
    return Fmt("&H%02X&", a);
}

std::string Truncate(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // não corta no meio de um code point
    return s.substr(0, cut) + "...";
}

std::string Label(int x, int y, int an, int size, const char* color, const std::string& alpha,
                 const std::string& text, bool bold = false, int border = 0) {
    return Fmt("{\\an%d\\pos(%d,%d)\\fs%d\\b%d\\bord%d\\shad0\\1c%s\\3c&H000000&\\alpha%s}", an, x, y, size,
               bold ? 1 : 0, border, color, alpha.c_str()) + text + "\n";
}

std::string Rect(int x, int y, int w, int h, const char* color, const std::string& alpha) {
    return Fmt("{\\an7\\pos(%d,%d)\\p1\\bord0\\shad0\\1c%s\\alpha%s}m 0 0 l %d 0 %d %d 0 %d{\\p0}\n", x, y, color,
               alpha.c_str(), w, w, h, h);
}

std::string BuildIdle(const Model& m) {
    const int cx = kWidth / 2 + m.offsetX;
    const int cy = 290 + m.offsetY;
    const std::string a = AlphaHex(m.dim);

    std::string out;
    out += Label(cx, cy, 5, 76, kWhite, a, "SwitchCast", /*bold=*/true);
    out += Rect(cx - 60, cy + 56, 120, 4, kAccent, a);
    out += Label(cx, cy + 112, 5, 34, kGray, a, "Pronto para transmitir");
    if (!m.networkOk) {
        out += Label(cx, cy + 170, 5, 28, kAccent, a, "Sem conex\xC3\xA3o Wi-Fi");
    } else {
        out += Label(cx, cy + 170, 5, 28, kGray, a, Text::AssEscape(m.addressLine));
    }
    out += Label(cx, cy + 214, 5, 24, kGray, AlphaHex(m.dim * 0.7f), Text::AssEscape(m.deviceName));
    return out;
}

std::string BuildControls(const Model& m) {
    const PlayerSnapshot& s = m.snapshot;
    const int64_t pos = m.scrubbing ? m.scrubPositionMs : s.positionMs;
    const std::string solid = AlphaHex(1.0f);

    std::string out;
    // Faixa escura translúcida atrás dos controles (preto real no OLED = pixels apagados).
    out += Rect(0, 540, kWidth, 180, "&H000000&", "&H50&");

    const std::string title = s.title.empty() ? std::string("Reproduzindo") : s.title;
    out += Label(56, 566, 7, 36, kWhite, solid, Text::AssEscape(Truncate(title, 90)), /*bold=*/true);

    std::string time = Text::FormatClock(pos);
    if (s.durationMs > 0) time += "  /  " + Text::FormatClock(s.durationMs);
    out += Label(56, 622, 7, 28, m.scrubbing ? kAccent : kGray, solid, time);

    const std::string vol = s.muted ? std::string("Mudo") : Fmt("Vol %d%%", s.volume);
    out += Label(kWidth - 56, 622, 9, 28, kGray, solid, vol);

    // Barra de progresso.
    const int barX = 56, barW = kWidth - 112, barY = 678, barH = 6;
    out += Rect(barX, barY, barW, barH, kWhite, "&H99&");
    if (s.durationMs > 0) {
        const double ratio = std::clamp(static_cast<double>(pos) / static_cast<double>(s.durationMs), 0.0, 1.0);
        const int filled = static_cast<int>(barW * ratio);
        out += Rect(barX, barY, std::max(filled, 1), barH, kAccent, solid);
        out += Fmt("{\\an7\\pos(%d,%d)\\p1\\bord0\\shad0\\1c%s}m 0 -11 l 11 0 0 11 -11 0{\\p0}\n",
                   barX + filled, barY + barH / 2, kWhite);
    }
    return out;
}

}  // namespace

std::string BuildAss(const Model& m) {
    const PlayerSnapshot& s = m.snapshot;
    std::string out;

    if (m.idle) {
        out += BuildIdle(m);
    } else {
        if (m.showControls || m.scrubbing || s.state == PlaybackState::Paused) out += BuildControls(m);

        if (s.state == PlaybackState::Paused) {
            // Ícone de pausa ao centro.
            out += Rect(kWidth / 2 - 30, kHeight / 2 - 40, 18, 80, kWhite, "&H30&");
            out += Rect(kWidth / 2 + 12, kHeight / 2 - 40, 18, 80, kWhite, "&H30&");
        }
        if (s.state == PlaybackState::Loading || s.buffering) {
            out += Label(kWidth / 2, kHeight / 2, 5, 40, kWhite, AlphaHex(1.0f),
                        s.state == PlaybackState::Loading ? "Carregando..." : "Buffering...", false, 3);
        }
    }

    if (!m.toast.empty()) {
        out += Label(kWidth / 2, 48, 8, 32, kWhite, AlphaHex(m.idle ? m.dim : 1.0f), Text::AssEscape(m.toast), true, 3);
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

}  // namespace SwitchCast::Osd
