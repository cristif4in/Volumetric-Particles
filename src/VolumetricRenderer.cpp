/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/VolumetricRenderer.cpp
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

        void* GetNoiseRaster()
        {
            return s_noiseRaster;
        }

        void Render()
        {
            if (!g_cfg.bEnabled || !s_noiseRaster)
                return;

            // don't draw anything while the game is paused / in menus -
            // the radar and pause menu are drawn right after drawingEvent
            // and inherit whatever render states we leave behind
            if (CTimer::m_UserPause || CTimer::m_CodePause)
                return;

            const auto* puffs = VolumetricPuffs::GetPuffs();
            const int   count = VolumetricPuffs::GetPuffCount();

            RwCamera* cam = TheCamera.m_pRwCamera; // actually Scene camera; use RwCameraGetCurrent
            if (!cam)
                return;

            // camera basis - slices are perpendicular to the view direction
            const RwMatrix& camMat = cam->viewMatrix;
            CVector camFwd(camMat.at.x, camMat.at.y, camMat.at.z);    // view 'at' axis
            CVector camRight(camMat.right.x, camMat.right.y, camMat.right.z);
            CVector camUp(camMat.up.x, camMat.up.y, camMat.up.z);
            camFwd.Normalize();
            camRight.Normalize();
            camUp.Normalize();

            // the camera's WORLD position - viewMatrix.pos is the negated
            // view transform (its magnitude is the distance from the world
            // origin), using it for the distance cull culled every puff
            const RwMatrix* camWorldMat = RwFrameGetLTM(RwCameraGetFrame(cam));
            const CVector camPos(camWorldMat->pos.x, camWorldMat->pos.y, camWorldMat->pos.z);

            // sun/ambient tint from the current timecycle (GfX/CloudWorks-style
            // lighting). master removed m_CurrentAmbientColor/m_CurrentDirectionalColor;
            // use the per-component ambient getters and the directional
            // brightness multiplier.
            const float gain = g_cfg.fLightGain;
            int tintR = std::clamp((int)(CTimeCycle::GetAmbientRed() * gain), 0, 255);
            int tintG = std::clamp((int)(CTimeCycle::GetAmbientGreen() * gain), 0, 255);
            int tintB = std::clamp((int)(CTimeCycle::GetAmbientBlue() * gain), 0, 255);

            // CloudWorks: Mie phase needs the light direction.
            // m_VectorToSun[slot] is the timecycle's vector towards the sun.
            CVector sunDir = CTimeCycle::m_VectorToSun[0];
            sunDir.Normalize();

            // ------------------------------------------------- render states
            // save EVERYTHING we are about to change: whatever is left set
            // here leaks into the HUD/radar pass right after this event
            void*  savedRaster      = nullptr;
            void*  savedSrcBlend    = nullptr;
            void*  savedDestBlend   = nullptr;
            void*  savedZWrite      = nullptr;
            void*  savedZTest       = nullptr;
            void*  savedCull        = nullptr;
            void*  savedShade       = nullptr;
            void*  savedTexFilter   = nullptr;
            void*  savedAlphaTest   = nullptr;
            void*  savedVtxAlpha    = nullptr;
            RwRenderStateGet(rwRENDERSTATETEXTURERASTER,      &savedRaster);
            RwRenderStateGet(rwRENDERSTATESRCBLEND,           &savedSrcBlend);
            RwRenderStateGet(rwRENDERSTATEDESTBLEND,          &savedDestBlend);
            RwRenderStateGet(rwRENDERSTATEZWRITEENABLE,       &savedZWrite);
            RwRenderStateGet(rwRENDERSTATEZTESTENABLE,        &savedZTest);
            RwRenderStateGet(rwRENDERSTATECULLMODE,           &savedCull);
            RwRenderStateGet(rwRENDERSTATESHADEMODE,          &savedShade);
            RwRenderStateGet(rwRENDERSTATETEXTUREFILTER,      &savedTexFilter);
            RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTION,  &savedAlphaTest);
            RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE,  &savedVtxAlpha);

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

            // verbose diagnostics: every frame with active puffs
            int activeCount = 0;
            for (int i = 0; i < count; i++)
                if (puffs[i].active)
                    activeCount++;
            static unsigned s_diagFrame = 0;
            const bool verbose = (activeCount > 0);
            if (verbose)
            {
                const unsigned f = ++s_diagFrame;
                if (f <= 60 || (f % 60) == 0)
                    Log("render f%u active=%d camPos=(%.1f,%.1f,%.1f) fwd=(%.2f,%.2f,%.2f) tint=(%d,%d,%d)",
                        f, activeCount, camPos.x, camPos.y, camPos.z,
                        camFwd.x, camFwd.y, camFwd.z, tintR, tintG, tintB);
            }
            int drawnCount = 0;
            int primFails = 0;

            for (int i = 0; i < count; i++)
            {
                const auto& p = puffs[i];
                if (!p.active)
                    continue;

                // life envelope: quick in, exponential-ish out (dissipation)
                const float t = p.age / p.lifetime;
                const float envelope = std::clamp(t * 4.0f, 0.0f, 1.0f) * std::exp(-t * 2.4f);
                const float puffAlpha = p.alpha * envelope;
                if (puffAlpha <= 0.004f)
                    continue;

                // distance cull (also cheap LOD: fewer slices far away)
                const float distSq = (p.pos - camPos).MagnitudeSqr();
                if (distSq > g_cfg.fMaxRenderDist * g_cfg.fMaxRenderDist)
                    continue;
                const int sliceCount = distSq > 40.0f * 40.0f ? std::max(3, slices / 2) : slices;

                // alpha per slice, compensated so N overlapping slices add up
                // to roughly the puff's target opacity (x1.5 gain so the
                // plume actually reads on screen; tune down if too heavy)
                const float sliceAlpha = std::min(1.0f, (1.0f - std::pow(1.0f - puffAlpha, 1.0f / (float)sliceCount)) * 1.5f);

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
                        // master's rw headers no longer expose
                        // RwIm3DVertexSetSurfaceProperties; the defaults are fine
                    }

                    vi += 4;
                }

                // draw this puff (indices cover only the used slices:
                // they are laid out for MAX_SLICES, so regenerate the tail)
                void* im3d = RwIm3DTransform(s_verts, vi, &identity, rwIM3D_VERTEXRGBA);
                RwBool primOk = FALSE;
                {
                    int ii = 0;
                    RwImVertexIndex idx[(MAX_SLICES - 1) * 6];
                    for (int s = 0; s < sliceCount - 1; s++)
                    {
                        RwImVertexIndex q = (RwImVertexIndex)(s * 4);
                        idx[ii++] = q + 0; idx[ii++] = q + 1; idx[ii++] = q + 2;
                        idx[ii++] = q + 0; idx[ii++] = q + 2; idx[ii++] = q + 3;
                    }
                    primOk = RwIm3DRenderIndexedPrimitive(rwPRIMTYPETRILIST, idx, ii);
                }
                RwIm3DEnd();
                drawnCount++;
                if (!primOk)
                    primFails++;

                if (verbose && (s_diagFrame % 60) == 1 && i == 0)
                {
                    Log("render puff0 im3d=%p primOk=%d distSq=%.0f radius=%.2f puffAlpha=%.3f sliceA=%.3f slices=%d",
                        im3d, (int)primOk, distSq, p.radius, puffAlpha, sliceAlpha, sliceCount);
                }
            }

            // ------------------------------------------------ restore states
            RwRenderStateSet(rwRENDERSTATETEXTURERASTER,      savedRaster);
            RwRenderStateSet(rwRENDERSTATESRCBLEND,           savedSrcBlend);
            RwRenderStateSet(rwRENDERSTATEDESTBLEND,          savedDestBlend);
            RwRenderStateSet(rwRENDERSTATEZWRITEENABLE,       savedZWrite);
            RwRenderStateSet(rwRENDERSTATEZTESTENABLE,        savedZTest);
            RwRenderStateSet(rwRENDERSTATECULLMODE,           savedCull);
            RwRenderStateSet(rwRENDERSTATESHADEMODE,          savedShade);
            RwRenderStateSet(rwRENDERSTATETEXTUREFILTER,      savedTexFilter);
            RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION,  savedAlphaTest);
            RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE,  savedVtxAlpha);

            if (verbose && ((s_diagFrame % 60) == 1 || (drawnCount == 0 && primFails == 0)))
            {
                // nothing drawn? dump the cull reasons for the first active puffs
                int dumped = 0;
                for (int i = 0; i < count && dumped < 3; i++)
                {
                    if (!puffs[i].active) continue;
                    const float t = puffs[i].age / puffs[i].lifetime;
                    const float env = std::clamp(t * 4.0f, 0.0f, 1.0f) * std::exp(-t * 2.4f);
                    const float dSq = (puffs[i].pos - camPos).MagnitudeSqr();
                    Log("nodraw f%u puff[%d] age=%.3f/%.2f alpha=%.3f env=%.3f puffAlpha=%.3f distSq=%.0f",
                        s_diagFrame, i, puffs[i].age, puffs[i].lifetime, puffs[i].alpha,
                        env, puffs[i].alpha * env, dSq);
                    dumped++;
                }
            }
        }
    }
}
