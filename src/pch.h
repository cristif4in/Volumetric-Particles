#pragma once
/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/pch.h
 *  
 *
 *  Credits:     cristif4in
 *               Server Redtimes Romania Project
 *
 ****************************************************************************/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>

// plugin-sdk master's Types.h only ships PascalCase aliases (UInt32 etc.);
// keep the lowercase ones this mod was written with.
using uint8  = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using int32  = std::int32_t;

// plugin-sdk (same dependency & layout as The-Definitive-UI / RedtimesUI)
#include "plugin.h"
#include "RenderWare.h"

#include "CCamera.h"
#include "CTimer.h"
#include "CWeather.h"
#include "CTimeCycle.h"
#include "CVehicle.h"
#include "CGeneral.h"
#include "CWorld.h"

namespace vp
{
    // tiny append-only log next to gta_sa.exe - the only way to see what
    // happened when the game dies without a crash handler
    inline void Log(const char* fmt, ...)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, "vp-cristi.log", "a") != 0 || !f)
            return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
}
