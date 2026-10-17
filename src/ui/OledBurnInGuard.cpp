// ui/OledBurnInGuard.cpp
#include "OledBurnInGuard.hpp"

namespace SwitchCast {

void OledBurnInGuard::Update(float deltaTime, bool isMediaPlaying) {
    if (isMediaPlaying) {
        mIdleTimeSeconds = 0.0f;
        mPixelShiftTimer = 0.0f;
        mDimmingFactor = 1.0f;
        mOffsetX = mOffsetY = 0;
        return;
    }

    mIdleTimeSeconds += deltaTime;
    if (mIdleTimeSeconds <= kIdleThresholdSec) {
        mDimmingFactor = 1.0f;
        return;
    }

    mDimmingFactor = kDimmedBrightness;

    mPixelShiftTimer += deltaTime;
    if (mPixelShiftTimer >= kShiftIntervalSec) {
        mPixelShiftTimer = 0.0f;
        std::uniform_int_distribution<int> dist(-kMaxShiftPixels, kMaxShiftPixels);
        mOffsetX = dist(mRng);
        mOffsetY = dist(mRng);
    }
}

}  // namespace SwitchCast
