// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#ifdef _WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <openxr/openxr_platform.h>
#endif

#include "Common/DynamicLibrary.h"

namespace VR::D3D11OpenXR
{
#ifdef _WIN32
struct DeviceBundle
{
  Common::DynamicLibrary d3d11_library;
  Common::DynamicLibrary dxgi_library;
  Microsoft::WRL::ComPtr<IDXGIFactory1> dxgi_factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_10_0;
};

bool QueryGraphicsRequirements(XrInstance instance, XrSystemId system_id,
                               XrGraphicsRequirementsD3D11KHR* out_requirements);
bool CreateMatchingDevice(const XrGraphicsRequirementsD3D11KHR& requirements,
                          bool enable_debug_layer, DeviceBundle* out_bundle);
bool CreateSessionFromRequirements(XrInstance instance, XrSystemId system_id,
                                   const XrGraphicsRequirementsD3D11KHR& requirements,
                                   ID3D11Device* device, XrSession* out_session);
bool SelectSwapchainFormat(XrSession session, int64_t* out_format);
const char* FormatToString(int64_t format);
#endif
}  // namespace VR::D3D11OpenXR

#endif  // ENABLE_VR
