#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstdio>
#include <algorithm>

#include "MinHook.h"
#include "dx11_hook.h"
#include "renderer.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using PresentFn = HRESULT(STDMETHODCALLTYPE *)(
    IDXGISwapChain *, UINT, UINT);

using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE *)(
    IDXGISwapChain *,
    UINT,
    UINT,
    UINT,
    DXGI_FORMAT,
    UINT);

static scs_log_t g_log = nullptr;
static std::atomic<float> g_vignetteIntensity{0.0f};
static std::atomic<float> g_blurAmount{0.0f};
static std::atomic<float> g_shakeAmount{0.0f};
static std::atomic<float> g_shakePhase{0.0f};
static std::atomic<float> g_showerValue{100.0f};

static PresentFn g_originalPresent = nullptr;
static ResizeBuffersFn g_originalResizeBuffers = nullptr;

static void *g_presentTarget = nullptr;
static void *g_resizeBuffersTarget = nullptr;

static bool g_minhookInitialized = false;
static bool g_presentHookCreated = false;
static bool g_resizeHookCreated = false;
static bool g_presentHookEnabled = false;
static bool g_resizeHookEnabled = false;

static std::atomic_bool g_presentLogged{false};
static std::atomic_bool g_swapchainInspected{false};

static void Log(const char *message)
{
  if (g_log)
    g_log(SCS_LOG_TYPE_message, message);
}

// --------------------------------------------------
// Inspect actual ETS2 swapchain
// --------------------------------------------------

static void InspectSwapChain(IDXGISwapChain *swapChain)
{
  bool expected = false;

  if (!g_swapchainInspected.compare_exchange_strong(
          expected, true))
    return;

  if (!swapChain)
  {
    g_swapchainInspected.store(false);
    return;
  }

  DXGI_SWAP_CHAIN_DESC swapDesc{};

  HRESULT hr = swapChain->GetDesc(&swapDesc);

  if (FAILED(hr))
  {
    Log("DRXexpandTM: SWAPCHAIN DESC FAILED");
    g_swapchainInspected.store(false);
    return;
  }

  char buffer[256];

  snprintf(
      buffer,
      sizeof(buffer),
      "DRXexpandTM: SWAPCHAIN RESOLUTION %ux%u",
      swapDesc.BufferDesc.Width,
      swapDesc.BufferDesc.Height);

  Log(buffer);

  snprintf(
      buffer,
      sizeof(buffer),
      "DRXexpandTM: SWAPCHAIN FORMAT %u",
      static_cast<unsigned>(swapDesc.BufferDesc.Format));

  Log(buffer);

  snprintf(
      buffer,
      sizeof(buffer),
      "DRXexpandTM: SWAPCHAIN SAMPLES %u",
      swapDesc.SampleDesc.Count);

  Log(buffer);

  ID3D11Device *device = nullptr;

  hr = swapChain->GetDevice(
      __uuidof(ID3D11Device),
      reinterpret_cast<void **>(&device));

  if (FAILED(hr) || !device)
  {
    Log("DRXexpandTM: D3D11 DEVICE QUERY FAILED");
    return;
  }

  snprintf(
      buffer,
      sizeof(buffer),
      "DRXexpandTM: DEVICE FEATURE LEVEL 0x%X",
      static_cast<unsigned>(device->GetFeatureLevel()));

  Log(buffer);

  ID3D11Texture2D *backBuffer = nullptr;

  hr = swapChain->GetBuffer(
      0,
      __uuidof(ID3D11Texture2D),
      reinterpret_cast<void **>(&backBuffer));

  if (SUCCEEDED(hr) && backBuffer)
  {
    D3D11_TEXTURE2D_DESC textureDesc{};

    backBuffer->GetDesc(&textureDesc);

    snprintf(
        buffer,
        sizeof(buffer),
        "DRXexpandTM: BACKBUFFER %ux%u FORMAT=%u SAMPLES=%u",
        textureDesc.Width,
        textureDesc.Height,
        static_cast<unsigned>(textureDesc.Format),
        textureDesc.SampleDesc.Count);

    Log(buffer);

    snprintf(
        buffer,
        sizeof(buffer),
        "DRXexpandTM: BACKBUFFER BIND FLAGS 0x%X",
        textureDesc.BindFlags);

    Log(buffer);

    backBuffer->Release();
  }
  else
  {
    Log("DRXexpandTM: BACKBUFFER ACQUISITION FAILED");
    g_swapchainInspected.store(false);
  }

  device->Release();
}

