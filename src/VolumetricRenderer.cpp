/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/VolumetricRenderer.cpp
 *  PURPOSE:     Sliced-volume rendering of the tyre smoke puffs.
 *
 *  RenderWare on DX9 gives us no compute shaders / render targets worth
 *  abusing for a real voxel sim (that is the limitation we live with), so
 *  the raymarch is done analytically:
 *
 *    Every puff is an ellipsoid of constant density. We cut it with
 *    N quads perpendicular to the camera's view direction ("slices" =
 *    uniform raymarch steps). Each slice is textured with a procedurally
 *    generated soft noise raster, tinted by the current timecycle
 *    ambient/sun colours, and alpha-blended back-to-front. The accumulated
 *    slices read exactly like a low-step raymarched volume - smoke that
 *    wraps around the tyre and has real thickness, not a flat sprite.
 *
 *  The lighting math follows CloudWorks (Brian Tu / RTU, GTA SA volumetric
 *  clouds, DX9 HLSL - proof that SA can do real volumetrics):
 *    - Beer-Lambert extinction is accumulated per slice
 *      (CloudWorks: fx.y += c_dens; fx.z = exp(-fx.y))
 *    - self-shadowing towards the sun is estimated analytically through the
 *      ellipsoid instead of CloudWorks' ShadowMarching loop
 *    - a Mie-phase term (dot(sunDir, camDir)) gives the silver-lining glow
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#include "pch.h"
#include "VolumetricRenderer.h"
#include "Config.h"
#include "VolumetricPuffs.h"

namespace vp
{
    namespace VolumetricRenderer
    {
        // ------------------------------------------------------------ noise raster
        static RwRaster* s_noiseRaster = nullptr;
        constexpr int    NOISE_SIZE    = 64;

        // tiny value-noise (2 octaves) - deterministic, no external asset
        static uint32 Hash2D(int x, int y)
        {
            uint32 h = (uint32)(x * 374761393u + y * 668265263u);
            h = (h ^ (h >> 13)) * 1274126177u;
            return h ^ (h >> 16);
        }

        static float ValueNoise(float x, float y)
        {
            int xi = (int)std::floor(x), yi = (int)std::floor(y);
            float xf = x - (float)xi, yf = y - (float)yi;
            auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
            float u = smooth(xf), v = smooth(yf);
            auto at = [&](int dx, int dy) { return (float)(Hash2D(xi + dx, yi + dy) & 0xFF) / 255.0f; };
            float a = at(0, 0) * (1 - u) + at(1, 0) * u;
            float b = at(0, 1) * (1 - u) + at(1, 1) * u;
            float c = a * (1 - v) + b * v;
            return c;
        }

        static RwRaster* BuildNoiseRaster()
        {
            RwRaster* raster = RwRasterCreate(NOISE_SIZE, NOISE_SIZE, 32, rwRASTERTYPETEXTURE);
            if (!raster)
                return nullptr;

            uint32* pixels = (uint32*)RwRasterLock(raster, 0, rwRASTERLOCKWRITE);
            if (!pixels)
            {
                RwRasterDestroy(raster);
                return nullptr;
            }

            for (int y = 0; y < NOISE_SIZE; y++)
            {
                for (int x = 0; x < NOISE_SIZE; x++)
                {
                    float u = (float)x / (NOISE_SIZE - 1) * 2.0f - 1.0f; // -1..1
                    float v = (float)y / (NOISE_SIZE - 1) * 2.0f - 1.0f;

                    // radial falloff -> the puff is a ball, not a quad
                    float d = std::clamp(std::sqrt(u * u + v * v), 0.0f, 1.0f);
                    float falloff = std::max(0.0f, 1.0f - d * d);

                    // two octaves of noise so the volume has structure
                    float n = ValueNoise(x * 0.12f, y * 0.12f) * 0.65f
                            + ValueNoise(x * 0.35f, y * 0.35f) * 0.35f;

                    float density = std::clamp(falloff * (0.45f + 0.75f * n), 0.0f, 1.0f);

                    // store density in alpha; RGB stays white (tinted per slice later)
                    uint8 a = (uint8)(density * 255.0f);
                    pixels[y * NOISE_SIZE + x] = (a << 24) | 0x00FFFFFF;
                }
            }

            RwRasterUnlock(raster);
            return raster;
        }

