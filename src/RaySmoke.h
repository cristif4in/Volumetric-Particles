/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/RaySmoke.h
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#pragma once

namespace vp
{
    namespace RaySmoke
    {
        // Draws one ray-marched volume cube per active puff using custom
        // D3D9 SM3.0 shaders (vs_3_0 / ps_3_0, compiled at runtime through
        // the system's d3dx9_43.dll). Returns false when the D3D path is
        // not available so the caller can fall back to the IM3D renderer.
        bool Render();

        // Releases all D3D resources (device lost / shutdown)
        void Shutdown();
    }
}
