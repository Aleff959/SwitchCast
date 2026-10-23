// ui/Renderer2D.cpp - Implementação do renderizador 2D em OpenGL 4.3.
#include "Renderer2D.hpp"

#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstring>
#include <algorithm>
#include "../utils/Log.hpp"

namespace SwitchCast {

namespace {

const char* kVertexShaderSource = R"(#version 330 core
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

uniform mat4 uProj;

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = inUV;
    vColor = inColor;
    gl_Position = uProj * vec4(inPos, 0.0, 1.0);
}
)";

const char* kFragmentShaderSource = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;

uniform sampler2D uTex;
uniform int uUseTex;

out vec4 fragColor;

void main() {
    if (uUseTex == 1) {
        float alpha = texture(uTex, vUV).r;
        fragColor = vec4(vColor.rgb, vColor.a * alpha);
    } else {
        fragColor = vColor;
    }
}
)";

// Fallback: matriz simples 8x16 de caracteres ASCII caso FreeType falhe
const uint8_t kFallbackFont8x16[96][16] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // space
    {0,0,0x18,0x3C,0x3C,0x18,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0}, // !
    // restante inicializado em runtime se necessário
};

} // namespace

Renderer2D::~Renderer2D() {
    Shutdown();
}

bool Renderer2D::Initialize(int screenWidth, int screenHeight) {
    mWidth = screenWidth;
    mHeight = screenHeight;

    if (!InitShaders()) return false;
    InitFont();

    mVertices.reserve(4096);
    return true;
}

void Renderer2D::Shutdown() {
    if (mFontTexture) {
        glDeleteTextures(1, &mFontTexture);
        mFontTexture = 0;
    }
    if (mVbo) {
        glDeleteBuffers(1, &mVbo);
        mVbo = 0;
    }
    if (mVao) {
        glDeleteVertexArrays(1, &mVao);
        mVao = 0;
    }
    if (mProgram) {
        glDeleteProgram(mProgram);
        mProgram = 0;
    }
}

bool Renderer2D::InitShaders() {
    auto compileShader = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            SC_LOG("Shader compile error: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShaderSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShaderSource);
    if (!vs || !fs) return false;

    mProgram = glCreateProgram();
    glAttachShader(mProgram, vs);
    glAttachShader(mProgram, fs);
    glLinkProgram(mProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(mProgram, GL_LINK_STATUS, &ok);
    if (!ok) return false;

    mLocProj = glGetUniformLocation(mProgram, "uProj");
    mLocUseTex = glGetUniformLocation(mProgram, "uUseTex");
    mLocTex = glGetUniformLocation(mProgram, "uTex");

    glGenVertexArrays(1, &mVao);
    glGenBuffers(1, &mVbo);

    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);

    // inPos: 2 floats
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, x));

    // inUV: 2 floats
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u));

    // inColor: 4 floats
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, r));

    glBindVertexArray(0);
    return true;
}

