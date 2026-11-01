#include "PuddleReflection.h"
#include "mod/local_diagnostics.h"

#include <mod/amlmod.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>

namespace PuddleReflection
{
    namespace
    {
        constexpr unsigned char kMagic[16] = {
            0x52, 0x45, 0x46, 0x01, 0x4C, 0x45, 0x43, 0x01,
            0x54, 0x31, 0x37, 0x01, 0x7B, 0x21, 0x5A, 0x01 };
        constexpr int CELLS = GRID - 1;
        constexpr int VERT_FLOATS = 6; // world(3) uv(2) valid(1)
        constexpr int MESH_VERTS = CELLS * CELLS * 6;

        std::atomic<bool> g_enabled{false};
        std::atomic<unsigned int> g_sentinelTex{0};
        std::atomic<int> g_frames{0};
        std::atomic<int> g_lastPassFrame{-1000};
        float g_wallDistance = 14.0f;

        struct Snapshot { Item items[MAX_ITEMS]; int count = 0; float time = 0.0f; Env env{}; };
        Snapshot g_snap[2];
        std::atomic<int> g_snapIndex{0};

        // Render-thread-only state.
        bool g_passDone = false;
        bool g_inPass = false;
        bool g_gpuReady = false;
        bool g_gpuFailed = false;
        bool g_uniformsLogged = false;
        bool g_centerLogged = false;
        GLuint g_program = 0;
        GLuint g_vao = 0;
        GLuint g_vbo = 0;
        GLuint g_sceneTex = 0;
        int g_sceneW = 0;
        int g_sceneH = 0;
        GLint g_uView = -1, g_uProj = -1, g_uCam = -1, g_uTime = -1;
        GLint g_uStrength = -1, g_uWall = -1, g_uScene = -1, g_uSeed = -1;
        GLint g_uSkyTop = -1, g_uSkyHor = -1, g_uLight = -1, g_uCloud = -1;
        GLuint g_gameProg = 0;
        GLint g_gameView = -1, g_gameProj = -1, g_gameObj = -1;
        unsigned long long g_passCount = 0;
        unsigned long long g_passMicros = 0;

        thread_local GLuint t_bound[8]{};
        thread_local int t_unit = 0;

