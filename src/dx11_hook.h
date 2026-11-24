
#pragma once

#include "scssdk_telemetry.h"

bool DX11Hook_Install(scs_log_t log);
void DX11Hook_Shutdown();
void DX11Hook_SetVignetteIntensity(float intensity);
void DX11Hook_SetBlurAmount(float amount);
void DX11Hook_SetScreenShake(float amount, float phase);
void DX11Hook_SetShowerValue(float value);
