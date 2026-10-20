#pragma once

namespace Rat
{
    constexpr int DEFAULT_MODEL_ID = 806;

    void Initialize(bool enabled, int modelId, int maxRats, int spawnIntervalMs,
        float moveSpeed, float spawnMinDistance, float spawnRadius,
        float cleanupRadius, float scale, float headingOffset,
        bool soundEnabled, float soundRadius, bool localLogEnabled);
    void Shutdown();
}
