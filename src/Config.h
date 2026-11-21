/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/Config.h
 *  PURPOSE:     vp-cristi.ini settings
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#pragma once

namespace vp
{
    struct Config
    {
        // master switch
        bool    bEnabled            = true;

        // only the local (driven) vehicle gets volumetric smoke;
        // other vehicles keep the stock SA tyre smoke (SA:MP friendly).
        bool    bLocalVehicleOnly   = true;

        // look / behaviour (mirrors GfX Volumetrics feel)
        float   fEmission           = 1.0f;   // how much smoke a hot tyre makes (1 = default)
        float   fDensity            = 0.7f;   // overall puff opacity
        float   fDissipation        = 0.20f;  // how fast smoke fades
        float   fSizeScale          = 1.0f;   // puff radius scale (also scales with tyre)
        float   fBuoyancy           = 0.55f;  // upward drift, m/s^2
        float   fWindAmount         = 0.6f;   // how much CWeather::Wind pushes the smoke
        float   fCarVelInherit      = 0.6f;   // 0..1 - how much of the car velocity puffs inherit

        // rendering
        int     nSlices             = 6;      // slices per puff (raymarch steps); 4..10
        float   fMaxRenderDist      = 60.0f;  // meters, beyond this no volumetric
        float   fLightGain          = 1.0f;   // sun/ambient tint gain

        bool Load(); // reads vp-cristi.ini next to the asi
    };

    extern Config g_cfg;
}
