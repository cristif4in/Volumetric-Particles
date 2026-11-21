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
    // master's CVehicle.h declares AddSingleWheelParticles with
    // (tWheelState, unsigned, float, float, CColPoint*, CVector*, float, int,
    // unsigned surfaceType, bool*, unsigned) - eCarWheel no longer exists
    typedef bool(__thiscall* AddSingleWheelParticles_t)(CVehicle*, tWheelState, uint32, float, float,
                                                        CColPoint&, CVector&, float, int, bool&, uint32);
    typedef void(__thiscall* AddParticle_t)(FxSystem_c*, const CVector&, const CVector&, float,
                                            const FxPrtMult_c&, float, float, float, bool);

    static AddSingleWheelParticles_t org_AddSingleWheelParticles =
        reinterpret_cast<AddSingleWheelParticles_t>(ADDR_AddSingleWheelParticles);
    static AddParticle_t             org_AddParticle =
        reinterpret_cast<AddParticle_t>(ADDR_FxSystemAddParticle);

    // ---------------------------------------------------------- hook engine
    // plugin-sdk master's patch::RedirectJump just overwrites the function
    // entry with a jmp. Calling "the original" through that same address
    // jumps straight back into our own detour -> infinite recursion ->
    // stack overflow (this is what crashed the game ~2s after joining:
    // SA:MP spawns FX particles on connect). So the hook is removed around
    // every call to the original; these functions run on the game's main
    // thread only, so the unhook->call->rehook window is safe.
    struct AutoHook
    {
        uintptr_t addr;
        void* detour;
        uint8_t saved[5];
        bool  installed;
    };

    static AutoHook s_hookWheels      = { ADDR_AddSingleWheelParticles, nullptr, {}, false };
    static AutoHook s_hookAddParticle = { ADDR_FxSystemAddParticle,    nullptr, {}, false };

    static void ApplyHook(AutoHook& h, bool install)
    {
        DWORD old = 0;
        if (!VirtualProtect((void*)h.addr, 5, PAGE_EXECUTE_READWRITE, &old))
        {
            Log("hook: VirtualProtect failed for %08X", (unsigned)h.addr);
            return;
        }
        if (install)
        {
            memcpy(h.saved, (const void*)h.addr, sizeof(h.saved));
            uint8_t* p = (uint8_t*)h.addr;
            p[0] = 0xE9;
            int32_t rel = (int32_t)((uintptr_t)h.detour - (h.addr + 5));
            memcpy(p + 1, &rel, sizeof(rel));
        }
        else
        {
            memcpy((void*)h.addr, h.saved, sizeof(h.saved));
        }
        FlushInstructionCache(GetCurrentProcess(), (const void*)h.addr, 5);
        VirtualProtect((void*)h.addr, 5, old, &old);
        h.installed = install;
    }

    // ------------------------------------------------------------- helpers
    static bool IsLocalDrivenVehicle(CVehicle* veh)
    {
        if (!veh)
            return false;

        // The player-info lookup works in both SP and SA:MP (client side):
        // it returns the vehicle the local player is in. plugin-sdk master
        // removed FindPlayer.h, so resolve it from CWorld directly.
        CVehicle* local = CWorld::Players[CWorld::PlayerInFocus].m_pPed->m_pVehicle;
        if (veh == local)
            return true;

        // SA:MP quirk: the local player ped can be a passenger in some
        // gamemodes - the user wants volumetrics on "what you're driving",
        // so also accept the vehicle whose driver is the local ped.
        // (master removed CPed::IsLocalPlayer())
        CPed* localPed = CWorld::Players[CWorld::PlayerInFocus].m_pPed;
        CPed* driver = veh->m_pDriver;
        return driver && driver == localPed;
    }

    // ---------------------------------------------------------- detours
    static bool CallOriginalWheels(CVehicle* self,
        tWheelState wheelState, uint32 wheelStatus, float suspRatio, float speed,
        CColPoint& colPoint, CVector& pos, float outsideVector_unused,
        int wheelIndex, bool& bloodState, uint32 flags)
    {
        const bool wasInstalled = s_hookWheels.installed;
        if (wasInstalled)
            ApplyHook(s_hookWheels, false); // original code back in place
        bool result = false;
        __try
        {
            result = org_AddSingleWheelParticles(self, wheelState, wheelStatus, suspRatio, speed,
                                                 colPoint, pos, outsideVector_unused,
                                                 wheelIndex, bloodState, flags);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("!!! EXCEPTION 0x%08X inside original AddSingleWheelParticles (veh=%p wheel=%d)",
                GetExceptionCode(), (void*)self, wheelIndex);
        }
        if (wasInstalled)
            ApplyHook(s_hookWheels, true);
        return result;
    }

    static void CallOriginalAddParticle(FxSystem_c* self,
        const CVector& pos, const CVector& vel, float timeSince,
        const FxPrtMult_c& fxMults, float rotZ, float lightMult,
        float lightMultLimit, bool createLocal)
    {
        const bool wasInstalled = s_hookAddParticle.installed;
        if (wasInstalled)
            ApplyHook(s_hookAddParticle, false);
        __try
        {
            org_AddParticle(self, pos, vel, timeSince, fxMults, rotZ, lightMult, lightMultLimit, createLocal);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("!!! EXCEPTION 0x%08X inside original FxSystem::AddParticle", GetExceptionCode());
        }
        if (wasInstalled)
            ApplyHook(s_hookAddParticle, true);
    }

    // SEH wrapper - SpawnFromTyre is C++, so its __try must live here
    static void SafeSpawnFromTyre(const CVector& pos, const CVector& vanillaVel,
                                  const FxPrtMult_c& mults, float lightMult,
                                  const CVector& carVelocity, CVehicle* veh)
    {
        __try
        {
            VolumetricPuffs::SpawnFromTyre(pos, vanillaVel, mults, lightMult, carVelocity, veh);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("!!! EXCEPTION 0x%08X in SpawnFromTyre - disabling tyre interception", GetExceptionCode());
            g_currentIsLocal = false;
        }
    }

    static bool __fastcall Our_AddSingleWheelParticles(
        CVehicle* self, void* /*edx*/,
        tWheelState wheelState, uint32 wheelStatus, float suspRatio, float speed,
        CColPoint& colPoint, CVector& pos, float outsideVector_unused,
        int wheelIndex, bool& bloodState, uint32 flags)
    {
        __try
        {
            // remember context so the AddParticle detour can attribute puffs
            g_currentVehicle = self;
            g_currentIsLocal = self && g_cfg.bEnabled
                               && (!g_cfg.bLocalVehicleOnly || IsLocalDrivenVehicle(self));
            g_lastCarVelocity = self->m_vecMoveSpeed;
            static bool s_firstHit = false;
            if (!s_firstHit)
            {
                s_firstHit = true;
                Log("wheel detour first hit veh=%p local=%d wheel=%d",
                    (void*)self, (int)g_currentIsLocal, wheelIndex);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("!!! EXCEPTION 0x%08X in Our_AddSingleWheelParticles prologue (veh=%p)",
                GetExceptionCode(), (void*)self);
        }

        return CallOriginalWheels(self, wheelState, wheelStatus, suspRatio, speed,
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
                static int s_spawnLogs = 0;
                if (s_spawnLogs++ < 5)
                    Log("SpawnFromTyre #%d pos=(%.1f,%.1f,%.1f) veh=%p light=%.2f",
                        s_spawnLogs, pos.x, pos.y, pos.z, (void*)g_currentVehicle, lightMult);
                SafeSpawnFromTyre(pos, vel, fxMults, lightMult, g_lastCarVelocity, g_currentVehicle);
                return; // vanilla smoke suppressed for the local vehicle
            }
        }

        CallOriginalAddParticle(self, pos, vel, timeSince, fxMults, rotZ, lightMult, lightMultLimit, createLocal);
    }

    // ------------------------------------------------------------- install
    void InstallTyreSmokeHooks()
    {
        s_hookWheels.detour      = (void*)&Our_AddSingleWheelParticles;
        s_hookAddParticle.detour = (void*)&Our_AddParticle;

        Log("hooks: AddSingleWheelParticles @ %08X, FxSystem::AddParticle @ %08X",
            (unsigned)ADDR_AddSingleWheelParticles, (unsigned)ADDR_FxSystemAddParticle);

        // plugin-sdk patch::RedirectJump replaced with our own E9 rel32 patch
        // that keeps the original bytes saved for the unhook->call->rehook
        ApplyHook(s_hookWheels, true);
        ApplyHook(s_hookAddParticle, true);
        Log("hooks installed (wheels=%d particle=%d)", (int)s_hookWheels.installed, (int)s_hookAddParticle.installed);
    }
}
