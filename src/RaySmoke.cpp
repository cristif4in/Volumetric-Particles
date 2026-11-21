/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/RaySmoke.cpp
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#include "pch.h"
#include "RaySmoke.h"
#include "VolumetricPuffs.h"
#include "Config.h"
#include "RenderWare.h"
#include "CCamera.h"

#include <d3d9.h>
#include <cstdlib>

// ---------------------------------------------------------------------------
// device access - SA 1.0 US keeps the IDirect3DDevice9 pointer here
// (same address the gta-sa-postfx reference project uses)
// ---------------------------------------------------------------------------
#define ADDR_D3DDEVICE ((uintptr_t)0xC97C28)

namespace vp
{
    namespace RaySmoke
    {
        // ------------------------------------------------------------ state
        static IDirect3DDevice9* s_dev      = nullptr;
        static bool              s_tried    = false;
        static bool              s_ok       = false;
        static unsigned          s_failLogs = 0;

        static IDirect3DVolumeTexture9* s_noiseVol = nullptr;
        static IDirect3DVertexBuffer9*  s_cubeVB   = nullptr;
        static IDirect3DVertexShader9*  s_vs       = nullptr;
        static IDirect3DPixelShader9*   s_ps       = nullptr;
        static HMODULE                  s_d3dx     = nullptr;

        static constexpr int NOISE_DIM = 32;
        static constexpr int MAX_CUBES_PER_FRAME = 96;

        // ------------------------------------------------------------ shaders
        // VS: cube vertices arrive pre-transformed in WORLD space; we only
        // project with the game's view*proj. The object-space corner is
        // passed through for the edge fade in the PS.
        static const char* VS_SRC =
            "float4x4 gVP : register(c0);                            \n"
            "struct VSOut {                                          \n"
            "    float4 pos    : POSITION;                           \n"
            "    float3 corner : TEXCOORD0;                          \n"
            "    float3 world  : TEXCOORD1;                          \n"
            "};                                                      \n"
            "VSOut VS_Main(float3 pos : POSITION0,                   \n"
            "               float3 corner : TEXCOORD0)               \n"
            "{                                                       \n"
            "    VSOut o;                                            \n"
            "    o.pos    = mul(float4(pos, 1.0f), gVP);             \n"
            "    o.corner = corner;                                  \n"
            "    o.world  = pos;                                     \n"
            "    return o;                                           \n"
            "}                                                       \n";

        // PS: front-to-back Beer-Lambert ray march through a 3D noise
        // volume, with a soft fade at the cube boundary and a soft melt
        // into the ground plane (no harsh clipping into the road/fenders).
        // NOTE: the edge fade is computed per MARCH SAMPLE (distance from
        // the cube center), never from the surface corner - every surface
        // pixel has a corner component of +-1 and would fade to zero.
        static const char* PS_SRC =
            "sampler3D gNoise  : register(s0);                       \n"
            "float3   gCamPos  : register(c0);                       \n"
            "float4   gCenter  : register(c1); // xyz = center, w = radius \n"
            "float4   gParams  : register(c2); // x=groundY y=time z=alpha w=brightness \n"
            "float3   gTint    : register(c3);                       \n"
            "                                                      \n"
            "float4 PS_Main(float3 corner : TEXCOORD0,               \n"
            "               float3 world  : TEXCOORD1) : COLOR       \n"
            "{                                                       \n"
            "    float3 rayDir = normalize(world - gCamPos);         \n"
            "    float  stepLen = (gCenter.w * 2.0) / 12.0;          \n"
            "    float4 acc = 0;                                     \n"
            "    float3 p = world;                                   \n"
            "                                                      \n"
            "    [unroll]                                            \n"
            "    for (int i = 0; i < 12; i++)                        \n"
            "    {                                                   \n"
            "        float3 rel = (p - gCenter.xyz) / gCenter.w;     \n"
            "        float edge = max(max(abs(rel.x), abs(rel.y)), abs(rel.z)); \n"
            "        float edgeFade = saturate((1.0 - edge) / 0.3);  \n"
            "                                                      \n"
            "        float groundFade = saturate((p.y - gParams.x) / 0.35); \n"
            "                                                      \n"
            "        float3 uvw = p * 0.35 + float3(0, -gParams.y * 0.2, 0); \n"
            "        float density = tex3D(gNoise, uvw).r;           \n"
            "        density *= edgeFade * groundFade;               \n"
            "                                                      \n"
            "        float a = saturate(density * gParams.z);        \n"
            "        acc.rgb += (1.0 - acc.a) * a * gTint * gParams.w; \n"
            "        acc.a   += (1.0 - acc.a) * a;                   \n"
            "                                                      \n"
            "        if (acc.a >= 0.95) break;                       \n"
            "        p -= rayDir * stepLen;                          \n"
            "    }                                                   \n"
            "    return acc;                                         \n"
            "}                                                       \n";

