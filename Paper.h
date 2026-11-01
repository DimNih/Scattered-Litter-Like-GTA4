#pragma once

namespace Paper
{
    // The paper model pivot sits slightly above its visible face. A small
    // negative offset makes the sheet settle into the road instead of
    // hovering visibly above it.
    constexpr float GROUND_OFFSET = -0.08f;
    constexpr float GROUND_ROTATION_X = -180.0f;
    constexpr float GROUND_ROTATION_Y = 88.0f;
    constexpr float GROUND_ROTATION_Z = -180.0f;

    void UpdateObject(int handle, float& x, float& y, float& z,
        float& velocityX, float& velocityY, float& velocityZ,
        float& rotation, bool& airborne, bool continuousFlight,
        float hoverBaseZ, float groundZ, float deltaSeconds);
    void Disturb(float& velocityX, float& velocityY, float& velocityZ,
        bool& airborne, float directionX, float directionY, float strength);
}