bool Renderer2D::InitFont() {
    FT_Library ft = nullptr;
    if (FT_Init_FreeType(&ft) != 0) {
        SC_LOG("FreeType init failed");
        return false;
    }

    PlFontData plFont{};
    bool hasPl = R_SUCCEEDED(plInitialize(PlServiceType_User));
    if (hasPl) {
        plGetSharedFontByType(&plFont, PlSharedFontType_Standard);
    }

    FT_Face face = nullptr;
    if (plFont.address && plFont.size > 0) {
        FT_New_Memory_Face(ft, (const FT_Byte*)plFont.address, plFont.size, 0, &face);
    }

    if (!face) {
        if (hasPl) plExit();
        FT_Done_FreeType(ft);
        SC_LOG("Could not load Nintendo shared font into FreeType");
        return false;
    }

    // Configura o tamanho base do glifo para kFontBaseSize (48px) para nitidez máxima
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(kFontBaseSize));

    mAtlasWidth = 1024;
    mAtlasHeight = 1024;
    std::vector<uint8_t> atlasData(mAtlasWidth * mAtlasHeight, 0);

    int curX = 1;
    int curY = 1;
    int maxRowH = 0;

    for (int c = 32; c < 256; ++c) {
        if (c > 126 && c < 160) continue; // Faixa de controle não imprimível do Latin-1
        if (FT_Load_Char(face, c, FT_LOAD_RENDER) != 0) continue;

        FT_Bitmap& bmp = face->glyph->bitmap;
        if (curX + static_cast<int>(bmp.width) + 1 >= mAtlasWidth) {
            curX = 1;
            curY += maxRowH + 1;
            maxRowH = 0;
        }

        if (curY + static_cast<int>(bmp.rows) + 1 >= mAtlasHeight) break;

        for (unsigned int r = 0; r < bmp.rows; ++r) {
            for (unsigned int col = 0; col < bmp.width; ++col) {
                atlasData[(curY + r) * mAtlasWidth + (curX + col)] = bmp.buffer[r * bmp.pitch + col];
            }
        }

        GlyphInfo& g = mGlyphs[c];
        g.u0 = (float)curX / mAtlasWidth;
        g.v0 = (float)curY / mAtlasHeight;
        g.u1 = (float)(curX + bmp.width) / mAtlasWidth;
        g.v1 = (float)(curY + bmp.rows) / mAtlasHeight;
        g.width = bmp.width;
        g.height = bmp.rows;
        g.bearingX = face->glyph->bitmap_left;
        g.bearingY = face->glyph->bitmap_top;
        g.advance = (int)(face->glyph->advance.x >> 6);

        curX += bmp.width + 2;
        if ((int)bmp.rows > maxRowH) maxRowH = bmp.rows;
    }

    FT_Done_Face(face);
    if (hasPl) plExit();
    FT_Done_FreeType(ft);

    glGenTextures(1, &mFontTexture);
    glBindTexture(GL_TEXTURE_2D, mFontTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, mAtlasWidth, mAtlasHeight, 0, GL_RED, GL_UNSIGNED_BYTE, atlasData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    mFontLoaded = true;
    return true;
}

void Renderer2D::BeginFrame(int screenWidth, int screenHeight) {
    mWidth = screenWidth;
    mHeight = screenHeight;
    mVertices.clear();
    mCurrentTextureMode = 0;

    glViewport(0, 0, mWidth, mHeight);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

void Renderer2D::EndFrame() {
    Flush();
}

void Renderer2D::Flush() {
    if (mVertices.empty()) return;

    glUseProgram(mProgram);

    // Matriz de projeção ortográfica 2D (origem no topo-esquerdo)
    float proj[16] = {
        2.0f / mWidth, 0.0f,           0.0f,  0.0f,
        0.0f,         -2.0f / mHeight, 0.0f,  0.0f,
        0.0f,          0.0f,          -1.0f,  0.0f,
       -1.0f,          1.0f,           0.0f,  1.0f
    };
    glUniformMatrix4fv(mLocProj, 1, GL_FALSE, proj);
    glUniform1i(mLocUseTex, mCurrentTextureMode);

    if (mCurrentTextureMode == 1 && mFontTexture) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mFontTexture);
        glUniform1i(mLocTex, 0);
    }

    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glBufferData(GL_ARRAY_BUFFER, mVertices.size() * sizeof(Vertex), mVertices.data(), GL_STREAM_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, mVertices.size());

    glBindVertexArray(0);
    mVertices.clear();
}

void Renderer2D::PushQuad(float x0, float y0, float x1, float y1,
                          float u0, float v0, float u1, float v1,
                          Color c) {
    mVertices.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a});
    mVertices.push_back({x1, y0, u1, v0, c.r, c.g, c.b, c.a});
    mVertices.push_back({x0, y1, u0, v1, c.r, c.g, c.b, c.a});

    mVertices.push_back({x1, y0, u1, v0, c.r, c.g, c.b, c.a});
    mVertices.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a});
    mVertices.push_back({x0, y1, u0, v1, c.r, c.g, c.b, c.a});
}

void Renderer2D::DrawRect(float x, float y, float w, float h, Color color) {
    if (mCurrentTextureMode != 0) {
        Flush();
        mCurrentTextureMode = 0;
    }
    PushQuad(x, y, x + w, y + h, 0, 0, 0, 0, color);
}