        EGLBoolean (*o_swap)(EGLDisplay, EGLSurface) = nullptr;
        void (*o_drawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
        void (*o_drawArrays)(GLenum, GLint, GLsizei) = nullptr;
        void (*o_bindTexture)(GLenum, GLuint) = nullptr;
        void (*o_activeTexture)(GLenum) = nullptr;
        void (*o_texImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
        void (*o_texSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;

        const char* kVert =
            "#version 300 es\n"
            "layout(location=0) in vec3 aWorld;\n"
            "layout(location=1) in vec2 aUv;\n"
            "layout(location=2) in float aValid;\n"
            "uniform mat4 uView; uniform mat4 uProj;\n"
            "out vec3 vWorld; out vec2 vUv; out float vValid;\n"
            "void main(){ vWorld=aWorld; vUv=aUv; vValid=aValid;\n"
            "  gl_Position=uProj*(uView*vec4(aWorld,1.0)); }\n";

        const char* kFrag =
            "#version 300 es\n"
            "precision highp float;\n"
            "uniform sampler2D uScene;\n"
            "uniform mat4 uView; uniform mat4 uProj;\n"
            "uniform vec3 uCam; uniform float uTime; uniform float uStrength; uniform float uWall;\n"
            "uniform vec4 uSeed;\n"
            "uniform vec3 uSkyTop; uniform vec3 uSkyHor; uniform float uLight; uniform float uCloud;\n"
            "in vec3 vWorld; in vec2 vUv; in float vValid; out vec4 oColor;\n"
            "float hash(vec2 p){ p=fract(p*vec2(123.34,456.21)); p+=dot(p,p+45.32); return fract(p.x*p.y); }\n"
            "float noise(vec2 p){ vec2 i=floor(p); vec2 f=fract(p); f=f*f*(3.0-2.0*f);\n"
            "  return mix(mix(hash(i),hash(i+vec2(1.0,0.0)),f.x),\n"
            "             mix(hash(i+vec2(0.0,1.0)),hash(i+vec2(1.0,1.0)),f.x),f.y); }\n"
            "float fbm(vec2 p){ float a=0.5; float s=0.0;\n"
            "  for(int k=0;k<4;k++){ s+=a*noise(p); p=p*2.03+17.1; a*=0.5; } return s; }\n"
            "float smin(float a,float b,float k){ float h=max(k-abs(a-b),0.0)/k; return min(a,b)-h*h*k*0.25; }\n"
            "vec3 sky(vec3 R){\n"
            "  float t=clamp(R.z,0.0,1.0);\n"
            "  vec3 c=mix(uSkyHor,uSkyTop,pow(t,0.55));\n"
            "  vec2 cuv=R.xy/(R.z+0.3)*1.3+vec2(uTime*0.012,0.0);\n"
            "  float n=fbm(cuv*1.7);\n"
            "  float cov=clamp(0.30+uCloud*0.65,0.0,0.95);\n"
            "  float cl=smoothstep(1.0-cov,1.0-cov+0.28,n)*smoothstep(0.0,0.22,R.z);\n"
            "  vec3 cc=mix(vec3(0.95,0.96,0.98),vec3(0.62,0.66,0.72),uCloud*0.6)*uLight;\n"
            "  return mix(c,cc,cl*0.9);\n"
            "}\n"
            "void main(){\n"
            "  vec2 p=vUv*2.0-1.0;\n"
            "  float ang=atan(p.y,p.x);\n"
            "  float r=length(p);\n"
            "  float amp=0.7+0.3*uSeed.x;\n"
            "  float b=0.62+amp*(0.095*sin(2.0*ang+uSeed.y*6.283)+0.07*sin(3.0*ang+uSeed.z*6.283+1.3)\n"
            "                   +0.03*sin(5.0*ang+uSeed.w*6.283+2.1)+0.012*sin(8.0*ang+uSeed.x*6.283+0.7));\n"
            "  float sd=r-b;\n"
            "  float la=uSeed.z*6.283;\n"
            "  vec2 c2=vec2(cos(la),sin(la))*0.40;\n"
            "  float r2=0.22+0.14*uSeed.w;\n"
            "  sd=smin(sd,length(p-c2)-r2,0.18);\n"
            "  sd+=(noise(p*5.0+uSeed.xy*31.0)-0.5)*0.045;\n"
            "  float shape=1.0-smoothstep(-0.05,0.035,sd);\n"
            "  shape*=smoothstep(0.35,0.95,vValid);\n"
            "  if(shape<0.01) discard;\n"
            "  float dep=smoothstep(0.0,0.4,-sd);\n"
            "  vec2 rp=p*3.2;\n"
            "  vec2 rip=vec2(noise(rp+vec2(uTime*0.35,0.0)),noise(rp.yx+vec2(0.0,-uTime*0.30)))-0.5;\n"
            // Derivative normal follows the sampled collision mesh slope;
            // procedural ripple is deliberately subtle to keep reflections calm.
            "  vec3 N=normalize(cross(dFdx(vWorld),dFdy(vWorld)));\n"
            "  if(N.z<0.0) N=-N;\n"
            "  N=normalize(N+vec3(rip*0.045,0.0));\n"
            "  vec3 V=normalize(vWorld-uCam);\n"
            "  vec3 R=reflect(V,N);\n"
                // uWall is the total ray distance. Scaling by the inverse
                // horizontal component made near-vertical rays travel hundreds
                // of world units, sampling scenery far away and reversing the
                // apparent placement when the camera looked down.
                "  vec3 H=vWorld+R*uWall;\n"
            "  vec4 c=uProj*(uView*vec4(H,1.0));\n"
            "  vec3 skyc=sky(R);\n"
            "  vec2 screenUv=gl_FragCoord.xy/vec2(textureSize(uScene,0));\n"
            // When the ray projects above/behind the camera (common when the
            // player looks down), use a small view-aligned screen sample instead
            // of losing all reflected scene content to the sky fallback.
            "  vec3 viewR=mat3(uView)*R;\n"
            "  vec4 rayClip=uProj*vec4(viewR,0.0);\n"
            "  vec2 raySlope=rayClip.xy/max(abs(rayClip.w),0.15);\n"
                "  vec2 fallbackDir=length(raySlope)>0.001?normalize(raySlope):vec2(0.0,1.0);\n"
                "  vec2 fallbackUv=clamp(screenUv+fallbackDir*clamp(uWall*0.006,0.06,0.18),vec2(0.015),vec2(0.985));\n"
            "  vec3 fallbackScene=texture(uScene,fallbackUv).rgb;\n"
            "  vec3 refl=mix(skyc,fallbackScene,0.68);\n"
            "  if(c.w>0.3){\n"
            "    vec2 uv=c.xy/c.w*0.5+0.5;\n"
            "    vec2 e=smoothstep(vec2(-0.02),vec2(0.08),uv)*(1.0-smoothstep(vec2(0.92),vec2(1.02),uv));\n"
            "    float vis=e.x*e.y;\n"
            "    vec3 sc=texture(uScene,clamp(uv,vec2(0.002),vec2(0.998))).rgb;\n"
            "    refl=mix(refl,sc,vis*0.90);\n"
            "  }\n"
            "  float cosT=clamp(dot(-V,N),0.0,1.0);\n"
            // Keep a visible reflection at top-down angles while strengthening
            // it naturally toward grazing angles.
            "  float F=0.28+0.72*pow(1.0-cosT,3.0);\n"
            "  float lt=0.35+0.65*uLight;\n"
            "  vec3 deep=vec3(0.035,0.17,0.27)*lt;\n"
            "  vec3 shallow=vec3(0.10,0.30,0.38)*lt;\n"
            "  vec3 body=mix(shallow,deep,dep);\n"
            "  vec3 col=mix(body,refl*vec3(0.92,0.97,1.0),clamp(F,0.0,0.92));\n"
            "  float alpha=shape*uStrength*(0.50+0.34*dep);\n"
            "  oColor=vec4(col,alpha);\n"
            "}\n";

        long long NowMicros()
        {
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<long long>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
        }

        void DrainGlErrors() { while (glGetError() != GL_NO_ERROR) {} }

        GLuint Compile(GLenum type, const char* source)
        {
            const GLuint shader = glCreateShader(type);
            if (!shader) return 0;
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint ok = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (ok == GL_TRUE) return shader;
            char log[1024]{};
            glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            LocalDiagnostics::Write("[REFL] %s shader compile failed: %s",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
            glDeleteShader(shader);
            return 0;
        }

        bool InitGpu()
        {
            if (g_gpuReady) return true;
            if (g_gpuFailed) return false;
            g_gpuFailed = true;

            const GLuint vs = Compile(GL_VERTEX_SHADER, kVert);
            const GLuint fs = vs ? Compile(GL_FRAGMENT_SHADER, kFrag) : 0;
            if (!vs || !fs)
            {
                if (vs) glDeleteShader(vs);
                if (fs) glDeleteShader(fs);
                return false;
            }
            g_program = glCreateProgram();
            glAttachShader(g_program, vs);
            glAttachShader(g_program, fs);
            glLinkProgram(g_program);
            glDeleteShader(vs);
            glDeleteShader(fs);
            GLint linked = GL_FALSE;
            glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE)
            {
                char log[1024]{};
                glGetProgramInfoLog(g_program, sizeof(log) - 1, nullptr, log);
                LocalDiagnostics::Write("[REFL] shader link failed: %s", log);
                glDeleteProgram(g_program);
                g_program = 0;
                return false;
            }

            g_uView = glGetUniformLocation(g_program, "uView");
            g_uProj = glGetUniformLocation(g_program, "uProj");
            g_uCam = glGetUniformLocation(g_program, "uCam");
            g_uTime = glGetUniformLocation(g_program, "uTime");
            g_uStrength = glGetUniformLocation(g_program, "uStrength");
            g_uWall = glGetUniformLocation(g_program, "uWall");
            g_uScene = glGetUniformLocation(g_program, "uScene");
            g_uSeed = glGetUniformLocation(g_program, "uSeed");
            g_uSkyTop = glGetUniformLocation(g_program, "uSkyTop");
            g_uSkyHor = glGetUniformLocation(g_program, "uSkyHor");
            g_uLight = glGetUniformLocation(g_program, "uLight");
            g_uCloud = glGetUniformLocation(g_program, "uCloud");

            GLint oldVao = 0, oldVbo = 0;
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldVbo);
            glGenVertexArrays(1, &g_vao);
            glGenBuffers(1, &g_vbo);
            glBindVertexArray(g_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
            glBufferData(GL_ARRAY_BUFFER, MESH_VERTS * VERT_FLOATS * sizeof(float), nullptr, GL_STREAM_DRAW);
            const GLsizei stride = VERT_FLOATS * sizeof(float);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(5 * sizeof(float)));
            glBindVertexArray(static_cast<GLuint>(oldVao));
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldVbo));
            if (!g_vao || !g_vbo)
            {
                LocalDiagnostics::Write("[REFL] GLES resource allocation failed.");
                return false;
            }
            g_gpuFailed = false;
            g_gpuReady = true;
            LocalDiagnostics::Write("[REFL] GLES 3 resources ready (program=%u vao=%u).", g_program, g_vao);
            return true;
        }