        // ------------------------------------------------------------ helpers
        static void Fail(const char* what)
        {
            if (s_failLogs < 20)
            {
                Log("raysmoke: FAILED %s - falling back to IM3D renderer", what);
                s_failLogs++;
            }
        }

        typedef HRESULT(WINAPI* CompileFn)(LPCSTR, UINT, const void*, void*,
                                           LPCSTR, LPCSTR, DWORD,
                                           void**, void**, void**);

        // ID3DXBuffer: IUnknown + GetBufferPointer + GetBufferSize
        struct DxBufferVT { void *qi, *addref, *release, *getptr, *getsize; };
        typedef void*     (__stdcall *GetBufPtr_t)(void*);
        typedef UINT      (__stdcall *GetBufSize_t)(void*);
        typedef void      (__stdcall *ReleaseFn)(void*);

        static void* BufPtr(void* buf)
        {
            if (!buf) return nullptr;
            DxBufferVT* vt = *(DxBufferVT**)buf;
            return ((GetBufPtr_t)vt->getptr)(buf);
        }
        static UINT BufSize(void* buf)
        {
            if (!buf) return 0;
            DxBufferVT* vt = *(DxBufferVT**)buf;
            return ((GetBufSize_t)vt->getsize)(buf);
        }
        static void BufRelease(void* buf)
        {
            if (!buf) return;
            DxBufferVT* vt = *(DxBufferVT**)buf;
            ((ReleaseFn)vt->release)(buf);
        }

        static bool CompileShaders()
        {
            if (!s_d3dx)
            {
                s_d3dx = LoadLibraryA("d3dx9_43.dll");
                if (!s_d3dx) s_d3dx = LoadLibraryA("d3dx9_40.dll");
                if (!s_d3dx) s_d3dx = LoadLibraryA("d3dx9_36.dll");
                if (!s_d3dx)
                {
                    Fail("load d3dx9 (no redistributable found)");
                    return false;
                }
            }

            CompileFn compile =
                (CompileFn)GetProcAddress(s_d3dx, "D3DXCompileShader");
            if (!compile)
            {
                Fail("GetProcAddress D3DXCompileShader");
                return false;
            }

            // --- vertex shader ---
            void *vsBuf = nullptr, *vsErr = nullptr, *vsTab = nullptr;
            HRESULT hr = compile(VS_SRC, (UINT)strlen(VS_SRC), nullptr, nullptr,
                                 "VS_Main", "vs_3_0", 0,
                                 &vsBuf, &vsErr, &vsTab);
            if (FAILED(hr))
            {
                Log("raysmoke: VS compile error: %s",
                    vsErr ? (const char*)BufPtr(vsErr) : "unknown");
                BufRelease(vsErr);
                return false;
            }
            const DWORD* vsCode = (const DWORD*)BufPtr(vsBuf);
            UINT vsLen = BufSize(vsBuf);
            void* vsBytecode = nullptr;
            if (vsCode && vsLen)
            {
                vsBytecode = malloc(vsLen);
                memcpy(vsBytecode, vsCode, vsLen);
            }
            BufRelease(vsBuf);
            BufRelease(vsTab);
            if (!vsBytecode)
            {
                Fail("VS buffer empty");
                return false;
            }

            // --- pixel shader ---
            void *psBuf = nullptr, *psErr = nullptr, *psTab = nullptr;
            hr = compile(PS_SRC, (UINT)strlen(PS_SRC), nullptr, nullptr,
                         "PS_Main", "ps_3_0", 0,
                         &psBuf, &psErr, &psTab);
            if (FAILED(hr))
            {
                Log("raysmoke: PS compile error: %s",
                    psErr ? (const char*)BufPtr(psErr) : "unknown");
                BufRelease(psErr);
                free(vsBytecode);
                return false;
            }
            const DWORD* psCode = (const DWORD*)BufPtr(psBuf);
            UINT psLen = BufSize(psBuf);
            void* psBytecode = nullptr;
            if (psCode && psLen)
            {
                psBytecode = malloc(psLen);
                memcpy(psBytecode, psCode, psLen);
            }
            BufRelease(psBuf);
            BufRelease(psTab);
            if (!psBytecode)
            {
                Fail("PS buffer empty");
                free(vsBytecode);
                return false;
            }

            bool ok = SUCCEEDED(s_dev->CreateVertexShader((const DWORD*)vsBytecode, &s_vs)) &&
                      SUCCEEDED(s_dev->CreatePixelShader((const DWORD*)psBytecode, &s_ps));
            if (!ok)
                Fail("CreateVertexShader/CreatePixelShader");

            free(vsBytecode);
            free(psBytecode);
            return ok;
        }