void Renderer2D::DrawRectOutline(float x, float y, float w, float h, float thickness, Color color) {
    DrawRect(x, y, w, thickness, color); // top
    DrawRect(x, y + h - thickness, w, thickness, color); // bottom
    DrawRect(x, y + thickness, thickness, h - 2 * thickness, color); // left
    DrawRect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color); // right
}

void Renderer2D::DrawRoundedRectOutline(float x, float y, float w, float h, float r, float thickness, Color color) {
    r = std::min(r, std::min(w / 2.0f, h / 2.0f));
    float t = std::min(thickness, r);
    if (t <= 0.0f) return;

    // 4 bordas retas (entre os cantos arredondados)
    DrawRect(x + r, y, w - 2.0f * r, t, color); // topo
    DrawRect(x + r, y + h - t, w - 2.0f * r, t, color); // base
    DrawRect(x, y + r, t, h - 2.0f * r, color); // esquerda
    DrawRect(x + w - t, y + r, t, h - 2.0f * r, color); // direita

    // 4 cantos em arcos suaves de espessura exata
    const int segments = 12;
    auto drawCornerRing = [this, color, t](float cx, float cy, float rOuter, float startAngle) {
        if (mCurrentTextureMode != 0) {
            Flush();
            mCurrentTextureMode = 0;
        }
        float rInner = rOuter - t;
        for (int i = 0; i < segments; ++i) {
            float a1 = startAngle + (float)i / segments * (M_PI / 2.0f);
            float a2 = startAngle + (float)(i + 1) / segments * (M_PI / 2.0f);

            float cos1 = std::cos(a1), sin1 = std::sin(a1);
            float cos2 = std::cos(a2), sin2 = std::sin(a2);

            float outX1 = cx + cos1 * rOuter, outY1 = cy + sin1 * rOuter;
            float outX2 = cx + cos2 * rOuter, outY2 = cy + sin2 * rOuter;
            float inX1  = cx + cos1 * rInner, inY1  = cy + sin1 * rInner;
            float inX2  = cx + cos2 * rInner, inY2  = cy + sin2 * rInner;

            // Triângulo 1: in1, out1, out2
            mVertices.push_back({inX1, inY1, 0, 0, color.r, color.g, color.b, color.a});
            mVertices.push_back({outX1, outY1, 0, 0, color.r, color.g, color.b, color.a});
            mVertices.push_back({outX2, outY2, 0, 0, color.r, color.g, color.b, color.a});

            // Triângulo 2: in1, out2, in2
            mVertices.push_back({inX1, inY1, 0, 0, color.r, color.g, color.b, color.a});
            mVertices.push_back({outX2, outY2, 0, 0, color.r, color.g, color.b, color.a});
            mVertices.push_back({inX2, inY2, 0, 0, color.r, color.g, color.b, color.a});
        }
    };

    drawCornerRing(x + r, y + r, r, M_PI);                    // top-left
    drawCornerRing(x + w - r, y + r, r, 3.0f * M_PI / 2.0f); // top-right
    drawCornerRing(x + w - r, y + h - r, r, 0.0f);          // bottom-right
    drawCornerRing(x + r, y + h - r, r, M_PI / 2.0f);       // bottom-left
}