// --------------------------------------------------
// Present hook
// --------------------------------------------------

static HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain *swapChain,
    UINT syncInterval,
    UINT flags)
{
  if (!g_presentLogged.exchange(true))
    Log("DRXexpandTM: PRESENT CALLED");

  InspectSwapChain(swapChain);
  Renderer_Render(
      swapChain,
      g_vignetteIntensity.load(std::memory_order_relaxed),
      g_blurAmount.load(std::memory_order_relaxed),
      g_shakeAmount.load(std::memory_order_relaxed),
      g_shakePhase.load(std::memory_order_relaxed),
      g_showerValue.load(std::memory_order_relaxed));

  return g_originalPresent(
      swapChain,
      syncInterval,
      flags);
}

// --------------------------------------------------
// ResizeBuffers hook
// --------------------------------------------------

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain *swapChain,
    UINT bufferCount,
    UINT width,
    UINT height,
    DXGI_FORMAT newFormat,
    UINT swapChainFlags)
{
  char buffer[160];

  snprintf(
      buffer,
      sizeof(buffer),
      "DRXexpandTM: RESIZE REQUEST %ux%u",
      width,
      height);

  Log(buffer);
  Renderer_Release();
  HRESULT hr = g_originalResizeBuffers(
      swapChain,
      bufferCount,
      width,
      height,
      newFormat,
      swapChainFlags);

  if (SUCCEEDED(hr))
  {
    Log("DRXexpandTM: RESIZE BUFFERS SUCCESS");

    // Inspeksi ulang pada Present berikutnya.
    g_swapchainInspected.store(false);
  }
  else
  {
    Log("DRXexpandTM: RESIZE BUFFERS FAILED");
  }

  return hr;
}

// --------------------------------------------------
// Obtain DXGI method addresses
// --------------------------------------------------

static bool GetSwapChainMethods(
    void **presentResult,
    void **resizeResult)
{
  const wchar_t *className =
      L"DRXexpandTM_DX11_Window";

  HINSTANCE instance = GetModuleHandleW(nullptr);

  WNDCLASSEXW wc{};

  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = className;

  ATOM atom = RegisterClassExW(&wc);

  if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return false;

  HWND hwnd = CreateWindowExW(
      0,
      className,
      L"DRXexpandTM",
      WS_OVERLAPPEDWINDOW,
      0, 0, 100, 100,
      nullptr,
      nullptr,
      instance,
      nullptr);

  if (!hwnd)
  {
    if (atom)
      UnregisterClassW(className, instance);

    return false;
  }

  DXGI_SWAP_CHAIN_DESC desc{};

  desc.BufferDesc.Width = 2;
  desc.BufferDesc.Height = 2;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BufferDesc.RefreshRate.Numerator = 60;
  desc.BufferDesc.RefreshRate.Denominator = 1;

  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 1;
  desc.OutputWindow = hwnd;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  const D3D_FEATURE_LEVEL levels[] =
      {
          D3D_FEATURE_LEVEL_11_0,
          D3D_FEATURE_LEVEL_10_1,
          D3D_FEATURE_LEVEL_10_0};

  IDXGISwapChain *swapChain = nullptr;
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;

  D3D_FEATURE_LEVEL featureLevel{};

  HRESULT hr = D3D11CreateDeviceAndSwapChain(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      0,
      levels,
      ARRAYSIZE(levels),
      D3D11_SDK_VERSION,
      &desc,
      &swapChain,
      &device,
      &featureLevel,
      &context);

  bool success = false;

  if (SUCCEEDED(hr) && swapChain)
  {
    void **vtable =
        *reinterpret_cast<void ***>(swapChain);

    // IDXGISwapChain::Present = slot 8
    // IDXGISwapChain::ResizeBuffers = slot 13

    *presentResult = vtable[8];
    *resizeResult = vtable[13];

    success =
        *presentResult != nullptr &&
        *resizeResult != nullptr;
  }

  if (context)
    context->Release();
  if (device)
    device->Release();
  if (swapChain)
    swapChain->Release();

  DestroyWindow(hwnd);

  if (atom)
    UnregisterClassW(className, instance);

  return success;
}

// --------------------------------------------------
// Install hooks
// --------------------------------------------------

