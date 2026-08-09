// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/OpenXRD3D11Common.h"

#ifdef ENABLE_VR

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "Common/Logging/Log.h"

namespace VR::D3D11OpenXR
{
namespace
{
using PFNCreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFNCreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PFND3D11CreateDevice =
    HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                     UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

constexpr std::array<D3D_FEATURE_LEVEL, 3> SUPPORTED_FEATURE_LEVELS = {
    D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};

bool CreateDXGIFactoryDynamic(Common::DynamicLibrary* dxgi_library, IDXGIFactory1** out_factory)
{
  if (!dxgi_library->Open("dxgi.dll"))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Failed to load dxgi.dll.");
    return false;
  }

  PFNCreateDXGIFactory1 create_factory1 = nullptr;
  if (dxgi_library->GetSymbol("CreateDXGIFactory1", &create_factory1) && create_factory1)
  {
    const HRESULT hr = create_factory1(__uuidof(IDXGIFactory1),
                                       reinterpret_cast<void**>(out_factory));
    if (SUCCEEDED(hr))
      return true;
  }

  PFNCreateDXGIFactory create_factory = nullptr;
  if (dxgi_library->GetSymbol("CreateDXGIFactory", &create_factory) && create_factory)
  {
    const HRESULT hr = create_factory(__uuidof(IDXGIFactory1),
                                      reinterpret_cast<void**>(out_factory));
    if (SUCCEEDED(hr))
      return true;
  }

  ERROR_LOG_FMT(OPENXR, "OpenXR: Failed to create a DXGI factory for utility session.");
  return false;
}

bool LuidMatches(const LUID& lhs, const LUID& rhs)
{
  return lhs.HighPart == rhs.HighPart && lhs.LowPart == rhs.LowPart;
}
}  // namespace

const char* FormatToString(int64_t format)
{
  switch (static_cast<DXGI_FORMAT>(format))
  {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return "DXGI_FORMAT_R8G8B8A8_UNORM";
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
  case DXGI_FORMAT_B8G8R8A8_UNORM:
    return "DXGI_FORMAT_B8G8R8A8_UNORM";
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return "DXGI_FORMAT_R16G16B16A16_FLOAT";
  default:
    return "UNKNOWN_DXGI_FORMAT";
  }
}

bool QueryGraphicsRequirements(XrInstance instance, XrSystemId system_id,
                               XrGraphicsRequirementsD3D11KHR* out_requirements)
{
  PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
  const XrResult addr_result = xrGetInstanceProcAddr(
      instance, "xrGetD3D11GraphicsRequirementsKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
  if (XR_FAILED(addr_result) || get_requirements == nullptr)
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Could not load xrGetD3D11GraphicsRequirementsKHR.");
    return false;
  }

  *out_requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  const XrResult result = get_requirements(instance, system_id, out_requirements);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrGetD3D11GraphicsRequirementsKHR failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  INFO_LOG_FMT(OPENXR,
               "OpenXR: D3D11 requirements adapter LUID {:#010x}{:08x}, min feature level {:#x}",
               out_requirements->adapterLuid.HighPart, out_requirements->adapterLuid.LowPart,
               static_cast<int>(out_requirements->minFeatureLevel));
  return true;
}