void Renderer2D::DrawRoundedRect(float x, float y, float w, float h, float r, Color color) {
    r = std::min(r, std::min(w / 2.0f, h / 2.0f));
    // 1. Corpo central (cobre largura total entre os cantos e altura total h)
    DrawRect(x + r, y, w - 2.0f * r, h, color);
    // 2. Lateral esquerda (apenas entre os cantos superior e inferior)
    DrawRect(x, y + r, r, h - 2.0f * r, color);
    // 3. Lateral direita (apenas entre os cantos superior e inferior)
    DrawRect(x + w - r, y + r, r, h - 2.0f * r, color);

    // 4 cantos arredondados (arcos preenchidos com alta suavidade)
    const int segments = 16;
    auto drawCorner = [this, color](float cx, float cy, float rad, float startAngle) {
        if (mCurrentTextureMode != 0) {
            Flush();
            mCurrentTextureMode = 0;
        }
        for (int i = 0; i < segments; ++i) {
            float a1 = startAngle + (float)i / segments * (M_PI / 2.0f);
            float a2 = startAngle + (float)(i + 1) / segments * (M_PI / 2.0f);
            mVertices.push_back({cx, cy, 0, 0, color.r, color.g, color.b, color.a});
            mVertices.push_back({cx + std::cos(a1) * rad, cy + std::sin(a1) * rad, 0, 0, color.r, color.g, color.b, color.a});
            mVertices.push_back({cx + std::cos(a2) * rad, cy + std::sin(a2) * rad, 0, 0, color.r, color.g, color.b, color.a});
        }
    };

    drawCorner(x + r, y + r, r, M_PI);             // top-left
    drawCorner(x + w - r, y + r, r, 3.0f * M_PI / 2.0f); // top-right
    drawCorner(x + w - r, y + h - r, r, 0.0f);           // bottom-right
    drawCorner(x + r, y + h - r, r, M_PI / 2.0f);        // bottom-left
}

void Renderer2D::DrawCircle(float cx, float cy, float radius, Color color) {
    if (mCurrentTextureMode != 0) {
        Flush();
        mCurrentTextureMode = 0;
    }
    const int segments = 24;
    for (int i = 0; i < segments; ++i) {
        float a1 = (float)i / segments * 2.0f * M_PI;
        float a2 = (float)(i + 1) / segments * 2.0f * M_PI;
        mVertices.push_back({cx, cy, 0, 0, color.r, color.g, color.b, color.a});
        mVertices.push_back({cx + std::cos(a1) * radius, cy + std::sin(a1) * radius, 0, 0, color.r, color.g, color.b, color.a});
        mVertices.push_back({cx + std::cos(a2) * radius, cy + std::sin(a2) * radius, 0, 0, color.r, color.g, color.b, color.a});
    }
}

void Renderer2D::DrawPill(float x, float y, float w, float h, Color color) {
    DrawRoundedRect(x, y, w, h, h * 0.5f, color);
}

void Renderer2D::DrawPillOutline(float x, float y, float w, float h, float thickness, Color color) {
    DrawRoundedRectOutline(x, y, w, h, h * 0.5f, thickness, color);
}

void Renderer2D::DrawProgressBar(float x, float y, float w, float h, float progress, Color bgColor, Color fillProgressColor) {
    progress = std::clamp(progress, 0.0f, 1.0f);
    DrawPill(x, y, w, h, bgColor);
    if (progress > 0.001f) {
        DrawPill(x, y, std::max(w * progress, h), h, fillProgressColor);
    }
}

void Renderer2D::DrawLine(float x0, float y0, float x1, float y1, float thickness, Color color) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) return;

    if (mCurrentTextureMode != 0) {
        Flush();
        mCurrentTextureMode = 0;
    }

    float halfThick = thickness * 0.5f;
    float nx = (-dy / len) * halfThick;
    float ny = (dx / len) * halfThick;

    mVertices.push_back({x0 + nx, y0 + ny, 0, 0, color.r, color.g, color.b, color.a});
    mVertices.push_back({x1 + nx, y1 + ny, 0, 0, color.r, color.g, color.b, color.a});
    mVertices.push_back({x1 - nx, y1 - ny, 0, 0, color.r, color.g, color.b, color.a});

    mVertices.push_back({x0 + nx, y0 + ny, 0, 0, color.r, color.g, color.b, color.a});
    mVertices.push_back({x1 - nx, y1 - ny, 0, 0, color.r, color.g, color.b, color.a});
    mVertices.push_back({x0 - nx, y0 - ny, 0, 0, color.r, color.g, color.b, color.a});
}

