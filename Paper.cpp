#include "Paper.h"

#include <aml-psdk/game_sa/utils/OpcodeCaller.h>

#include <cmath>

namespace Paper
{
    void Disturb(float& velocityX, float& velocityY, float& velocityZ,
        bool& airborne, float directionX, float directionY, float strength)
    {
        velocityX = velocityX * 0.35f + directionX * strength;
        velocityY = velocityY * 0.35f + directionY * strength;
        // Pedestrians make a small puff that nudges the sheet; vehicles create
        // a stronger gust capable of lofting it properly.
        velocityZ = strength <= 1.5f ? 0.45f : 2.4f + strength * 0.45f;
        airborne = true;
    }

    void UpdateObject(int handle, float& x, float& y, float& z,
        float& velocityX, float& velocityY, float& velocityZ,
        float& rotation, bool& airborne, bool continuousFlight,
        float hoverBaseZ, float groundZ, float deltaSeconds)
    {
        if (deltaSeconds <= 0.0f || handle < 0) return;
        if (deltaSeconds > 0.1f) deltaSeconds = 0.1f;

        if (airborne)
        {
            // Some sheets ride the wind continuously; others lose lift and fall.
            if (continuousFlight)
                z = hoverBaseZ + std::sin(rotation * 0.035f) * 0.25f;
            else
                velocityZ -= 4.2f * deltaSeconds;
            velocityX += std::sin(rotation * 0.043f) * 0.35f * deltaSeconds;
            velocityY += std::cos(rotation * 0.037f) * 0.28f * deltaSeconds;
            x += velocityX * deltaSeconds;
            y += velocityY * deltaSeconds;
            if (!continuousFlight)
                z += velocityZ * deltaSeconds;
            if (!continuousFlight && z <= groundZ + GROUND_OFFSET && velocityZ < 0.0f)
            {
                z = groundZ + GROUND_OFFSET;
                velocityZ = 0.0f;
                velocityX *= 0.45f;
                velocityY *= 0.45f;
                airborne = false;
                // Keep the paper flat on the ground on the very landing frame.
                Command<Commands::SET_OBJECT_ROTATION>(handle,
                    GROUND_ROTATION_X, GROUND_ROTATION_Y, GROUND_ROTATION_Z);
            }
        }
        else
        {
            z = groundZ + GROUND_OFFSET;
            // Once settled, paper stays still and flat until traffic disturbs it.
            velocityX = 0.0f;
            velocityY = 0.0f;
            velocityZ = 0.0f;
        }
        if (airborne)
            rotation += (70.0f + velocityY * 18.0f) * deltaSeconds;
        if (rotation >= 360.0f) rotation -= 360.0f;

        Command<Commands::SET_OBJECT_COORDINATES>(handle, x, y, z);
        if (airborne)
            Command<Commands::SET_OBJECT_ROTATION>(handle,
                GROUND_ROTATION_X + std::sin(rotation * 0.01745329252f) * 22.0f,
                GROUND_ROTATION_Y + std::cos(rotation * 0.013f) * 18.0f,
                GROUND_ROTATION_Z + rotation);
        else
            Command<Commands::SET_OBJECT_ROTATION>(handle,
                GROUND_ROTATION_X, GROUND_ROTATION_Y, GROUND_ROTATION_Z);
    }
}
