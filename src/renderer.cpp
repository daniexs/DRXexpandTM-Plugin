
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <mutex>
#include <cstdio>
#include <cstring>

#include "renderer.h"
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

struct RendererResources
{
  IDXGISwapChain *swapChain = nullptr;

  ID3D11DeviceContext *immediate = nullptr;
  ID3D11DeviceContext *deferred = nullptr;

  ID3D11Texture2D *backBuffer = nullptr;
  ID3D11Texture2D *sourceTexture = nullptr;

  ID3D11RenderTargetView *renderTarget = nullptr;
  ID3D11ShaderResourceView *sourceView = nullptr;

  ID3D11VertexShader *vertexShader = nullptr;
  ID3D11PixelShader *pixelShader = nullptr;

  ID3D11SamplerState *sampler = nullptr;
  ID3D11Buffer *constantBuffer = nullptr;

  ID3D11RasterizerState *rasterizer = nullptr;
  ID3D11DepthStencilState *depthState = nullptr;

  UINT width = 0;
  UINT height = 0;
  UINT samples = 1;

  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

  bool ready = false;
};

static RendererResources g_renderer;
static std::mutex g_rendererMutex;
static scs_log_t g_rendererLog = nullptr;

static std::atomic_bool g_renderLogged{false};
static std::atomic_bool g_rendererReadyLogged{false};
static std::atomic_bool g_rendererFailedLogged{false};

void Renderer_SetLogger(scs_log_t log)
{
  g_rendererLog = log;
}

static void RendererLog(const char *message)
{
  if (g_rendererLog)
    g_rendererLog(SCS_LOG_TYPE_message, message);
}

static int g_moduleAnchor = 0;

static void ReleaseResource(ID3D11DeviceChild *&resource)
{
  if (resource)
  {
    resource->Release();
    resource = nullptr;
  }
}

template <typename T>
static void ReleaseCom(T *&resource)
{
  if (resource)
  {
    resource->Release();
    resource = nullptr;
  }
}

static void ReleaseResourcesUnlocked()
{
  ReleaseCom(g_renderer.depthState);
  ReleaseCom(g_renderer.rasterizer);
  ReleaseCom(g_renderer.constantBuffer);
  ReleaseCom(g_renderer.sampler);

  ReleaseCom(g_renderer.pixelShader);
  ReleaseCom(g_renderer.vertexShader);

  ReleaseCom(g_renderer.sourceView);
  ReleaseCom(g_renderer.renderTarget);

  ReleaseCom(g_renderer.sourceTexture);
  ReleaseCom(g_renderer.backBuffer);

  ReleaseCom(g_renderer.deferred);
  ReleaseCom(g_renderer.immediate);

  g_renderer.swapChain = nullptr;

  g_renderer.width = 0;
  g_renderer.height = 0;
  g_renderer.samples = 1;
  g_renderer.format = DXGI_FORMAT_UNKNOWN;

  g_renderer.ready = false;
}

static bool GetShaderPath(wchar_t *path, DWORD capacity)
{
  HMODULE module = nullptr;

  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&g_moduleAnchor),
          &module))
  {
    return false;
  }

  DWORD length = GetModuleFileNameW(
      module,
      path,
      capacity);

  if (length == 0 || length >= capacity)
    return false;

  wchar_t *separator = wcsrchr(path, L'\\');

  if (!separator)
    return false;

  *(separator + 1) = L'\0';

  return wcscat_s(
             path,
             capacity,
             L"shaders\\vignette.hlsl") == 0;
}

static bool CompileShader(
    const wchar_t *path,
    const char *entry,
    const char *target,
    ID3DBlob **output)
{
  ID3DBlob *errors = nullptr;

  HRESULT hr = D3DCompileFromFile(
      path,
      nullptr,
      D3D_COMPILE_STANDARD_FILE_INCLUDE,
      entry,
      target,
      D3DCOMPILE_ENABLE_STRICTNESS,
      0,
      output,
      &errors);

  if (errors)
  {
    if (FAILED(hr))
    {
      char message[1024]{};

      size_t size = errors->GetBufferSize();

      if (size >= sizeof(message))
        size = sizeof(message) - 1;

      memcpy(
          message,
          errors->GetBufferPointer(),
          size);

      message[size] = '\0';
      if (g_rendererLog)
      {
        g_rendererLog(
            SCS_LOG_TYPE_error,
            message);
      }

      OutputDebugStringA(message);
    }

    errors->Release();
  }

  return SUCCEEDED(hr);
}

