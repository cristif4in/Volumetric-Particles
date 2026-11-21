/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/Config.cpp
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#include "pch.h"
#include "Config.h"

namespace vp
{
    Config g_cfg;

    bool Config::Load()
    {
        // plugin-sdk resolves the module path of the asi itself
        char iniPath[MAX_PATH];
        plugin::getDataFilePath(iniPath, "vp-cristi.ini");

        auto ival = [&](const char* key, int def) { return GetPrivateProfileIntA("vp", key, def, iniPath); };
        auto fval = [&](const char* key, float def) {
            char buf[64], defbuf[64];
            sprintf_s(defbuf, "%.3f", def);
            GetPrivateProfileStringA("vp", key, defbuf, buf, sizeof(buf), iniPath);
            return (float)atof(buf);
        };

        bEnabled          = ival("Enabled", bEnabled) != 0;
        bLocalVehicleOnly = ival("LocalVehicleOnly", bLocalVehicleOnly) != 0;
        fEmission         = fval("Emission", fEmission);
        fDensity          = fval("Density", fDensity);
        fDissipation      = fval("Dissipation", fDissipation);
        fSizeScale        = fval("SizeScale", fSizeScale);
        fBuoyancy         = fval("Buoyancy", fBuoyancy);
        fWindAmount       = fval("WindAmount", fWindAmount);
        fCarVelInherit    = fval("CarVelInherit", fCarVelInherit);
        nSlices           = std::clamp(ival("Slices", nSlices), 3, 10);
        fMaxRenderDist    = fval("MaxRenderDist", fMaxRenderDist);
        fLightGain        = fval("LightGain", fLightGain);

        return true;
    }
}