        bool ResolveGameMatrices(GLuint program)
        {
            if (program == g_gameProg && g_gameView >= 0 && g_gameProj >= 0) return true;
            g_gameProg = program;
            g_gameView = g_gameProj = g_gameObj = -1;
            GLint count = 0;
            glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
            for (GLint i = 0; i < count; ++i)
            {
                char name[96]{};
                GLsizei length = 0;
                GLint size = 0;
                GLenum type = 0;
                glGetActiveUniform(program, static_cast<GLuint>(i), sizeof(name) - 1,
                    &length, &size, &type, name);
                if (!g_uniformsLogged)
                    LocalDiagnostics::Write("[REFL] game program %u uniform[%d] %s type=0x%x",
                        program, i, name, type);
                if (type != GL_FLOAT_MAT4) continue;
                std::string lower(name);
                for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const GLint location = glGetUniformLocation(program, name);
                if (lower.find("proj") != std::string::npos) g_gameProj = location;
                else if (lower.find("view") != std::string::npos) g_gameView = location;
                else if (lower.find("obj") != std::string::npos || lower.find("world") != std::string::npos ||
                         lower.find("model") != std::string::npos) g_gameObj = location;
            }
            g_uniformsLogged = true;
            LocalDiagnostics::Write("[REFL] matrices: view=%d proj=%d obj=%d", g_gameView, g_gameProj, g_gameObj);
            return g_gameView >= 0 && g_gameProj >= 0;
        }

