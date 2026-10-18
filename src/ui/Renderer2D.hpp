// ui/Renderer2D.hpp - Renderizador 2D nativo em OpenGL 4.3 para o SwitchCast.
// Desenha a interface completa (cards, textos, badges, barras) diretamente na tela,
// sem depender da libmpv para renderizar elementos de UI quando o player está ocioso.
#pragma once

#include <glad/glad.h>
#include <cstdint>
#include <string>
#include <vector>

namespace SwitchCast {

struct Color {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

    static Color Rgb(uint8_t r, uint8_t g, uint8_t b) {
        return {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
    }
    static Color Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }
    static Color Hex(uint32_t rgb, float a = 1.0f) {
        return {((rgb >> 16) & 0xFF) / 255.0f,
                ((rgb >> 8) & 0xFF) / 255.0f,
                (rgb & 0xFF) / 255.0f,
                a};
    }
};

enum class TextAlign {
    Left,
    Center,
    Right
};

class Renderer2D {
public:
    Renderer2D() = default;
    ~Renderer2D();

    bool Initialize(int screenWidth, int screenHeight);
    void Shutdown();

    void BeginFrame(int screenWidth, int screenHeight);
    void EndFrame();

    // Primitivas de renderização 2D
    void DrawRect(float x, float y, float w, float h, Color color);
    void DrawRoundedRect(float x, float y, float w, float h, float radius, Color color);
    void DrawRectOutline(float x, float y, float w, float h, float thickness, Color color);
    void DrawRoundedRectOutline(float x, float y, float w, float h, float radius, float thickness, Color color);
    void DrawCircle(float cx, float cy, float radius, Color color);
    void DrawPill(float x, float y, float w, float h, Color color);
    void DrawPillOutline(float x, float y, float w, float h, float thickness, Color color);
    void DrawProgressBar(float x, float y, float w, float h, float progress, Color bgColor, Color fillProgressColor);

    // Renderização de texto (FreeType com fonte nativa do Switch ou fallback de bitmap)
    void DrawText(float x, float y, const std::string& text, float size, Color color, TextAlign align = TextAlign::Left);
    float MeasureTextWidth(const std::string& text, float size);

private:
    struct Vertex {
        float x, y;
        float u, v;
        float r, g, b, a;
    };

    struct GlyphInfo {
        float u0, v0, u1, v1;
        int width, height;
        int bearingX, bearingY;
        int advance;
    };

    bool InitShaders();
    bool InitFont();
    void Flush();
    void PushQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  Color color);

    int mWidth = 1280;
    int mHeight = 720;

    GLuint mProgram = 0;
    GLuint mVao = 0;
    GLuint mVbo = 0;
    GLint mLocProj = -1;
    GLint mLocUseTex = -1;
    GLint mLocTex = -1;

    GLuint mFontTexture = 0;
    int mAtlasWidth = 512;
    int mAtlasHeight = 512;
    GlyphInfo mGlyphs[256] = {};
    bool mFontLoaded = false;

    std::vector<Vertex> mVertices;
    int mCurrentTextureMode = 0; // 0 = sem textura (cor plana), 1 = fonte alpha
};

}  // namespace SwitchCast
