/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/Main.cpp
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

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
            // SA keeps the frame delta (seconds) in ms_fTimeStep; master
            // removed the GetTimeStepInSeconds() wrapper
            const float dt = CTimer::ms_fTimeStep;
            vp::VolumetricPuffs::Update(dt);
        };

        // per-frame: draw after the world has been rendered
        plugin::Events::drawingEvent += []() {
            vp::VolumetricRenderer::Render();
        };
    }
} volumetricParticlesPlugin;
