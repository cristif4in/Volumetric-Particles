/*****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/Main.cpp
 *  PURPOSE:     Plugin entry point - wires the events together.
 *
 *  What this mod does:
 *      Replaces the tyre smoke of the vehicle YOU are driving with a
 *      volumetric approximation of GfX Volumetrics' tyre smoke:
 *      puff volumes injected at the slipping wheel contacts, advected by
 *      the car velocity + wind, buoyant, dissipating, rendered as
 *      CloudWorks-style lit sliced volumes.
 *
 *  Multiplayer (SA:MP):
 *      Everything is client-side. Only the locally driven vehicle's smoke
 *      is replaced; every other vehicle keeps the stock GTA SA tyre smoke,
 *      exactly as you asked (other players also see stock smoke from you,
 *      since nothing is synced).
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 *  Built the same way as RedtimesUI (The-Definitive-UI): plugin-sdk ASI
 *  plugin for GTA SA 1.0 US, hooks via plugin::patch.
 *
 *****************************************************************************/

#include "pch.h"
#include "Config.h"
#include "TyreSmokeHook.h"
#include "VolumetricPuffs.h"
#include "VolumetricRenderer.h"

class VolumetricParticles
{
public:
    VolumetricParticles()
    {
        // RW initialized - safe to read config, install hooks, create rasters
        plugin::Events::initRwEvent += []() {
            vp::g_cfg.Load();
            vp::VolumetricRenderer::Init();
            vp::VolumetricPuffs::Init();
            vp::InstallTyreSmokeHooks();
        };

        // RW shutdown - release what we own
        plugin::Events::shutdownRwEvent += []() {
            vp::VolumetricRenderer::Shutdown();
            vp::VolumetricPuffs::Shutdown();
        };

        // D3D device lost/restored - the noise raster must be rebuilt
        plugin::Events::d3dLostEvent += []() {
            vp::VolumetricRenderer::Shutdown();
        };
        plugin::Events::d3dResetEvent += []() {
            vp::VolumetricRenderer::Init();
        };

        // per-frame: simulate the puffs with the game's timestep
        plugin::Events::gameProcessEvent += []() {
            const float dt = CTimer::GetTimeStepInSeconds();
            vp::VolumetricPuffs::Update(dt);
        };

        // per-frame: draw after the world has been rendered
        plugin::Events::drawingEvent += []() {
            vp::VolumetricRenderer::Render();
        };
    }
} volumetricParticlesPlugin;