void Renderer2D::DrawDiamondGlow(float cx, float cy, float radiusX, float radiusY, Color centerColor, Color edgeColor) {
    if (mCurrentTextureMode != 0) {
        Flush();
        mCurrentTextureMode = 0;
    }

    const int rings = 6;
    for (int r = 0; r < rings; ++r) {
        float t0 = (float)r / rings;
        float t1 = (float)(r + 1) / rings;

        // Decaimento quadrático para simulação perfeita de desfoque gaussiano
        float a0 = centerColor.a * (1.0f - t0 * t0) + edgeColor.a * (t0 * t0);
        float a1 = centerColor.a * (1.0f - t1 * t1) + edgeColor.a * (t1 * t1);

        Color c0 = {
            centerColor.r * (1.0f - t0) + edgeColor.r * t0,
            centerColor.g * (1.0f - t0) + edgeColor.g * t0,
            centerColor.b * (1.0f - t0) + edgeColor.b * t0,
            a0
        };
        Color c1 = {
            centerColor.r * (1.0f - t1) + edgeColor.r * t1,
            centerColor.g * (1.0f - t1) + edgeColor.g * t1,
            centerColor.b * (1.0f - t1) + edgeColor.b * t1,
            a1
        };

        // 4 vértices do anel interno (losango)
        float inTopX = cx, inTopY = cy - radiusY * t0;
        float inRightX = cx + radiusX * t0, inRightY = cy;
        float inBotX = cx, inBotY = cy + radiusY * t0;
        float inLeftX = cx - radiusX * t0, inLeftY = cy;

        // 4 vértices do anel externo (losango)
        float outTopX = cx, outTopY = cy - radiusY * t1;
        float outRightX = cx + radiusX * t1, outRightY = cy;
        float outBotX = cx, outBotY = cy + radiusY * t1;
        float outLeftX = cx - radiusX * t1, outLeftY = cy;

        auto pushQuadCol = [this](float x0, float y0, Color ca,
                                  float x1, float y1, Color cb,
                                  float x2, float y2, Color cc,
                                  float x3, float y3, Color cd) {
            mVertices.push_back({x0, y0, 0, 0, ca.r, ca.g, ca.b, ca.a});
            mVertices.push_back({x1, y1, 0, 0, cb.r, cb.g, cb.b, cb.a});
            mVertices.push_back({x2, y2, 0, 0, cc.r, cc.g, cc.b, cc.a});

            mVertices.push_back({x0, y0, 0, 0, ca.r, ca.g, ca.b, ca.a});
            mVertices.push_back({x2, y2, 0, 0, cc.r, cc.g, cc.b, cc.a});
            mVertices.push_back({x3, y3, 0, 0, cd.r, cd.g, cd.b, cd.a});
        };

        // 4 quadriláteros do anel
        pushQuadCol(inTopX, inTopY, c0, outTopX, outTopY, c1, outRightX, outRightY, c1, inRightX, inRightY, c0);
        pushQuadCol(inRightX, inRightY, c0, outRightX, outRightY, c1, outBotX, outBotY, c1, inBotX, inBotY, c0);
        pushQuadCol(inBotX, inBotY, c0, outBotX, outBotY, c1, outLeftX, outLeftY, c1, inLeftX, inLeftY, c0);
        pushQuadCol(inLeftX, inLeftY, c0, outLeftX, outLeftY, c1, outTopX, outTopY, c1, inTopX, inTopY, c0);
    }
}