        // 3D fbm value-noise, three octaves, trilinear over hashed grids
        static uint32 Hash3(int x, int y, int z)
        {
            uint32 h = (uint32)(x * 374761393u + y * 668265263u + z * 2147483647u);
            h = (h ^ (h >> 13)) * 1274126177u;
            return h ^ (h >> 16);
        }
        static float GridNoise(float x, float y, float z, int cells)
        {
            float fx = x * cells, fy = y * cells, fz = z * cells;
            int x0 = (int)fx, y0 = (int)fy, z0 = (int)fz;
            float tx = fx - x0, ty = fy - y0, tz = fz - z0;
            tx = tx * tx * (3 - 2 * tx);
            ty = ty * ty * (3 - 2 * ty);
            tz = tz * tz * (3 - 2 * tz);
            auto at = [&](int a, int b, int c) {
                return (float)((Hash3(a, b, c) >> 8) & 0xFF) / 255.0f;
            };
            float c00 = at(x0, y0, z0)         * (1 - tx) + at(x0 + 1, y0, z0)         * tx;
            float c10 = at(x0, y0 + 1, z0)     * (1 - tx) + at(x0 + 1, y0 + 1, z0)     * tx;
            float c01 = at(x0, y0, z0 + 1)     * (1 - tx) + at(x0 + 1, y0, z0 + 1)     * tx;
            float c11 = at(x0, y0 + 1, z0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1, z0 + 1) * tx;
            float c0 = c00 * (1 - ty) + c10 * ty;
            float c1 = c01 * (1 - ty) + c11 * ty;
            return c0 * (1 - tz) + c1 * tz;
        }

        static bool CreateNoiseVolume()
        {
            if (FAILED(s_dev->CreateVolumeTexture(NOISE_DIM, NOISE_DIM, NOISE_DIM, 1, 0,
                                                  D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                                  &s_noiseVol, nullptr)))
            {
                Fail("CreateVolumeTexture");
                return false;
            }

            D3DLOCKED_BOX lb;
            if (FAILED(s_noiseVol->LockBox(0, &lb, nullptr, 0)))
            {
                Fail("LockBox volume");
                return false;
            }

            uint8_t* slice = (uint8_t*)lb.pBits;
            for (int z = 0; z < NOISE_DIM; z++)
            {
                for (int y = 0; y < NOISE_DIM; y++)
                {
                    uint8_t* row = slice + (size_t)y * lb.RowPitch;
                    for (int x = 0; x < NOISE_DIM; x++)
                    {
                        float u = (float)x / NOISE_DIM, v = (float)y / NOISE_DIM, w = (float)z / NOISE_DIM;
                        // fbm: three octaves of value noise
                        float n = GridNoise(u, v, w, 3) * 0.50f
                                + GridNoise(u, v, w, 6) * 0.30f
                                + GridNoise(u, v, w, 12) * 0.20f;
                        uint8_t d = (uint8_t)(std::clamp(n, 0.0f, 1.0f) * 255.0f);
                        // A8R8G8B8: B G R A byte order - keep noise in R
                        row[x * 4 + 0] = 0;
                        row[x * 4 + 1] = 0;
                        row[x * 4 + 2] = d;
                        row[x * 4 + 3] = 0;
                    }
                }
                slice += lb.SlicePitch;
            }
            s_noiseVol->UnlockBox(0);
            return true;
        }