static bool CreateShaders(ID3D11Device *device)
{
  wchar_t shaderPath[MAX_PATH]{};

  if (!GetShaderPath(shaderPath, MAX_PATH))
    return false;

  ID3DBlob *vsBlob = nullptr;
  ID3DBlob *psBlob = nullptr;

  bool success = false;

  if (!CompileShader(
          shaderPath,
          "VSMain",
          "vs_5_0",
          &vsBlob))
  {
    goto cleanup;
  }

  if (!CompileShader(
          shaderPath,
          "PSMain",
          "ps_5_0",
          &psBlob))
  {
    goto cleanup;
  }

  if (FAILED(device->CreateVertexShader(
          vsBlob->GetBufferPointer(),
          vsBlob->GetBufferSize(),
          nullptr,
          &g_renderer.vertexShader)))
  {
    goto cleanup;
  }

  if (FAILED(device->CreatePixelShader(
          psBlob->GetBufferPointer(),
          psBlob->GetBufferSize(),
          nullptr,
          &g_renderer.pixelShader)))
  {
    goto cleanup;
  }

  success = true;

cleanup:

  ReleaseCom(vsBlob);
  ReleaseCom(psBlob);

  return success;
}

static bool CreateResources(IDXGISwapChain *swapChain)
{
  ID3D11Device *device = nullptr;

  HRESULT hr = swapChain->GetDevice(
      __uuidof(ID3D11Device),
      reinterpret_cast<void **>(&device));

  if (FAILED(hr) || !device)
    return false;

  bool success = false;

  do
  {
    device->GetImmediateContext(
        &g_renderer.immediate);

    hr = device->CreateDeferredContext(
        0,
        &g_renderer.deferred);

    if (FAILED(hr))
      break;

    hr = swapChain->GetBuffer(
        0,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(&g_renderer.backBuffer));

    if (FAILED(hr))
      break;

    D3D11_TEXTURE2D_DESC desc{};

    g_renderer.backBuffer->GetDesc(&desc);

    if (desc.Width == 0 ||
        desc.Height == 0 ||
        desc.SampleDesc.Count == 0 ||
        !(desc.BindFlags & D3D11_BIND_RENDER_TARGET))
    {
      break;
    }

    g_renderer.width = desc.Width;
    g_renderer.height = desc.Height;
    g_renderer.samples = desc.SampleDesc.Count;
    g_renderer.format = desc.Format;

    hr = device->CreateRenderTargetView(
        g_renderer.backBuffer,
        nullptr,
        &g_renderer.renderTarget);

    if (FAILED(hr))
      break;

    // Texture perantara selalu menggunakan 1 sample.
    D3D11_TEXTURE2D_DESC sourceDesc = desc;

    sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sourceDesc.Usage = D3D11_USAGE_DEFAULT;
    sourceDesc.CPUAccessFlags = 0;
    sourceDesc.MiscFlags = 0;

    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.SampleDesc.Quality = 0;

    hr = device->CreateTexture2D(
        &sourceDesc,
        nullptr,
        &g_renderer.sourceTexture);

    if (FAILED(hr))
      break;

    hr = device->CreateShaderResourceView(
        g_renderer.sourceTexture,
        nullptr,
        &g_renderer.sourceView);

    if (FAILED(hr))
      break;

    if (!CreateShaders(device))
      break;

    // Sampler.
    D3D11_SAMPLER_DESC samplerDesc{};

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;

    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device->CreateSamplerState(
        &samplerDesc,
        &g_renderer.sampler);

    if (FAILED(hr))
      break;

    // Constant buffer: 32 bytes (two 16-byte registers).
    D3D11_BUFFER_DESC cbDesc{};

    cbDesc.ByteWidth = 32;
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    hr = device->CreateBuffer(
        &cbDesc,
        nullptr,
        &g_renderer.constantBuffer);

    if (FAILED(hr))
      break;

    // Rasterizer.
    D3D11_RASTERIZER_DESC rasterDesc{};

    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;

    rasterDesc.MultisampleEnable =
        desc.SampleDesc.Count > 1;

    hr = device->CreateRasterizerState(
        &rasterDesc,
        &g_renderer.rasterizer);

    if (FAILED(hr))
      break;

    // Depth disabled for fullscreen post-processing.
    D3D11_DEPTH_STENCIL_DESC depthDesc{};

    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depthDesc.StencilEnable = FALSE;

    hr = device->CreateDepthStencilState(
        &depthDesc,
        &g_renderer.depthState);

    if (FAILED(hr))
      break;

    g_renderer.swapChain = swapChain;
    g_renderer.ready = true;

    success = true;

  } while (false);

  device->Release();

  if (!success)
    ReleaseResourcesUnlocked();

  return success;
}

