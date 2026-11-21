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

#include <algorithm>
#include <cmath>
#include <cstdint>
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
