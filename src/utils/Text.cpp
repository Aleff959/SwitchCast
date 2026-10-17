#include "Text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace SwitchCast::Text {

std::string FormatClock(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t total = ms / 1000;
    const int h = static_cast<int>(total / 3600);
    const int m = static_cast<int>((total / 60) % 60);
    const int s = static_cast<int>(total % 60);
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof buf, "%d:%02d:%02d", h, m, s);
    else       std::snprintf(buf, sizeof buf, "%d:%02d", m, s);
    return buf;
}

std::string FormatUpnpTime(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t total = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", static_cast<int>(total / 3600),
                  static_cast<int>((total / 60) % 60), static_cast<int>(total % 60));
    return buf;
}

bool ParseUpnpTime(const std::string& text, int64_t& outMs) {
    const std::string t = Trim(text);
    if (t.empty()) return false;

    double parts[3] = {0, 0, 0};
    int count = 0;
    std::size_t pos = 0;
    while (pos <= t.size() && count < 3) {
        std::size_t next = t.find(':', pos);
        const std::string token = t.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (token.empty()) return false;
        char* end = nullptr;
        const double v = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0' || v < 0) return false;
        parts[count++] = v;
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    if (count < 2) return false;

    double seconds = 0;
    if (count == 3) seconds = parts[0] * 3600 + parts[1] * 60 + parts[2];
    else            seconds = parts[0] * 60 + parts[1];
    outMs = static_cast<int64_t>(seconds * 1000.0);
    return true;
}

std::string XmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

std::string XmlUnescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] != '&') { out += in[i++]; continue; }
        const std::size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += in[i++]; continue; }
        const std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp")       out += '&';
        else if (ent == "lt")   out += '<';
        else if (ent == "gt")   out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            const unsigned long cp = std::strtoul(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else {
            out += in.substr(i, semi - i + 1);  // entidade desconhecida: preserva
        }
        i = semi + 1;
    }
    return out;
}

std::string AssEscape(const std::string& in) {
    // Mesma estratégia do osc.lua do mpv: '\' vira '\' + U+FEFF para não formar tags.
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\xEF\xBB\xBF"; break;
            case '{':  out += "\\{"; break;
            case '}':  out += "\\}"; break;
            case '\n': out += "\\N"; break;
            case '\r': break;
            default:   out += c;
        }
    }
    return out;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

std::string Trim(const std::string& in) {
    std::size_t b = 0, e = in.size();
    while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    return in.substr(b, e - b);
}

}  // namespace SwitchCast::Text
