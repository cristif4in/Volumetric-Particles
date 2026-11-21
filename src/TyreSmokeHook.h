/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/TyreSmokeHook.h
 *  PURPOSE:     Intercepts the vanilla tyre smoke emission so it can be
 *               replaced with volumetric smoke for the local vehicle only.
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#pragma once

namespace vp
{
    // Installs the detours once RenderWare is initialized.
    void InstallTyreSmokeHooks();
}
