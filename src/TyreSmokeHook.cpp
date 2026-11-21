/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/TyreSmokeHook.cpp
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#include "pch.h"
#include "TyreSmokeHook.h"
#include "Config.h"
#include "VolumetricPuffs.h"

namespace vp
{
    // ---------------------------------------------------------------- addresses
    constexpr uintptr_t ADDR_AddSingleWheelParticles = 0x6DE880;
    constexpr uintptr_t ADDR_AddSingleWheelParticlesEnd = 0x6DFB80; // generous end bound of the fn
    constexpr uintptr_t ADDR_FxSystemAddParticle     = 0x4AA440;

    // ------------------------------------------------------------- state
    static CVehicle* g_currentVehicle     = nullptr; // vehicle processed by AddSingleWheelParticles right now
    static bool      g_currentIsLocal     = false;
    static CVector   g_lastCarVelocity;              // cached m_vecMoveSpeed for puff inheritance

    // ------------------------------------------------------------ originals
    typedef bool(__thiscall* AddSingleWheelParticles_t)(CVehicle*, tWheelState, uint32, float, float,
                                                        CColPoint&, CVector&, float, eCarWheel, bool&, uint32);
    typedef void(__thiscall* AddParticle_t)(FxSystem_c*, const CVector&, const CVector&, float,
                                            const FxPrtMult_c&, float, float, float, bool);

    static AddSingleWheelParticles_t org_AddSingleWheelParticles = nullptr;
    static AddParticle_t             org_AddParticle             = nullptr;

    // ------------------------------------------------------------- helpers
    static bool IsLocalDrivenVehicle(CVehicle* veh)
    {
        if (!veh)
            return false;

        // FindPlayerVehicle() works in both SP and SA:MP (client side):
        // it returns the ped/vehicle the local player is in.
        CVehicle* local = FindPlayerVehicle();
        if (veh == local)
            return true;

        // SA:MP quirk: the local player ped can be a passenger in some
        // gamemodes - the user wants volumetrics on "what you're driving",
        // so also accept the vehicle whose driver is the local ped.
        CPed* driver = veh->m_pDriver;
        return driver && driver->IsLocalPlayer();
    }

    // ---------------------------------------------------------- detours
    static bool __fastcall Our_AddSingleWheelParticles(
        CVehicle* self, void* /*edx*/,
        tWheelState wheelState, uint32 wheelStatus, float suspRatio, float speed,
        CColPoint& colPoint, CVector& pos, float outsideVector_unused,
        eCarWheel wheelIndex, bool& bloodState, uint32 flags)
    {
        // remember context so the AddParticle detour can attribute puffs
        g_currentVehicle = self;
        g_currentIsLocal = g_cfg.bEnabled && (!g_cfg.bLocalVehicleOnly || IsLocalDrivenVehicle(self));
        g_lastCarVelocity = self->m_vecMoveSpeed;

        return org_AddSingleWheelParticles(self, wheelState, wheelStatus, suspRatio, speed,
                                           colPoint, pos, outsideVector_unused,
                                           wheelIndex, bloodState, flags);
    }

    static void __fastcall Our_AddParticle(
        FxSystem_c* self, void* /*edx*/,
        const CVector& pos, const CVector& vel, float timeSince,
        const FxPrtMult_c& fxMults, float rotZ, float lightMult,
        float lightMultLimit, bool createLocal)
    {
        if (g_currentIsLocal)
        {
            const void* ret = _ReturnAddress();
            const uintptr_t ra = reinterpret_cast<uintptr_t>(ret);

            // Only the tyre-smoke calls of AddSingleWheelParticles get replaced;
            // exhaust/damage smoke etc. keep using the same FxSystem untouched.
            if (ra >= ADDR_AddSingleWheelParticles && ra < ADDR_AddSingleWheelParticlesEnd)
            {
                VolumetricPuffs::SpawnFromTyre(pos, vel, fxMults, lightMult, g_lastCarVelocity, g_currentVehicle);
                return; // vanilla smoke suppressed for the local vehicle
            }
        }

        org_AddParticle(self, pos, vel, timeSince, fxMults, rotZ, lightMult, lightMultLimit, createLocal);
    }

    // ------------------------------------------------------------- install
    void InstallTyreSmokeHooks()
    {
        org_AddSingleWheelParticles =
            reinterpret_cast<AddSingleWheelParticles_t>(ADDR_AddSingleWheelParticles);
        org_AddParticle =
            reinterpret_cast<AddParticle_t>(ADDR_FxSystemAddParticle);

        // plugin-sdk inline hooks (same mechanism RedtimesUI's HookManager wraps)
        plugin::patch::RedirectJump(ADDR_AddSingleWheelParticles, Our_AddSingleWheelParticles);
        plugin::patch::RedirectJump(ADDR_FxSystemAddParticle, Our_AddParticle);
    }
}