        struct CubeVert
        {
            float x, y, z;      // world position
            float cx, cy, cz;   // cube corner [-1..1]
        };

        // unit cube corners + 12 triangles (36 verts)
        static const float CUBE_CORNERS[8][3] = {
            {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
            {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
        };
        static const uint16 CUBE_TRIS[36] = {
            0,2,1, 0,3,2,   4,5,6, 4,6,7,   0,1,5, 0,5,4,
            3,7,6, 3,6,2,   0,4,7, 0,7,3,   1,2,6, 1,6,5
        };

        static bool CreateCubeVB()
        {
            if (FAILED(s_dev->CreateVertexBuffer(sizeof(CubeVert) * 36 * MAX_CUBES_PER_FRAME,
                                                 D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                                 0, D3DPOOL_DEFAULT, &s_cubeVB, nullptr)))
            {
                Fail("CreateVertexBuffer");
                return false;
            }
            return true;
        }

        // ------------------------------------------------------------- state
        static bool TryInit()
        {
            s_tried = true;
            s_dev = *(IDirect3DDevice9**)ADDR_D3DDEVICE;
            if (!s_dev)
            {
                Fail("device pointer (0xC97C28) not set yet");
                return false;
            }

            if (!CompileShaders())  return false;
            if (!CreateNoiseVolume()) return false;
            if (!CreateCubeVB())    return false;

            s_ok = true;
            Log("raysmoke: D3D9 volumetric renderer initialized (noise=%d^3, shaders ok)", NOISE_DIM);
            return true;
        }

        static void Release()
        {
            if (s_noiseVol) { s_noiseVol->Release(); s_noiseVol = nullptr; }
            if (s_cubeVB)   { s_cubeVB->Release();   s_cubeVB   = nullptr; }
            if (s_vs)       { s_vs->Release();       s_vs       = nullptr; }
            if (s_ps)       { s_ps->Release();       s_ps       = nullptr; }
            s_ok = false;
            s_tried = false; // allow retry after device reset
        }

        // 4x4 multiply, row-vector convention (v * A * B == v * C)
        static void MatMul(const float a[16], const float b[16], float out[16])
        {
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                {
                    float s = 0;
                    for (int k = 0; k < 4; k++)
                        s += a[i * 4 + k] * b[k * 4 + j];
                    out[i * 4 + j] = s;
                }
        }

        // ------------------------------------------------------------- render
        bool Render()
        {
            if (!s_tried && !TryInit())
                return false;
            if (!s_ok || !g_cfg.bEnabled)
                return false;

            if (!s_dev)
            {
                s_dev = *(IDirect3DDevice9**)ADDR_D3DDEVICE;
                if (!s_dev) return false;
            }

            const VolumetricPuffs::Puff* puffs = VolumetricPuffs::GetPuffs();
            const int   count = VolumetricPuffs::GetPuffCount();

            int active = 0;
            for (int i = 0; i < count && active < MAX_CUBES_PER_FRAME; i++)
                if (puffs[i].active)
                    active++;
            if (!active)
                return false;

            // camera position (world) for the ray origin
            RwCamera* cam = TheCamera.m_pRwCamera;
            if (!cam)
                return false;
            const RwMatrix* camWorld = RwFrameGetLTM(RwCameraGetFrame(cam));
            const float camX = camWorld->pos.x, camY = camWorld->pos.y, camZ = camWorld->pos.z;

            // grab the game's view & projection transforms (set by the RW
            // pipeline for the active camera at this point in the frame)
            D3DMATRIX view, proj;
            if (FAILED(s_dev->GetTransform(D3DTS_VIEW, &view)) ||
                FAILED(s_dev->GetTransform(D3DTS_PROJECTION, &proj)))
            {
                Fail("GetTransform");
                return false;
            }
            float vp[16];
            MatMul((const float*)view.m, (const float*)proj.m, vp);

            // ------------------------------------------------ save states
            IDirect3DVertexShader9* oldVS = nullptr;
            IDirect3DPixelShader9*  oldPS = nullptr;
            IDirect3DBaseTexture9*  oldTex0 = nullptr;
            IDirect3DVertexBuffer9* oldVB = nullptr;
            IDirect3DIndexBuffer9*  oldIB = nullptr;
            UINT oldStreamOff = 0, oldStride = 0;
            DWORD oldFVF = 0;
            DWORD s_ZWrite = 0, s_ZTest = 0, s_AlphaBlend = 0, s_SrcBlend = 0,
                  s_DestBlend = 0, s_Cull = 0, s_Lighting = 0, s_Normalize = 0;
            DWORD sam0Min = 0, sam0Mag = 0, sam0Mip = 0, sam0AddrU = 0, sam0AddrV = 0, sam0AddrW = 0;
            s_dev->GetVertexShader(&oldVS);
            s_dev->GetPixelShader(&oldPS);
            s_dev->GetTexture(0, &oldTex0);
            s_dev->GetStreamSource(0, &oldVB, &oldStreamOff, &oldStride);
            s_dev->GetIndices(&oldIB, &oldIBCount);
            s_dev->GetFVF(&oldFVF);
            s_dev->GetSamplerState(0, D3DSAMP_MINFILTER, &sam0Min);
            s_dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &sam0Mag);
            s_dev->GetSamplerState(0, D3DSAMP_MIPFILTER, &sam0Mip);
            s_dev->GetSamplerState(0, D3DSAMP_ADDRESSU,  &sam0AddrU);
            s_dev->GetSamplerState(0, D3DSAMP_ADDRESSV,  &sam0AddrV);
            s_dev->GetSamplerState(0, D3DSAMP_ADDRESSW,  &sam0AddrW);
            s_dev->GetRenderState(D3DRS_ZWRITEENABLE,     &s_ZWrite);
            s_dev->GetRenderState(D3DRS_ZENABLE,          &s_ZTest);
            s_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &s_AlphaBlend);
            s_dev->GetRenderState(D3DRS_SRCBLEND,         &s_SrcBlend);
            s_dev->GetRenderState(D3DRS_DESTBLEND,        &s_DestBlend);
            s_dev->GetRenderState(D3DRS_CULLMODE,         &s_Cull);
            s_dev->GetRenderState(D3DRS_LIGHTING,         &s_Lighting);
            s_dev->GetRenderState(D3DRS_NORMALIZENORMALS, &s_Normalize);

            // ------------------------------------------------ our states
            s_dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
            s_dev->SetRenderState(D3DRS_ZENABLE,          TRUE);   // depth-test vs world
            s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            s_dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
            s_dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
            s_dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
            s_dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
            s_dev->SetVertexShader(s_vs);
            s_dev->SetPixelShader(s_ps);
            s_dev->SetFVF(0);
            // pixel shader declares gNoise at register(s0) -> device sampler 0
            s_dev->SetTexture(0, s_noiseVol);
            s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            s_dev->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);

