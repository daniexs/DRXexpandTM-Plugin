#pragma once

#include <dxgi.h>
#include "scssdk_telemetry.h"

void Renderer_SetLogger(scs_log_t log);

void Renderer_Render(
    IDXGISwapChain *swapChain,
    float intensity,
    float blurAmount);

void Renderer_Release();
