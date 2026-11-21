#pragma once
/****************************************************************************
 *
 *  PROJECT:     Volumetric-Particles (vp-cristi)
 *  FILE:        src/pch.h
 *  PURPOSE:     Precompiled header / global includes
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

// plugin-sdk (same dependency & layout as The-Definitive-UI / RedtimesUI)
#include "plugin.h"
#include "RenderWare.h"

#include "CCamera.h"
#include "CTimer.h"
#include "CWeather.h"
#include "CTimeCycle.h"
#include "CVehicle.h"
#include "CGeneral.h"
#include "FindPlayer.h"