            // PS constants: camera, per-frame time
            const float time = (float)CTimer::m_snTimeInMilliseconds * 0.001f;
            float psC0[4] = { camX, camY, camZ, 0.0f };
            float tint[4] = { 0.85f, 0.85f, 0.85f, 0.0f };
            tint[0] *= std::clamp(g_cfg.fLightGain, 0.0f, 2.0f);
            tint[1] *= std::clamp(g_cfg.fLightGain, 0.0f, 2.0f);
            tint[2] *= std::clamp(g_cfg.fLightGain, 0.0f, 2.0f);
            s_dev->SetPixelShaderConstantF(0, psC0, 1);
            s_dev->SetPixelShaderConstantF(3, tint, 1);
            // VS constant: view*proj (same for every cube this frame)
            s_dev->SetVertexShaderConstantF(0, vp, 4);
            s_dev->SetStreamSource(0, s_cubeVB, 0, sizeof(CubeVert));

            // ------------------------------------------------ draw cubes
            int drawn = 0;
            CubeVert* verts = nullptr;
            if (FAILED(s_cubeVB->Lock(0, 0, (void**)&verts, D3DLOCK_DISCARD)))
            {
                Fail("vertex buffer lock");
                return false;
            }

            for (int i = 0; i < count && drawn < MAX_CUBES_PER_FRAME; i++)
            {
                const VolumetricPuffs::Puff& p = puffs[i];
                if (!p.active)
                    continue;

                const float t = p.age / p.lifetime;
                const float env = std::clamp(t * 4.0f, 0.0f, 1.0f) * std::exp(-t * 2.4f);
                const float puffAlpha = p.alpha * env;
                if (puffAlpha <= 0.01f)
                    continue;

                const float distSq =
                    (p.pos.x - camX) * (p.pos.x - camX) +
                    (p.pos.y - camY) * (p.pos.y - camY) +
                    (p.pos.z - camZ) * (p.pos.z - camZ);
                if (distSq > g_cfg.fMaxRenderDist * g_cfg.fMaxRenderDist)
                    continue;

                // per-puff constants
                float psC1[4] = { p.pos.x, p.pos.y, p.pos.z, p.radius };
                float psC2[4] = { p.groundZ, time, puffAlpha * 2.2f, p.brightness };
                s_dev->SetPixelShaderConstantF(1, psC1, 1);
                s_dev->SetPixelShaderConstantF(2, psC2, 1);

                // world-space cube verts (pre-transformed; VS only projects),
                // each cube gets its own 36-vertex slot in the dynamic buffer
                CubeVert* out = verts + drawn * 36;
                for (int vI = 0; vI < 36; vI++)
                {
                    const float* c = CUBE_CORNERS[CUBE_TRIS[vI]];
                    out[vI].x = p.pos.x + c[0] * p.radius;
                    out[vI].y = p.pos.y + c[1] * p.radius;
                    out[vI].z = p.pos.z + c[2] * p.radius;
                    out[vI].cx = c[0]; out[vI].cy = c[1]; out[vI].cz = c[2];
                }

                s_dev->DrawPrimitive(D3DPT_TRIANGLELIST, drawn * 36, 12);
                drawn++;
            }

