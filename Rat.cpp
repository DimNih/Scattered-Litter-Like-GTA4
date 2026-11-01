#include "Rat.h"

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <aml-psdk/game_sa/Events.h>
#include <aml-psdk/game_sa/engine/World.h>
#include <aml-psdk/game_sa/engine/ModelInfo.h>
#include <aml-psdk/game_sa/engine/Streaming.h>
#include <aml-psdk/game_sa/base/Timer.h>
#include <aml-psdk/game_sa/entity/Entity.h>
#include <aml-psdk/game_sa/entity/PlayerPed.h>
#include <aml-psdk/game_sa/utils/OpcodeCaller.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Rat
{
    namespace
    {
        constexpr int MAX_RATS = 12;
        constexpr int STREAMING_GAME_REQUIRED = 2;
        constexpr int MAX_NEARBY_WORLD_ENTITIES = 256;
        constexpr float BIN_WANDER_RADIUS = 8.5f;
        constexpr float BIN_MIN_OFFSET = 0.85f;
        constexpr unsigned int ANCHOR_COOLDOWN_MS = 90000U;
        constexpr float ANCHOR_COOLDOWN_RADIUS = 18.0f;
        constexpr int MAX_CONFIGURED_RATS = 3;

        // GTA SA object IDs for bins, waste bins, skips and dumpsters.
        constexpr int TRASH_BIN_MODEL_IDS[] = {
            1227, 1235, 1300,
            1328, 1329, 1330, 1331, 1332, 1333, 1334, 1335, 1336, 1337, 1338, 1339,
            1343, 1344, 1345, 1347, 1358, 1359, 1362, 1365, 1371, 1372,
            1409, 1415, 1430, 1439, 1442, 1574, 2613, 2770, 2890, 2923, 3035
        };
        constexpr int TRASH_BIN_MODEL_COUNT =
            sizeof(TRASH_BIN_MODEL_IDS) / sizeof(TRASH_BIN_MODEL_IDS[0]);
        // Loose rubbish, black bags, street rubbish and procedural rubbish.
        // These are valid rat anchors too; the rat remains within the same
        // wider local wander area instead of spawning randomly on the road.
        constexpr int TRASH_DEBRIS_MODEL_IDS[] = {
            849, 850, 851, 852, 853, 854, 910, 926, 928,
            1236, 1264, 1265, 1327, 16444,
            18099, 18100, 18101,
            2670, 2671, 2672, 2673, 2674, 2675, 2676, 2677,
            2770, 2840, 2860, 2866, 3005, 3008, 3011, 3012,
            3092, 3098
        };
        constexpr int TRASH_DEBRIS_MODEL_COUNT =
            sizeof(TRASH_DEBRIS_MODEL_IDS) / sizeof(TRASH_DEBRIS_MODEL_IDS[0]);

        struct RatObject
        {
            int handle = -1;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f;
            float homeX = 0.0f, homeY = 0.0f, homeZ = 0.0f;
            bool active = false;
        };

        RatObject g_rats[MAX_RATS];
        struct RecentAnchor
        {
            float x = 0.0f;
            float y = 0.0f;
            unsigned int usedAt = 0;
            bool valid = false;
        };
        RecentAnchor g_recentAnchors[16];
        int g_recentAnchorCursor = 0;
        // Stock tall-grass model replaced with the rat mesh by XkiLoader.
        int g_modelId = DEFAULT_MODEL_ID;
        int g_maxRats = 5;
        int g_spawnIntervalMs = 6000;
        float g_moveSpeed = 3.0f;
        float g_ratScale = 0.10f;
        float g_headingOffset = -90.0f;
        bool g_soundEnabled = true;
        float g_soundRadius = 20.0f;
        unsigned int g_nextSqueakTime = 0;
        bool g_soundInitAttempted = false;
        SLObjectItf g_audioEngineObject = nullptr;
        SLEngineItf g_audioEngine = nullptr;
        SLObjectItf g_outputMixObject = nullptr;
        SLObjectItf g_audioPlayerObject = nullptr;
        SLPlayItf g_audioPlay = nullptr;
        SLAndroidSimpleBufferQueueItf g_audioQueue = nullptr;
        SLVolumeItf g_audioVolume = nullptr;
        std::vector<SLint16> g_squeakSamples;
        SLuint32 g_squeakSampleRate = 0;
        float g_spawnMinDistance = 12.0f;
        float g_spawnRadius = 48.0f;
        float g_cleanupRadius = 90.0f;
        unsigned int g_nextSpawnTime = 0;
        unsigned int g_lastTickTime = 0;
        unsigned int g_nextModelCheckTime = 0;
        unsigned int g_nextModelLogTime = 0;
        bool g_modelRequested = false;
        bool g_modelReady = false;
        bool g_enabled = false;
        bool g_spawnFailureLogged = false;
        bool g_binSearchFailureLogged = false;
        bool g_playerUnavailableLogged = false;
        bool g_interiorLogged = false;
        bool g_modelStateLogged = false;
        bool g_localLogEnabled = true;
        FILE* g_localLog = nullptr;
        char g_localLogPath[512] = {};

        void LocalLog(const char* format, ...)
        {
            if (!g_localLogEnabled || !g_localLog) return;
            std::fprintf(g_localLog, "[%u ms] ", CTimer::GetTimeMS());
            va_list args;
            va_start(args, format);
            std::vfprintf(g_localLog, format, args);
            va_end(args);
            std::fputc('\n', g_localLog);
            std::fflush(g_localLog);
        }

        float RandomFloat(float minValue, float maxValue)
        {
            const float t = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
            return minValue + (maxValue - minValue) * t;
        }

        float HeadingForDirection(float dx, float dy)
        {
            return std::atan2(-dx, dy) * 57.2957795f + g_headingOffset;
        }

        unsigned int ReadU32LE(const unsigned char* p)
        {
            return static_cast<unsigned int>(p[0])
                | (static_cast<unsigned int>(p[1]) << 8)
                | (static_cast<unsigned int>(p[2]) << 16)
                | (static_cast<unsigned int>(p[3]) << 24);
        }

        bool LoadSqueakWav()
        {
            const char* dataPath = (aml != nullptr) ? aml->GetAndroidDataPath() : nullptr;
            if (!dataPath || !dataPath[0])
            {
                LocalLog("Rat sound unavailable: AML data path is not available.");
                return false;
            }

            char path[640] = {};
            const size_t pathLength = std::strlen(dataPath);
            std::snprintf(path, sizeof(path), "%s%srat/rat_squeak.wav", dataPath,
                pathLength > 0 && (dataPath[pathLength - 1] == '/' || dataPath[pathLength - 1] == '\\') ? "" : "/");
            FILE* file = std::fopen(path, "rb");
            if (!file)
            {
                LocalLog("Rat sound file not found: %s", path);
                return false;
            }
            std::fseek(file, 0, SEEK_END);
            const long fileLength = std::ftell(file);
            std::rewind(file);
            if (fileLength < 44 || fileLength > 8 * 1024 * 1024)
            {
                std::fclose(file);
                LocalLog("Rat sound WAV has an invalid size (%ld bytes).", fileLength);
                return false;
            }
            std::vector<unsigned char> bytes(static_cast<size_t>(fileLength));
            const size_t bytesRead = std::fread(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
            if (bytesRead != bytes.size() || std::memcmp(bytes.data(), "RIFF", 4) != 0
                || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
            {
                LocalLog("Rat sound file is not a readable RIFF/WAVE file.");
                return false;
            }

            unsigned int audioFormat = 0, channels = 0, sampleRate = 0, bitsPerSample = 0;
            const unsigned char* sampleData = nullptr;
            size_t sampleDataLength = 0;
            for (size_t offset = 12; offset + 8 <= bytes.size();)
            {
                const unsigned int chunkLength = ReadU32LE(bytes.data() + offset + 4);
                const size_t chunkStart = offset + 8;
                if (chunkLength > bytes.size() - chunkStart) break;
                if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunkLength >= 16)
                {
                    audioFormat = bytes[chunkStart] | (static_cast<unsigned int>(bytes[chunkStart + 1]) << 8);
                    channels = bytes[chunkStart + 2] | (static_cast<unsigned int>(bytes[chunkStart + 3]) << 8);
                    sampleRate = ReadU32LE(bytes.data() + chunkStart + 4);
                    bitsPerSample = bytes[chunkStart + 14] | (static_cast<unsigned int>(bytes[chunkStart + 15]) << 8);
                }
                else if (std::memcmp(bytes.data() + offset, "data", 4) == 0)
                {
                    sampleData = bytes.data() + chunkStart;
                    sampleDataLength = chunkLength;
                }
                offset = chunkStart + chunkLength + (chunkLength & 1U);
            }
            if (audioFormat != 1 || channels != 1 || bitsPerSample != 16
                || sampleRate < 8000 || sampleRate > 48000 || !sampleData || sampleDataLength < 2)
            {
                LocalLog("Rat WAV must be mono PCM16 at 8-48 kHz (format=%u channels=%u bits=%u rate=%u).",
                    audioFormat, channels, bitsPerSample, sampleRate);
                return false;
            }
            g_squeakSamples.resize(sampleDataLength / 2);
            for (size_t i = 0; i < g_squeakSamples.size(); ++i)
            {
                const unsigned int sample = sampleData[i * 2]
                    | (static_cast<unsigned int>(sampleData[i * 2 + 1]) << 8);
                g_squeakSamples[i] = static_cast<SLint16>(static_cast<short>(sample));
            }
            g_squeakSampleRate = sampleRate * 1000U;
            LocalLog("Loaded rat sound: %s (%u Hz, %u samples).", path, sampleRate,
                static_cast<unsigned int>(g_squeakSamples.size()));
            return true;
        }

        bool InitializeRatAudio()
        {
            if (g_soundInitAttempted) return g_audioPlay != nullptr;
            g_soundInitAttempted = true;
            if (!LoadSqueakWav()) return false;

            if (slCreateEngine(&g_audioEngineObject, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS
                || (*g_audioEngineObject)->Realize(g_audioEngineObject, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS
                || (*g_audioEngineObject)->GetInterface(g_audioEngineObject, SL_IID_ENGINE, &g_audioEngine) != SL_RESULT_SUCCESS)
            {
                LocalLog("Could not initialize OpenSL ES engine for rat sound.");
                return false;
            }
            if ((*g_audioEngine)->CreateOutputMix(g_audioEngine, &g_outputMixObject, 0, nullptr, nullptr) != SL_RESULT_SUCCESS
                || (*g_outputMixObject)->Realize(g_outputMixObject, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
            {
                LocalLog("Could not initialize OpenSL ES output mix for rat sound.");
                return false;
            }

            SLDataLocator_AndroidSimpleBufferQueue queueLocator = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1};
            SLDataFormat_PCM pcmFormat = {SL_DATAFORMAT_PCM, 1, g_squeakSampleRate,
                SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
            SLDataSource source = {&queueLocator, &pcmFormat};
            SLDataLocator_OutputMix outputLocator = {SL_DATALOCATOR_OUTPUTMIX, g_outputMixObject};
            SLDataSink sink = {&outputLocator, nullptr};
            const SLInterfaceID interfaceIds[] = {SL_IID_PLAY, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
            const SLboolean interfaceRequired[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
            if ((*g_audioEngine)->CreateAudioPlayer(g_audioEngine, &g_audioPlayerObject, &source, &sink,
                    3, const_cast<SLInterfaceID*>(interfaceIds), const_cast<SLboolean*>(interfaceRequired)) != SL_RESULT_SUCCESS
                || (*g_audioPlayerObject)->Realize(g_audioPlayerObject, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS
                || (*g_audioPlayerObject)->GetInterface(g_audioPlayerObject, SL_IID_PLAY, &g_audioPlay) != SL_RESULT_SUCCESS
                || (*g_audioPlayerObject)->GetInterface(g_audioPlayerObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_audioQueue) != SL_RESULT_SUCCESS)
            {
                LocalLog("Could not create OpenSL ES rat sound player.");
                return false;
            }
            (*g_audioPlayerObject)->GetInterface(g_audioPlayerObject, SL_IID_VOLUME, &g_audioVolume);
            LocalLog("Rat squeak/chitter sound player is ready.");
            return true;
        }

        void UpdateRatSound(unsigned int now, const CVector& playerPosition)
        {
            if (!g_soundEnabled || static_cast<int>(now - g_nextSqueakTime) < 0) return;
            float nearestDistanceSq = g_soundRadius * g_soundRadius;
            int nearbyRats = 0;
            for (const RatObject& rat : g_rats)
            {
                if (!rat.active) continue;
                const float dx = rat.x - playerPosition.x;
                const float dy = rat.y - playerPosition.y;
                const float dz = rat.z - playerPosition.z;
                const float distanceSq = dx * dx + dy * dy + dz * dz;
                if (distanceSq <= g_soundRadius * g_soundRadius)
                {
                    ++nearbyRats;
                    if (distanceSq < nearestDistanceSq) nearestDistanceSq = distanceSq;
                }
            }
            if (nearbyRats == 0 || !InitializeRatAudio()) return;

            const float distance = std::sqrt(nearestDistanceSq);
            const float distanceGain = 0.25f + 0.75f * (1.0f - distance / g_soundRadius);
            const float volume = 0.22f * distanceGain * distanceGain;
            if (g_audioVolume)
            {
                const SLmillibel level = static_cast<SLmillibel>(2000.0f * std::log10(volume));
                (*g_audioVolume)->SetVolumeLevel(g_audioVolume, level);
            }
            const SLresult queued = (*g_audioQueue)->Enqueue(g_audioQueue, g_squeakSamples.data(),
                static_cast<SLuint32>(g_squeakSamples.size() * sizeof(SLint16)));
            if (queued == SL_RESULT_SUCCESS)
            {
                (*g_audioPlay)->SetPlayState(g_audioPlay, SL_PLAYSTATE_PLAYING);
                const unsigned int baseInterval = nearbyRats >= 5 ? 1050U : 2850U - static_cast<unsigned int>(nearbyRats - 1) * 420U;
                const unsigned int jitter = static_cast<unsigned int>(std::rand() % 550);
                g_nextSqueakTime = now + baseInterval + jitter;
                LocalLog("Rat squeak/chitter call played: nearby=%d distance=%.1f volume=%.2f next=%ums.",
                    nearbyRats, distance, volume, baseInterval + jitter);
            }
        }

        void ShutdownRatAudio()
        {
            if (g_audioPlayerObject) (*g_audioPlayerObject)->Destroy(g_audioPlayerObject);
            if (g_outputMixObject) (*g_outputMixObject)->Destroy(g_outputMixObject);
            if (g_audioEngineObject) (*g_audioEngineObject)->Destroy(g_audioEngineObject);
            g_audioPlayerObject = nullptr;
            g_outputMixObject = nullptr;
            g_audioEngineObject = nullptr;
            g_audioPlay = nullptr;
            g_audioQueue = nullptr;
            g_audioVolume = nullptr;
            g_audioEngine = nullptr;
            g_squeakSamples.clear();
        }

        int Count()
        {
            int count = 0;
            for (const RatObject& rat : g_rats)
                if (rat.active) ++count;
            return count;
        }

        bool WasAnchorUsedRecently(float x, float y, unsigned int now)
        {
            const float cooldownSq = ANCHOR_COOLDOWN_RADIUS * ANCHOR_COOLDOWN_RADIUS;
            for (RecentAnchor& anchor : g_recentAnchors)
            {
                if (!anchor.valid) continue;
                if (now - anchor.usedAt >= ANCHOR_COOLDOWN_MS)
                {
                    anchor.valid = false;
                    continue;
                }
                const float dx = anchor.x - x;
                const float dy = anchor.y - y;
                if (dx * dx + dy * dy < cooldownSq) return true;
            }
            return false;
        }

        void RememberAnchor(float x, float y, unsigned int now)
        {
            RecentAnchor& anchor = g_recentAnchors[g_recentAnchorCursor];
            anchor.x = x;
            anchor.y = y;
            anchor.usedAt = now;
            anchor.valid = true;
            g_recentAnchorCursor = (g_recentAnchorCursor + 1) %
                (sizeof(g_recentAnchors) / sizeof(g_recentAnchors[0]));
        }

        bool IsTrashAnchorModel(int modelId)
        {
            for (int i = 0; i < TRASH_BIN_MODEL_COUNT; ++i)
                if (TRASH_BIN_MODEL_IDS[i] == modelId) return true;
            for (int i = 0; i < TRASH_DEBRIS_MODEL_COUNT; ++i)
                if (TRASH_DEBRIS_MODEL_IDS[i] == modelId) return true;
            return false;
        }

        bool IsBeachSurface(const CVector& position)
        {
            CColPoint collisionPoint = {};
            CEntity* hitEntity = nullptr;
            if (!CWorld::ProcessVerticalLine(
                    CVector(position.x, position.y, position.z + 8.0f), position.z - 8.0f,
                    collisionPoint, hitEntity,
                    true, false, false, true, false, true, nullptr))
                return false;
            return collisionPoint.m_nSurfaceTypeA == SURFACE_SAND_BEACH ||
                collisionPoint.m_nSurfaceTypeA == SURFACE_CONCRETE_BEACH ||
                collisionPoint.m_nSurfaceTypeB == SURFACE_SAND_BEACH ||
                collisionPoint.m_nSurfaceTypeB == SURFACE_CONCRETE_BEACH;
        }

        bool IsWaterAt(const CVector& position, float groundZ)
        {
            float waterZ = 0.0f;
            if (!Command<Commands::GET_WATER_HEIGHT_AT_COORDS>(
                    position.x, position.y, groundZ + 2.0f, &waterZ))
                return false;
            return std::isfinite(waterZ) && groundZ <= waterZ + 0.75f;
        }

        bool FindBinWanderPoint(const CVector& binPosition, float radius,
            float& x, float& y, float& z)
        {
            for (int attempt = 0; attempt < 18; ++attempt)
            {
                const float angle = RandomFloat(0.0f, 6.28318531f);
                const float distance = RandomFloat(BIN_MIN_OFFSET, radius);
                x = binPosition.x + std::cos(angle) * distance;
                y = binPosition.y + std::sin(angle) * distance;
                if (!Command<Commands::GET_GROUND_Z_FOR_3D_COORD>(x, y, binPosition.z + 12.0f, &z))
                    continue;
                if (!std::isfinite(z) || std::fabs(z - binPosition.z) > 3.0f) continue;
                if (IsBeachSurface(CVector(x, y, z))) continue;
                if (IsWaterAt(CVector(x, y, z), z)) continue;
                return true;
            }
            return false;
        }

        bool FindNearbyTrashAnchor(const CVector& playerPosition,
            unsigned int now, float& binX, float& binY, float& binZ, int& binModelId)
        {
            CEntity* entities[MAX_NEARBY_WORLD_ENTITIES] = {};
            i16 entityCount = 0;
            CWorld::FindObjectsInRange(playerPosition, g_spawnRadius, true, &entityCount,
                MAX_NEARBY_WORLD_ENTITIES, entities,
                true, false, false, true, false);

            float nearestDistanceSq = g_spawnRadius * g_spawnRadius;
            bool found = false;
            for (int i = 0; i < entityCount; ++i)
            {
                CEntity* entity = entities[i];
                if (!entity || (!entity->IsObject() && !entity->IsBuilding())) continue;
                if (!IsTrashAnchorModel(static_cast<int>(entity->m_nModelIndex))) continue;

                const CVector position = entity->GetPosition();
                const float dx = position.x - playerPosition.x;
                const float dy = position.y - playerPosition.y;
                const float dz = position.z - playerPosition.z;
                const float distanceSq = dx * dx + dy * dy + dz * dz;
                if (distanceSq < g_spawnMinDistance * g_spawnMinDistance ||
                    distanceSq > g_spawnRadius * g_spawnRadius || distanceSq >= nearestDistanceSq)
                    continue;

                float groundZ = position.z;
                if (!Command<Commands::GET_GROUND_Z_FOR_3D_COORD>(
                        position.x, position.y, position.z + 20.0f, &groundZ))
                    continue;
                if (!std::isfinite(groundZ) || std::fabs(groundZ - position.z) > 8.0f)
                    continue;
                if (IsWaterAt(CVector(position.x, position.y, groundZ), groundZ)) continue;
                if (WasAnchorUsedRecently(position.x, position.y, now)) continue;

                bool occupied = false;
                for (const RatObject& rat : g_rats)
                {
                    if (!rat.active) continue;
                    const float homeDx = rat.homeX - position.x;
                    const float homeDy = rat.homeY - position.y;
                    if (homeDx * homeDx + homeDy * homeDy < 4.0f) {
                        occupied = true;
                        break;
                    }
                }
                if (occupied) continue;

                nearestDistanceSq = distanceSq;
                binX = position.x;
                binY = position.y;
                binZ = groundZ;
                binModelId = static_cast<int>(entity->m_nModelIndex);
                found = true;
            }
            return found;
        }

        bool Spawn(const CVector& playerPosition, unsigned int now)
        {
            RatObject* slot = nullptr;
            for (RatObject& rat : g_rats)
                if (!rat.active) { slot = &rat; break; }
            if (!slot) return false;

            float binX = 0.0f, binY = 0.0f, binZ = 0.0f;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            int binModelId = 0;
            if (!FindNearbyTrashAnchor(playerPosition, now, binX, binY, binZ, binModelId))
            {
                if (!g_binSearchFailureLogged)
                {
                    LocalLog("Spawn skipped: no loaded trash-bin or loose-trash object ID found within %.1f-%.1f units.",
                        g_spawnMinDistance, g_spawnRadius);
                    g_binSearchFailureLogged = true;
                }
                return false;
            }
            g_binSearchFailureLogged = false;
            slot->homeX = binX;
            slot->homeY = binY;
            slot->homeZ = binZ;
            if (!FindBinWanderPoint(CVector(binX, binY, binZ), BIN_WANDER_RADIUS, x, y, z))
                return false;
            if (!FindBinWanderPoint(CVector(binX, binY, binZ), BIN_WANDER_RADIUS,
                    slot->targetX, slot->targetY, slot->targetZ))
            {
                slot->targetX = x;
                slot->targetY = y;
                slot->targetZ = z;
            }

            int handle = -1;
            Command<Commands::CREATE_OBJECT>(g_modelId, x, y, z, &handle);
            if (handle < 0)
            {
                if (!g_spawnFailureLogged)
                {
                    logger->Error("Could not create rat model %d.", g_modelId);
                    LocalLog("CREATE_OBJECT failed for model ID %d.", g_modelId);
                    g_spawnFailureLogged = true;
                }
                return false;
            }
            Command<Commands::SET_OBJECT_COLLISION>(handle, false);
            const float dx = slot->targetX - x;
            const float dy = slot->targetY - y;
            const float heading = HeadingForDirection(dx, dy);
            Command<Commands::SET_OBJECT_HEADING>(handle, heading);
            Command<Commands::SET_OBJECT_SCALE>(handle, g_ratScale);

            slot->handle = handle;
            slot->x = x; slot->y = y; slot->z = z;
            slot->active = true;
            RememberAnchor(binX, binY, now);
            logger->Info("Spawned rat model %d beside trash-bin object %d at %.1f, %.1f, %.1f.",
                g_modelId, binModelId, x, y, z);
            LocalLog("Rat spawned beside bin: ratModel=%d binModel=%d handle=%d bin=(%.2f, %.2f, %.2f) position=(%.2f, %.2f, %.2f).",
                g_modelId, binModelId, handle, binX, binY, binZ, x, y, z);
            g_spawnFailureLogged = false;
            return true;
        }

        void Delete(RatObject& rat)
        {
            if (!rat.active || rat.handle < 0) return;
            int handle = rat.handle;
            Command<Commands::DELETE_OBJECT>(&handle);
            rat.handle = -1;
            rat.active = false;
        }

        void Tick()
        {
            if (!g_enabled) return;
            CPlayerPed* player = FindPlayerPed(0);
            if (!player)
            {
                if (!g_playerUnavailableLogged)
                {
                    LocalLog("Waiting: local player ped is not available yet.");
                    g_playerUnavailableLogged = true;
                }
                return;
            }
            g_playerUnavailableLogged = false;
            if (player->GetInterior() != 0)
            {
                if (!g_interiorLogged)
                {
                    LocalLog("Rat spawning paused in interior %d.", static_cast<int>(player->GetInterior()));
                    g_interiorLogged = true;
                }
                return;
            }
            g_interiorLogged = false;

            const unsigned int now = CTimer::GetTimeMS();
            float deltaSeconds = g_lastTickTime ? (now - g_lastTickTime) * 0.001f : 0.0f;
            g_lastTickTime = now;
            if (deltaSeconds > 0.1f) deltaSeconds = 0.1f;

            if (!g_modelRequested)
            {
                if (static_cast<int>(now - g_nextModelCheckTime) < 0) return;
                if (!g_modelStateLogged)
                {
                    CBaseModelInfo* const* modelInfos = CModelInfo::ms_modelInfoPtrs;
                    const CStreamingInfo* const streamingInfos = CStreaming::ms_aInfoForModel;
                    const CBaseModelInfo* modelInfo = modelInfos ? modelInfos[g_modelId] : nullptr;
                    const CStreamingInfo& streamingInfo = streamingInfos[g_modelId];
                    LocalLog("Before request: modelInfo=%s streamStatus=%u cdPos=%u cdSize=%u cdIndexed=%d.",
                        modelInfo ? "registered" : "null",
                        static_cast<unsigned int>(streamingInfo.m_status),
                        streamingInfo.m_cdPosn, streamingInfo.m_cdSize,
                        CStreaming::IsObjectInCdImage(g_modelId) ? 1 : 0);
                    g_modelStateLogged = true;
                }
                if (!CStreaming::IsObjectInCdImage(g_modelId))
                {
                    if (static_cast<int>(now - g_nextModelLogTime) >= 0)
                    {
                        logger->Info("Rat model %d is not indexed in a game IMG; requesting it anyway.", g_modelId);
                        LocalLog("Model ID %d is not indexed in a game IMG; requesting it anyway.", g_modelId);
                        g_nextModelLogTime = now + 10000U;
                    }
                }
                CStreaming::RequestModel(g_modelId, STREAMING_GAME_REQUIRED);
                g_modelRequested = true;
                g_nextModelLogTime = now + 10000U;
                LocalLog("Requested streaming for model ID %d (IMG-indexed=%d).", g_modelId,
                    CStreaming::IsObjectInCdImage(g_modelId) ? 1 : 0);
                return;
            }
            if (!g_modelReady)
            {
                if (!Command<Commands::HAS_MODEL_LOADED>(g_modelId))
                {
                    if (static_cast<int>(now - g_nextModelLogTime) >= 0)
                    {
                        CBaseModelInfo* const* modelInfos = CModelInfo::ms_modelInfoPtrs;
                        const CStreamingInfo* const streamingInfos = CStreaming::ms_aInfoForModel;
                        const CBaseModelInfo* modelInfo = modelInfos ? modelInfos[g_modelId] : nullptr;
                        const CStreamingInfo& streamingInfo = streamingInfos[g_modelId];
                        LocalLog("Model ID %d still waiting: modelInfo=%s streamStatus=%u cdPos=%u cdSize=%u.",
                            g_modelId, modelInfo ? "registered" : "null",
                            static_cast<unsigned int>(streamingInfo.m_status),
                            streamingInfo.m_cdPosn, streamingInfo.m_cdSize);
                        g_nextModelLogTime = now + 10000U;
                    }
                    return;
                }
                g_modelReady = true;
                g_nextSpawnTime = now + 2500U;
                logger->Info("Rat model %d is ready.", g_modelId);
                LocalLog("Model ID %d finished streaming; first spawn scheduled in 2500 ms.", g_modelId);
            }

            const CVector playerPosition = player->GetPosition();
            const float cleanupSq = g_cleanupRadius * g_cleanupRadius;
            for (RatObject& rat : g_rats)
            {
                if (!rat.active) continue;
                const float playerDx = rat.x - playerPosition.x;
                const float playerDy = rat.y - playerPosition.y;
                const float playerDz = rat.z - playerPosition.z;
                if (playerDx * playerDx + playerDy * playerDy + playerDz * playerDz > cleanupSq)
                {
                    Delete(rat);
                    continue;
                }

                const float homeDx = rat.homeX - rat.x;
                const float homeDy = rat.homeY - rat.y;
                if (homeDx * homeDx + homeDy * homeDy > BIN_WANDER_RADIUS * BIN_WANDER_RADIUS)
                {
                    if (!FindBinWanderPoint(CVector(rat.homeX, rat.homeY, rat.homeZ),
                            BIN_WANDER_RADIUS, rat.targetX, rat.targetY, rat.targetZ))
                        continue;
                }

                float dx = rat.targetX - rat.x;
                float dy = rat.targetY - rat.y;
                const float distance = std::sqrt(dx * dx + dy * dy);
                if (distance < 1.0f)
                {
                    if (!FindBinWanderPoint(CVector(rat.homeX, rat.homeY, rat.homeZ),
                            BIN_WANDER_RADIUS,
                            rat.targetX, rat.targetY, rat.targetZ))
                        continue;
                    dx = rat.targetX - rat.x;
                    dy = rat.targetY - rat.y;
                }
                const float newDistance = std::sqrt(dx * dx + dy * dy);
                if (newDistance <= 0.01f) continue;
                const float step = g_moveSpeed * deltaSeconds;
                const float move = (step < newDistance) ? step : newDistance;
                rat.x += dx / newDistance * move;
                rat.y += dy / newDistance * move;
                rat.z += (rat.targetZ - rat.z) * 0.08f;
                Command<Commands::SET_OBJECT_COORDINATES>(rat.handle, rat.x, rat.y, rat.z);
                Command<Commands::SET_OBJECT_HEADING>(rat.handle, HeadingForDirection(dx, dy));
            }

            UpdateRatSound(now, playerPosition);

            if (Count() >= g_maxRats || static_cast<int>(now - g_nextSpawnTime) < 0) return;
            if (Spawn(playerPosition, now)) g_nextSpawnTime = now + static_cast<unsigned int>(g_spawnIntervalMs);
            else g_nextSpawnTime = now + 1500U;
        }
    }

    void Initialize(bool enabled, int modelId, int maxRats, int spawnIntervalMs,
        float moveSpeed, float spawnMinDistance, float spawnRadius,
        float cleanupRadius, float scale, float headingOffset,
        bool soundEnabled, float soundRadius, bool localLogEnabled)
    {
        if (g_localLog)
        {
            std::fclose(g_localLog);
            g_localLog = nullptr;
        }
        g_localLogEnabled = localLogEnabled;
        g_localLogPath[0] = '\0';
        if (g_localLogEnabled)
        {
            const char* dataPath = (aml != nullptr) ? aml->GetAndroidDataPath() : nullptr;
            if (dataPath && dataPath[0])
            {
                std::snprintf(g_localLogPath, sizeof(g_localLogPath), "%s/RubbishRat.log", dataPath);
                // Puddle diagnostics append to this same file. Use append
                // mode so separate FILE handles never overwrite their entries.
                g_localLog = std::fopen(g_localLogPath, "a");
            }
            if (g_localLog)
                LocalLog("Rat diagnostics started. Log file: %s", g_localLogPath);
            else
                logger->Error("Could not open rat local log file at '%s'.", g_localLogPath[0] ? g_localLogPath : "<AML data path unavailable>");
        }

        g_modelId = modelId;
        g_maxRats = maxRats < 1 ? 1 : (maxRats > MAX_CONFIGURED_RATS ? MAX_CONFIGURED_RATS : maxRats);
        g_spawnIntervalMs = spawnIntervalMs < 15000 ? 15000 : spawnIntervalMs;
        g_moveSpeed = moveSpeed < 0.2f ? 0.2f : (moveSpeed > 4.0f ? 4.0f : moveSpeed);
        // Keep the rat small even if an existing INI still requests 0.15.
        g_ratScale = scale < 0.03f ? 0.03f : (scale > 0.10f ? 0.10f : scale);
        g_headingOffset = headingOffset;
        g_soundEnabled = soundEnabled;
        g_soundRadius = soundRadius < 3.0f ? 3.0f : (soundRadius > 100.0f ? 100.0f : soundRadius);
        g_spawnMinDistance = spawnMinDistance < 4.0f ? 4.0f : spawnMinDistance;
        g_spawnRadius = spawnRadius < g_spawnMinDistance + 4.0f
            ? g_spawnMinDistance + 4.0f : spawnRadius;
        g_cleanupRadius = cleanupRadius < g_spawnRadius + 8.0f
            ? g_spawnRadius + 8.0f : cleanupRadius;
        g_enabled = enabled && g_modelId > 0;
        LocalLog("Config: enabled=%d modelId=%d maxRats=%d interval=%dms speed=%.2f scale=%.2f spawnDistance=%.1f-%.1f cleanup=%.1f",
            g_enabled ? 1 : 0, g_modelId, g_maxRats, g_spawnIntervalMs,
            g_moveSpeed, g_ratScale, g_spawnMinDistance, g_spawnRadius, g_cleanupRadius);
        if (!g_enabled)
        {
            LocalLog("Rat spawning disabled by config or invalid model ID.");
            return;
        }

        logger->Info("Rat enabled: model=%d max=%d spawn=%.1f-%.1f cleanup=%.1f interval=%dms speed=%.2f scale=%.2f headingOffset=%.1f sound=%d radius=%.1f",
            g_modelId, g_maxRats, g_spawnMinDistance, g_spawnRadius,
            g_cleanupRadius, g_spawnIntervalMs, g_moveSpeed, g_ratScale, g_headingOffset,
            g_soundEnabled ? 1 : 0, g_soundRadius);
        logger->Info("Rat local diagnostics: %s", g_localLog ? g_localLogPath : "disabled/unavailable");
        LocalLog("Rat module initialized; waiting for game process.");
        if (g_enabled)
            Events::gameProcessEvent.after += []() { Tick(); };
    }

    void Shutdown()
    {
        LocalLog("Rat module shutting down; active rats=%d.", Count());
        for (RatObject& rat : g_rats) Delete(rat);
        ShutdownRatAudio();
        g_enabled = false;
        if (g_localLog)
        {
            std::fclose(g_localLog);
            g_localLog = nullptr;
        }
    }
}
