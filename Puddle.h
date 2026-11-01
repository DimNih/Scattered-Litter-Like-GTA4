#pragma once

namespace Puddle
{
    void Initialize(bool enabled, int maxPuddles, int spawnIntervalMs,
        float spawnMinDistance, float spawnRadius, float cleanupRadius,
        unsigned int lifeMinMs = 35000U, unsigned int lifeMaxMs = 70000U,
        bool reflection = true, float reflectionDistance = 14.0f);
    void Shutdown();
}
