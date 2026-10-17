#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

#include <aml-psdk/game_sa/Events.h>
#include <aml-psdk/game_sa/engine/Streaming.h>
#include <aml-psdk/game_sa/entity/Camera.h>
#include <aml-psdk/game_sa/entity/PlayerPed.h>
#include <aml-psdk/game_sa/base/Timer.h>
#include <aml-psdk/game_sa/utils/OpcodeCaller.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

MYMODCFG(net.dimas.roadobjectsgta4, Road Objects GTA IV - Starter, 0.5, Dimas)

namespace
{
    // Verified GTA:SA model ID: PROC_RUBBISH_4 (2672), PROC_RUB.txd.
    // These four built-in debris models are visual fallbacks. Custom Rub_*.dff
    // files require the CLEO+ special-render-object loader and are not normal SA IDs.
    constexpr int MODEL_VARIANTS[] = { 2670, 2671, 2672, 2673 };
    constexpr int MODEL_VARIANT_COUNT = sizeof(MODEL_VARIANTS) / sizeof(MODEL_VARIANTS[0]);
    constexpr int STREAMING_GAME_REQUIRED = 2;
    constexpr int MAX_TRACKED_OBJECTS = 50;
    constexpr int MAX_ROAD_CANDIDATES = 32;

    struct LitterObject
    {
        int handle = -1;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool active = false;
    };

    LitterObject g_objects[MAX_TRACKED_OBJECTS];
    bool g_modelAvailable[MODEL_VARIANT_COUNT] = {};
    bool g_enabled = true;
    bool g_modelRequested = false;
    bool g_modelReady = false;
    bool g_modelUnavailable = false;
    bool g_modelWaitLogged = false;
    bool g_spawnLogged = false;
    bool g_spawnFailureLogged = false;
    bool g_roadSearchFailureLogged = false;
    int g_maxObjects = 50;
    float g_spawnRadius = 70.0f;
    float g_cleanupRadius = 90.0f;
    float g_minSpawnDistance = 28.0f;
    float g_minObjectSpacing = 2.5f;
    float g_roadLateralRange = 8.0f;
    int g_spawnIntervalMs = 700;
    unsigned int g_nextSpawnTime = 0;
    unsigned int g_modelRequestTime = 0;
    FILE* g_logFile = nullptr;
    char g_logPath[512] = {};

    void WriteLogToFile(eLogPrio priority, const char* message)
    {
        if (!g_logFile) return;
        std::fprintf(g_logFile, "[%s] %s\n", logger->EnumPrio(priority), message ? message : "");
        std::fflush(g_logFile);
    }

    bool StartFileLogging()
    {
        if (!aml) return false;
        const char* dataPath = aml->GetAndroidDataPath();
        if (!dataPath || !dataPath[0]) return false;

        const int pathLength = std::snprintf(g_logPath, sizeof(g_logPath),
            "%s/RoadObjectsGTA4.log", dataPath);
        if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(g_logPath))
        {
            g_logPath[0] = '\0';
            return false;
        }