            s_cubeVB->Unlock();

            // ------------------------------------------------ restore states
            s_dev->SetVertexShader(oldVS);
            s_dev->SetPixelShader(oldPS);
            s_dev->SetTexture(0, oldTex0);
            s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, sam0Min);
            s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, sam0Mag);
            s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, sam0Mip);
            s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  sam0AddrU);
            s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  sam0AddrV);
            s_dev->SetSamplerState(0, D3DSAMP_ADDRESSW,  sam0AddrW);
            if (oldVB) s_dev->SetStreamSource(0, oldVB, oldStreamOff, oldStride);
            if (oldIB) s_dev->SetIndices(oldIB);
            s_dev->SetFVF(oldFVF);
            s_dev->SetRenderState(D3DRS_ZWRITEENABLE,     s_ZWrite);
            s_dev->SetRenderState(D3DRS_ZENABLE,          s_ZTest);
            s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, s_AlphaBlend);
            s_dev->SetRenderState(D3DRS_SRCBLEND,         s_SrcBlend);
            s_dev->SetRenderState(D3DRS_DESTBLEND,        s_DestBlend);
            s_dev->SetRenderState(D3DRS_CULLMODE,         s_Cull);
            s_dev->SetRenderState(D3DRS_LIGHTING,         s_Lighting);
            s_dev->SetRenderState(D3DRS_NORMALIZENORMALS, s_Normalize);

            if (oldTex0) oldTex0->Release();
            if (oldVB)   oldVB->Release();
            if (oldIB)   oldIB->Release();

            return drawn > 0;
        }

        void Shutdown()
        {
            Release();
            s_tried = false;
        }
    }
}