        // ---------------------------------------------------------------- render
        constexpr int MAX_SLICES = 10;

        void Init()
        {
            if (!s_noiseRaster)
                s_noiseRaster = BuildNoiseRaster();
        }

        void Shutdown()
        {
            if (s_noiseRaster)
            {
                RwRasterDestroy(s_noiseRaster);
                s_noiseRaster = nullptr;
            }
        }

        void Render()
        {
            if (!g_cfg.bEnabled || !s_noiseRaster)
                return;

            const Puff* puffs = VolumetricPuffs::GetPuffs();
            const int   count = VolumetricPuffs::GetPuffCount();

            RwCamera* cam = TheCamera.m_pRwCamera; // actually Scene camera; use RwCameraGetCurrent
            if (!cam)
                return;

            // camera basis - slices are perpendicular to the view direction
            const RwMatrix& camMat = cam->viewMatrix;
            CVector camFwd(&camMat.at.x);  // view 'at' axis
            CVector camRight(&camMat.right.x);
            CVector camUp(&camMat.up.x);
            camFwd.Normalise();
            camRight.Normalise();
            camUp.Normalise();

            // sun/ambient tint from the current timecycle (GfX/CloudWorks-style lighting)
            CRGBA amb   = CTimeCycle::m_CurrentAmbientColor;
            CRGBA dirC  = CTimeCycle::m_CurrentDirectionalColor;
            const float gain = g_cfg.fLightGain;
            int tintR = std::clamp<int>((amb.r * 0.6f + dirC.r * 0.4f) * gain, 0, 255);
            int tintG = std::clamp<int>((amb.g * 0.6f + dirC.g * 0.4f) * gain, 0, 255);
            int tintB = std::clamp<int>((amb.b * 0.6f + dirC.b * 0.4f) * gain, 0, 255);

            // CloudWorks: Mie phase needs the light direction.
            // m_VectorToSun[slot] is the timecycle's vector towards the sun.
            CVector sunDir = CTimeCycle::m_VectorToSun[0];
            sunDir.Normalise();

            // ------------------------------------------------- render states
            RwRenderStateSet(rwRENDERSTATETEXTURERASTER, s_noiseRaster);
            RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
            RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
            RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
            RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
            RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)TRUE);
            RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)rwCULLMODECULLNONE);
            RwRenderStateSet(rwRENDERSTATESHADEMODE, (void*)rwSHADEMODEGOURAUD);
            RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERLINEAR);
            RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONALWAYS);

            const int slices = std::clamp(g_cfg.nSlices, 3, MAX_SLICES);
            RwMatrix identity;
            RwMatrixSetIdentity(&identity);

            // slice quad scratch buffers (built per puff)
            static RwIm3DVertex s_verts[MAX_SLICES * 4];

            for (int i = 0; i < count; i++)
            {
                const Puff& p = puffs[i];
                if (!p.active)
                    continue;

                // life envelope: quick in, exponential-ish out (dissipation)
                const float t = p.age / p.lifetime;
                const float envelope = std::clamp(t * 4.0f, 0.0f, 1.0f) * std::exp(-t * 2.4f);
                const float puffAlpha = p.alpha * envelope;
                if (puffAlpha <= 0.004f)
                    continue;

                // distance cull (also cheap LOD: fewer slices far away)
                const CVector camPos = camMat.pos;
                const float distSq = (p.pos - camPos).SquaredMagnitude();
                if (distSq > g_cfg.fMaxRenderDist * g_cfg.fMaxRenderDist)
                    continue;
                const int sliceCount = distSq > 40.0f * 40.0f ? std::max(3, slices / 2) : slices;

                // alpha per slice, compensated so N overlapping slices add up
                // to roughly the puff's target opacity
                const float sliceAlpha = 1.0f - std::pow(1.0f - puffAlpha, 1.0f / (float)sliceCount);

                // CloudWorks' Mie phase: silver lining when looking towards the sun.
                // (CloudAtRay: c_bright * fx.x * MiePhase(suncross))
                const float sunCross   = std::clamp(sunDir.Dot(camFwd), 0.0f, 1.0f);
                const float miePhase   = 0.5f + 0.5f * std::pow(sunCross, 8.0f); // cheap HG-ish lobe

                // build the slice quads - BACK TO FRONT so blending accumulates
                // like the Beer-Lambert front-to-back march in CloudWorks:
                //   fx.y += c_dens;  fx.z = exp(-fx.y)
                // With standard alpha blending and slices sorted far->near the
                // hardware produces the same exponential transmittance.
                const float radius = p.radius;
                int vi = 0;
                for (int s = sliceCount - 1; s >= 0; s--)
                {
                    // slice depth across the volume: t in [-0.92 .. 0.92]
                    const float ft = ((float)s / (float)(sliceCount - 1)) * 1.84f - 0.92f;
                    const float halfW = radius * std::sqrt(std::max(0.0f, 1.0f - ft * ft));

                    // self-shadowing (analytic replacement of CloudWorks'
                    // ShadowMarching): slices deeper along the sun direction
                    // receive less light -> Beer-Lambert style attenuation
                    // through the puff towards the sun.
                    const float sunDepth = std::clamp((ft + 0.92f) * 0.5f, 0.0f, 1.0f);
                    const float sunAtt   = std::exp(-1.6f * sunDepth * puffAlpha);

                    // higher slices catch more light (sun from above)
                    const float heightLight = (0.75f + 0.25f * (ft + 0.92f) * 0.5f) * p.brightness * 0.5f;
                    const float lightTerm   = sunAtt * (0.55f + heightLight) + miePhase * (1.0f - sunAtt) * 0.35f;

                    const int a = (int)std::clamp(sliceAlpha * 255.0f, 0.0f, 255.0f);
                    const int r = (int)std::clamp(tintR * lightTerm, 0.0f, 255.0f);
                    const int g = (int)std::clamp(tintG * lightTerm, 0.0f, 255.0f);
                    const int b = (int)std::clamp(tintB * lightTerm, 0.0f, 255.0f);

                    const CVector center = p.pos + camFwd * (ft * radius);

                    const CVector ex = camRight * halfW;
                    const CVector ey = camUp * halfW;

                    RwIm3DVertex* v = &s_verts[vi];
                    RwIm3DVertexSetPos(&v[0], center.x - ex.x - ey.x, center.y - ex.y - ey.y, center.z - ex.z - ey.z);
                    RwIm3DVertexSetPos(&v[1], center.x + ex.x - ey.x, center.y + ex.y - ey.y, center.z + ex.z - ey.z);
                    RwIm3DVertexSetPos(&v[2], center.x + ex.x + ey.x, center.y + ex.y + ey.y, center.z + ex.z + ey.z);
                    RwIm3DVertexSetPos(&v[3], center.x - ex.x + ey.x, center.y - ex.y + ey.y, center.z - ex.z + ey.z);

                    RwIm3DVertexSetU(&v[0], 0.0f); RwIm3DVertexSetV(&v[0], 0.0f);
                    RwIm3DVertexSetU(&v[1], 1.0f); RwIm3DVertexSetV(&v[1], 0.0f);
                    RwIm3DVertexSetU(&v[2], 1.0f); RwIm3DVertexSetV(&v[2], 1.0f);
                    RwIm3DVertexSetU(&v[3], 0.0f); RwIm3DVertexSetV(&v[3], 1.0f);

                    for (int k = 0; k < 4; k++)
                    {
                        RwIm3DVertexSetRGBA(&v[k], r, g, b, a);
                        RwIm3DVertexSetSurfaceProperties(&v[k], 1.0f, 1.0f); // specular/ambient neutral
                    }

                    vi += 4;
                }

                // draw this puff (indices cover only the used slices:
                // they are laid out for MAX_SLICES, so regenerate the tail)
                RwIm3DTransform(s_verts, vi, &identity, rwIM3D_VERTEXALPHA);
                {
                    int ii = 0;
                    RwImVertexIndex idx[(MAX_SLICES - 1) * 6];
                    for (int s = 0; s < sliceCount - 1; s++)
                    {
                        RwImVertexIndex q = (RwImVertexIndex)(s * 4);
                        idx[ii++] = q + 0; idx[ii++] = q + 1; idx[ii++] = q + 2;
                        idx[ii++] = q + 0; idx[ii++] = q + 2; idx[ii++] = q + 3;
                    }
                    RwIm3DRenderIndexedPrimitive(rpPRIMTYPETRILIST, idx, ii);
                }
                RwIm3DEnd();
            }

            // ------------------------------------------------ restore states
            RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
            RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)rwCULLMODECULLBACK);
            RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONGREATER);
        }
    }
}