bool CreateMatchingDevice(const XrGraphicsRequirementsD3D11KHR& requirements,
                          bool enable_debug_layer, DeviceBundle* out_bundle)
{
  if (!out_bundle)
    return false;

  PFND3D11CreateDevice d3d11_create_device = nullptr;
  if (!out_bundle->d3d11_library.Open("d3d11.dll") ||
      !out_bundle->d3d11_library.GetSymbol("D3D11CreateDevice", &d3d11_create_device))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Failed to load D3D11CreateDevice.");
    return false;
  }

  IDXGIFactory1* raw_factory = nullptr;
  if (!CreateDXGIFactoryDynamic(&out_bundle->dxgi_library, &raw_factory))
    return false;
  out_bundle->dxgi_factory.Attach(raw_factory);

  for (UINT index = 0;; ++index)
  {
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (out_bundle->dxgi_factory->EnumAdapters(index, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;

    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc)))
      continue;

    if (LuidMatches(desc.AdapterLuid, requirements.adapterLuid))
    {
      out_bundle->adapter = adapter;
      break;
    }
  }

  if (!out_bundle->adapter)
  {
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: No adapter matched the runtime LUID; falling back to the default "
                 "adapter.");
  }

  const auto try_create_device = [&](UINT flags) -> HRESULT {
    return d3d11_create_device(out_bundle->adapter.Get(),
                               out_bundle->adapter ? D3D_DRIVER_TYPE_UNKNOWN :
                                                     D3D_DRIVER_TYPE_HARDWARE,
                               nullptr, flags, SUPPORTED_FEATURE_LEVELS.data(),
                               static_cast<UINT>(SUPPORTED_FEATURE_LEVELS.size()),
                               D3D11_SDK_VERSION, &out_bundle->device, &out_bundle->feature_level,
                               &out_bundle->context);
  };

  HRESULT hr = E_FAIL;
  if (enable_debug_layer)
    hr = try_create_device(D3D11_CREATE_DEVICE_DEBUG);
  if (!enable_debug_layer || FAILED(hr))
    hr = try_create_device(0);

  if (FAILED(hr))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: D3D11CreateDevice failed ({:#010x}).",
                  static_cast<unsigned>(hr));
    return false;
  }

  if (out_bundle->feature_level < requirements.minFeatureLevel)
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: D3D11 device feature level {:#x} is below runtime minimum {:#x}.",
                  static_cast<int>(out_bundle->feature_level),
                  static_cast<int>(requirements.minFeatureLevel));
    return false;
  }

  return true;
}

bool CreateSessionFromRequirements(XrInstance instance, XrSystemId system_id,
                                   const XrGraphicsRequirementsD3D11KHR& requirements,
                                   ID3D11Device* device, XrSession* out_session)
{
  if (!device || !out_session)
    return false;

  XrGraphicsBindingD3D11KHR d3d11_binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
  d3d11_binding.device = device;

  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &d3d11_binding;
  session_info.systemId = system_id;

  *out_session = XR_NULL_HANDLE;
  const XrResult result = xrCreateSession(instance, &session_info, out_session);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: xrCreateSession failed ({}), adapter LUID {:#010x}{:08x}, minimum "
                  "feature level {:#x}.",
                  static_cast<int>(result), requirements.adapterLuid.HighPart,
                  requirements.adapterLuid.LowPart, static_cast<int>(requirements.minFeatureLevel));
    return false;
  }

  return true;
}

bool SelectSwapchainFormat(XrSession session, int64_t* out_format)
{
  uint32_t format_count = 0;
  XrResult result = xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
  if (XR_FAILED(result) || format_count == 0)
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrEnumerateSwapchainFormats (count) failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  std::vector<int64_t> runtime_formats(format_count);
  result = xrEnumerateSwapchainFormats(session, format_count, &format_count, runtime_formats.data());
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrEnumerateSwapchainFormats failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  for (const int64_t format : runtime_formats)
    INFO_LOG_FMT(OPENXR, "OpenXR: Runtime swapchain format {} ({})",
                 static_cast<long long>(format), FormatToString(format));

  // Prefer sRGB formats so the OpenXR compositor correctly decodes Dolphin's
  // gamma-encoded XFB. With a UNORM swapchain the runtime treats our sRGB bytes
  // as linear and the image appears ~2x too bright.
  static constexpr std::array<DXGI_FORMAT, 5> preferred_formats = {
      DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
      DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_R16G16B16A16_FLOAT};

  for (const DXGI_FORMAT preferred : preferred_formats)
  {
    const int64_t wanted = static_cast<int64_t>(preferred);
    if (std::find(runtime_formats.begin(), runtime_formats.end(), wanted) != runtime_formats.end())
    {
      *out_format = wanted;
      INFO_LOG_FMT(OPENXR, "OpenXR: Selected swapchain format {} ({}).",
                   static_cast<long long>(*out_format), FormatToString(*out_format));
      return true;
    }
  }

  *out_format = runtime_formats.front();
  WARN_LOG_FMT(OPENXR,
               "OpenXR: No preferred D3D11 swapchain format found; falling back to runtime "
               "format {} ({}).",
               static_cast<long long>(*out_format), FormatToString(*out_format));
  return true;
}
}  // namespace VR::D3D11OpenXR

#endif

#endif  // ENABLE_VR