bool DX11Hook_Install(scs_log_t log)
{
  g_log = log;
  Renderer_SetLogger(log);
  Log("DRXexpandTM: INITIALIZING DX11 HOOK");

  MH_STATUS status = MH_Initialize();

  if (status != MH_OK &&
      status != MH_ERROR_ALREADY_INITIALIZED)
  {
    Log("DRXexpandTM: MINHOOK INITIALIZATION FAILED");
    return false;
  }

  g_minhookInitialized = (status == MH_OK);

  if (!GetSwapChainMethods(
          &g_presentTarget,
          &g_resizeBuffersTarget))
  {
    Log("DRXexpandTM: DX11 DEVICE CREATION FAILED");
    DX11Hook_Shutdown();
    return false;
  }

  Log("DRXexpandTM: DX11 DEVICE FOUND");

  status = MH_CreateHook(
      g_presentTarget,
      reinterpret_cast<LPVOID>(&HookedPresent),
      reinterpret_cast<LPVOID *>(&g_originalPresent));

  if (status != MH_OK)
  {
    Log("DRXexpandTM: PRESENT HOOK CREATION FAILED");
    DX11Hook_Shutdown();
    return false;
  }

  g_presentHookCreated = true;

  status = MH_CreateHook(
      g_resizeBuffersTarget,
      reinterpret_cast<LPVOID>(&HookedResizeBuffers),
      reinterpret_cast<LPVOID *>(&g_originalResizeBuffers));

  if (status != MH_OK)
  {
    Log("DRXexpandTM: RESIZE HOOK CREATION FAILED");
    DX11Hook_Shutdown();
    return false;
  }

  g_resizeHookCreated = true;

  status = MH_EnableHook(g_presentTarget);

  if (status != MH_OK)
  {
    Log("DRXexpandTM: PRESENT HOOK ENABLE FAILED");
    DX11Hook_Shutdown();
    return false;
  }

  g_presentHookEnabled = true;

  status = MH_EnableHook(g_resizeBuffersTarget);

  if (status != MH_OK)
  {
    Log("DRXexpandTM: RESIZE HOOK ENABLE FAILED");
    DX11Hook_Shutdown();
    return false;
  }

  g_resizeHookEnabled = true;

  Log("DRXexpandTM: PRESENT HOOK INSTALLED");
  Log("DRXexpandTM: RESIZE HOOK INSTALLED");

  return true;
}

// --------------------------------------------------
// Shutdown
// --------------------------------------------------

void DX11Hook_Shutdown()
{
  if (g_presentHookEnabled)
  {
    MH_DisableHook(g_presentTarget);
    g_presentHookEnabled = false;
  }

  if (g_resizeHookEnabled)
  {
    MH_DisableHook(g_resizeBuffersTarget);
    g_resizeHookEnabled = false;
  }

  if (g_presentHookCreated)
  {
    MH_RemoveHook(g_presentTarget);
    g_presentHookCreated = false;
  }

  if (g_resizeHookCreated)
  {
    MH_RemoveHook(g_resizeBuffersTarget);
    g_resizeHookCreated = false;
  }

  Renderer_Release();

  if (g_minhookInitialized)
  {
    MH_Uninitialize();
    g_minhookInitialized = false;
  }

  g_presentTarget = nullptr;
  g_resizeBuffersTarget = nullptr;

  g_originalPresent = nullptr;
  g_originalResizeBuffers = nullptr;

  Log("DRXexpandTM: DX11 HOOK SHUTDOWN");
}

void DX11Hook_SetVignetteIntensity(float intensity)
{
  intensity = std::clamp(intensity, 0.0f, 1.0f);

  g_vignetteIntensity.store(
      intensity,
      std::memory_order_relaxed);
}

void DX11Hook_SetBlurAmount(float amount)
{
  amount = std::clamp(amount, 0.0f, 1.0f);

  g_blurAmount.store(
      amount,
      std::memory_order_relaxed);
}

void DX11Hook_SetScreenShake(float amount, float phase)
{
  amount = std::clamp(amount, 0.0f, 1.0f);
  g_shakeAmount.store(amount, std::memory_order_relaxed);
  g_shakePhase.store(phase, std::memory_order_relaxed);
}

void DX11Hook_SetShowerValue(float value)
{
  value = std::clamp(value, 0.0f, 100.0f);
  g_showerValue.store(value, std::memory_order_relaxed);
}
