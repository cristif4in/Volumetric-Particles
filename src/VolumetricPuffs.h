/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/VolumetricPuffs.h
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#pragma once

#include "RenderWare.h"

class CVehicle;

namespace vp
{
    namespace VolumetricPuffs
    {
        struct Puff
        {
            CVector pos;
            CVector vel;
            float   age;
            float   lifetime;
            float   radius;      // current puff radius (meters)
            float   growth;      // radius growth (m/s)
            float   alpha;       // base opacity at spawn (already scaled by density/emission)
            float   brightness;  // road brightness passed by the game (lightMult)
            uint32  seed;        // random seed for the per-puff noise rotation
            bool    active;
        };

        constexpr int MAX_PUFFS = 320;

        void Init();
        void Shutdown();

        // Called from the hooked vanilla AddParticle - converts one vanilla
        // tyre-smoke particle into one volumetric puff.
        void SpawnFromTyre(const CVector& pos, const CVector& vanillaVel, const FxPrtMult_c& mults,
                           float lightMult, const CVector& carVelocity, CVehicle* veh);

        // Simulation step, called once per frame.
        void Update(float dt);

        Puff* GetPuffs();
        int   GetPuffCount();

        float GetAmbientLight(); // current sun/ambient tint strength 0..1
    }
}
