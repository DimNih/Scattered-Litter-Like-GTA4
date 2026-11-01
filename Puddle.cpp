#include "Puddle.h"
#include "PuddleReflection.h"
#include "mod/local_diagnostics.h"

#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>

#include <aml-psdk/game_sa/Events.h>
#include <aml-psdk/game_sa/base/Timer.h>
#include <aml-psdk/game_sa/entity/PlayerPed.h>
#include <aml-psdk/game_sa/entity/Camera.h>
#include <aml-psdk/game_sa/engine/RsGlobal.h>
#include <aml-psdk/game_sa/engine/Shadows.h>
#include <aml-psdk/game_sa/engine/Sprite.h>
#include <aml-psdk/game_sa/other/Clock.h>
#include <aml-psdk/game_sa/other/Weather.h>
#include <aml-psdk/game_sa/utils/OpcodeCaller.h>
#include <aml-psdk/renderware/RwImage.h>
#include <aml-psdk/renderware/RwRaster.h>
#include <aml-psdk/renderware/RwTexture.h>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Puddle
{
    namespace
    {
        constexpr int MAX_PUDDLES = 4;
        // Spawn one puddle at a time to avoid visible clusters popping in.
        constexpr int PUDDLES_PER_SPAWN_BATCH = 1;
        constexpr unsigned int WORLD_WARMUP_MS = 9000U;
        constexpr unsigned int SPAWN_FADE_IN_MS = 2000U;
        constexpr unsigned int SPAWN_FADE_OUT_MS = 30000U;
        // Natural-sized wet patches, visible without looking like large decals.
        constexpr float DECAL_RADIUS_MIN = 1.2f;
        constexpr float DECAL_RADIUS_MAX = 2.9f;
        struct PuddleObject
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            float radius = 2.0f;
            float lengthScale = 1.0f;
            float widthScale = 1.0f;
            float heading = 0.0f;
            unsigned int bornAt = 0;
            unsigned int lifeMs = 60000U;
            bool active = false;
            // Water surface grid and per-puddle procedural shape seed.
            float ax = 0.0f, ay = 0.0f, sx = 0.0f, sy = 0.0f;
            float gridZ[PuddleReflection::GRID * PuddleReflection::GRID] = {};
            float gridValid[PuddleReflection::GRID * PuddleReflection::GRID] = {};
            float seed[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        };

        PuddleObject g_puddles[MAX_PUDDLES];
        int g_maxPuddles = 3;
        int g_spawnIntervalMs = 3500;
        float g_spawnMinDistance = 10.0f;
        float g_spawnRadius = 55.0f;
        float g_cleanupRadius = 85.0f;
        unsigned int g_lifeMinMs = 35000U;
        unsigned int g_lifeMaxMs = 70000U;
        unsigned int g_nextSpawnTime = 0;
        unsigned int g_lastTickTime = 0;
        bool g_enabled = false;
        bool g_spawnSearchFailureLogged = false;
        bool g_warmupLogged = false;
        bool g_textureLoadAttempted = false;
        bool g_shadowSubmitLogged = false;
        RwRaster* g_puddleRaster = nullptr;
        RwTexture* g_puddleTexture = nullptr;
        std::vector<RwUInt8> g_puddleMask;
        int g_puddleMaskWidth = 0;
        int g_puddleMaskHeight = 0;
        bool g_reflectionEnabled = true;
        float g_reflectionWall = 14.0f;
    #if 0 // This build's direct shadow callback has EGLcontext=0x0; no safe GLES target is available.
        bool g_shadowHookLogged = false;
        GLuint g_reflectionProgram = 0;
        GLuint g_sceneCaptureTexture = 0;
        GLuint g_reflectionMaskTexture = 0;
        GLint g_sceneUniform = -1;
        GLint g_maskUniform = -1;
        GLint g_mirrorLineUniform = -1;
        GLint g_timeUniform = -1;
        GLint g_strengthUniform = -1;
        GLint g_sceneCaptureWidth = 0;
        GLint g_sceneCaptureHeight = 0;
        bool g_reflectionInitFailedLogged = false;
        bool g_reflectionRenderLogged = false;
        bool g_missingGlContextLogged = false;
    #endif

        float RandomFloat(float minValue, float maxValue)
        {
            const float t = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
            return minValue + (maxValue - minValue) * t;
        }

        unsigned int RandomUInt(unsigned int minValue, unsigned int maxValue)
        {
            if (maxValue <= minValue) return minValue;
            const unsigned int span = maxValue - minValue;
            return minValue + static_cast<unsigned int>(std::rand()) % (span + 1U);
        }

        int Count()
        {
            int count = 0;
            for (const PuddleObject& puddle : g_puddles)
                if (puddle.active) ++count;
            return count;
        }

        void Delete(PuddleObject& puddle);

        void BuildOrganicPuddleMask(RwImage* image)
        {
            if (!image || !image->cpPixels || image->depth < 32 ||
                image->stride < image->width * 4)
                return;

            for (int y = 0; y < image->height; ++y)
            {
                RwUInt8* row = image->cpPixels + y * image->stride;
                const float ny = (static_cast<float>(y) + 0.5f) * 2.0f /
                    static_cast<float>(image->height) - 1.0f;
                for (int x = 0; x < image->width; ++x)
                {
                    const int pixel = x * 4;
                    const float nx = (static_cast<float>(x) + 0.5f) * 2.0f /
                        static_cast<float>(image->width) - 1.0f;
                    const float angle = std::atan2(ny, nx);
                    float edge = 0.0f;

                    // Irregular central pool plus two joined shallow lobes.
                    // Max-compositing their signed edges makes a natural,
                    // asymmetric footprint rather than a stretched oval.
                    const float coreX = nx + 0.08f;
                    const float coreY = ny - 0.02f;
                    const float coreAngle = std::atan2(coreY / 0.69f, coreX / 0.94f);
                    const float coreRadius = std::sqrt(
                        (coreX / 0.94f) * (coreX / 0.94f) +
                        (coreY / 0.69f) * (coreY / 0.69f));
                    edge = std::max(edge, 1.0f - coreRadius +
                        0.055f * std::sin(coreAngle * 3.0f + 0.7f) +
                        0.032f * std::sin(coreAngle * 7.0f - 0.4f) +
                        0.018f * std::sin(angle * 13.0f + 1.2f));

                    const float lobeOne = std::sqrt(
                        ((nx - 0.49f) / 0.43f) * ((nx - 0.49f) / 0.43f) +
                        ((ny + 0.03f) / 0.37f) * ((ny + 0.03f) / 0.37f));
                    edge = std::max(edge, (1.0f - lobeOne) * 0.72f);
                    const float lobeTwo = std::sqrt(
                        ((nx + 0.48f) / 0.36f) * ((nx + 0.48f) / 0.36f) +
                        ((ny - 0.31f) / 0.28f) * ((ny - 0.31f) / 0.28f));
                    edge = std::max(edge, (1.0f - lobeTwo) * 0.64f);

                    const float coverage = std::max(0.0f, std::min(1.0f, edge / 0.12f));
                    const float softEdge = coverage * coverage * (3.0f - 2.0f * coverage);
                    const float mottling = 0.86f + 0.14f *
                        (0.5f + 0.5f * std::sin(nx * 17.0f + ny * 11.0f) *
                            std::sin(ny * 23.0f - nx * 9.0f));
                    const RwUInt8 alpha = static_cast<RwUInt8>(softEdge * mottling * 142.0f);
                    row[pixel] = alpha ? 38 : 0;
                    row[pixel + 1] = alpha ? 43 : 0;
                    row[pixel + 2] = alpha ? 41 : 0;
                    row[pixel + 3] = alpha;
                }
            }
        }

        bool HasUsablePuddleAlpha(const RwImage* image)
        {
            if (!image || !image->cpPixels || image->depth < 32 || image->stride < image->width * 4)
                return false;
            std::size_t transparentPixels = 0;
            std::size_t visiblePixels = 0;
            for (int y = 0; y < image->height; ++y)
            {
                const RwUInt8* row = image->cpPixels + y * image->stride;
                for (int x = 0; x < image->width; ++x)
                {
                    if (row[x * 4 + 3] == 0) ++transparentPixels;
                    else ++visiblePixels;
                }
            }
            const std::size_t pixelCount = static_cast<std::size_t>(image->width) * image->height;
            // Reject opaque images (often an outdated installed copy), which
            // otherwise become full rectangular shadow decals in-game.
            return visiblePixels > 0 && transparentPixels > pixelCount / 100U;
        }

        unsigned int BoostPuddleAlpha(RwImage* image)
        {
            if (!image || !image->cpPixels || image->depth < 32 || image->stride < image->width * 4)
                return 0;
            unsigned int maxAlpha = 0;
            for (int y = 0; y < image->height; ++y)
            {
                RwUInt8* row = image->cpPixels + y * image->stride;
                for (int x = 0; x < image->width; ++x)
                {
                    RwUInt8& alpha = row[x * 4 + 3];
                    if (alpha == 0) continue;
                    const unsigned int boosted = std::min(160U,
                        (static_cast<unsigned int>(alpha) * 145U + 50U) / 100U);
                    alpha = static_cast<RwUInt8>(boosted);
                    if (boosted > maxAlpha) maxAlpha = boosted;
                }
            }
            return maxAlpha;
        }

        void TintBecekWetGrey(RwImage* image)
        {
            if (!image || !image->cpPixels || image->depth < 32 || image->stride < image->width * 4)
                return;
            for (int y = 0; y < image->height; ++y)
            {
                RwUInt8* row = image->cpPixels + y * image->stride;
                for (int x = 0; x < image->width; ++x)
                {
                    const int pixel = x * 4;
                    if (row[pixel + 3] == 0)
                    {
                        // Keep fully transparent texels black too. The game's
                        // shadow paths can expose RGB values from transparent
                        // texels as a rectangular halo on some devices.
                        row[pixel] = 0;
                        row[pixel + 1] = 0;
                        row[pixel + 2] = 0;
                        continue;
                    }
                    const float luminance = (row[pixel] * 0.30f +
                        row[pixel + 1] * 0.59f + row[pixel + 2] * 0.11f) / 255.0f;
                    // Keep the puddle dark like wet asphalt rather than a
                    // pale painted decal or bright additive outline.
                    row[pixel] = static_cast<RwUInt8>(58.0f + luminance * 17.0f);
                    row[pixel + 1] = static_cast<RwUInt8>(62.0f + luminance * 17.0f);
                    row[pixel + 2] = static_cast<RwUInt8>(60.0f + luminance * 16.0f);
                }
            }
        }

    #if 0 // This texture became the white outline decals visible in the user's screenshot.
        RwImage* CreateRippleImage(const RwImage* source)
        {
            if (!source || !source->cpPixels || source->depth < 32 ||
                source->stride < source->width * 4)
                return nullptr;

            RwImage* image = RwImageCreate(source->width, source->height, 32);
            if (!image || !RwImageAllocatePixels(image) || image->stride < image->width * 4)
            {
                if (image) RwImageDestroy(image);
                return nullptr;
            }

            for (int y = 0; y < source->height; ++y)
            {
                const RwUInt8* sourceRow = source->cpPixels + y * source->stride;
                RwUInt8* row = image->cpPixels + y * image->stride;
                const float ny = (static_cast<float>(y) + 0.5f) * 2.0f /
                    static_cast<float>(source->height) - 1.0f;
                for (int x = 0; x < source->width; ++x)
                {
                    const int pixel = x * 4;
                    row[pixel] = 0;
                    row[pixel + 1] = 0;
                    row[pixel + 2] = 0;
                    row[pixel + 3] = 0;
                    if (sourceRow[pixel + 3] == 0) continue;

                    const float nx = (static_cast<float>(x) + 0.5f) * 2.0f /
                        static_cast<float>(source->width) - 1.0f;
                    const float radius = std::sqrt((nx + 0.16f) * (nx + 0.16f) +
                        (ny - 0.08f) * (ny - 0.08f));
                    const float angle = std::atan2(ny - 0.08f, nx + 0.16f);
                    const float warpedRadius = radius + 0.06f * std::sin(angle * 3.0f) +
                        0.035f * std::sin(angle * 7.0f + 0.8f);
                    const float rings = std::sin(warpedRadius * 20.0f);
                    const float broken = 0.5f + 0.5f * std::sin(angle * 4.0f + radius * 11.0f);
                    const float highlight = std::max(0.0f, rings - 0.86f) * broken;
                    const float alpha = std::min(22.0f,
                        highlight * static_cast<float>(sourceRow[pixel + 3]) * 0.22f);
                    if (alpha < 1.0f) continue;
                    row[pixel] = 124;
                    row[pixel + 1] = 131;
                    row[pixel + 2] = 126;
                    row[pixel + 3] = static_cast<RwUInt8>(alpha);
                }
            }
            return image;
        }
    #endif

        bool CreatePuddleTexture(RwImage* image, const char* name,
            RwRaster*& raster, RwTexture*& texture)
        {
            RwInt32 width = image->width;
            RwInt32 height = image->height;
            RwInt32 depth = image->depth;
            RwInt32 format = 0;
            if (!RwImageFindRasterFormat(image, rwRASTERTYPETEXTURE,
                    &width, &height, &depth, &format))
                return false;

            depth = 32;
            format = (format & ~static_cast<RwInt32>(rwRASTERFORMATPIXELFORMATMASK)) |
                static_cast<RwInt32>(rwRASTERFORMAT8888);
            raster = RwRasterCreate(width, height, depth, format);
            if (!raster || !RwRasterSetFromImage(raster, image))
            {
                if (raster) RwRasterDestroy(raster);
                raster = nullptr;
                return false;
            }

            texture = RwTextureCreate(raster);
            if (!texture)
            {
                RwRasterDestroy(raster);
                raster = nullptr;
                return false;
            }
            RwTextureSetName(texture, name);
            return true;
        }

        RwImage* CreateBecekFallbackImage()
        {
            constexpr int size = 512;
            RwImage* image = RwImageCreate(size, size, 32);
            if (!image || !RwImageAllocatePixels(image))
            {
                if (image) RwImageDestroy(image);
                return nullptr;
            }

            for (int y = 0; y < size; ++y)
            {
                RwUInt8* row = image->cpPixels + y * image->stride;
                for (int x = 0; x < size; ++x)
                {
                    const float nx = (x - 63.5f) / 58.0f;
                    const float ny = (y - 63.5f) / 49.0f;
                    const float radius = std::sqrt(nx * nx + ny * ny);
                    const float angle = std::atan2(ny, nx);
                    const float unevenEdge = 1.0f + 0.09f * std::sin(angle * 3.0f + 0.4f) +
                        0.05f * std::sin(angle * 7.0f - 1.1f) +
                        0.03f * std::sin(angle * 11.0f + 0.8f);
                    const float edge = unevenEdge - radius;
                    const float coverage = std::max(0.0f, std::min(1.0f, edge / 0.16f));
                    const float smooth = coverage * coverage * (3.0f - 2.0f * coverage);
                    const int pixel = x * 4;
                    row[pixel + 0] = 116;
                    row[pixel + 1] = 124;
                    row[pixel + 2] = 132;
                    row[pixel + 3] = static_cast<RwUInt8>(smooth * 115.0f);
                }
            }
            return image;
        }

        unsigned int ReadBigEndianU32(const unsigned char* data)
        {
            return (static_cast<unsigned int>(data[0]) << 24) |
                (static_cast<unsigned int>(data[1]) << 16) |
                (static_cast<unsigned int>(data[2]) << 8) |
                static_cast<unsigned int>(data[3]);
        }

        unsigned char PaethPredictor(unsigned char left, unsigned char up, unsigned char upperLeft)
        {
            const int p = static_cast<int>(left) + up - upperLeft;
            const int pa = std::abs(p - left);
            const int pb = std::abs(p - up);
            const int pc = std::abs(p - upperLeft);
            return pa <= pb && pa <= pc ? left : (pb <= pc ? up : upperLeft);
        }

        RwImage* LoadBecekPng(const char* path)
        {
            FILE* file = std::fopen(path, "rb");
            if (!file) return nullptr;
            std::fseek(file, 0, SEEK_END);
            const long fileSize = std::ftell(file);
            std::rewind(file);
            if (fileSize < 33 || fileSize > 4 * 1024 * 1024)
            {
                std::fclose(file);
                return nullptr;
            }

            std::vector<unsigned char> png(static_cast<std::size_t>(fileSize));
            const bool readOk = std::fread(png.data(), 1, png.size(), file) == png.size();
            std::fclose(file);
            static const unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
            if (!readOk || std::memcmp(png.data(), signature, sizeof(signature)) != 0)
                return nullptr;

            unsigned int width = 0, height = 0;
            unsigned char bitDepth = 0, colorType = 0, interlace = 0;
            std::vector<unsigned char> compressed;
            for (std::size_t offset = 8; offset + 12 <= png.size();)
            {
                const unsigned int chunkSize = ReadBigEndianU32(png.data() + offset);
                const std::size_t chunkData = offset + 8;
                if (chunkSize > png.size() - chunkData - 4) return nullptr;
                const unsigned char* type = png.data() + offset + 4;
                const unsigned char* data = png.data() + chunkData;
                if (std::memcmp(type, "IHDR", 4) == 0 && chunkSize == 13)
                {
                    width = ReadBigEndianU32(data);
                    height = ReadBigEndianU32(data + 4);
                    bitDepth = data[8];
                    colorType = data[9];
                    interlace = data[12];
                }
                else if (std::memcmp(type, "IDAT", 4) == 0)
                {
                    compressed.insert(compressed.end(), data, data + chunkSize);
                }
                else if (std::memcmp(type, "IEND", 4) == 0)
                {
                    break;
                }
                offset = chunkData + chunkSize + 4;
            }

            // This asset is a small, non-interlaced 8-bit RGBA PNG. Reject
            // other layouts rather than guessing and risking malformed data.
            if (width == 0 || height == 0 || width > 1024 || height > 1024 ||
                bitDepth != 8 || colorType != 6 || interlace != 0 || compressed.empty())
                return nullptr;

            const std::size_t stride = static_cast<std::size_t>(width) * 4;
            const std::size_t filteredSize = (stride + 1) * height;
            if (filteredSize > 8 * 1024 * 1024) return nullptr;
            std::vector<unsigned char> filtered(filteredSize);
            uLongf decodedSize = static_cast<uLongf>(filtered.size());
            if (uncompress(filtered.data(), &decodedSize, compressed.data(),
                    static_cast<uLong>(compressed.size())) != Z_OK || decodedSize != filtered.size())
                return nullptr;

            std::vector<unsigned char> pixels(stride * height);
            for (unsigned int y = 0; y < height; ++y)
            {
                const unsigned char filter = filtered[static_cast<std::size_t>(y) * (stride + 1)];
                const unsigned char* source = filtered.data() + static_cast<std::size_t>(y) * (stride + 1) + 1;
                unsigned char* row = pixels.data() + static_cast<std::size_t>(y) * stride;
                const unsigned char* previous = y ? row - stride : nullptr;
                if (filter > 4) return nullptr;
                for (std::size_t x = 0; x < stride; ++x)
                {
                    const unsigned char left = x >= 4 ? row[x - 4] : 0;
                    const unsigned char up = previous ? previous[x] : 0;
                    const unsigned char upperLeft = previous && x >= 4 ? previous[x - 4] : 0;
                    unsigned char predictor = 0;
                    if (filter == 1) predictor = left;
                    else if (filter == 2) predictor = up;
                    else if (filter == 3) predictor = static_cast<unsigned char>((static_cast<unsigned int>(left) + up) / 2);
                    else if (filter == 4) predictor = PaethPredictor(left, up, upperLeft);
                    row[x] = static_cast<unsigned char>(source[x] + predictor);
                }
            }

            RwImage* image = RwImageCreate(static_cast<RwInt32>(width),
                static_cast<RwInt32>(height), 32);
            if (!image || !RwImageAllocatePixels(image) || image->stride < static_cast<RwInt32>(stride))
            {
                if (image) RwImageDestroy(image);
                return nullptr;
            }
            for (unsigned int y = 0; y < height; ++y)
                std::memcpy(image->cpPixels + static_cast<std::size_t>(y) * image->stride,
                    pixels.data() + static_cast<std::size_t>(y) * stride, stride);
            return image;
        }

        bool EnsurePuddleTexture()
        {
            if (g_puddleTexture) return true;
            if (g_textureLoadAttempted) return false;
            g_textureLoadAttempted = true;

            // Generate the complete mask in code. An external oval PNG was
            // the source of the repeated pale ellipse look, so puddles no
            // longer depend on a copied texture or shader asset.
            RwImage* image = CreateBecekFallbackImage();
            if (!image)
            {
                LocalDiagnostics::Write("Could not create generated organic puddle canvas.");
                return false;
            }
            BuildOrganicPuddleMask(image);
            const unsigned int maxAlpha = BoostPuddleAlpha(image);
            g_puddleMaskWidth = image->width;
            g_puddleMaskHeight = image->height;
            g_puddleMask.resize(static_cast<std::size_t>(g_puddleMaskWidth) *
                static_cast<std::size_t>(g_puddleMaskHeight));
            for (int y = 0; y < g_puddleMaskHeight; ++y)
            {
                const RwUInt8* row = image->cpPixels + y * image->stride;
                for (int x = 0; x < g_puddleMaskWidth; ++x)
                    g_puddleMask[static_cast<std::size_t>(y) * g_puddleMaskWidth + x] =
                        row[x * 4 + 3];
            }
            TintBecekWetGrey(image);
            // The render-thread hook recognizes this texture when RenderWare
            // uploads it. The alpha mask was copied before stamping the marker.
            PuddleReflection::StampMagic(image->cpPixels);
            PuddleReflection::SetMask(g_puddleMask.data(), g_puddleMaskWidth, g_puddleMaskHeight);
            const RwInt32 width = image->width;
            const RwInt32 height = image->height;
            if (!CreatePuddleTexture(image, "becek", g_puddleRaster, g_puddleTexture))
            {
                RwImageDestroy(image);
                LocalDiagnostics::Write("Could not create RenderWare texture for generated puddle mask.");
                return false;
            }
            RwImageDestroy(image);

            LocalDiagnostics::Write("Generated dark organic puddle texture ready as RGBA8888 (%dx%d) maxAlpha=%u; ripple decals disabled.",
                width, height, maxAlpha);
            return true;
        }

    #if 0 // Retained as a reference only; every tested hook, including shadow drawing, had no EGL context.
        GLuint CompileReflectionShader(GLenum type, const char* source)
        {
            const GLuint shader = glCreateShader(type);
            if (!shader)
            {
                const GLenum error = glGetError();
                LocalDiagnostics::Write("Puddle GLES2 glCreateShader failed: type=0x%x GLerror=0x%x version=%s.",
                    static_cast<unsigned int>(type), static_cast<unsigned int>(error),
                    reinterpret_cast<const char*>(glGetString(GL_VERSION)));
                return 0;
            }
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_TRUE) return shader;

            GLint logLength = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
            std::vector<char> log(static_cast<std::size_t>(std::max(2, logLength)), '\0');
            GLsizei length = 0;
            glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), &length, log.data());
            const char* stage = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
            LocalDiagnostics::Write("Puddle %s shader compile failed (GLerror=0x%x, GL=%s): %s",
                stage, static_cast<unsigned int>(glGetError()),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)), log.data());
            logger->Info("Puddle %s shader compile failed: %s", stage, log.data());
            glDeleteShader(shader);
            return 0;
        }

        bool EnsureLiveReflectionResources()
        {
            if (g_reflectionProgram && g_sceneCaptureTexture && g_reflectionMaskTexture)
                return true;
            if (g_reflectionInitFailedLogged) return false;
            if (g_puddleMask.empty() || g_puddleMaskWidth <= 0 || g_puddleMaskHeight <= 0)
                return false;

            static const char vertexSource[] =
                "precision highp float;"
                "attribute vec4 aPosition;"
                "attribute vec2 aScreenUv;"
                "attribute vec2 aMaskUv;"
                "varying vec2 vScreenUv;"
                "varying vec2 vMaskUv;"
                "void main(){gl_Position=aPosition;vScreenUv=aScreenUv;vMaskUv=aMaskUv;}";
            static const char fragmentSource[] =
                "precision mediump float;"
                "uniform sampler2D uScene;"
                "uniform sampler2D uMask;"
                "uniform float uMirrorLine;"
                "uniform float uTime;"
                "uniform float uStrength;"
                "varying vec2 vScreenUv;"
                "varying vec2 vMaskUv;"
                "void main(){"
                "float mask=texture2D(uMask,vMaskUv).a;"
                "float shape=smoothstep(0.025,0.43,mask);"
                "float wave=sin(vMaskUv.x*20.0+vMaskUv.y*8.0+uTime*0.9);"
                "vec2 uv=vec2(vScreenUv.x+wave*0.0015,"
                "2.0*uMirrorLine-vScreenUv.y+wave*0.0025);"
                "uv=clamp(uv,vec2(0.002),vec2(0.998));"
                "vec3 reflected=texture2D(uScene,uv).rgb;"
                "float glint=pow(max(0.0,sin(vMaskUv.x*15.0-vMaskUv.y*9.0+uTime)),12.0)*0.07;"
                "vec3 water=reflected*vec3(0.67,0.74,0.78)+vec3(glint);"
                "gl_FragColor=vec4(water,shape*uStrength);"
                "}";

            GLint previousProgram = 0;
            GLint previousActiveTexture = 0;
            GLint previousTexture0 = 0;
            GLint previousTexture1 = 0;
            GLint previousUnpackAlignment = 4;
            glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
            glActiveTexture(GL_TEXTURE1);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture1);

            if (!g_reflectionProgram)
            {
                const GLuint vertex = CompileReflectionShader(GL_VERTEX_SHADER, vertexSource);
                const GLuint fragment = CompileReflectionShader(GL_FRAGMENT_SHADER, fragmentSource);
                if (!vertex || !fragment)
                {
                    if (vertex) glDeleteShader(vertex);
                    if (fragment) glDeleteShader(fragment);
                    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
                    if (!g_reflectionInitFailedLogged)
                    {
                        LocalDiagnostics::Write("Live puddle reflections disabled: GLES2 shader compilation failed.");
                        g_reflectionInitFailedLogged = true;
                    }
                    return false;
                }

                g_reflectionProgram = glCreateProgram();
                glAttachShader(g_reflectionProgram, vertex);
                glAttachShader(g_reflectionProgram, fragment);
                glBindAttribLocation(g_reflectionProgram, 0, "aPosition");
                glBindAttribLocation(g_reflectionProgram, 1, "aScreenUv");
                glBindAttribLocation(g_reflectionProgram, 2, "aMaskUv");
                glLinkProgram(g_reflectionProgram);
                glDeleteShader(vertex);
                glDeleteShader(fragment);

                GLint linked = GL_FALSE;
                glGetProgramiv(g_reflectionProgram, GL_LINK_STATUS, &linked);
                if (linked != GL_TRUE)
                {
                    char log[512] = {};
                    GLsizei length = 0;
                    glGetProgramInfoLog(g_reflectionProgram, sizeof(log), &length, log);
                    LocalDiagnostics::Write("Puddle reflection shader link failed: %s", log);
                    glDeleteProgram(g_reflectionProgram);
                    g_reflectionProgram = 0;
                    g_reflectionInitFailedLogged = true;
                    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
                    return false;
                }
                g_sceneUniform = glGetUniformLocation(g_reflectionProgram, "uScene");
                g_maskUniform = glGetUniformLocation(g_reflectionProgram, "uMask");
                g_mirrorLineUniform = glGetUniformLocation(g_reflectionProgram, "uMirrorLine");
                g_timeUniform = glGetUniformLocation(g_reflectionProgram, "uTime");
                g_strengthUniform = glGetUniformLocation(g_reflectionProgram, "uStrength");
            }

            if (!g_sceneCaptureTexture)
            {
                glGenTextures(1, &g_sceneCaptureTexture);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, g_sceneCaptureTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }

            if (!g_reflectionMaskTexture)
            {
                glGenTextures(1, &g_reflectionMaskTexture);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, g_reflectionMaskTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, g_puddleMaskWidth,
                    g_puddleMaskHeight, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
                    g_puddleMask.data());
            }

            glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture1));
            glUseProgram(static_cast<GLuint>(previousProgram));
            glActiveTexture(static_cast<GLenum>(previousActiveTexture));
            if (!g_reflectionProgram || !g_sceneCaptureTexture || !g_reflectionMaskTexture)
            {
                LocalDiagnostics::Write("Live puddle reflections disabled: GLES2 texture/program creation failed.");
                g_reflectionInitFailedLogged = true;
                return false;
            }
            return true;
        }

        struct ReflectionGLState
        {
            GLint program = 0;
            GLint activeTexture = GL_TEXTURE0;
            GLint texture0 = 0;
            GLint texture1 = 0;
            GLint arrayBuffer = 0;
            GLint blendSrcRgb = GL_ONE;
            GLint blendDstRgb = GL_ZERO;
            GLint blendSrcAlpha = GL_ONE;
            GLint blendDstAlpha = GL_ZERO;
            GLint depthFunc = GL_LESS;
            GLint cullMode = GL_BACK;
            GLboolean blend = GL_FALSE;
            GLboolean depthTest = GL_FALSE;
            GLboolean cullFace = GL_FALSE;
            GLboolean scissorTest = GL_FALSE;
            GLboolean depthWrite = GL_TRUE;
            GLboolean colorWrite[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
            GLint attribEnabled[3] = {};
            GLint attribSize[3] = {};
            GLint attribType[3] = {};
            GLint attribNormalized[3] = {};
            GLint attribStride[3] = {};
            GLint attribBuffer[3] = {};
            void* attribPointer[3] = {};
        };

        void SaveReflectionGLState(ReflectionGLState& state)
        {
            glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &state.activeTexture);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state.arrayBuffer);
            glGetIntegerv(GL_BLEND_SRC_RGB, &state.blendSrcRgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &state.blendDstRgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &state.blendSrcAlpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &state.blendDstAlpha);
            glGetIntegerv(GL_DEPTH_FUNC, &state.depthFunc);
            glGetIntegerv(GL_CULL_FACE_MODE, &state.cullMode);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &state.depthWrite);
            glGetBooleanv(GL_COLOR_WRITEMASK, state.colorWrite);
            state.blend = glIsEnabled(GL_BLEND);
            state.depthTest = glIsEnabled(GL_DEPTH_TEST);
            state.cullFace = glIsEnabled(GL_CULL_FACE);
            state.scissorTest = glIsEnabled(GL_SCISSOR_TEST);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture0);
            glActiveTexture(GL_TEXTURE1);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture1);

            for (GLuint i = 0; i < 3; ++i)
            {
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &state.attribEnabled[i]);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &state.attribSize[i]);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &state.attribType[i]);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &state.attribNormalized[i]);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &state.attribStride[i]);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &state.attribBuffer[i]);
                glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &state.attribPointer[i]);
            }
            glActiveTexture(static_cast<GLenum>(state.activeTexture));
        }

        void RestoreReflectionGLState(const ReflectionGLState& state)
        {
            for (GLuint i = 0; i < 3; ++i)
            {
                glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(state.attribBuffer[i]));
                glVertexAttribPointer(i, state.attribSize[i],
                    static_cast<GLenum>(state.attribType[i]),
                    state.attribNormalized[i] ? GL_TRUE : GL_FALSE,
                    state.attribStride[i], state.attribPointer[i]);
                if (state.attribEnabled[i]) glEnableVertexAttribArray(i);
                else glDisableVertexAttribArray(i);
            }
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(state.arrayBuffer));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(state.texture0));
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(state.texture1));
            glActiveTexture(static_cast<GLenum>(state.activeTexture));
            glUseProgram(static_cast<GLuint>(state.program));
            glBlendFuncSeparate(static_cast<GLenum>(state.blendSrcRgb),
                static_cast<GLenum>(state.blendDstRgb),
                static_cast<GLenum>(state.blendSrcAlpha),
                static_cast<GLenum>(state.blendDstAlpha));
            glDepthFunc(static_cast<GLenum>(state.depthFunc));
            glDepthMask(state.depthWrite);
            glColorMask(state.colorWrite[0], state.colorWrite[1],
                state.colorWrite[2], state.colorWrite[3]);
            glCullFace(static_cast<GLenum>(state.cullMode));
            if (state.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            if (state.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            if (state.cullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
            if (state.scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        }

        void RenderLivePuddleReflections(unsigned int now)
        {
            if (!g_enabled || Count() == 0)
                return;

            // Some game callbacks run outside the active RenderWare/GLES
            // camera update. GLES calls without a current context can return
            // zero without an error, which otherwise looks like shader failure.
            const GLubyte* glVersion = glGetString(GL_VERSION);
            if (!glVersion)
            {
                if (!g_missingGlContextLogged)
                {
                    LocalDiagnostics::Write("Puddle reflection skipped: no current GLES context at this render hook.");
                    g_missingGlContextLogged = true;
                }
                return;
            }
            if (!EnsurePuddleTexture() || !EnsureLiveReflectionResources())
                return;

            GLint viewport[4] = {};
            GLint maxTextureSize = 0;
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
            const int width = viewport[2];
            const int height = viewport[3];
            if (width <= 0 || height <= 0 || width > maxTextureSize || height > maxTextureSize)
                return;

            ReflectionGLState state;
            SaveReflectionGLState(state);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_sceneCaptureTexture);
            if (g_sceneCaptureWidth != width || g_sceneCaptureHeight != height)
            {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height,
                    0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
                g_sceneCaptureWidth = width;
                g_sceneCaptureHeight = height;
            }
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                viewport[0], viewport[1], width, height);

            const float logicalWidth = RsGlobal.maximumWidth > 0
                ? static_cast<float>(RsGlobal.maximumWidth) : static_cast<float>(width);
            const float logicalHeight = RsGlobal.maximumHeight > 0
                ? static_cast<float>(RsGlobal.maximumHeight) : static_cast<float>(height);
            const float viewportTop = logicalHeight -
                static_cast<float>(viewport[1] + viewport[3]);
            const RwCamera* camera = TheCamera.m_pRwCamera;
            const float nearPlane = camera ? camera->nearPlane : 0.9f;
            const float farPlane = camera ? camera->farPlane : 1000.0f;
            if (farPlane <= nearPlane)
            {
                RestoreReflectionGLState(state);
                return;
            }
            const float timeSeconds = static_cast<float>(now) * 0.001f;

            glUseProgram(g_reflectionProgram);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_sceneCaptureTexture);
            glUniform1i(g_sceneUniform, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_reflectionMaskTexture);
            glUniform1i(g_maskUniform, 1);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);

            for (const PuddleObject& puddle : g_puddles)
            {
                if (!puddle.active) continue;
                const unsigned int age = now - puddle.bornAt;
                if (age >= puddle.lifeMs) continue;
                const float alongRadius = puddle.radius * 1.45f * puddle.lengthScale;
                const float acrossRadius = puddle.radius * 0.70f * puddle.widthScale;
                const float headingCos = std::cos(puddle.heading);
                const float headingSin = std::sin(puddle.heading);
                const float ax = headingCos * alongRadius;
                const float ay = headingSin * alongRadius;
                const float sx = -headingSin * acrossRadius;
                const float sy = headingCos * acrossRadius;
                const CVector corners[4] = {
                    CVector(puddle.x - ax - sx, puddle.y - ay - sy, puddle.z + 0.03f),
                    CVector(puddle.x + ax - sx, puddle.y + ay - sy, puddle.z + 0.03f),
                    CVector(puddle.x - ax + sx, puddle.y - ay + sy, puddle.z + 0.03f),
                    CVector(puddle.x + ax + sx, puddle.y + ay + sy, puddle.z + 0.03f)
                };
                const float maskUv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
                float vertices[4][8] = {};
                float mirrorLine = 0.0f;
                bool projected = true;
                for (int i = 0; i < 4; ++i)
                {
                    CVector screen;
                    float scaleX = 0.0f, scaleY = 0.0f;
                    if (!CSprite::CalcScreenCoors(corners[i], &screen,
                            &scaleX, &scaleY, true, true) || screen.z <= nearPlane)
                    {
                        projected = false;
                        break;
                    }
                    const float pixelX = screen.x * width / logicalWidth;
                    const float pixelY = screen.y * height / logicalHeight;
                    const float u = (pixelX - viewport[0]) / width;
                    const float v = 1.0f - ((pixelY - viewportTop) / height);
                    const float depth = std::max(nearPlane, std::min(farPlane, screen.z));
                    const float ndcZ = (farPlane + nearPlane) / (farPlane - nearPlane) -
                        (2.0f * farPlane * nearPlane) / ((farPlane - nearPlane) * depth);
                    vertices[i][0] = 2.0f * u - 1.0f;
                    vertices[i][1] = 2.0f * v - 1.0f;
                    vertices[i][2] = ndcZ;
                    vertices[i][3] = 1.0f;
                    vertices[i][4] = u;
                    vertices[i][5] = v;
                    vertices[i][6] = maskUv[i][0];
                    vertices[i][7] = maskUv[i][1];
                    mirrorLine += v * 0.25f;
                }
                if (!projected) continue;

                const float ageRatio = static_cast<float>(age) /
                    static_cast<float>(puddle.lifeMs);
                const float fade = ageRatio < 0.55f ? 1.0f :
                    1.0f - std::min(1.0f, (ageRatio - 0.55f) / 0.45f);
                const float strength = fade * 0.52f;
                glUniform1f(g_mirrorLineUniform, mirrorLine);
                glUniform1f(g_timeUniform, timeSeconds + puddle.heading);
                glUniform1f(g_strengthUniform, strength);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), vertices[0]);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), &vertices[0][4]);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), &vertices[0][6]);
                glEnableVertexAttribArray(0);
                glEnableVertexAttribArray(1);
                glEnableVertexAttribArray(2);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }

            RestoreReflectionGLState(state);
            if (!g_reflectionRenderLogged)
            {
                LocalDiagnostics::Write("Puddle screen-space scene reflection active: captured viewport %dx%d. This is a mirrored screen sample, not depth-ray SSR.",
                    width, height);
                g_reflectionRenderLogged = true;
            }
        }

        void ShutdownLiveReflectionResources()
        {
            if (g_reflectionProgram) glDeleteProgram(g_reflectionProgram);
            if (g_sceneCaptureTexture) glDeleteTextures(1, &g_sceneCaptureTexture);
            if (g_reflectionMaskTexture) glDeleteTextures(1, &g_reflectionMaskTexture);
            g_reflectionProgram = 0;
            g_sceneCaptureTexture = 0;
            g_reflectionMaskTexture = 0;
            g_sceneCaptureWidth = 0;
            g_sceneCaptureHeight = 0;
            g_reflectionInitFailedLogged = false;
            g_reflectionRenderLogged = false;
            g_missingGlContextLogged = false;
        }
    #endif

        // Approximate the timecycle sky from game-clock keyframes and blend it
        // toward grey according to the current weather cloud coverage.
        PuddleReflection::Env ComputeSkyEnv()
        {
            struct Key { float h; float top[3]; float hor[3]; float light; };
            static const Key keys[] = {
                { 0.0f, {0.010f, 0.020f, 0.060f}, {0.030f, 0.040f, 0.090f}, 0.15f},
                { 5.0f, {0.010f, 0.020f, 0.060f}, {0.030f, 0.040f, 0.090f}, 0.15f},
                { 6.5f, {0.200f, 0.280f, 0.500f}, {0.900f, 0.550f, 0.380f}, 0.55f},
                { 8.5f, {0.220f, 0.450f, 0.850f}, {0.620f, 0.760f, 0.920f}, 1.00f},
                {16.0f, {0.220f, 0.450f, 0.850f}, {0.620f, 0.760f, 0.920f}, 1.00f},
                {18.0f, {0.280f, 0.300f, 0.500f}, {0.950f, 0.500f, 0.300f}, 0.65f},
                {19.5f, {0.040f, 0.060f, 0.140f}, {0.200f, 0.120f, 0.150f}, 0.28f},
                {21.0f, {0.010f, 0.020f, 0.060f}, {0.030f, 0.040f, 0.090f}, 0.15f},
                {24.0f, {0.010f, 0.020f, 0.060f}, {0.030f, 0.040f, 0.090f}, 0.15f},
            };
            const float hour = static_cast<float>(static_cast<int>(CClock::ms_nGameClockHours)) +
                static_cast<float>(static_cast<int>(CClock::ms_nGameClockMinutes)) / 60.0f;
            PuddleReflection::Env env{};
            const int n = static_cast<int>(sizeof(keys) / sizeof(keys[0]));
            int k = 0;
            while (k + 2 < n && hour > keys[k + 1].h) ++k;
            const Key& a = keys[k];
            const Key& b = keys[k + 1];
            const float t = std::max(0.0f, std::min(1.0f,
                (hour - a.h) / std::max(0.001f, b.h - a.h)));
            for (int i = 0; i < 3; ++i)
            {
                env.skyTop[i] = a.top[i] + (b.top[i] - a.top[i]) * t;
                env.skyHor[i] = a.hor[i] + (b.hor[i] - a.hor[i]) * t;
            }
            env.light = a.light + (b.light - a.light) * t;
            const float cloud = static_cast<float>(CWeather::CloudCoverage);
            env.cloud = std::max(0.0f, std::min(1.0f, cloud));
            const float grey = env.cloud * 0.55f;
            for (int i = 0; i < 3; ++i)
            {
                const float lumTop = (env.skyTop[0] + env.skyTop[1] + env.skyTop[2]) / 3.0f;
                const float lumHor = (env.skyHor[0] + env.skyHor[1] + env.skyHor[2]) / 3.0f;
                env.skyTop[i] += (lumTop - env.skyTop[i]) * grey;
                env.skyHor[i] += (lumHor - env.skyHor[i]) * grey;
            }
            return env;
        }

        // Sample collision ground height at every grid vertex. Height jumps
        // above 0.45m are faded so walls/poles do not become water surfaces.
        void BuildSurfaceGrid(PuddleObject& puddle)
        {
            constexpr int grid = PuddleReflection::GRID;
            for (int j = 0; j < grid; ++j)
                for (int i = 0; i < grid; ++i)
                {
                    const float u = static_cast<float>(i) / (grid - 1) * 2.0f - 1.0f;
                    const float v = static_cast<float>(j) / (grid - 1) * 2.0f - 1.0f;
                    const float wx = puddle.x + puddle.ax * u + puddle.sx * v;
                    const float wy = puddle.y + puddle.ay * u + puddle.sy * v;
                    float groundZ = 0.0f;
                    const bool found = Command<Commands::GET_GROUND_Z_FOR_3D_COORD>(
                        wx, wy, puddle.z + 1.2f, &groundZ);
                    const bool near = found && std::isfinite(groundZ) &&
                        std::fabs(groundZ - puddle.z) <= 0.45f;
                    const int index = j * grid + i;
                    puddle.gridZ[index] = (near ? groundZ : puddle.z) + 0.035f;
                    puddle.gridValid[index] = near ? 1.0f : 0.0f;
                }
        }

        void StorePuddleShadows(unsigned int now)
        {
            if (!g_puddleTexture) return;
            PuddleReflection::Item items[PuddleReflection::MAX_ITEMS];
            int itemCount = 0;
            const bool reflectionActive = g_reflectionEnabled && PuddleReflection::IsActive();
            for (PuddleObject& puddle : g_puddles)
            {
                if (!puddle.active) continue;
                const unsigned int age = now - puddle.bornAt;
                if (age >= puddle.lifeMs)
                {
                    Delete(puddle);
                    continue;
                }

                const float fadeIn = std::min(1.0f,
                    static_cast<float>(age) / static_cast<float>(SPAWN_FADE_IN_MS));
                const unsigned int fadeOutStart = puddle.lifeMs > SPAWN_FADE_OUT_MS
                    ? puddle.lifeMs - SPAWN_FADE_OUT_MS : puddle.lifeMs / 2U;
                const float fadeOut = age < fadeOutStart ? 1.0f
                    : 1.0f - std::min(1.0f, static_cast<float>(age - fadeOutStart) /
                        static_cast<float>(puddle.lifeMs - fadeOutStart));
                const float opacity = fadeIn * fadeOut;
                const float alongRadius = puddle.radius * 1.45f * puddle.lengthScale;
                const float acrossRadius = puddle.radius * 0.70f * puddle.widthScale;
                const float alongX = std::cos(puddle.heading) * alongRadius;
                const float alongY = std::sin(puddle.heading) * alongRadius;
                const float acrossX = -std::sin(puddle.heading) * acrossRadius;
                const float acrossY = std::cos(puddle.heading) * acrossRadius;
                CVector position(puddle.x, puddle.y, puddle.z + 0.02f);
                const i16 brightness = reflectionActive
                    ? static_cast<i16>(std::lround(opacity * 12.0f))
                    : static_cast<i16>(std::lround(opacity * 190.0f));
                if (g_reflectionEnabled && itemCount < PuddleReflection::MAX_ITEMS)
                {
                    PuddleReflection::Item& item = items[itemCount++];
                    item.cx = puddle.x; item.cy = puddle.y;
                    item.ax = puddle.ax; item.ay = puddle.ay;
                    item.sx = puddle.sx; item.sy = puddle.sy;
                    std::memcpy(item.z, puddle.gridZ, sizeof(item.z));
                    std::memcpy(item.valid, puddle.gridValid, sizeof(item.valid));
                    std::memcpy(item.seed, puddle.seed, sizeof(item.seed));
                    item.strength = opacity;
                }
                CShadows::StoreShadowToBeRendered(SHADOW_DEFAULT, g_puddleTexture,
                    &position, alongX, alongY, acrossX, acrossY, brightness,
                    54, 60, 57, 2.0f, false, 1.0f, nullptr, false);
                if (!g_shadowSubmitLogged)
                {
                    LocalDiagnostics::Write("First puddle shadow submitted: center=(%.1f, %.1f, %.1f) halfSize=(%.1f, %.1f) alpha=%d.",
                        position.x, position.y, position.z,
                        std::sqrt(alongX * alongX + alongY * alongY),
                        std::sqrt(acrossX * acrossX + acrossY * acrossY), brightness);
                    g_shadowSubmitLogged = true;
                }
            }
            if (g_reflectionEnabled)
                PuddleReflection::Publish(items, itemCount, static_cast<float>(now) * 0.001f,
                    ComputeSkyEnv());
        }

        void Delete(PuddleObject& puddle)
        {
            if (!puddle.active) return;
            puddle.active = false;
        }

        bool IsFarEnoughFromExisting(float x, float y, float z)
        {
            constexpr float MIN_SPACING = 4.0f;
            const float spacingSq = MIN_SPACING * MIN_SPACING;
            for (const PuddleObject& puddle : g_puddles)
            {
                if (!puddle.active) continue;
                const float dx = puddle.x - x;
                const float dy = puddle.y - y;
                const float dz = puddle.z - z;
                if (dx * dx + dy * dy + dz * dz < spacingSq) return false;
            }
            return true;
        }

        bool IsWaterSurface(float x, float y, float groundZ)
        {
            float waterZ = 0.0f;
            if (!Command<Commands::GET_WATER_HEIGHT_AT_COORDS>(x, y, groundZ + 2.0f, &waterZ))
                return false;
            return std::isfinite(waterZ) && groundZ <= waterZ + 0.25f;
        }

        bool IsPointInCurrentView(float x, float y, float z)
        {
            const float logicalWidth = RsGlobal.maximumWidth > 0
                ? static_cast<float>(RsGlobal.maximumWidth) : 0.0f;
            const float logicalHeight = RsGlobal.maximumHeight > 0
                ? static_cast<float>(RsGlobal.maximumHeight) : 0.0f;
            if (logicalWidth <= 0.0f || logicalHeight <= 0.0f)
                return false;

            CVector screen;
            float scaleX = 0.0f;
            float scaleY = 0.0f;
            if (!CSprite::CalcScreenCoors(CVector(x, y, z + 0.04f), &screen,
                    &scaleX, &scaleY, true, true))
                return false;
            if (!std::isfinite(screen.x) || !std::isfinite(screen.y) ||
                !std::isfinite(screen.z) || screen.z <= 0.0f)
                return false;

            // Treat a padded viewport as visible so the outer edge of a
            // puddle cannot pop in just beyond the screen boundary.
            const float marginX = logicalWidth * 0.08f;
            const float marginY = logicalHeight * 0.08f;
            if (screen.x < -marginX || screen.x > logicalWidth + marginX ||
                screen.y < -marginY || screen.y > logicalHeight + marginY)
                return false;

            CVector spot(x, y, z + 0.04f);
            return TheCamera.IsSphereVisible(&spot, 4.5f);
        }

        bool FindGroundPoint(const CVector& playerPosition,
            float& x, float& y, float& z, float& heading)
        {
            const float searchMinDistance = std::max(8.0f, g_spawnMinDistance);
            const float searchMaxDistance = std::max(searchMinDistance + 4.0f,
                std::min(g_spawnRadius, 22.0f));
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                // Sample all directions and query the ground directly. This
                // allows puddles on sidewalks, parking lots, yards and other
                // nearby terrain, not just at car-path nodes in the road.
                const float angle = RandomFloat(0.0f, 6.28318531f);
                const float distance = RandomFloat(searchMinDistance, searchMaxDistance);
                x = playerPosition.x + std::cos(angle) * distance;
                y = playerPosition.y + std::sin(angle) * distance;
                if (!Command<Commands::GET_GROUND_Z_FOR_3D_COORD>(
                        x, y, playerPosition.z + 40.0f, &z))
                    continue;
                if (!std::isfinite(z) || std::fabs(z - playerPosition.z) > 15.0f) continue;
                if (IsWaterSurface(x, y, z)) continue;

                // Keep new puddles outside the padded camera view. They fade
                // in over time, so turning toward one does not reveal it at full opacity.
                if (IsPointInCurrentView(x, y, z))
                    continue;

                // Keep candidates local even if a ground query returns data
                // from another streamed sector.
                const float finalDx = x - playerPosition.x;
                const float finalDy = y - playerPosition.y;
                const float finalDistanceSq = finalDx * finalDx + finalDy * finalDy;
                const float maxAcceptedDistance = searchMaxDistance + 1.0f;
                if (finalDistanceSq > maxAcceptedDistance * maxAcceptedDistance)
                {
                    if (!g_spawnSearchFailureLogged)
                    {
                        LocalDiagnostics::Write("Puddle ground point rejected: final distance=%.1f exceeds %.1f from player.",
                            std::sqrt(finalDistanceSq), maxAcceptedDistance);
                        g_spawnSearchFailureLogged = true;
                    }
                    continue;
                }
                if (!IsFarEnoughFromExisting(x, y, z)) continue;

                heading = RandomFloat(0.0f, 6.28318531f);
                return true;
            }
            return false;
        }

        bool Spawn(const CVector& playerPosition)
        {
            PuddleObject* slot = nullptr;
            for (PuddleObject& puddle : g_puddles)
                if (!puddle.active) { slot = &puddle; break; }
            if (!slot) return false;

            float x = 0.0f, y = 0.0f, z = 0.0f, heading = 0.0f;
            if (!FindGroundPoint(playerPosition, x, y, z, heading))
            {
                if (!g_spawnSearchFailureLogged)
                {
                    logger->Info("No suitable nearby ground point found for a puddle yet; will retry.");
                    LocalDiagnostics::Write("Puddle spawn failed: no suitable nearby ground point found; retrying.");
                    g_spawnSearchFailureLogged = true;
                }
                return false;
            }
            g_spawnSearchFailureLogged = false;

            // Project the supplied becek texture onto any nearby visible
            // ground surface; placement is not restricted to road nodes.
            slot->x = x;
            slot->y = y;
            slot->z = z;
            slot->radius = RandomFloat(DECAL_RADIUS_MIN, DECAL_RADIUS_MAX);
            // Keep the underlying mesh roughly round; variation is procedural
            // and comes from the unique shader seed for this puddle.
            slot->lengthScale = RandomFloat(0.88f, 1.15f);
            slot->widthScale = RandomFloat(0.88f, 1.15f);
            slot->heading = heading;
            slot->bornAt = CTimer::GetTimeMS();
            slot->lifeMs = RandomUInt(g_lifeMinMs, g_lifeMaxMs);
            {
                const float along = slot->radius * 1.25f * slot->lengthScale;
                const float across = slot->radius * 1.25f * slot->widthScale;
                slot->ax = std::cos(heading) * along;
                slot->ay = std::sin(heading) * along;
                slot->sx = -std::sin(heading) * across;
                slot->sy = std::cos(heading) * across;
                for (float& seed : slot->seed) seed = RandomFloat(0.0f, 1.0f);
                BuildSurfaceGrid(*slot);
            }
            slot->active = true;
            logger->Info("Spawned ground puddle at %.1f, %.1f, %.1f (radius %.1f heading %.1f).",
                x, y, z, slot->radius, heading * 57.2957795f);
            const float distanceToPlayer = std::sqrt(
                (x - playerPosition.x) * (x - playerPosition.x) +
                (y - playerPosition.y) * (y - playerPosition.y));
            LocalDiagnostics::Write("Puddle ground point accepted: anchor=(%.1f, %.1f, %.1f) distance=%.1f radius=%.1f heading=%.1f life=%ums.",
                x, y, z, distanceToPlayer, slot->radius, heading * 57.2957795f,
                slot->lifeMs);
            LocalDiagnostics::Write("Puddle spawned: center=(%.1f, %.1f, %.1f) radius=%.1f world units.",
                x, y, z, slot->radius);
            return true;
        }

        void Tick()
        {
            if (!g_enabled) return;
            CPlayerPed* player = FindPlayerPed(0);
            if (!player || player->GetInterior() != 0) return;

            const unsigned int now = CTimer::GetTimeMS();
            g_lastTickTime = now;
            if (static_cast<int>(now - g_nextSpawnTime) < 0)
            {
                if (!g_warmupLogged)
                {
                    LocalDiagnostics::Write("Puddle warmup: waiting %ums for world/camera before road spawn.",
                        WORLD_WARMUP_MS);
                    g_warmupLogged = true;
                }
                return;
            }
            // Create the RenderWare texture from the game-process update,
            // before the renderer starts its per-frame road/shadow passes.
            EnsurePuddleTexture();
            const CVector playerPosition = player->GetPosition();
            const float cleanupSq = g_cleanupRadius * g_cleanupRadius;
            for (PuddleObject& puddle : g_puddles)
            {
                if (!puddle.active) continue;
                const float dx = puddle.x - playerPosition.x;
                const float dy = puddle.y - playerPosition.y;
                const float dz = puddle.z - playerPosition.z;
                if (dx * dx + dy * dy + dz * dz > cleanupSq ||
                    now - puddle.bornAt >= puddle.lifeMs)
                    Delete(puddle);
            }

            const int remaining = g_maxPuddles - Count();
            if (remaining <= 0) return;
            const int batchSize = std::min(PUDDLES_PER_SPAWN_BATCH, remaining);
            int spawned = 0;
            for (int i = 0; i < batchSize; ++i)
            {
                if (!Spawn(playerPosition)) break;
                ++spawned;
            }
            if (spawned > 0)
                g_nextSpawnTime = now + static_cast<unsigned int>(g_spawnIntervalMs);
            else
                g_nextSpawnTime = now + 1200U;
        }
    }

    void Initialize(bool enabled, int maxPuddles, int spawnIntervalMs,
        float spawnMinDistance, float spawnRadius, float cleanupRadius,
        unsigned int lifeMinMs, unsigned int lifeMaxMs,
        bool reflection, float reflectionDistance)
    {
        LocalDiagnostics::Open();
        g_reflectionEnabled = reflection;
        g_reflectionWall = reflectionDistance;
        const int requestedMax = maxPuddles < 1 ? 1 : (maxPuddles > 2 ? 2 : maxPuddles);
        g_maxPuddles = static_cast<int>(RandomUInt(1U, static_cast<unsigned int>(requestedMax)));
        g_spawnIntervalMs = spawnIntervalMs < 20000 ? 20000 : spawnIntervalMs;
        g_spawnMinDistance = spawnMinDistance < 4.0f ? 4.0f : spawnMinDistance;
        g_spawnRadius = spawnRadius < g_spawnMinDistance + 8.0f
            ? g_spawnMinDistance + 8.0f : spawnRadius;
        g_cleanupRadius = cleanupRadius < g_spawnRadius + 8.0f
            ? g_spawnRadius + 8.0f : cleanupRadius;
        g_lifeMinMs = std::max(45000U, std::min(lifeMinMs, 240000U));
        g_lifeMaxMs = std::max(g_lifeMinMs, std::min(lifeMaxMs, 300000U));
        for (PuddleObject& puddle : g_puddles)
            Delete(puddle);
        g_enabled = enabled;
        g_nextSpawnTime = CTimer::GetTimeMS() + WORLD_WARMUP_MS;
        g_warmupLogged = false;

        logger->Info("Puddle config: enabled=%d render=natural-becek-shadow max=%d (randomized 2-%d) batch=%d distance=%.1f-%.1f cleanup=%.1f interval=%dms",
            g_enabled ? 1 : 0, g_maxPuddles, requestedMax,
            PUDDLES_PER_SPAWN_BATCH,
            g_spawnMinDistance, g_spawnRadius, g_cleanupRadius,
            g_spawnIntervalMs);
        g_shadowSubmitLogged = false;
        LocalDiagnostics::Write("Puddle build: generated-organic-dark-mask=1 native-soft-shadow=1 additive-outline-ripples=0 GLES3-screen-reflection=%d maxPuddles=%d batch=%d fadeOut=%ums.",
            g_reflectionEnabled ? 1 : 0,
            g_maxPuddles, PUDDLES_PER_SPAWN_BATCH, SPAWN_FADE_OUT_MS);
        LocalDiagnostics::Write("Puddle init: enabled=%d render=natural-becek-shadow max=%d distance=%.1f-%.1f cleanup=%.1f interval=%dms life=%u-%ums.",
            g_enabled ? 1 : 0, g_maxPuddles, g_spawnMinDistance, g_spawnRadius,
            g_cleanupRadius, g_spawnIntervalMs, g_lifeMinMs, g_lifeMaxMs);
        if (g_enabled && g_reflectionEnabled)
            PuddleReflection::Install(g_reflectionWall);
        if (g_enabled)
        {
            Events::gameProcessEvent.after += []() { Tick(); };
            // GTA consumes the transient shadow queue during rendering. Add
            // puddles immediately before the road pass, not after game logic,
            // where the engine may clear the queue before it is drawn.
            Events::roadsRenderEvent.before += []()
            {
                if (g_enabled)
                    StorePuddleShadows(CTimer::GetTimeMS());
            };
        }
    }

    void Shutdown()
    {
        PuddleReflection::Shutdown();
        for (PuddleObject& puddle : g_puddles) Delete(puddle);
        if (g_puddleTexture) RwTextureDestroy(g_puddleTexture);
        if (g_puddleRaster) RwRasterDestroy(g_puddleRaster);
        g_puddleTexture = nullptr;
        g_puddleRaster = nullptr;
        g_textureLoadAttempted = false;
        g_enabled = false;
        LocalDiagnostics::Write("Puddle shutdown.");
        LocalDiagnostics::Close();
    }
}