void Renderer_Render(
    IDXGISwapChain *swapChain,
    float intensity,
    float blurAmount,
    float shakeAmount,
    float shakePhase)
{
  if (!swapChain)
    return;
  if (!g_renderLogged.exchange(true))
  {
    RendererLog(
        "DRXexpandTM: VIGNETTE RENDER CALLED");
  }
  intensity = (intensity < 0.0f)
                  ? 0.0f
                  : (intensity > 1.0f ? 1.0f : intensity);
  blurAmount = (blurAmount < 0.0f)
                   ? 0.0f
                   : (blurAmount > 1.0f ? 1.0f : blurAmount);
  shakeAmount = (shakeAmount < 0.0f)
                    ? 0.0f
                    : (shakeAmount > 1.0f ? 1.0f : shakeAmount);

  if (intensity <= 0.001f && blurAmount <= 0.001f && shakeAmount <= 0.001f)
    return;

  std::lock_guard<std::mutex> lock(g_rendererMutex);

  if (!g_renderer.ready ||
      g_renderer.swapChain != swapChain)
  {
    ReleaseResourcesUnlocked();

    if (!CreateResources(swapChain))
    {
      if (!g_rendererFailedLogged.exchange(true))
      {
        RendererLog(
            "DRXexpandTM: RENDERER RESOURCE CREATION FAILED");
      }

      return;
    }

    if (!g_rendererReadyLogged.exchange(true))
    {
      char buffer[160];

      snprintf(
          buffer,
          sizeof(buffer),
          "DRXexpandTM: RENDERER READY %ux%u",
          g_renderer.width,
          g_renderer.height);

      RendererLog(buffer);
    }
  }

  ID3D11DeviceContext *deferred =
      g_renderer.deferred;

  ID3D11DeviceContext *immediate =
      g_renderer.immediate;

  if (!deferred || !immediate)
    return;

  deferred->ClearState();

  // Unbind outputs before copying the backbuffer.
  deferred->OMSetRenderTargets(
      0,
      nullptr,
      nullptr);

  if (g_renderer.samples > 1)
  {
    deferred->ResolveSubresource(
        g_renderer.sourceTexture,
        0,
        g_renderer.backBuffer,
        0,
        g_renderer.format);
  }
  else
  {
    deferred->CopyResource(
        g_renderer.sourceTexture,
        g_renderer.backBuffer);
  }

  struct EffectParams
  {
    float intensity;
    float strength;
    float aspectRatio;
    float blurAmount;
    float shakeAmount;
    float shakePhase;
    float padding0;
    float padding1;
  };

  EffectParams params{};

  params.intensity = intensity;
  params.strength = 0.80f; // actually 0.30
  params.aspectRatio =
      static_cast<float>(g_renderer.width) /
      static_cast<float>(g_renderer.height);
  params.blurAmount = blurAmount;
  params.shakeAmount = shakeAmount;
  params.shakePhase = shakePhase;

  deferred->UpdateSubresource(
      g_renderer.constantBuffer,
      0,
      nullptr,
      &params,
      0,
      0);

  ID3D11RenderTargetView *rtv =
      g_renderer.renderTarget;

  ID3D11ShaderResourceView *srv =
      g_renderer.sourceView;

  ID3D11SamplerState *sampler =
      g_renderer.sampler;

  ID3D11Buffer *constantBuffer =
      g_renderer.constantBuffer;

  D3D11_VIEWPORT viewport{};

  viewport.Width = static_cast<float>(g_renderer.width);
  viewport.Height = static_cast<float>(g_renderer.height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  deferred->RSSetViewports(1, &viewport);

  deferred->RSSetState(g_renderer.rasterizer);

  deferred->OMSetRenderTargets(
      1,
      &rtv,
      nullptr);

  deferred->OMSetDepthStencilState(
      g_renderer.depthState,
      0);

  deferred->IASetInputLayout(nullptr);

  deferred->IASetPrimitiveTopology(
      D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  deferred->VSSetShader(
      g_renderer.vertexShader,
      nullptr,
      0);

  deferred->PSSetShader(
      g_renderer.pixelShader,
      nullptr,
      0);

  deferred->GSSetShader(nullptr, nullptr, 0);
  deferred->HSSetShader(nullptr, nullptr, 0);
  deferred->DSSetShader(nullptr, nullptr, 0);

  deferred->PSSetShaderResources(
      0,
      1,
      &srv);

  deferred->PSSetSamplers(
      0,
      1,
      &sampler);

  deferred->PSSetConstantBuffers(
      0,
      1,
      &constantBuffer);

  deferred->Draw(3, 0);

  ID3D11CommandList *commandList = nullptr;

  HRESULT hr = deferred->FinishCommandList(
      FALSE,
      &commandList);

  if (SUCCEEDED(hr) && commandList)
  {
    // TRUE restores the immediate context state
    // after executing our post-processing commands.
    immediate->ExecuteCommandList(
        commandList,
        TRUE);

    commandList->Release();
  }
}

void Renderer_Release()
{
  std::lock_guard<std::mutex> lock(g_rendererMutex);

  ReleaseResourcesUnlocked();
}
