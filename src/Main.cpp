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

// SEH wrapper - __try cannot live in a function with unwinding C++ objects,
// and the event lambdas would trigger C2712. Keeps the game alive when the
// renderer throws an access violation and writes WHERE it happened.
static void SafeRender()
{
    __try
    {
        vp::VolumetricRenderer::Render();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vp::Log("!!! EXCEPTION 0x%08X in VolumetricRenderer::Render - disabling mod for this session", GetExceptionCode());
        vp::g_cfg.bEnabled = false; // don't crash the game every frame
    }
}

static void SafeUpdate(float dt)
{
    __try
    {
        vp::VolumetricPuffs::Update(dt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vp::Log("!!! EXCEPTION 0x%08X in VolumetricPuffs::Update - disabling mod for this session", GetExceptionCode());
        vp::g_cfg.bEnabled = false;
    }
}

class VolumetricParticles
{
public:
    VolumetricParticles()
    {
        vp::Log("=== vp-cristi loaded, waiting for RW init ===");

        // RW initialized - safe to read config, install hooks, create rasters
        plugin::Events::initRwEvent += []() {
            vp::Log("initRwEvent: loading config, building raster, installing hooks");
            vp::g_cfg.Load();
            vp::Log("config loaded (Enabled=%d Emission=%.2f Density=%.2f Slices=%d)",
                    (int)vp::g_cfg.bEnabled, vp::g_cfg.fEmission, vp::g_cfg.fDensity, vp::g_cfg.nSlices);
            vp::VolumetricRenderer::Init();
            vp::Log("renderer init done (raster=%p)", (void*)vp::VolumetricRenderer::GetNoiseRaster());
            vp::VolumetricPuffs::Init();
            vp::Log("puffs init done");
            vp::InstallTyreSmokeHooks();
            vp::Log("tyre smoke hooks installed");
        };

        // RW shutdown - release what we own
        plugin::Events::shutdownRwEvent += []() {
            vp::Log("shutdownRwEvent: releasing renderer + puffs");
            vp::VolumetricRenderer::Shutdown();
            vp::VolumetricPuffs::Shutdown();
        };

        // D3D device lost/restored - the noise raster must be rebuilt
        plugin::Events::d3dLostEvent += []() {
            vp::Log("d3dLostEvent: releasing raster");
            vp::VolumetricRenderer::Shutdown();
        };
        plugin::Events::d3dResetEvent += []() {
            vp::Log("d3dResetEvent: rebuilding raster");
            vp::VolumetricRenderer::Init();
        };

        // per-frame: simulate the puffs with the game's timestep
        plugin::Events::gameProcessEvent += []() {
            // SA keeps the frame delta (seconds) in ms_fTimeStep; master
            // removed the GetTimeStepInSeconds() wrapper
            const float dt = CTimer::ms_fTimeStep;
            static int s_tick = 0;
            const int n = ++s_tick;
            // log every frame for the first 100, then every 500th
            if (n <= 100 || (n % 500) == 0)
                vp::Log("tick %d dt=%.5f UserPause=%d CodePause=%d",
                        n, dt, (int)CTimer::m_UserPause, (int)CTimer::m_CodePause);
            SafeUpdate(dt);
        };

        // per-frame: draw after the world has been rendered
        plugin::Events::drawingEvent += []() {
            static int s_draw = 0;
            const int n = ++s_draw;
            if (n <= 100 || (n % 500) == 0)
                vp::Log("drawingEvent %d (cam=%p)", n, (void*)TheCamera.m_pRwCamera);
            SafeRender();
        };
    }
} volumetricParticlesPlugin;