        g_logFile = std::fopen(g_logPath, "a");
        if (!g_logFile) return false;
        logger->SetMessageCB(WriteLogToFile);
        return true;
    }

    int RandomInt(int minValue, int maxValue)
    {
        if (maxValue <= minValue) return minValue;
        return minValue + (std::rand() % (maxValue - minValue + 1));
    }

    float RandomFloat(float minValue, float maxValue)
    {
        const float t = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
        return minValue + (maxValue - minValue) * t;
    }

    int CountObjects()
    {
        int count = 0;
        for (const auto& object : g_objects)
            if (object.active) ++count;
        return count;
    }

    void DeleteObject(LitterObject& object)
    {
        if (!object.active || object.handle < 0) return;
        int handle = object.handle;
        Command<Commands::DELETE_OBJECT>(&handle);
        object.handle = -1;
        object.active = false;
    }

    void CleanupFarObjects(float playerX, float playerY, float playerZ)
    {
        const float maxDistSq = g_cleanupRadius * g_cleanupRadius;
        for (auto& object : g_objects)
        {
            if (!object.active) continue;
            const float dx = object.x - playerX;
            const float dy = object.y - playerY;
            const float dz = object.z - playerZ;
            if (dx * dx + dy * dy + dz * dz > maxDistSq)
                DeleteObject(object);
        }
    }

    bool IsFarEnoughFromExisting(float x, float y, float z)
    {
        const float spacingSq = g_minObjectSpacing * g_minObjectSpacing;
        for (const auto& object : g_objects)
        {
            if (!object.active) continue;
            const float dx = object.x - x;
            const float dy = object.y - y;
            const float dz = object.z - z;
            if (dx * dx + dy * dy + dz * dz < spacingSq) return false;
        }
        return true;
    }

    bool SpawnOne(CPlayerPed* player, const CVector& playerPos)
    {
        LitterObject* slot = nullptr;
        for (auto& object : g_objects)
            if (!object.active) { slot = &object; break; }
        if (!slot) return false;

        const CVector forward = player->GetForward();
        const CVector right = player->GetRightDirection();
        bool foundRoadSpot = false;
        float x = 0.0f, y = 0.0f, groundZ = 0.0f, roadHeading = 0.0f;
        for (int attempt = 0; attempt < MAX_ROAD_CANDIDATES; ++attempt)
        {
            // Prefer the forward arc, but occasionally sample all around CJ.
            // The radius keeps the eventual object away from the player's view.
            float angle = 0.0f;
            if (RandomInt(0, 99) < 85)
                angle = RandomFloat(-1.74532925f, 1.74532925f); // +/- 100 degrees
            else
                angle = RandomFloat(0.0f, 6.28318531f);
            const float radius = RandomFloat(g_minSpawnDistance, g_spawnRadius);
            const float lateral = RandomFloat(-2.0f, 2.0f);
            const float dirX = forward.x * std::cos(angle) + right.x * std::sin(angle);
            const float dirY = forward.y * std::cos(angle) + right.y * std::sin(angle);
            const float queryX = playerPos.x + dirX * radius + right.x * lateral;
            const float queryY = playerPos.y + dirY * radius + right.y * lateral;
            const float queryZ = playerPos.z;
            float nodeX = queryX, nodeY = queryY, nodeZ = queryZ;
            float nodeHeading = 0.0f;

            if (!Command<Commands::GET_CLOSEST_CAR_NODE_WITH_HEADING>(
                    queryX, queryY, queryZ, &nodeX, &nodeY, &nodeZ, &nodeHeading))
                continue;

            const float nodeDx = nodeX - queryX;
            const float nodeDy = nodeY - queryY;
            if (nodeDx * nodeDx + nodeDy * nodeDy > 100.0f) continue;

            // CPlaceable heading basis: right=(cos,sin), forward=(-sin,cos).
            // Small offsets stay on the road; larger offsets reach curb/sidewalk.
            const float headingRad = nodeHeading * 0.01745329252f;
            const bool sidewalk = RandomInt(0, 99) < 45;
            const float side = (RandomInt(0, 1) == 0) ? -1.0f : 1.0f;
            const float edgeOffset = sidewalk
                ? side * RandomFloat(4.0f, g_roadLateralRange)
                : RandomFloat(-3.5f, 3.5f);
            const float alongRoad = RandomFloat(-4.0f, 4.0f);
            x = nodeX + std::cos(headingRad) * edgeOffset - std::sin(headingRad) * alongRoad;
            y = nodeY + std::sin(headingRad) * edgeOffset + std::cos(headingRad) * alongRoad;

            const float fromPlayerX = x - playerPos.x;
            const float fromPlayerY = y - playerPos.y;
            const float distanceFromPlayer = std::sqrt(fromPlayerX * fromPlayerX + fromPlayerY * fromPlayerY);
            if (distanceFromPlayer < g_minSpawnDistance || distanceFromPlayer > g_spawnRadius + 12.0f) continue;

            if (!Command<Commands::GET_GROUND_Z_FOR_3D_COORD>(x, y, nodeZ + 10.0f, &groundZ))
                continue;
            if (!std::isfinite(groundZ) || std::fabs(groundZ - nodeZ) > 8.0f) continue;

            // Reject visible close-range points; distant visible spots are
            // allowed so front-of-player road litter can still accumulate.
            CVector spot(x, y, groundZ);
            if (distanceFromPlayer < g_minSpawnDistance + 22.0f && TheCamera.IsSphereVisible(&spot, 1.2f))
                continue;
            if (!IsFarEnoughFromExisting(x, y, groundZ)) continue;

            foundRoadSpot = true;
            roadHeading = RandomFloat(0.0f, 360.0f);
            break;
        }

        if (!foundRoadSpot)
        {
            if (!g_roadSearchFailureLogged)
            {
                logger->Info("No suitable road point found in the spawn radius yet; will retry.");
                g_roadSearchFailureLogged = true;
            }
            return false;
        }

        int availableModels[MODEL_VARIANT_COUNT];
        int availableCount = 0;
        for (int i = 0; i < MODEL_VARIANT_COUNT; ++i)
            if (g_modelAvailable[i]) availableModels[availableCount++] = MODEL_VARIANTS[i];
        if (availableCount == 0) return false;

        const int modelId = availableModels[RandomInt(0, availableCount - 1)];
        int handle = -1;
        Command<Commands::CREATE_OBJECT>(modelId, x, y, groundZ, &handle);
        if (handle < 0)
        {
            if (!g_spawnFailureLogged)
            {
                logger->Error("CREATE_OBJECT failed: model=%d pos=(%.1f, %.1f, %.1f).", modelId, x, y, groundZ);
                g_spawnFailureLogged = true;
            }
            return false;
        }
        Command<Commands::SET_OBJECT_HEADING>(handle, roadHeading);
        Command<Commands::SET_OBJECT_SCALE>(handle, RandomFloat(0.75f, 1.25f));

        slot->handle = handle;
        slot->x = x;
        slot->y = y;
        slot->z = groundZ;
        slot->active = true;
        if (!g_spawnLogged)
        {
            logger->Info("First road litter created: model=%d handle=%d pos=(%.1f, %.1f, %.1f).",
                modelId, handle, x, y, groundZ);
            g_spawnLogged = true;
        }
        return true;
    }

    void Tick()
    {
        if (!g_enabled || g_modelUnavailable) return;
        CPlayerPed* player = FindPlayerPed(0);
        if (!player) return;

        // AML can load a mod before GTA has initialized its streaming system.
        // Request the built-in variants from the game-process callback.
        if (!g_modelRequested)
        {
            int availableCount = 0;
            for (int i = 0; i < MODEL_VARIANT_COUNT; ++i)
            {
                const int modelId = MODEL_VARIANTS[i];
                if (!CStreaming::IsObjectInCdImage(modelId))
                {
                    logger->Info("Built-in debris model %d unavailable; skipping it.", modelId);
                    continue;
                }
                CStreaming::RequestModel(modelId, STREAMING_GAME_REQUIRED);
                g_modelAvailable[i] = true;
                ++availableCount;
            }
            if (availableCount == 0)
            {
                logger->Error("None of the built-in debris models are available in this game.");
                g_modelUnavailable = true;
                return;
            }
            g_modelRequested = true;
            g_modelRequestTime = CTimer::GetTimeMS();
            logger->Info("Requested %d built-in litter model variants.", availableCount);
            return;
        }
        if (!g_modelReady)
        {
            bool allReady = true;
            for (int i = 0; i < MODEL_VARIANT_COUNT; ++i)
            {
                if (g_modelAvailable[i] && !Command<Commands::HAS_MODEL_LOADED>(MODEL_VARIANTS[i]))
                {
                    allReady = false;
                    break;
                }
            }
            if (!allReady)
            {
                if (!g_modelWaitLogged && CTimer::GetTimeMS() - g_modelRequestTime > 10000U)
                {
                    logger->Error("Built-in litter variants are still loading after 10 seconds.");
                    g_modelWaitLogged = true;
                }
                return;
            }
            g_modelReady = true;
            g_nextSpawnTime = CTimer::GetTimeMS() + 2500U;
            logger->Info("Built-in litter model variants are ready.");
        }

        if (player->GetInterior() != 0) return;

        const CVector playerPos = player->GetPosition();
        CleanupFarObjects(playerPos.x, playerPos.y, playerPos.z);

        const unsigned int now = CTimer::GetTimeMS();
        if (static_cast<int>(now - g_nextSpawnTime) < 0) return;
        if (CountObjects() >= g_maxObjects) return;

        if (SpawnOne(player, playerPos))
            g_nextSpawnTime = now + static_cast<unsigned int>(g_spawnIntervalMs);
        else
            g_nextSpawnTime = now + 1000U;
    }
}

