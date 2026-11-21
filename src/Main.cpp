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
#include "RaySmoke.h"

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
            vp::RaySmoke::Shutdown();
        };

        // D3D device lost/restored - the noise raster + D3D resources must be rebuilt
        plugin::Events::d3dLostEvent += []() {
            vp::Log("d3dLostEvent: releasing raster + D3D resources");
            vp::VolumetricRenderer::Shutdown();
            vp::RaySmoke::Shutdown();
        };
        plugin::Events::d3dResetEvent += []() {
            vp::Log("d3dResetEvent: rebuilding raster");
            vp::VolumetricRenderer::Init();
        };

        // per-frame: simulate in drawingEvent.
        // NOTE: plugin-sdk master's gameProcessEvent does NOT fire under
        // SA:MP (confirmed via logs - zero ticks while drawingEvent fires
        // every frame).
        plugin::Events::drawingEvent += []() {
            static int s_draw = 0;
            const int n = ++s_draw;
            // SA's CTimer::ms_fTimeStep is in 1/50s ticks (1.0 at 50fps),
            // not seconds - confirmed by the tick-rate logs
            const float dt = CTimer::ms_fTimeStep / 50.0f;
            if (n <= 100 || (n % 500) == 0)
                vp::Log("drawingEvent %d dt=%.5f (cam=%p)",
                        n, dt, (void*)TheCamera.m_pRwCamera);
            SafeUpdate(dt);
        };

        // draw INSIDE the scene render: drawingEvent fires in the HUD phase,
        // after the camera update has ended, and IM3D vertices projected with
        // no active camera produce zero pixels (confirmed: primOk=1 but
        // nothing on screen). renderEffectsEvent fires at the RenderEffects
        // call inside the camera update - exactly where world effects belong.
        plugin::Events::renderEffectsEvent += []() {
            // ray-marched D3D9 volumes first; IM3D slices as fallback
            if (!vp::RaySmoke::Render())
                SafeRender();
        };
    }
} volumetricParticlesPlugin;