void Renderer2D::DrawPlayStationSymbol(char symbol, float cx, float cy, float size, Color color, bool withShadow) {
    if (withShadow) {
        // Regra do Design System: Sombra preta profunda com deslocamento dx=2, dy=2.5
        DrawPlayStationSymbol(symbol, cx + 2.0f, cy + 2.5f, size, Color::Hex(0x000000, 0.95f), false);
    }

    const float half = size * 0.5f;
    const float thick = std::max(1.6f, size * 0.085f);

    switch (symbol) {
        case 'P': // Ícone Play Preenchido ▶ (apontando para a direita)
        case '>': {
            if (mCurrentTextureMode != 0) {
                Flush();
                mCurrentTextureMode = 0;
            }
            const float xLeft  = cx - half * 0.65f;
            const float xRight = cx + half * 0.85f;
            const float yTop   = cy - half * 0.85f;
            const float yBot   = cy + half * 0.85f;
            mVertices.push_back({xLeft,  yTop, 0, 0, color.r, color.g, color.b, color.a});
            mVertices.push_back({xRight, cy,   0, 0, color.r, color.g, color.b, color.a});
            mVertices.push_back({xLeft,  yBot, 0, 0, color.r, color.g, color.b, color.a});
            break;
        }
        case 'D': // Losango ◇
        case 'd': {
            float r = half * 0.85f;
            DrawLine(cx, cy - r, cx + r, cy, thick, color);
            DrawLine(cx + r, cy, cx, cy + r, thick, color);
            DrawLine(cx, cy + r, cx - r, cy, thick, color);
            DrawLine(cx - r, cy, cx, cy - r, thick, color);
            break;
        }
        case 'T': // Triângulo △
        case '^': {
            float x0 = cx, y0 = cy - half;
            float x1 = cx + half, y1 = cy + half * 0.8f;
            float x2 = cx - half, y2 = cy + half * 0.8f;
            DrawLine(x0, y0, x1, y1, thick, color);
            DrawLine(x1, y1, x2, y2, thick, color);
            DrawLine(x2, y2, x0, y0, thick, color);
            break;
        }
        case 'C': // Círculo ○
        case 'O': {
            float rad = half * 0.85f;
            const int segs = 16;
            for (int i = 0; i < segs; ++i) {
                float a0 = (float)i / segs * 2.0f * M_PI;
                float a1 = (float)(i + 1) / segs * 2.0f * M_PI;
                DrawLine(cx + std::cos(a0) * rad, cy + std::sin(a0) * rad,
                         cx + std::cos(a1) * rad, cy + std::sin(a1) * rad, thick, color);
            }
            break;
        }
        case 'X': // Cruz ✕
        case 'x': {
            float r = half * 0.75f;
            DrawLine(cx - r, cy - r, cx + r, cy + r, thick, color);
            DrawLine(cx - r, cy + r, cx + r, cy - r, thick, color);
            break;
        }
        case 'S': // Quadrado □
        case '#': {
            float r = half * 0.72f;
            DrawLine(cx - r, cy - r, cx + r, cy - r, thick, color);
            DrawLine(cx + r, cy - r, cx + r, cy + r, thick, color);
            DrawLine(cx + r, cy + r, cx - r, cy + r, thick, color);
            DrawLine(cx - r, cy + r, cx - r, cy - r, thick, color);
            break;
        }
        default:
            break;
    }
}

void Renderer2D::DrawPlayStationPattern(float x, float y, float w, float h, float spacing) {
    const char symbols[] = {'D', 'C', 'S', 'T'};
    int rowIdx = 0;

    for (float curY = y + 16.0f; curY < y + h; curY += spacing, ++rowIdx) {
        float startX = (rowIdx % 2 == 1) ? (x + spacing * 0.5f) : (x + 20.0f);
        int colIdx = 0;
        for (float curX = startX; curX < x + w; curX += spacing, ++colIdx) {
            char sym = symbols[(rowIdx * 2 + colIdx) % 4];

            // Cores sutis com acentos ocasionais dos Joy-Cons
            Color col = Color::Rgba(255, 255, 255, 20);
            if ((colIdx + rowIdx) % 5 == 1) {
                col = Color::Rgba(0, 195, 227, 42);  // Joy-Con L Neon Blue
            } else if ((colIdx + rowIdx) % 5 == 3) {
                col = Color::Rgba(255, 60, 40, 42);   // Joy-Con R Neon Red
            }

            DrawPlayStationSymbol(sym, curX, curY, 14.0f, col, true);
        }
    }
}

void Renderer2D::DrawRoundedRectWithShadow(float x, float y, float w, float h, float radius,
                                           Color fillColor, Color outlineColor, float outlineThickness,
                                           float shadowOffsetY, float shadowSpread, Color shadowColor) {
    // Sombra projetada profunda
    DrawRoundedRect(x - shadowSpread * 0.5f, y + shadowOffsetY,
                    w + shadowSpread, h,
                    radius + shadowSpread * 0.4f, shadowColor);

    // Superfície do card
    DrawRoundedRect(x, y, w, h, radius, fillColor);

    // Delineado de borda sólida escura
    if (outlineThickness > 0.0f) {
        DrawRoundedRectOutline(x, y, w, h, radius, outlineThickness, outlineColor);
    }
}