ON_MOD_LOAD()
{
    logger->SetTag("RoadObjectsGTA4");
    const bool fileLoggingEnabled = StartFileLogging();
    if (fileLoggingEnabled)
        logger->Info("Writing mod log to %s", g_logPath);
    else
        logger->Error("Could not open the mod log under the game's Android data folder; continuing with Logcat only.");
    logger->Info("RoadObjectsGTA4 loaded (built-in GTA SA litter variants).");

    g_enabled = cfg->GetBool("Enabled", true, "General");
    g_maxObjects = cfg->GetInt("MaxObjects", 50, "General");
    g_minSpawnDistance = cfg->GetFloat("MinSpawnDistance", 28.0f, "General");
    g_spawnRadius = cfg->GetFloat("SpawnRadius", 70.0f, "General");
    g_minObjectSpacing = cfg->GetFloat("MinObjectSpacing", 2.5f, "General");
    g_roadLateralRange = cfg->GetFloat("RoadLateralRange", 8.0f, "General");
    g_cleanupRadius = cfg->GetFloat("CleanupRadius", 90.0f, "General");
    g_spawnIntervalMs = cfg->GetInt("SpawnIntervalMs", 700, "General");

    logger->Info("Config: enabled=%d max=%d minDistance=%.1f maxDistance=%.1f spacing=%.1f cleanup=%.1f interval=%dms",
        g_enabled, g_maxObjects, g_minSpawnDistance, g_spawnRadius, g_minObjectSpacing,
        g_cleanupRadius, g_spawnIntervalMs);

    if (g_maxObjects < 1) g_maxObjects = 1;
    if (g_maxObjects > MAX_TRACKED_OBJECTS) g_maxObjects = MAX_TRACKED_OBJECTS;
    if (g_minSpawnDistance < 20.0f) g_minSpawnDistance = 20.0f;
    if (g_minSpawnDistance > 38.0f) g_minSpawnDistance = 38.0f;
    if (g_spawnRadius < g_minSpawnDistance + 8.0f) g_spawnRadius = g_minSpawnDistance + 8.0f;
    if (g_spawnRadius > 75.0f) g_spawnRadius = 75.0f;
    if (g_minObjectSpacing < 1.5f) g_minObjectSpacing = 1.5f;
    if (g_minObjectSpacing > 12.0f) g_minObjectSpacing = 12.0f;
    if (g_roadLateralRange < 4.0f) g_roadLateralRange = 4.0f;
    if (g_roadLateralRange > 12.0f) g_roadLateralRange = 12.0f;
    if (g_cleanupRadius < g_spawnRadius + 8.0f) g_cleanupRadius = g_spawnRadius + 8.0f;
    if (g_cleanupRadius > 100.0f) g_cleanupRadius = 100.0f;
    if (g_spawnIntervalMs < 400) g_spawnIntervalMs = 400;
    if (g_spawnIntervalMs > 10000) g_spawnIntervalMs = 10000;

    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    logger->Info("Using %d built-in litter variants; max=%d radius=%.1f-%.1f spacing=%.1f cleanup=%.1f interval=%dms",
        MODEL_VARIANT_COUNT, g_maxObjects, g_minSpawnDistance, g_spawnRadius,
        g_minObjectSpacing, g_cleanupRadius, g_spawnIntervalMs);
    logger->Info("PC Rub_1-Rub_27 DFF/TXD hashes are not Android model IDs; built-in models use random scale and heading.");
    Events::gameProcessEvent.after += []() { Tick(); };
}

ON_MOD_UNLOAD()
{
    logger->SetMessageCB(nullptr);
    if (g_logFile)
    {
        std::fflush(g_logFile);
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}