        bool EnsureSceneTexture(const GLint viewport[4])
        {
            if (viewport[2] <= 0 || viewport[3] <= 0) return false;
            if (!g_sceneTex) glGenTextures(1, &g_sceneTex);
            if (!g_sceneTex) return false;
            glBindTexture(GL_TEXTURE_2D, g_sceneTex);
            if (g_sceneW != viewport[2] || g_sceneH != viewport[3])
            {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, viewport[2], viewport[3],
                    0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                g_sceneW = viewport[2];
                g_sceneH = viewport[3];
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            return true;
        }

        bool TriggerNow()
        {
            if (!g_enabled.load(std::memory_order_relaxed) || g_passDone || g_inPass) return false;
            const GLuint sentinel = g_sentinelTex.load(std::memory_order_relaxed);
            if (!sentinel) return false;
            bool bound = false;
            for (int i = 0; i < 4; ++i) if (t_bound[i] == sentinel) bound = true;
            if (!bound) return false;
            return g_snap[g_snapIndex.load(std::memory_order_acquire)].count > 0;
        }

        bool CaptureScene()
        {
            g_inPass = true;
            DrainGlErrors();
            GLint viewport[4] = {};
            glGetIntegerv(GL_VIEWPORT, viewport);
            GLint previous = 0, active = GL_TEXTURE0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
            const bool ok = InitGpu() && EnsureSceneTexture(viewport);
            if (ok)
            {
                glBindTexture(GL_TEXTURE_2D, g_sceneTex);
                // Capture the already-rendered scene, including peds and
                // vehicles when present. The shader samples this image in
                // screen space; it does not ray-trace entity geometry/depth.
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    viewport[0], viewport[1], viewport[2], viewport[3]);
            }
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
            glActiveTexture(static_cast<GLenum>(active));
            DrainGlErrors();
            g_inPass = false;
            return ok;
        }