namespace {
inline int NextUtf8CodePoint(const std::string& text, size_t& i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
        return c;
    }
    // Sequência de 2 bytes (0xC0 a 0xDF) ex.: acentos comuns como á, é, í, ó, ú, ç, ã
    if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
        unsigned char c2 = static_cast<unsigned char>(text[i + 1]);
        if ((c2 & 0xC0) == 0x80) {
            i += 1;
            return ((c & 0x1F) << 6) | (c2 & 0x3F);
        }
    }
    // Sequência de 3 bytes (0xE0 a 0xEF)
    else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
        unsigned char c2 = static_cast<unsigned char>(text[i + 1]);
        unsigned char c3 = static_cast<unsigned char>(text[i + 2]);
        if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
            i += 2;
            return ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        }
    }
    // Sequência de 4 bytes (0xF0 a 0xF7) ex.: emojis e símbolos adicionais
    else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
        unsigned char c2 = static_cast<unsigned char>(text[i + 1]);
        unsigned char c3 = static_cast<unsigned char>(text[i + 2]);
        unsigned char c4 = static_cast<unsigned char>(text[i + 3]);
        if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80 && (c4 & 0xC0) == 0x80) {
            i += 3;
            return ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
        }
    }
    return c;
}
}  // namespace

float Renderer2D::MeasureTextWidth(const std::string& text, float size) {
    float scale = size / kFontBaseSize;
    float width = 0.0f;
    for (size_t i = 0; i < text.size(); ++i) {
        int code = NextUtf8CodePoint(text, i);
        if (code >= 32 && code < 256 && mGlyphs[code].advance > 0) {
            width += mGlyphs[code].advance * scale;
        } else if (code >= 32) {
            width += 14.0f * scale;
        }
    }
    return width;
}

void Renderer2D::DrawText(float x, float y, const std::string& text, float size, Color color, TextAlign align) {
    if (!mFontLoaded) {
        // Fallback visual: retângulo marcando o texto caso o atlas não tenha sido gerado
        DrawRect(x, y, MeasureTextWidth(text, size), size, Color::Rgba(80, 80, 80, 180));
        return;
    }

    if (mCurrentTextureMode != 1) {
        Flush();
        mCurrentTextureMode = 1;
    }

    float scale = size / kFontBaseSize;
    float drawX = x;

    if (align == TextAlign::Center) {
        drawX -= MeasureTextWidth(text, size) / 2.0f;
    } else if (align == TextAlign::Right) {
        drawX -= MeasureTextWidth(text, size);
    }

    for (size_t i = 0; i < text.size(); ++i) {
        int code = NextUtf8CodePoint(text, i);
        if (code < 32) continue;

        if (code == 32) {
            float spAdv = (mGlyphs[32].advance > 0) ? static_cast<float>(mGlyphs[32].advance) : 14.0f;
            drawX += spAdv * scale;
            continue;
        }

        if (code < 256 && mGlyphs[code].width > 0 && mGlyphs[code].height > 0) {
            const GlyphInfo& g = mGlyphs[code];
            float x0 = drawX + g.bearingX * scale;
            float y0 = y + (kFontBaseSize * scale) - (g.bearingY * scale);
            float x1 = x0 + g.width * scale;
            float y1 = y0 + g.height * scale;

            PushQuad(x0, y0, x1, y1, g.u0, g.v0, g.u1, g.v1, color);
            drawX += g.advance * scale;
        } else if (code < 256 && mGlyphs[code].advance > 0) {
            drawX += mGlyphs[code].advance * scale;
        } else {
            drawX += 14.0f * scale;
        }
    }
}

void Renderer2D::DrawTextWithShadow(float x, float y, const std::string& text, float size, Color color,
                                    TextAlign align, float shadowDx, float shadowDy, Color shadowColor) {
    // 1. Sombra preta profunda desenhada primeiro
    DrawText(x + shadowDx, y + shadowDy, text, size, shadowColor, align);
    // 2. Texto principal frontal
    DrawText(x, y, text, size, color, align);
}

} // namespace SwitchCast

