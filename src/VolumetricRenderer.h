/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/VolumetricRenderer.h
 *  PURPOSE:     Renders the puff pool as sliced volumes (fake raymarch).
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#pragma once

namespace vp
{
    namespace VolumetricRenderer
    {
        void Init();     // builds the noise raster (after RW init)
        void Shutdown(); // destroys the raster (on RW shutdown / device reset)
        void Render();   // one call per frame from the drawing event
    }
}