        int BuildMesh(const Item& it, float* out)
        {
            int n = 0;
            auto vert = [&](int i, int j)
            {
                const float u = static_cast<float>(i) / CELLS, v = static_cast<float>(j) / CELLS;
                const int k = j * GRID + i;
                out[n * VERT_FLOATS + 0] = it.cx + it.ax * (2.0f * u - 1.0f) + it.sx * (2.0f * v - 1.0f);
                out[n * VERT_FLOATS + 1] = it.cy + it.ay * (2.0f * u - 1.0f) + it.sy * (2.0f * v - 1.0f);
                out[n * VERT_FLOATS + 2] = it.z[k];
                out[n * VERT_FLOATS + 3] = u;
                out[n * VERT_FLOATS + 4] = v;
                out[n * VERT_FLOATS + 5] = it.valid[k];
                ++n;
            };
            for (int j = 0; j < CELLS; ++j)
                for (int i = 0; i < CELLS; ++i)
                {
                    vert(i, j); vert(i + 1, j); vert(i, j + 1);
                    vert(i + 1, j); vert(i + 1, j + 1); vert(i, j + 1);
                }
            return n;
        }

        void DrawOverlay()
        {
            g_inPass = true;
            const long long t0 = NowMicros();
            DrainGlErrors();

            GLint prog = 0, vao = 0, vbo = 0, activeTex = GL_TEXTURE0, tex0 = 0;
            GLint blendSrcRgb, blendDstRgb, blendSrcA, blendDstA, depthFunc;
            GLfloat polyFactor = 0, polyUnits = 0;
            GLboolean depthMask = GL_TRUE, colorMask[4] = {1, 1, 1, 1};
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
            glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
            glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
            glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &polyFactor);
            glGetFloatv(GL_POLYGON_OFFSET_UNITS, &polyUnits);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
            glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
            const GLboolean blend = glIsEnabled(GL_BLEND), depthTest = glIsEnabled(GL_DEPTH_TEST),
                cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST),
                stencil = glIsEnabled(GL_STENCIL_TEST), poly = glIsEnabled(GL_POLYGON_OFFSET_FILL);
            glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);

            bool drew = false;
            float view[16] = {}, proj[16] = {};
            if (ResolveGameMatrices(static_cast<GLuint>(prog)))
            {
                glGetUniformfv(static_cast<GLuint>(prog), g_gameView, view);
                glGetUniformfv(static_cast<GLuint>(prog), g_gameProj, proj);
                if (g_gameObj >= 0 && !g_centerLogged)
                {
                    float obj[16];
                    glGetUniformfv(static_cast<GLuint>(prog), g_gameObj, obj);
                    LocalDiagnostics::Write("[REFL] obj diag [0]=%.2f [5]=%.2f [10]=%.2f [12..14]=(%.2f %.2f %.2f)",
                        obj[0], obj[5], obj[10], obj[12], obj[13], obj[14]);
                }
                const float camX = -(view[0] * view[12] + view[1] * view[13] + view[2] * view[14]);
                const float camY = -(view[4] * view[12] + view[5] * view[13] + view[6] * view[14]);
                const float camZ = -(view[8] * view[12] + view[9] * view[13] + view[10] * view[14]);

                const Snapshot& snap = g_snap[g_snapIndex.load(std::memory_order_acquire)];
                if (g_sceneTex)
                {
                    glUseProgram(g_program);
                    glUniformMatrix4fv(g_uView, 1, GL_FALSE, view);
                    glUniformMatrix4fv(g_uProj, 1, GL_FALSE, proj);
                    glUniform3f(g_uCam, camX, camY, camZ);
                    glUniform1f(g_uTime, snap.time);
                    glUniform1f(g_uWall, g_wallDistance);
                    glUniform1i(g_uScene, 0);
                    glUniform3fv(g_uSkyTop, 1, snap.env.skyTop);
                    glUniform3fv(g_uSkyHor, 1, snap.env.skyHor);
                    glUniform1f(g_uLight, snap.env.light);
                    glUniform1f(g_uCloud, snap.env.cloud);
                    glBindTexture(GL_TEXTURE_2D, g_sceneTex);

                    glEnable(GL_BLEND);
                    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(GL_LEQUAL);
                    glDepthMask(GL_FALSE);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glDisable(GL_CULL_FACE);
                    glDisable(GL_SCISSOR_TEST);
                    glDisable(GL_STENCIL_TEST);
                    glEnable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(-2.0f, -4.0f);

                    glBindVertexArray(g_vao);
                    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
                    static thread_local float mesh[MESH_VERTS * VERT_FLOATS];
                    for (int i = 0; i < snap.count; ++i)
                    {
                        const Item& it = snap.items[i];
                        const int verts = BuildMesh(it, mesh);
                        if (!g_centerLogged)
                        {
                            const float cz = it.z[(GRID / 2) * GRID + GRID / 2];
                            float vc[4], pc[4];
                            for (int r = 0; r < 4; ++r)
                                vc[r] = view[r] * it.cx + view[4 + r] * it.cy + view[8 + r] * cz + view[12 + r];
                            for (int r = 0; r < 4; ++r)
                                pc[r] = proj[r] * vc[0] + proj[4 + r] * vc[1] + proj[8 + r] * vc[2] + proj[12 + r] * vc[3];
                            const float w = pc[3] != 0.0f ? pc[3] : 1.0f;
                            LocalDiagnostics::Write("[REFL] center world=(%.1f %.1f %.1f) cam=(%.1f %.1f %.1f) clip w=%.2f ndc=(%.2f %.2f %.2f)",
                                it.cx, it.cy, cz, camX, camY, camZ, pc[3], pc[0] / w, pc[1] / w, pc[2] / w);
                            g_centerLogged = true;
                        }
                        glBufferData(GL_ARRAY_BUFFER, verts * VERT_FLOATS * sizeof(float), mesh, GL_STREAM_DRAW);
                        glUniform1f(g_uStrength, it.strength);
                        glUniform4fv(g_uSeed, 1, it.seed);
                        o_drawArrays(GL_TRIANGLES, 0, verts);
                        drew = true;
                    }
                }
            }

            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex0));
            glActiveTexture(static_cast<GLenum>(activeTex));
            glBindVertexArray(static_cast<GLuint>(vao));
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(vbo));
            glUseProgram(static_cast<GLuint>(prog));
            glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcA, blendDstA);
            glDepthFunc(depthFunc);
            glDepthMask(depthMask);
            glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
            glPolygonOffset(polyFactor, polyUnits);
            if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
            if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
            if (stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
            if (poly) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);
            DrainGlErrors();

            if (drew)
            {
                g_lastPassFrame.store(g_frames.load());
                ++g_passCount;
                g_passMicros += static_cast<unsigned long long>(NowMicros() - t0);
            }
            g_inPass = false;
        }

        void H_bindTexture(GLenum target, GLuint texture)
        {
            if (!g_inPass && target == GL_TEXTURE_2D && t_unit >= 0 && t_unit < 8)
                t_bound[t_unit] = texture;
            o_bindTexture(target, texture);
        }

        void H_activeTexture(GLenum unit)
        {
            if (!g_inPass) t_unit = static_cast<int>(unit) - static_cast<int>(GL_TEXTURE0);
            o_activeTexture(unit);
        }

        void CheckMagic(GLint level, GLint x, GLint y, GLsizei width, const void* pixels, const char* via)
        {
            if (g_inPass || level != 0 || x != 0 || y != 0 || width < 4 || !pixels) return;
            if (std::memcmp(pixels, kMagic, sizeof(kMagic)) != 0) return;
            const GLuint texture = t_unit >= 0 && t_unit < 8 ? t_bound[t_unit] : 0;
            if (texture && texture != g_sentinelTex.load())
            {
                g_sentinelTex.store(texture);
                LocalDiagnostics::Write("[REFL] sentinel puddle texture identified: GL id=%u via %s.", texture, via);
            }
        }

        void H_texImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height,
            GLint border, GLenum format, GLenum type, const void* pixels)
        {
            if (target == GL_TEXTURE_2D) CheckMagic(level, 0, 0, width, pixels, "glTexImage2D");
            o_texImage2D(target, level, internalFormat, width, height, border, format, type, pixels);
        }

        void H_texSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
            GLenum format, GLenum type, const void* pixels)
        {
            if (target == GL_TEXTURE_2D) CheckMagic(level, x, y, width, pixels, "glTexSubImage2D");
            o_texSubImage2D(target, level, x, y, width, height, format, type, pixels);
        }

        void H_drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
        {
            if (TriggerNow())
            {
                g_passDone = true;
                const bool captured = CaptureScene();
                o_drawElements(mode, count, type, indices);
                if (captured) DrawOverlay();
                return;
            }
            o_drawElements(mode, count, type, indices);
        }

        void H_drawArrays(GLenum mode, GLint first, GLsizei count)
        {
            if (TriggerNow())
            {
                g_passDone = true;
                const bool captured = CaptureScene();
                o_drawArrays(mode, first, count);
                if (captured) DrawOverlay();
                return;
            }
            o_drawArrays(mode, first, count);
        }

        EGLBoolean H_swap(EGLDisplay display, EGLSurface surface)
        {
            const int frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
            g_passDone = false;
            if (frame % 600 == 0 && g_enabled.load(std::memory_order_relaxed))
            {
                const Snapshot& snapshot = g_snap[g_snapIndex.load(std::memory_order_acquire)];
                LocalDiagnostics::Write("[REFL] frame=%d puddles=%d sentinel=%u passes=%llu avg=%lluus active=%d",
                    frame, snapshot.count, g_sentinelTex.load(), g_passCount,
                    g_passCount ? g_passMicros / g_passCount : 0ULL, IsActive() ? 1 : 0);
                if (snapshot.count > 0 && g_sentinelTex.load() == 0)
                    LocalDiagnostics::Write("[REFL] WARNING: puddles exist but the magic texture upload was never seen.");
            }
            return o_swap(display, surface);
        }

        bool HookSym(void* library, const char* symbol, void* hook, void** original)
        {
            if (!library) return false;
            const uintptr_t address = aml->GetSym(library, symbol);
            if (!address)
            {
                LocalDiagnostics::Write("[REFL] symbol not found: %s", symbol);
                return false;
            }
            const bool ok = aml->Hook(reinterpret_cast<void*>(address), hook, original);
            if (!ok) LocalDiagnostics::Write("[REFL] hook failed: %s", symbol);
            return ok;
        }
    }

    void Install(float wallDistance)
    {
        if (!std::isfinite(wallDistance)) wallDistance = 14.0f;
        g_wallDistance = wallDistance < 4.0f ? 4.0f : (wallDistance > 60.0f ? 60.0f : wallDistance);
        void* egl = aml->GetLibHandle("libEGL.so");
        void* gles = aml->GetLibHandle("libGLESv2.so");
        bool ok = true;
        ok &= HookSym(egl, "eglSwapBuffers", reinterpret_cast<void*>(H_swap), reinterpret_cast<void**>(&o_swap));
        ok &= HookSym(gles, "glDrawElements", reinterpret_cast<void*>(H_drawElements), reinterpret_cast<void**>(&o_drawElements));
        ok &= HookSym(gles, "glDrawArrays", reinterpret_cast<void*>(H_drawArrays), reinterpret_cast<void**>(&o_drawArrays));
        ok &= HookSym(gles, "glBindTexture", reinterpret_cast<void*>(H_bindTexture), reinterpret_cast<void**>(&o_bindTexture));
        ok &= HookSym(gles, "glActiveTexture", reinterpret_cast<void*>(H_activeTexture), reinterpret_cast<void**>(&o_activeTexture));
        ok &= HookSym(gles, "glTexImage2D", reinterpret_cast<void*>(H_texImage2D), reinterpret_cast<void**>(&o_texImage2D));
        ok &= HookSym(gles, "glTexSubImage2D", reinterpret_cast<void*>(H_texSubImage2D), reinterpret_cast<void**>(&o_texSubImage2D));
        g_enabled.store(ok, std::memory_order_release);
        LocalDiagnostics::Write("[REFL] install %s (wall=%.1fm; GLES 3 required).",
            ok ? "OK" : "INCOMPLETE - reflection disabled", g_wallDistance);
    }

    void Shutdown()
    {
        g_enabled.store(false, std::memory_order_release);
    }

    void SetMask(const unsigned char*, int, int) {}

    void StampMagic(unsigned char* rgbaRow0) { std::memcpy(rgbaRow0, kMagic, sizeof(kMagic)); }

    void Publish(const Item* items, int count, float timeSeconds, const Env& env)
    {
        const int next = 1 - g_snapIndex.load(std::memory_order_relaxed);
        Snapshot& snapshot = g_snap[next];
        snapshot.count = count > MAX_ITEMS ? MAX_ITEMS : count;
        for (int i = 0; i < snapshot.count; ++i) snapshot.items[i] = items[i];
        snapshot.time = timeSeconds;
        snapshot.env = env;
        g_snapIndex.store(next, std::memory_order_release);
    }

    bool IsActive()
    {
        return g_enabled.load(std::memory_order_acquire) &&
            g_frames.load(std::memory_order_relaxed) - g_lastPassFrame.load(std::memory_order_relaxed) < 60;
    }
}
