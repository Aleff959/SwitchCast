// ui/OledBurnInGuard.hpp
#pragma once

#include <random>

namespace SwitchCast {

// Proteção contra retenção de imagem no painel OLED: quando não há mídia,
// reduz o brilho da UI e desloca os elementos estáticos a cada 20 s.
class OledBurnInGuard {
public:
    static constexpr float kIdleThresholdSec = 45.0f;
    static constexpr float kShiftIntervalSec = 20.0f;
    static constexpr float kDimmedBrightness = 0.35f;
    static constexpr int kMaxShiftPixels = 4;

    void Update(float deltaTime, bool isMediaPlaying);
    void NotifyActivity() { mIdleTimeSeconds = 0.0f; }

    float DimmingFactor() const { return mDimmingFactor; }  // 1.0 = brilho normal
    int OffsetX() const { return mOffsetX; }
    int OffsetY() const { return mOffsetY; }
    bool IsProtecting() const { return mDimmingFactor < 1.0f; }

private:
    std::mt19937 mRng{std::random_device{}()};
    float mIdleTimeSeconds = 0.0f;
    float mPixelShiftTimer = 0.0f;
    float mDimmingFactor = 1.0f;
    int mOffsetX = 0;
    int mOffsetY = 0;
};

}  // namespace SwitchCast
