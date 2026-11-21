/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/VolumetricPuffs.cpp
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#include "pch.h"
#include "VolumetricPuffs.h"
#include "Config.h"

namespace vp
{
    namespace VolumetricPuffs
    {
        static Puff  s_puffs[MAX_PUFFS];
        static int   s_cursor = 0; // ring-buffer allocation cursor
        static uint32 s_seed  = 0x1234;

        static float FastRandom01()
        {
            // xorshift - cheap, deterministic, no C runtime state
            s_seed ^= s_seed << 13;
            s_seed ^= s_seed >> 17;
            s_seed ^= s_seed << 5;
            return (float)(s_seed & 0xFFFFFF) / (float)0xFFFFFF;
        }

        // ------------------------------------------------------------------
        void Init()
        {
            memset(s_puffs, 0, sizeof(s_puffs));
            s_cursor = 0;
        }

        void Shutdown()
        {
            Init();
        }

        // ------------------------------------------------------- spawn
        void SpawnFromTyre(const CVector& pos, const CVector& vanillaVel, const FxPrtMult_c& mults,
                           float lightMult, const CVector& carVelocity, CVehicle* veh)
        {
            if (!g_cfg.bEnabled)
                return;

            Puff& p = s_puffs[s_cursor];
            s_cursor = (s_cursor + 1) % MAX_PUFFS;

            const float emission = std::clamp(g_cfg.fEmission, 0.0f, 4.0f);

            p.active    = true;
            p.age       = 0.0f;
            // vanilla SA tyre smoke lives ~1.5s; volumetrics are allowed to
            // linger a bit longer so the "stays in the world" feel comes out
            p.lifetime  = 2.2f + FastRandom01() * 1.3f;
            p.lifetime *= (0.6f + 0.4f * mults.m_fLife);

            // radius: start tight around the tyre, expand like a real plume
            p.radius = 0.22f * g_cfg.fSizeScale * mults.m_fSize;
            p.growth = 0.9f * g_cfg.fSizeScale;

            // opacity: vanilla alpha scaled by our density + emission settings
            p.alpha      = std::clamp(mults.m_color.alpha * g_cfg.fDensity * (0.5f + 0.5f * emission), 0.0f, 1.0f);
            p.brightness = lightMult;

            // velocity: mix the vanilla kick (sideways puff) with the car's
            // velocity so the smoke stays where the car left it
            p.vel.x = vanillaVel.x + carVelocity.x * g_cfg.fCarVelInherit;
            p.vel.y = vanillaVel.y + carVelocity.y * g_cfg.fCarVelInherit;
            p.vel.z = vanillaVel.z * 0.5f + 0.35f;

            // small per-puff jitter so puffs don't line up into a wall
            p.pos.x = pos.x + (FastRandom01() - 0.5f) * 0.25f;
            p.pos.y = pos.y + (FastRandom01() - 0.5f) * 0.25f;
            p.pos.z = pos.z + 0.05f + FastRandom01() * 0.1f;

            p.seed = s_seed;
        }

        // ------------------------------------------------------- simulate
        static void Simulate(Puff& p, float dt)
        {
            const float dissipation = std::clamp(g_cfg.fDissipation, 0.0f, 1.5f);

            // drag (air resistance) - exponential decay, frame-rate independent
            const float drag = 1.35f;
            const float damp = std::exp(-drag * dt);
            p.vel.x *= damp;
            p.vel.y *= damp;
            p.vel.z *= damp;

            // buoyancy: hot tyre smoke rises (GfX-style vertical drift)
            p.vel.z += g_cfg.fBuoyancy * dt;

            // wind advection
            CVector wind = CWeather::WindDir;
            wind.Normalize();
            p.vel.x += wind.x * CWeather::Wind * g_cfg.fWindAmount * dt;
            p.vel.y += wind.y * CWeather::Wind * g_cfg.fWindAmount * dt;

            p.pos += p.vel * dt;
            p.age += dt;
            p.radius += p.growth * dt;
            p.growth *= std::exp(-0.5f * dt); // expansion slows down

            if (p.age >= p.lifetime)
                p.active = false;
        }

        void Update(float dt)
        {
            dt = std::clamp(dt, 0.0f, 0.1f); // guard against pauses/alt-tab spikes
            if (!g_cfg.bEnabled)
                return;

            // verbose: every frame with active puffs, dump the simulation state
            int active = 0;
            for (int i = 0; i < MAX_PUFFS; i++)
                if (s_puffs[i].active)
                    active++;

            for (int i = 0; i < MAX_PUFFS; i++)
            {
                Puff& p = s_puffs[i];
                if (!p.active)
                    continue;
                Simulate(p, dt);
            }

            if (active > 0)
            {
                static unsigned s_updateFrame = 0;
                const unsigned f = ++s_updateFrame;
                if (f <= 60 || (f % 60) == 0)
                {
                    // dump the first 3 active puffs after simulating
                    int dumped = 0;
                    for (int i = 0; i < MAX_PUFFS && dumped < 3; i++)
                    {
                        if (!s_puffs[i].active) continue;
                        Log("sim f%u dt=%.5f puff[%d] age=%.3f/%.2f alpha=%.3f radius=%.2f pos=(%.1f,%.1f,%.1f)",
                            f, dt, i, s_puffs[i].age, s_puffs[i].lifetime, s_puffs[i].alpha,
                            s_puffs[i].radius, s_puffs[i].pos.x, s_puffs[i].pos.y, s_puffs[i].pos.z);
                        dumped++;
                    }
                }
            }
        }

        // ------------------------------------------------------- access
        Puff* GetPuffs()             { return s_puffs; }
        int   GetPuffCount()         { return MAX_PUFFS; }

        // ------------------------------------------------------- lighting
        // Cheap "sun/sky" term: CTimeCycle ambient colour luminance,
        // so the smoke is bright at noon, dim at night, tinted at sunset.
        // master removed m_CurrentAmbientColor; use the per-component getters
        float GetAmbientLight()
        {
            float lum = (CTimeCycle::GetAmbientRed() * 0.30f
                       + CTimeCycle::GetAmbientGreen() * 0.59f
                       + CTimeCycle::GetAmbientBlue() * 0.11f) / 255.0f;
            return std::clamp(lum, 0.0f, 1.0f);
        }
    }
}
