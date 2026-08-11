// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#include "VideoCommon/VR/OpenXRManager.h"

#if defined(ANDROID)
#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#ifndef XR_USE_TIMESPEC
#define XR_USE_TIMESPEC
#endif
#include <ctime>
#include <openxr/openxr_platform.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <unknwn.h>
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#include <openxr/openxr_platform.h>
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#if defined(ANDROID)
#include <android/log.h>
#include <mutex>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <string_view>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

namespace VR
{
std::unique_ptr<OpenXRManager> g_openxr;

namespace
{
const char* ReferenceSpaceTypeName(XrReferenceSpaceType type)
{
  switch (type)
  {
  case XR_REFERENCE_SPACE_TYPE_LOCAL:
    return "LOCAL";
  case XR_REFERENCE_SPACE_TYPE_STAGE:
    return "STAGE";
  case XR_REFERENCE_SPACE_TYPE_VIEW:
    return "VIEW";
  default:
    return "UNKNOWN";
  }
}

#if defined(ANDROID)
std::mutex s_android_openxr_mutex;
JavaVM* s_android_vm = nullptr;
jobject s_android_activity = nullptr;
jobject s_android_application_context = nullptr;
bool s_android_loader_initialized = false;

static bool EnsureAndroidOpenXRLoaderInitialized()
{
  std::lock_guard guard{s_android_openxr_mutex};

  if (s_android_loader_initialized)
    return true;

  if (!s_android_vm || (!s_android_activity && !s_android_application_context))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Android VM/context not set before loader initialization.");
    return false;
  }

  XrLoaderInitInfoAndroidKHR loader_init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
  loader_init.applicationVM = s_android_vm;
  // Pass the ACTIVITY as the loader-init context (an Activity is a Context), matching
  // Meta's samples and known-working GLES OpenXR apps (Lambda1VR, Quake2Quest). Meta's
  // legacy GLES session path hooks this context for activity-readiness/launch-id
  // tracking; with a plain Application context the runtime skips the launch-id query,
  // assigns no volumetric-window token, and parks the session in IDLE forever.
  loader_init.applicationContext =
      s_android_activity ? s_android_activity : s_android_application_context;

  PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
  XrResult result = xrGetInstanceProcAddr(
      XR_NULL_HANDLE, "xrInitializeLoaderKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
  if (XR_FAILED(result) || initialize_loader == nullptr)
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Could not load xrInitializeLoaderKHR ({}).",
                  static_cast<int>(result));
    return false;
  }

  result = initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_init));
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrInitializeLoaderKHR failed ({}).", static_cast<int>(result));
    return false;
  }

  s_android_loader_initialized = true;
  INFO_LOG_FMT(OPENXR, "OpenXR: Android loader initialized.");
  return true;
}

static uint32_t GetCurrentAndroidThreadId()
{
#if defined(SYS_gettid)
  const long tid = syscall(SYS_gettid);
#elif defined(__NR_gettid)
  const long tid = syscall(__NR_gettid);
#else
  const long tid = gettid();
#endif
  return tid > 0 ? static_cast<uint32_t>(tid) : 0;
}

static XrAndroidThreadTypeKHR ToXrAndroidThreadType(OpenXRManager::AndroidThreadType type)
{
  switch (type)
  {
  case OpenXRManager::AndroidThreadType::ApplicationMain:
    return XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR;
  case OpenXRManager::AndroidThreadType::ApplicationWorker:
    return XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR;
  case OpenXRManager::AndroidThreadType::RendererMain:
    return XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR;
  case OpenXRManager::AndroidThreadType::RendererWorker:
    return XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR;
  }

  return XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR;
}

static const char* AndroidThreadTypeName(OpenXRManager::AndroidThreadType type)
{
  switch (type)
  {
  case OpenXRManager::AndroidThreadType::ApplicationMain:
    return "application-main";
  case OpenXRManager::AndroidThreadType::ApplicationWorker:
    return "application-worker";
  case OpenXRManager::AndroidThreadType::RendererMain:
    return "renderer-main";
  case OpenXRManager::AndroidThreadType::RendererWorker:
    return "renderer-worker";
  }

  return "unknown";
}

static bool HasAndroidThreadTypeFallback(OpenXRManager::AndroidThreadType type)
{
  return type == OpenXRManager::AndroidThreadType::RendererWorker;
}

static OpenXRManager::AndroidThreadType GetAndroidThreadTypeFallback(
    OpenXRManager::AndroidThreadType type)
{
  // Some Quest runtime builds advertise XR_KHR_android_thread_settings but reject
  // XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR. The Vulkan submit thread still does
  // renderer work, so retry as renderer-main instead of leaving it untagged.
  if (type == OpenXRManager::AndroidThreadType::RendererWorker)
    return OpenXRManager::AndroidThreadType::RendererMain;

  return type;
}

static bool TrySetAndroidApplicationThread(XrInstance instance, XrSession session,
                                           PFN_xrSetAndroidApplicationThreadKHR set_thread,
                                           OpenXRManager::AndroidThreadType requested_type,
                                           OpenXRManager::AndroidThreadType type,
                                           uint32_t thread_id, std::string_view label,
                                           bool fallback)
{
  const XrResult result = set_thread(session, ToXrAndroidThreadType(type), thread_id);
  const char* label_data = label.empty() ? "" : label.data();
  if (XR_FAILED(result))
  {
    char result_string[XR_MAX_RESULT_STRING_SIZE]{};
    xrResultToString(instance, result, result_string);
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: xrSetAndroidApplicationThreadKHR failed for {} thread '{}' "
                 "(requested {}, tid={}): {}",
                 AndroidThreadTypeName(type), label, AndroidThreadTypeName(requested_type),
                 thread_id, result_string);
    __android_log_print(ANDROID_LOG_WARN, "DolphinXR",
                        "OpenXR: xrSetAndroidApplicationThreadKHR failed for %s thread '%.*s' "
                        "(requested %s, tid=%u): %s",
                        AndroidThreadTypeName(type), static_cast<int>(label.size()), label_data,
                        AndroidThreadTypeName(requested_type), thread_id, result_string);
    return false;
  }

  INFO_LOG_FMT(OPENXR,
               "OpenXR: Registered Android {} thread '{}' (requested {}, tid={}{}).",
               AndroidThreadTypeName(type), label, AndroidThreadTypeName(requested_type),
               thread_id, fallback ? ", fallback" : "");
  __android_log_print(ANDROID_LOG_INFO, "DolphinXR",
                      "OpenXR: registered Android %s thread '%.*s' (requested %s, tid=%u%s)",
                      AndroidThreadTypeName(type), static_cast<int>(label.size()), label_data,
                      AndroidThreadTypeName(requested_type), thread_id,
                      fallback ? ", fallback" : "");
  return true;
}

static bool SetAndroidApplicationThreadWithFallback(
    XrInstance instance, XrSession session, PFN_xrSetAndroidApplicationThreadKHR set_thread,
    OpenXRManager::AndroidThreadType type, uint32_t thread_id, std::string_view label)
{
  if (TrySetAndroidApplicationThread(instance, session, set_thread, type, type, thread_id, label,
                                     false))
  {
    return true;
  }

  if (!HasAndroidThreadTypeFallback(type))
    return false;

  const OpenXRManager::AndroidThreadType fallback_type = GetAndroidThreadTypeFallback(type);
  WARN_LOG_FMT(OPENXR, "OpenXR: Retrying Android thread '{}' registration as {}.", label,
               AndroidThreadTypeName(fallback_type));
  __android_log_print(ANDROID_LOG_WARN, "DolphinXR",
                      "OpenXR: retrying Android thread '%.*s' registration as %s",
                      static_cast<int>(label.size()), label.empty() ? "" : label.data(),
                      AndroidThreadTypeName(fallback_type));
  return TrySetAndroidApplicationThread(instance, session, set_thread, type, fallback_type,
                                        thread_id, label, true);
}

static const char* PerfSettingsDomainName(XrPerfSettingsDomainEXT domain)
{
  switch (domain)
  {
  case XR_PERF_SETTINGS_DOMAIN_CPU_EXT:
    return "CPU";
  case XR_PERF_SETTINGS_DOMAIN_GPU_EXT:
    return "GPU";
  default:
    return "unknown";
  }
}
#endif

static XrQuaternionf MultiplyQuaternions(const XrQuaternionf& a, const XrQuaternionf& b)
{
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static void CopyOpenXRName(char* dst, size_t dst_size, std::string_view src)
{
  std::memset(dst, 0, dst_size);
  const size_t copy_size = std::min(dst_size - 1, src.size());
  std::memcpy(dst, src.data(), copy_size);
}

static std::string PathToString(XrInstance instance, XrPath path)
{
  if (instance == XR_NULL_HANDLE || path == XR_NULL_PATH)
    return {};

  uint32_t required_size = 0;
  if (XR_FAILED(xrPathToString(instance, path, 0, &required_size, nullptr)) || required_size == 0)
    return {};

  std::string result(required_size, '\0');
  if (XR_FAILED(xrPathToString(instance, path, required_size, &required_size, result.data())))
    return {};

  if (!result.empty() && result.back() == '\0')
    result.pop_back();
  return result;
}

}  // namespace

// Checks an XrResult and returns false (with an error log) on failure.
// Requires m_instance to be valid for error string lookup.
#define XR_CHECK(expr)                                                                             \
  do                                                                                               \
  {                                                                                                \
    const XrResult _r = (expr);                                                                    \
    if (XR_FAILED(_r))                                                                             \
    {                                                                                              \
      char _buf[XR_MAX_RESULT_STRING_SIZE]{};                                                      \
      xrResultToString(m_instance, _r, _buf);                                                      \
      ERROR_LOG_FMT(OPENXR, "OpenXR: {} failed: {}", #expr, _buf);                                  \
      return false;                                                                                 \
    }                                                                                              \
  } while (false)

std::vector<const char*> OpenXRManager::GetAvailableControllerExtensions()
{
#if defined(ANDROID)
  if (!EnsureAndroidOpenXRLoaderInitialized())
    return {};
#endif

  static const std::array<const char*, 3> s_optional = {
      XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME,
      XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME,
      XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME,
  };

  uint32_t ext_count = 0;
  xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
  std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());

  std::vector<const char*> result;
  for (const char* wanted : s_optional)
  {
    if (std::any_of(exts.begin(), exts.end(), [wanted](const XrExtensionProperties& e) {
          return std::string_view{e.extensionName} == wanted;
        }))
    {
      result.push_back(wanted);
    }
  }
  return result;
}

std::vector<const char*> OpenXRManager::GetAvailableFoveationExtensions(bool for_vulkan)
{
#if defined(ANDROID)
  if (!EnsureAndroidOpenXRLoaderInitialized())
    return {};
#endif

  // All three are needed to configure and apply a foveation profile; foveation_vulkan
  // additionally exposes the runtime's fragment density map images to the app.
  if (!IsRuntimeExtensionSupported(XR_FB_FOVEATION_EXTENSION_NAME) ||
      !IsRuntimeExtensionSupported(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME) ||
      !IsRuntimeExtensionSupported(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME))
  {
    return {};
  }

  std::vector<const char*> result = {XR_FB_FOVEATION_EXTENSION_NAME,
                                     XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME,
                                     XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME};
  if (for_vulkan)
  {
    // Spelled out because the XR_FB_FOVEATION_VULKAN_EXTENSION_NAME macro lives in
    // openxr_platform.h behind XR_USE_GRAPHICS_API_VULKAN, which this file doesn't define.
    static constexpr const char* kFoveationVulkanExt = "XR_FB_foveation_vulkan";
    if (!IsRuntimeExtensionSupported(kFoveationVulkanExt))
      return {};
    result.push_back(kFoveationVulkanExt);
  }
  return result;
}

bool OpenXRManager::IsFoveationUsable() const
{
  return m_xrCreateFoveationProfileFB != nullptr && m_xrUpdateSwapchainFB != nullptr &&
         Config::Get(Config::GFX_VR_FOVEATION_LEVEL) > Config::GFX_VR_FOVEATION_LEVEL_OFF;
}

bool OpenXRManager::ApplyFoveationToSwapchain(XrSwapchain swapchain)
{
  if (!IsFoveationUsable() || m_session == XR_NULL_HANDLE || swapchain == XR_NULL_HANDLE)
    return false;

  const int level = std::clamp(Config::Get(Config::GFX_VR_FOVEATION_LEVEL),
                               Config::GFX_VR_FOVEATION_LEVEL_OFF,
                               Config::GFX_VR_FOVEATION_LEVEL_MAX);
  const bool dynamic = Config::Get(Config::GFX_VR_FOVEATION_DYNAMIC);

  XrFoveationLevelProfileCreateInfoFB level_info{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
  level_info.level = static_cast<XrFoveationLevelFB>(level);
  level_info.verticalOffset = 0.0f;
  level_info.dynamic =
      dynamic ? XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB : XR_FOVEATION_DYNAMIC_DISABLED_FB;

  XrFoveationProfileCreateInfoFB profile_info{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
  profile_info.next = &level_info;

  XrFoveationProfileFB profile = XR_NULL_HANDLE;
  XrResult result = m_xrCreateFoveationProfileFB(m_session, &profile_info, &profile);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: xrCreateFoveationProfileFB failed ({}).",
                 static_cast<int>(result));
    return false;
  }

  XrSwapchainStateFoveationFB foveation_state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
  foveation_state.flags = 0;
  foveation_state.profile = profile;
  result = m_xrUpdateSwapchainFB(
      swapchain, reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&foveation_state));

  // The runtime snapshots the profile during the update; ours can go away immediately.
  if (m_xrDestroyFoveationProfileFB != nullptr)
    m_xrDestroyFoveationProfileFB(profile);

  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: xrUpdateSwapchainFB (foveation) failed ({}).",
                 static_cast<int>(result));
    return false;
  }

  INFO_LOG_FMT(OPENXR, "OpenXR: Foveation applied: level {} ({}dynamic).", level,
               dynamic ? "" : "non-");
  return true;
}

bool OpenXRManager::IsExtensionEnabled(std::string_view ext_name) const
{
  return std::any_of(m_enabled_extensions.begin(), m_enabled_extensions.end(),
                     [ext_name](const std::string& e) { return e == ext_name; });
}

bool OpenXRManager::IsRuntimeExtensionSupported(std::string_view ext_name)
{
#if defined(ANDROID)
  if (!EnsureAndroidOpenXRLoaderInitialized())
    return false;
#endif

  uint32_t ext_count = 0;
  xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
  std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());

  return std::any_of(exts.begin(), exts.end(), [ext_name](const XrExtensionProperties& e) {
    return std::string_view{e.extensionName} == ext_name;
  });
}

OpenXRManager::OpenXRManager() = default;

OpenXRManager::~OpenXRManager()
{
  DestroySession();

  if (m_instance != XR_NULL_HANDLE)
    xrDestroyInstance(m_instance);
}

bool OpenXRManager::CreateInstance(const std::vector<const char*>& extra_extensions)
{
#if defined(ANDROID)
  if (!EnsureAndroidOpenXRLoaderInitialized())
    return false;
#endif

  // Log available API layers.
  uint32_t layer_count = 0;
  xrEnumerateApiLayerProperties(0, &layer_count, nullptr);
  std::vector<XrApiLayerProperties> layers(layer_count, {XR_TYPE_API_LAYER_PROPERTIES});
  xrEnumerateApiLayerProperties(layer_count, &layer_count, layers.data());
  for (const auto& layer : layers)
    INFO_LOG_FMT(OPENXR, "OpenXR: Available API layer: {}", layer.layerName);

  // Enumerate and verify extensions.
  uint32_t ext_count = 0;
  xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
  std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());
  for (const auto& ext : exts)
    INFO_LOG_FMT(OPENXR, "OpenXR: Available extension: {}", ext.extensionName);

  const auto runtime_has = [&exts](std::string_view name) {
    return std::any_of(exts.begin(), exts.end(), [name](const XrExtensionProperties& e) {
      return std::string_view{e.extensionName} == name;
    });
  };

  for (const char* required : extra_extensions)
  {
    if (!runtime_has(required))
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: Required extension '{}' not available.", required);
      return false;
    }
  }

  // XR_FB_passthrough: Windows (Meta Horizon Link exposes it when passthrough over Link
  // is enabled) and Quest standalone. Vulkan-only — the dedicated coverage target that
  // produces the projection layer's alpha is a Vulkan feature.
  std::vector<const char*> enabled_extensions(extra_extensions);
#if defined(_WIN32) || defined(ANDROID)
  const bool is_vulkan_binding =
      std::find_if(extra_extensions.begin(), extra_extensions.end(), [](const char* extension) {
        return std::string_view{extension} == "XR_KHR_vulkan_enable" ||
               std::string_view{extension} == "XR_KHR_vulkan_enable2";
      }) != extra_extensions.end();
  if (is_vulkan_binding && runtime_has(XR_FB_PASSTHROUGH_EXTENSION_NAME))
    enabled_extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
#endif

#if defined(ANDROID)
  // Quest supplies its production hand mesh through OpenXR. Retrieve that mesh once and skin it
  // from Touch controller input instead of drawing app-authored placeholder hands.
  if (runtime_has(XR_EXT_HAND_TRACKING_EXTENSION_NAME) &&
      runtime_has(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME))
  {
    enabled_extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    enabled_extensions.push_back(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME);
    if (runtime_has(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME))
      enabled_extensions.push_back(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
  }
#endif

  // Time-domain conversion: lets input sampling locate controller poses at measured "now"
  // instead of the predicted display time. Prediction extrapolates fast-moving controllers
  // several frames ahead, which sprays the aim ray during fast wrist motion.
#if defined(_WIN32)
  if (runtime_has(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME))
    enabled_extensions.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
#elif defined(ANDROID)
  if (runtime_has(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME))
    enabled_extensions.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
#endif

  XrVersion requested_api_version = XR_CURRENT_API_VERSION;
  INFO_LOG_FMT(OPENXR, "OpenXR: Requesting API version {}.{}.{}.",
               XR_VERSION_MAJOR(requested_api_version), XR_VERSION_MINOR(requested_api_version),
               XR_VERSION_PATCH(requested_api_version));

  XrApplicationInfo app_info{};
  std::strncpy(app_info.applicationName, "Dolphin Emulator", XR_MAX_APPLICATION_NAME_SIZE - 1);
  app_info.applicationVersion = 1;
  std::strncpy(app_info.engineName, "Dolphin", XR_MAX_ENGINE_NAME_SIZE - 1);
  app_info.engineVersion = 1;
  app_info.apiVersion = requested_api_version;

  // Record which extensions we are enabling for later profile gating.
  m_enabled_extensions.clear();
  for (const char* ext : enabled_extensions)
    m_enabled_extensions.emplace_back(ext);

  XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
  create_info.applicationInfo = app_info;
  create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
  create_info.enabledExtensionNames = enabled_extensions.data();

#if defined(ANDROID)
  XrInstanceCreateInfoAndroidKHR android_create_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
  {
    std::lock_guard guard{s_android_openxr_mutex};
    if (!s_android_vm || !s_android_activity)
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: Android VM/activity not set before xrCreateInstance.");
      return false;
    }
    android_create_info.applicationVM = s_android_vm;
    android_create_info.applicationActivity = s_android_activity;
  }
  create_info.next = &android_create_info;
#endif

  XrResult result = xrCreateInstance(&create_info, &m_instance);
  if (result == XR_ERROR_API_VERSION_UNSUPPORTED && requested_api_version != XR_API_VERSION_1_0)
  {
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: Runtime rejected API version {}.{}.{}; retrying with 1.0.",
                 XR_VERSION_MAJOR(requested_api_version), XR_VERSION_MINOR(requested_api_version),
                 XR_VERSION_PATCH(requested_api_version));
    app_info.apiVersion = XR_API_VERSION_1_0;
    create_info.applicationInfo = app_info;
    result = xrCreateInstance(&create_info, &m_instance);
  }

  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: xrCreateInstance failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  xrGetInstanceProperties(m_instance, &props);
  m_runtime_name = props.runtimeName;
  m_quest_or_vd_runtime.reset();
  INFO_LOG_FMT(OPENXR, "OpenXR: Runtime '{}' version {}.{}.{}", props.runtimeName,
               XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
               XR_VERSION_PATCH(props.runtimeVersion));

  if (IsExtensionEnabled(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    XrResult refresh_rate_result =
        xrGetInstanceProcAddr(m_instance, "xrGetDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&m_xrGetDisplayRefreshRateFB));
    if (XR_FAILED(refresh_rate_result) || m_xrGetDisplayRefreshRateFB == nullptr)
    {
      WARN_LOG_FMT(OPENXR,
                   "OpenXR: XR_FB_display_refresh_rate enabled but "
                   "xrGetDisplayRefreshRateFB could not be loaded ({}).",
                   static_cast<int>(refresh_rate_result));
      m_xrGetDisplayRefreshRateFB = nullptr;
    }

    else
    {
      INFO_LOG_FMT(OPENXR, "OpenXR: XR_FB_display_refresh_rate enabled.");
    }
  }

  if (IsExtensionEnabled(XR_FB_FOVEATION_EXTENSION_NAME) &&
      IsExtensionEnabled(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME))
  {
    const auto load_foveation_pfn = [this](const char* name, auto* out_pfn) {
      const XrResult r = xrGetInstanceProcAddr(m_instance, name,
                                               reinterpret_cast<PFN_xrVoidFunction*>(out_pfn));
      if (XR_FAILED(r) || *out_pfn == nullptr)
      {
        WARN_LOG_FMT(OPENXR, "OpenXR: XR_FB_foveation enabled but {} could not be loaded ({}).",
                     name, static_cast<int>(r));
        *out_pfn = nullptr;
        return false;
      }
      return true;
    };

    if (load_foveation_pfn("xrCreateFoveationProfileFB", &m_xrCreateFoveationProfileFB) &&
        load_foveation_pfn("xrDestroyFoveationProfileFB", &m_xrDestroyFoveationProfileFB) &&
        load_foveation_pfn("xrUpdateSwapchainFB", &m_xrUpdateSwapchainFB))
    {
      INFO_LOG_FMT(OPENXR, "OpenXR: XR_FB_foveation enabled.");
    }
    else
    {
      m_xrCreateFoveationProfileFB = nullptr;
      m_xrDestroyFoveationProfileFB = nullptr;
      m_xrUpdateSwapchainFB = nullptr;
    }
  }

#if defined(_WIN32) || defined(ANDROID)
  if (IsExtensionEnabled(XR_FB_PASSTHROUGH_EXTENSION_NAME))
  {
    const auto load_pfn = [this](const char* name, auto* out_pfn) {
      const XrResult r = xrGetInstanceProcAddr(m_instance, name,
                                               reinterpret_cast<PFN_xrVoidFunction*>(out_pfn));
      if (XR_FAILED(r) || *out_pfn == nullptr)
      {
        WARN_LOG_FMT(OPENXR, "OpenXR: XR_FB_passthrough enabled but {} could not be loaded ({}).",
                     name, static_cast<int>(r));
        *out_pfn = nullptr;
        return false;
      }
      return true;
    };

    const bool loaded = load_pfn("xrCreatePassthroughFB", &m_xrCreatePassthroughFB) &&
                        load_pfn("xrDestroyPassthroughFB", &m_xrDestroyPassthroughFB) &&
                        load_pfn("xrPassthroughStartFB", &m_xrPassthroughStartFB) &&
                        load_pfn("xrPassthroughPauseFB", &m_xrPassthroughPauseFB) &&
                        load_pfn("xrCreatePassthroughLayerFB", &m_xrCreatePassthroughLayerFB) &&
                        load_pfn("xrDestroyPassthroughLayerFB", &m_xrDestroyPassthroughLayerFB) &&
                        load_pfn("xrPassthroughLayerPauseFB", &m_xrPassthroughLayerPauseFB) &&
                        load_pfn("xrPassthroughLayerResumeFB", &m_xrPassthroughLayerResumeFB);
    if (loaded)
    {
      INFO_LOG_FMT(OPENXR, "OpenXR: XR_FB_passthrough enabled.");
    }
    else
    {
      m_xrCreatePassthroughFB = nullptr;
      m_xrDestroyPassthroughFB = nullptr;
      m_xrPassthroughStartFB = nullptr;
      m_xrPassthroughPauseFB = nullptr;
      m_xrCreatePassthroughLayerFB = nullptr;
      m_xrDestroyPassthroughLayerFB = nullptr;
      m_xrPassthroughLayerPauseFB = nullptr;
      m_xrPassthroughLayerResumeFB = nullptr;
    }
  }
#endif

#if defined(ANDROID)
  if (IsExtensionEnabled(XR_EXT_HAND_TRACKING_EXTENSION_NAME) &&
      IsExtensionEnabled(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME))
  {
    const auto load_hand_pfn = [this](const char* name, auto* out_pfn) {
      const XrResult r = xrGetInstanceProcAddr(m_instance, name,
                                               reinterpret_cast<PFN_xrVoidFunction*>(out_pfn));
      if (XR_FAILED(r) || *out_pfn == nullptr)
      {
        WARN_LOG_FMT(OPENXR, "OpenXR: hand mesh enabled but {} could not be loaded ({}).", name,
                     static_cast<int>(r));
        *out_pfn = nullptr;
        return false;
      }
      return true;
    };

    if (load_hand_pfn("xrCreateHandTrackerEXT", &m_xrCreateHandTrackerEXT) &&
        load_hand_pfn("xrDestroyHandTrackerEXT", &m_xrDestroyHandTrackerEXT) &&
        load_hand_pfn("xrLocateHandJointsEXT", &m_xrLocateHandJointsEXT) &&
        load_hand_pfn("xrGetHandMeshFB", &m_xrGetHandMeshFB))
    {
      INFO_LOG_FMT(OPENXR, "OpenXR: Quest runtime hand mesh support enabled.");
    }
    else
    {
      m_xrCreateHandTrackerEXT = nullptr;
      m_xrDestroyHandTrackerEXT = nullptr;
      m_xrLocateHandJointsEXT = nullptr;
      m_xrGetHandMeshFB = nullptr;
    }
  }

  if (IsExtensionEnabled(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME))
  {
    const auto load_sim_pfn = [this](const char* name, auto* out_pfn) {
      const XrResult r = xrGetInstanceProcAddr(m_instance, name,
                                               reinterpret_cast<PFN_xrVoidFunction*>(out_pfn));
      if (XR_FAILED(r) || *out_pfn == nullptr)
      {
        *out_pfn = nullptr;
        return false;
      }
      return true;
    };
    if (load_sim_pfn("xrResumeSimultaneousHandsAndControllersTrackingMETA",
                     &m_xrResumeSimultaneousHandsAndControllersTrackingMETA) &&
        load_sim_pfn("xrPauseSimultaneousHandsAndControllersTrackingMETA",
                     &m_xrPauseSimultaneousHandsAndControllersTrackingMETA))
    {
      INFO_LOG_FMT(OPENXR, "OpenXR: simultaneous Quest hands + controllers available.");
    }
  }

  if (IsExtensionEnabled(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
  {
    XrResult thread_settings_result =
        xrGetInstanceProcAddr(m_instance, "xrSetAndroidApplicationThreadKHR",
                              &m_xrSetAndroidApplicationThreadKHR);
    if (XR_FAILED(thread_settings_result) || m_xrSetAndroidApplicationThreadKHR == nullptr)
    {
      WARN_LOG_FMT(OPENXR,
                   "OpenXR: XR_KHR_android_thread_settings enabled but "
                   "xrSetAndroidApplicationThreadKHR could not be loaded ({}).",
                   static_cast<int>(thread_settings_result));
      m_xrSetAndroidApplicationThreadKHR = nullptr;
    }
    else
    {
      INFO_LOG_FMT(OPENXR, "OpenXR: XR_KHR_android_thread_settings enabled.");
    }
  }

  if (IsExtensionEnabled(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
  {
    XrResult perf_settings_result =
        xrGetInstanceProcAddr(m_instance, "xrPerfSettingsSetPerformanceLevelEXT",
                              &m_xrPerfSettingsSetPerformanceLevelEXT);
    if (XR_FAILED(perf_settings_result) || m_xrPerfSettingsSetPerformanceLevelEXT == nullptr)
    {
      WARN_LOG_FMT(OPENXR,
                   "OpenXR: XR_EXT_performance_settings enabled but "
                   "xrPerfSettingsSetPerformanceLevelEXT could not be loaded ({}).",
                   static_cast<int>(perf_settings_result));
      m_xrPerfSettingsSetPerformanceLevelEXT = nullptr;
    }
    else
    {
      INFO_LOG_FMT(OPENXR, "OpenXR: XR_EXT_performance_settings enabled.");
    }
  }
#endif

  return true;
}

#if defined(ANDROID)
void OpenXRManager::SetAndroidAppInfo(JavaVM* vm, JNIEnv* env, jobject activity)
{
  std::lock_guard guard{s_android_openxr_mutex};

  if (s_android_activity)
    env->DeleteGlobalRef(s_android_activity);
  if (s_android_application_context)
    env->DeleteGlobalRef(s_android_application_context);

  s_android_vm = vm;
  s_android_loader_initialized = false;
  s_android_activity = activity ? env->NewGlobalRef(activity) : nullptr;
  s_android_application_context = nullptr;

  if (!activity)
    return;

  jclass activity_class = env->GetObjectClass(activity);
  jmethodID get_application_context =
      env->GetMethodID(activity_class, "getApplicationContext", "()Landroid/content/Context;");
  jobject application_context = env->CallObjectMethod(activity, get_application_context);
  if (application_context)
  {
    s_android_application_context = env->NewGlobalRef(application_context);
    env->DeleteLocalRef(application_context);
  }
  env->DeleteLocalRef(activity_class);
}

void OpenXRManager::ClearAndroidAppInfo(JNIEnv* env)
{
  std::lock_guard guard{s_android_openxr_mutex};

  if (s_android_activity)
  {
    env->DeleteGlobalRef(s_android_activity);
    s_android_activity = nullptr;
  }
  if (s_android_application_context)
  {
    env->DeleteGlobalRef(s_android_application_context);
    s_android_application_context = nullptr;
  }

  s_android_vm = nullptr;
  s_android_loader_initialized = false;
}

bool OpenXRManager::RegisterCurrentAndroidThread(AndroidThreadType type, std::string_view label)
{
  if (m_xrSetAndroidApplicationThreadKHR == nullptr)
    return false;

  const uint32_t thread_id = GetCurrentAndroidThreadId();
  if (thread_id == 0)
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Could not determine Android thread id for '{}'.", label);
    return false;
  }

  {
    // Threads spawn before the video backend creates the XrSession (Core.cpp:344/506
    // both fire from emu/GPU thread bootstrap, before SetSession runs). Queue and
    // replay from SetSession so Meta's runtime sees the thread tags it needs to
    // grant big.LITTLE scheduling and DVFS escalation.
    //
    // The m_session check and the push must be inside the same lock the flush uses,
    // otherwise SetSession's flush could run between our check and our push and leave
    // the entry orphaned.
    std::lock_guard guard(m_pending_thread_registrations_mutex);
    if (m_session == XR_NULL_HANDLE)
    {
      m_pending_thread_registrations.push_back({thread_id, type, std::string(label)});
      INFO_LOG_FMT(OPENXR,
                   "OpenXR: Deferring Android {} thread '{}' (tid={}) — session not yet "
                   "created.",
                   AndroidThreadTypeName(type), label, thread_id);
      return false;
    }
  }

  const auto set_thread = reinterpret_cast<PFN_xrSetAndroidApplicationThreadKHR>(
      m_xrSetAndroidApplicationThreadKHR);
  return SetAndroidApplicationThreadWithFallback(m_instance, m_session, set_thread, type,
                                                 thread_id, label);
}

void OpenXRManager::FlushPendingAndroidThreadRegistrations()
{
  if (m_session == XR_NULL_HANDLE || m_xrSetAndroidApplicationThreadKHR == nullptr)
    return;

  std::vector<PendingAndroidThreadRegistration> to_flush;
  {
    std::lock_guard guard(m_pending_thread_registrations_mutex);
    to_flush.swap(m_pending_thread_registrations);
  }

  if (to_flush.empty())
    return;

  const auto set_thread = reinterpret_cast<PFN_xrSetAndroidApplicationThreadKHR>(
      m_xrSetAndroidApplicationThreadKHR);

  for (const auto& pending : to_flush)
  {
    SetAndroidApplicationThreadWithFallback(m_instance, m_session, set_thread, pending.type,
                                            pending.thread_id, pending.label);
  }
}

bool OpenXRManager::RequestAndroidHighPerformanceLevel()
{
  if (m_session == XR_NULL_HANDLE || m_xrPerfSettingsSetPerformanceLevelEXT == nullptr)
    return false;

  if (!Config::Get(Config::GFX_VR_QUEST_CPU_LEVEL_5_HINT))
  {
    INFO_LOG_FMT(OPENXR,
                 "OpenXR: Quest CPU level 5 hint disabled; not requesting sustained high.");
    return false;
  }

  const auto set_performance_level = reinterpret_cast<PFN_xrPerfSettingsSetPerformanceLevelEXT>(
      m_xrPerfSettingsSetPerformanceLevelEXT);

  const auto request_domain = [&](XrPerfSettingsDomainEXT domain) {
    constexpr XrPerfSettingsLevelEXT level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
    const XrResult result = set_performance_level(m_session, domain, level);
    if (XR_FAILED(result))
    {
      char result_string[XR_MAX_RESULT_STRING_SIZE]{};
      xrResultToString(m_instance, result, result_string);
      WARN_LOG_FMT(OPENXR,
                   "OpenXR: xrPerfSettingsSetPerformanceLevelEXT failed for {}: {}",
                   PerfSettingsDomainName(domain), result_string);
      return false;
    }

    INFO_LOG_FMT(OPENXR, "OpenXR: Requested {} performance level SUSTAINED_HIGH.",
                 PerfSettingsDomainName(domain));
    return true;
  };

  const bool cpu_ok = request_domain(XR_PERF_SETTINGS_DOMAIN_CPU_EXT);
  const bool gpu_ok = request_domain(XR_PERF_SETTINGS_DOMAIN_GPU_EXT);
  return cpu_ok && gpu_ok;
}
#endif

bool OpenXRManager::InitializeSystem()
{
  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

  const XrResult result = xrGetSystem(m_instance, &system_info, &m_system_id);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: xrGetSystem failed ({}). Is a headset connected?",
                  static_cast<int>(result));
    return false;
  }

  XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
#if defined(_WIN32) || defined(ANDROID)
  XrSystemPassthroughPropertiesFB passthrough_props{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
  if (m_xrCreatePassthroughFB != nullptr)
    props.next = &passthrough_props;
#endif
  xrGetSystemProperties(m_instance, m_system_id, &props);
  m_system_name = props.systemName;
  m_quest_or_vd_runtime.reset();
  m_system_vendor_id = props.vendorId;
#if defined(_WIN32) || defined(ANDROID)
  m_system_supports_fb_passthrough = passthrough_props.supportsPassthrough == XR_TRUE;
#else
  m_system_supports_fb_passthrough = false;
#endif
  INFO_LOG_FMT(OPENXR, "OpenXR: System '{}' (vendor {:08x})", props.systemName, props.vendorId);
  if (m_xrCreatePassthroughFB != nullptr)
  {
    INFO_LOG_FMT(OPENXR, "OpenXR: XR_FB_passthrough system support: {}",
                 m_system_supports_fb_passthrough ? "yes" : "no");
  }

  return true;
}

bool OpenXRManager::EnumerateViewConfigurations()
{
  uint32_t view_count = 0;
  XrResult result = xrEnumerateViewConfigurationViews(
      m_instance, m_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);

  if (XR_FAILED(result) || view_count != 2)
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: Failed to enumerate view configs or unexpected count ({}). "
                  "Expected 2 views for stereo.",
                  view_count);
    return false;
  }

  m_view_config_views.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_system_id,
                                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             view_count, &view_count, m_view_config_views.data()));

  // Bake the user's resolution scale into the recommended sizes so every consumer
  // (swapchain creation, per-eye blit rects, GetEyeWidth/Height) stays consistent.
  // Read through Config::Get: this can run before g_ActiveConfig is populated.
  const float resolution_scale = std::clamp(Config::Get(Config::GFX_VR_RESOLUTION_SCALE),
                                            Config::GFX_VR_RESOLUTION_SCALE_MIN,
                                            Config::GFX_VR_RESOLUTION_SCALE_MAX);

  for (uint32_t i = 0; i < view_count; ++i)
  {
    auto& view = m_view_config_views[i];
    const uint32_t recommended_w = view.recommendedImageRectWidth;
    const uint32_t recommended_h = view.recommendedImageRectHeight;

    if (std::abs(resolution_scale - 1.0f) > 0.001f)
    {
      // Round to a multiple of 4 and clamp to the runtime's limits.
      const auto scale_dim = [resolution_scale](uint32_t dim, uint32_t max_dim) {
        const auto scaled = static_cast<uint32_t>(std::lround(dim * resolution_scale / 4.0)) * 4;
        return std::clamp<uint32_t>(scaled, 64, max_dim);
      };
      view.recommendedImageRectWidth = scale_dim(recommended_w, view.maxImageRectWidth);
      view.recommendedImageRectHeight = scale_dim(recommended_h, view.maxImageRectHeight);
    }

    INFO_LOG_FMT(OPENXR, "OpenXR: Eye {} recommended {}x{} (max {}x{}), using {}x{} (scale {})", i,
                 recommended_w, recommended_h, view.maxImageRectWidth, view.maxImageRectHeight,
                 view.recommendedImageRectWidth, view.recommendedImageRectHeight,
                 resolution_scale);
  }

  uint32_t blend_count = 0;
  xrEnumerateEnvironmentBlendModes(m_instance, m_system_id,
                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blend_count,
                                   nullptr);
  m_supported_blend_modes.resize(blend_count);
  xrEnumerateEnvironmentBlendModes(m_instance, m_system_id,
                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blend_count,
                                   &blend_count, m_supported_blend_modes.data());
  INFO_LOG_FMT(OPENXR, "OpenXR: {} environment blend mode(s) supported", blend_count);
  if (SupportsAlphaBlend())
    INFO_LOG_FMT(OPENXR, "OpenXR: Alpha-blend passthrough (AR) is available");

  return true;
}

void OpenXRManager::SetSession(XrSession session)
{
  // Session-owned passthrough and runtime hand-mesh objects must be dropped before switching.
  DestroyFBPassthrough();
  DestroyTabletopHandMeshes();

  m_session = session;
  m_tabletop_runtime_enabled.store(g_ActiveConfig.vr_tabletop_mode, std::memory_order_release);
  m_tabletop_toggle_right_stick_was_down = false;
  m_touch_wrist_calibration_valid = {false, false};
  m_touch_wrist_from_grip = {};
  // A fresh XR session gets a fresh canonical Animal Crossing tabletop basis. Later runtime camera
  // toggles intentionally keep this basis so transient Camera2 pitch/roll cannot tilt the board.
  m_ac_tabletop_stable_basis_valid = false;
  m_ac_tabletop_camera_anchor_valid = false;
  m_ac_tabletop_transition_active = false;
  m_ac_tabletop_scene_id = -1;
  m_ac_tabletop_camera_invalid_frames = 0;
  m_ac_tabletop_next_camera_scan_time = 0;
  m_tabletop_reanchor_requested.store(false, std::memory_order_release);

  if (m_session == XR_NULL_HANDLE)
  {
    DestroyInputActions();
    ResetInputActionsState();
    return;
  }

  CaptureStartupDisplayRefreshRateFromExtension();

#if defined(ANDROID)
  // Replay any thread tags that were deferred while the session was being created.
  FlushPendingAndroidThreadRegistrations();
#endif

  if (!InitializeInputActions())
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Controller input actions unavailable.");
    ResetInputActionsState();
  }

  const bool hand_mesh_ready = InitializeTabletopHandMeshes();
  if (!hand_mesh_ready)
  {
    INFO_LOG_FMT(OPENXR, "OpenXR: Runtime hand mesh unavailable; tabletop hands use fallback.");
  }
  else
  {
    // Do not resume XR_META_simultaneous_hands_and_controllers for controller-driven tabletop
    // hands. On Quest 3 the held/unheld detector can switch /user/hand/* between controller and
    // hand interaction profiles independently, which makes Touch action states disappear even
    // while the physical controllers are being used. We only need XR_FB_hand_tracking_mesh here:
    // its immutable Meta mesh has already been queried above, so keep simultaneous tracking paused
    // and let the runtime expose the normal stable Touch controller interaction profiles.
    m_simultaneous_hands_controllers_active = false;
    INFO_LOG_FMT(OPENXR,
                 "OpenXR: Meta runtime hand mesh ready; simultaneous hands/controllers left paused "
                 "for stable Touch input.");
  }
}

void OpenXRManager::SetSwapchain(IOpenXRSwapchain* swapchain)
{
  if (swapchain == nullptr)
  {
    // Stop before the backend destroys the swapchains: the pacing thread's heartbeat
    // layers reference swapchain handles, and EndFrame takes the graphics queue lock.
    StopFrameThread();
    m_swapchain = nullptr;
    // XFB pose stamps hold poses in the (possibly outgoing) session's reference space.
    m_xfb_pose_stamps = {};
    m_xfb_pose_stamp_next = 0;
    m_present_eye_views_valid = false;
    return;
  }

  m_swapchain = swapchain;
  if (!swapchain->SupportsDetachedFrameLoop())
  {
    INFO_LOG_FMT(OPENXR, "OpenXR: backend requires the inline frame flow (no pacing thread).");
  }
  else if (Config::Get(Config::GFX_VR_USE_XR_PACING_THREAD))
  {
    StartFrameThread();
  }
  else
  {
    INFO_LOG_FMT(OPENXR, "OpenXR: XR pacing thread disabled by config (legacy frame flow).");
  }
}

void OpenXRManager::StartFrameThread()
{
  if (m_frame_thread.joinable())
    return;

  m_frame_thread_should_exit.store(false, std::memory_order_release);
  m_frame_thread_running.store(true, std::memory_order_release);
  m_frame_thread = std::thread(&OpenXRManager::FrameThreadLoop, this);
  INFO_LOG_FMT(OPENXR, "OpenXR: XR pacing thread started.");
}

void OpenXRManager::StopFrameThread()
{
  if (!m_frame_thread.joinable())
    return;

  m_frame_thread_should_exit.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(m_publish_mutex);
    m_publish_cv.notify_all();
  }
  m_frame_thread.join();
  m_frame_thread_running.store(false, std::memory_order_release);
  INFO_LOG_FMT(OPENXR, "OpenXR: XR pacing thread stopped.");
}

void OpenXRManager::ShutdownSession()
{
  StopFrameThread();

  if (m_session == XR_NULL_HANDLE || !m_session_running.load(std::memory_order_acquire))
    return;

  INFO_LOG_FMT(OPENXR, "OpenXR: Requesting running session exit before teardown.");
  const XrResult request_result = xrRequestExitSession(m_session);
  if (XR_FAILED(request_result))
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: xrRequestExitSession failed during teardown ({}).",
                 static_cast<int>(request_result));
    return;
  }

  // xrRequestExitSession is asynchronous. Give the runtime enough time to report STOPPING,
  // whose event handler performs the only spec-valid xrEndSession call, and then EXITING. Waiting
  // for the terminal event is important on PC runtimes: the compositor can still be consuming the
  // last submitted swapchain image after xrEndSession has merely made the session non-running.
  // Destruction remains legal from any state, so a misbehaving runtime cannot hang shutdown.
  constexpr auto shutdown_timeout = std::chrono::milliseconds(500);
  const auto deadline = std::chrono::steady_clock::now() + shutdown_timeout;
  while (m_session_state != XR_SESSION_STATE_EXITING &&
         m_session_state != XR_SESSION_STATE_LOSS_PENDING &&
         std::chrono::steady_clock::now() < deadline)
  {
    PollEvents();
    if (m_session_state != XR_SESSION_STATE_EXITING &&
        m_session_state != XR_SESSION_STATE_LOSS_PENDING)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (m_session_state != XR_SESSION_STATE_EXITING &&
      m_session_state != XR_SESSION_STATE_LOSS_PENDING)
  {
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: Runtime did not finish session exit within 500 ms (state={}); "
                 "continuing bounded teardown.",
                 static_cast<int>(m_session_state));
  }
}

void OpenXRManager::DestroySession()
{
  SetSwapchain(nullptr);
  ShutdownSession();
  DestroyInputActions();
  ResetInputActionsState();
  DestroyTabletopHandMeshes();
  DestroyFBPassthrough();

  if (m_reference_space != XR_NULL_HANDLE)
  {
    xrDestroySpace(m_reference_space);
    m_reference_space = XR_NULL_HANDLE;
  }

  if (m_session != XR_NULL_HANDLE)
  {
    xrDestroySession(m_session);
    m_session = XR_NULL_HANDLE;
  }

  // Everything below belongs to a session or its frame loop. Keep only the instance/system and
  // instance-level extension state so a subsequent game starts from the same state as a newly
  // constructed manager without forcing the runtime through VR_Shutdown/VR_Init again.
  m_session_state = XR_SESSION_STATE_UNKNOWN;
  m_session_running.store(false, std::memory_order_release);
  m_session_focused.store(false, std::memory_order_release);
  m_exit_render_loop = false;
  m_frame_state = {XR_TYPE_FRAME_STATE};
  m_view_state_flags = 0;
  m_predicted_display_time_snapshot.store(0, std::memory_order_release);
  m_predicted_display_period_snapshot.store(0, std::memory_order_release);
  m_should_render_snapshot.store(false, std::memory_order_release);
  m_last_predicted_display_time = 0;
  m_estimated_display_period_ms.store(0.0, std::memory_order_release);
  m_startup_display_refresh_rate_hz = 0.0f;

  {
    std::lock_guard<std::mutex> lock(m_publish_mutex);
    m_published_frame = {};
    m_published_frame.quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    m_publish_serial = 0;
  }
  m_video_handoff_active.store(0, std::memory_order_release);

  m_views = {};
  m_eye_views = {};
  m_rendered_eye_views = {};
  m_submitted_eye_views = {};
  m_present_eye_views = {};
  m_present_eye_views_valid = false;
  m_xfb_pose_stamps = {};
  m_xfb_pose_stamp_next = 0;
  m_xfb_pose_stamp_serial = 0;
  m_logged_interaction_profiles = {XR_NULL_PATH, XR_NULL_PATH};
  m_home_set = false;
  m_home_position = {0.f, 0.f, 0.f};
  m_recenter_requested.store(false, std::memory_order_release);
  m_flat_screen_pose_valid = false;
  m_flat_screen_pose = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
  m_flat_quad_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  m_controller_anchor_cache_valid = {false, false};
}

void OpenXRManager::PublishFrame(const std::array<XrCompositionLayerProjectionView, 2>& views,
                                 XrCompositionLayerFlags layer_flags)
{
  // Frames rendered before the first LocateViews of a session carry unseeded (all-zero
  // orientation) poses; submitting or heartbeating those gets XR_ERROR_POSE_INVALID.
  const auto valid_orientation = [](const XrQuaternionf& q) {
    return q.x != 0.0f || q.y != 0.0f || q.z != 0.0f || q.w != 0.0f;
  };
  if (!valid_orientation(views[0].pose.orientation) ||
      !valid_orientation(views[1].pose.orientation))
  {
    return;
  }

  std::lock_guard<std::mutex> lock(m_publish_mutex);
  m_published_frame.is_quad = false;
  m_published_frame.views = views;
  m_published_frame.layer_flags = layer_flags;
  m_publish_serial++;
  m_publish_cv.notify_all();
}

void OpenXRManager::PublishQuadFrame(const XrCompositionLayerQuad& quad)
{
  std::lock_guard<std::mutex> lock(m_publish_mutex);
  m_published_frame.is_quad = true;
  m_published_frame.quad = quad;
  m_publish_serial++;
  m_publish_cv.notify_all();
}

void OpenXRManager::FrameThreadLoop()
{
  Common::SetCurrentThreadName("OpenXR Pacing");
#if defined(ANDROID)
  // Meta's runtime applies big.LITTLE pinning / DVFS escalation to tagged threads.
  RegisterCurrentAndroidThread(AndroidThreadType::RendererWorker, "OpenXR Pacing");
  // A fast core, but off the CPU/Video cores so the wakeup-heavy pacing loop doesn't
  // steal cycles from the emulator's hot threads.
  if (Config::Get(Config::GFX_VR_PIN_EMULATION_CORES))
  {
    const int core =
        Common::PinCurrentThreadToPerformanceCore(Common::ThreadCoreRole::VRPacing);
    if (core >= 0)
      INFO_LOG_FMT(OPENXR, "OpenXR: Pinned pacing thread to performance core cpu{}.", core);
  }
#endif

  // Last content handed over by the game; re-submitted every display period while no
  // fresh frame arrives ("heartbeat"). The compositor reprojects it with the current
  // head pose (ATW), which is what makes sub-refresh-rate games feel smooth — the job
  // the old Opcode Replay re-rendering used to do, at zero GPU cost.
  PublishedXRFrame last_frame;
  bool have_frame = false;
  uint64_t consumed_serial = 0;

  // Rolling 5s instrumentation window.
  u64 stats_start_us = Common::Timer::NowUs();
  u32 stat_cycles = 0, stat_fresh = 0, stat_repeat = 0, stat_empty = 0;
  u32 stat_handoff_waited = 0;  // 50us slices spent waiting out a video-thread handoff
  double stat_wait_ms = 0.0, stat_end_ms = 0.0, stat_content_wait_ms = 0.0;

  // Smoothed xrEndFrame cost. Cheap on Quest (~0.5ms) but 1–5ms on PC runtimes like
  // Virtual Desktop; the content-wait budget must shrink accordingly or a full pacing
  // cycle exceeds the display period and the thread can no longer hold refresh rate.
  double endframe_ema_us = 500.0;
  u64 last_cycle_us = 0;

  // Heartbeat mode (g_ActiveConfig.vr_eager_heartbeat, read live each cycle so it can be
  // toggled mid-session for A/B testing):
  //
  // Eager (standalone default): run one cycle per display period and re-submit the last
  // frame whenever the game is slower — there is no runtime motion smoothing on Quest,
  // so filling every compositor slot is a straight win (measured: 72/72, 0 stales).
  //
  // Lazy (PC default): wait for the game to publish BEFORE starting a frame cycle, so the
  // runtime sees the app's true cadence. PC runtimes (Virtual Desktop, SteamVR, Meta
  // Link) key their motion smoothing (SSW/ASW) off the app frame rate — a full-rate
  // heartbeat makes them disable it and exposes the raw sub-refresh cadence beat as
  // head-tracking judder. Heartbeat then fires only as a keep-alive when content stops
  // flowing entirely (loading screens, shader compilation).

  while (!m_frame_thread_should_exit.load(std::memory_order_acquire))
  {
    const bool eager_heartbeat = g_ActiveConfig.vr_eager_heartbeat;
    const u64 cycle_start_us = Common::Timer::NowUs();
    if (!PollEvents())
      break;

    if (!m_session_running.load(std::memory_order_acquire))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    if (!eager_heartbeat)
    {
      // Pace to the game: block here (outside the XR frame protocol) until a frame is
      // published or the keep-alive interval elapses.
      std::unique_lock<std::mutex> lock(m_publish_mutex);
      m_publish_cv.wait_for(lock, std::chrono::milliseconds(150), [&] {
        return m_publish_serial != consumed_serial ||
               m_frame_thread_should_exit.load(std::memory_order_acquire);
      });
      if (m_frame_thread_should_exit.load(std::memory_order_acquire))
        break;
    }

    const u64 t0 = Common::Timer::NowUs();
    if (!WaitFrame())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    const u64 t1 = Common::Timer::NowUs();
    if (!BeginFrame())
      continue;

    const bool should_render = m_should_render_snapshot.load(std::memory_order_acquire);
    const int64_t period_ns = m_predicted_display_period_snapshot.load(std::memory_order_acquire);

    // Wait for the game to hand over a fresh frame for as long as this cycle can afford:
    // one display period minus the time already spent since xrWaitFrame returned, minus
    // the (smoothed) xrEndFrame cost on this runtime, minus a scheduling margin. This is
    // the longest pickup window that still keeps the full cycle within one period — a
    // shorter window makes frames slip a whole cycle (visible judder), a longer one
    // makes the pacing thread miss compositor deadlines (VDXR's xrEndFrame alone costs
    // 1–5 ms, vs ~0.5 ms on Quest).
    const u64 t2 = Common::Timer::NowUs();
    bool fresh = false;
    if (should_render)
    {
      const int64_t since_wait_ns = static_cast<int64_t>(t2 - t1) * 1000;
      int64_t budget_ns = period_ns - since_wait_ns -
                          static_cast<int64_t>(endframe_ema_us * 1000.0) - 1'000'000;
      budget_ns = std::clamp<int64_t>(budget_ns, 0, period_ns);
      // Catch-up: a healthy cycle equals one period (xrWaitFrame throttles), so only
      // treat >125% of a period as an overrun worth skipping the content wait for.
      if (static_cast<int64_t>(last_cycle_us) * 1000 > period_ns + period_ns / 4)
        budget_ns = 0;
      // Lazy mode already waited for content before the cycle started; just consume.
      if (!eager_heartbeat)
        budget_ns = 0;

      std::unique_lock<std::mutex> lock(m_publish_mutex);
      if (budget_ns > 0 && m_publish_serial == consumed_serial)
      {
        m_publish_cv.wait_for(lock, std::chrono::nanoseconds(budget_ns), [&] {
          return m_publish_serial != consumed_serial ||
                 m_frame_thread_should_exit.load(std::memory_order_acquire);
        });
      }
      if (m_publish_serial != consumed_serial)
      {
        last_frame = m_published_frame;
        consumed_serial = m_publish_serial;
        have_frame = true;
        fresh = true;
      }
    }

    // Never submit while the video thread is swapping the front swapchain image and
    // publishing its matching pose — landing in that window submits the new image with
    // the old pose (backwards ATW warp = a previously shown frame flashes). Bounded so
    // a stuck video thread can never hang the pacing loop; on the last iteration we
    // proceed anyway (a rare single-frame mismatch beats missing the compositor).
    for (int spins = 0;
         m_video_handoff_active.load(std::memory_order_acquire) > 0 && spins < 40; ++spins)
    {
      stat_handoff_waited++;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    // A publish may have completed during that wait; consume it so the pose we submit
    // matches the image the video thread just made front.
    {
      std::unique_lock<std::mutex> lock(m_publish_mutex);
      if (m_publish_serial != consumed_serial)
      {
        last_frame = m_published_frame;
        consumed_serial = m_publish_serial;
        have_frame = true;
        fresh = true;
      }
    }
    const u64 t3 = Common::Timer::NowUs();

    // Build the layer stack from the newest content we have. Passthrough layer
    // prepending, blend mode, and the graphics queue lock live in EndFrameDetached.
    XrCompositionLayerProjection projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerBaseHeader*> layers;
    const bool submit_content = should_render && have_frame;
    if (submit_content)
    {
      if (last_frame.is_quad)
      {
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&last_frame.quad));
      }
      else
      {
        projection_layer.layerFlags = last_frame.layer_flags;
        projection_layer.space = m_reference_space;
        projection_layer.viewCount = 2;
        projection_layer.views = last_frame.views.data();
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection_layer));
      }
    }

    EndFrameDetached(m_frame_state.predictedDisplayTime, GetActiveBlendMode(), submit_content,
                     layers);
    const u64 t4 = Common::Timer::NowUs();
    endframe_ema_us = endframe_ema_us * 0.8 + static_cast<double>(t4 - t3) * 0.2;
    last_cycle_us = t4 - cycle_start_us;

    stat_cycles++;
    if (fresh)
      stat_fresh++;
    else if (submit_content)
      stat_repeat++;
    else
      stat_empty++;
    stat_wait_ms += (t1 - t0) / 1000.0;
    stat_content_wait_ms += (t3 - t2) / 1000.0;
    stat_end_ms += (t4 - t3) / 1000.0;

    const u64 now_us = Common::Timer::NowUs();
    if (now_us - stats_start_us >= 5'000'000 && stat_cycles > 0)
    {
      INFO_LOG_FMT(OPENXR,
                   "XRPacing: {:.1f} cycles/s (fresh={} repeat={} empty={}) | per cycle: "
                   "xrWaitFrame={:.2f}ms content_wait={:.2f}ms xrEndFrame={:.2f}ms | "
                   "handoff_waits={} (x50us)",
                   stat_cycles / ((now_us - stats_start_us) / 1'000'000.0), stat_fresh,
                   stat_repeat, stat_empty, stat_wait_ms / stat_cycles,
                   stat_content_wait_ms / stat_cycles, stat_end_ms / stat_cycles,
                   stat_handoff_waited);
      stats_start_us = now_us;
      stat_cycles = stat_fresh = stat_repeat = stat_empty = 0;
      stat_handoff_waited = 0;
      stat_wait_ms = stat_end_ms = stat_content_wait_ms = 0.0;
    }
  }

  m_frame_thread_running.store(false, std::memory_order_release);
  INFO_LOG_FMT(OPENXR, "OpenXR: XR pacing thread exiting.");
}

void OpenXRManager::CaptureStartupDisplayRefreshRateFromExtension()
{
  if (m_startup_display_refresh_rate_hz > 0.0f || m_session == XR_NULL_HANDLE ||
      m_xrGetDisplayRefreshRateFB == nullptr)
  {
    return;
  }

  float refresh_rate_hz = 0.0f;
  const XrResult result = m_xrGetDisplayRefreshRateFB(m_session, &refresh_rate_hz);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: xrGetDisplayRefreshRateFB failed ({}).",
                 static_cast<int>(result));
    return;
  }

  SetStartupDisplayRefreshRate(refresh_rate_hz, "XR_FB_display_refresh_rate");
}

void OpenXRManager::SetStartupDisplayRefreshRate(float refresh_rate_hz, std::string_view source)
{
  if (m_startup_display_refresh_rate_hz > 0.0f || refresh_rate_hz <= 0.0f)
    return;

  m_startup_display_refresh_rate_hz = refresh_rate_hz;
  INFO_LOG_FMT(OPENXR, "OpenXR: Startup display refresh rate is {:.2f} Hz from {}.",
               m_startup_display_refresh_rate_hz, source);
  Config::OnConfigChanged();
}

const TabletopHandMesh* OpenXRManager::GetTabletopHandMesh(uint32_t hand) const
{
  if (hand >= m_tabletop_hand_meshes.size() || !m_tabletop_hand_meshes[hand].valid)
    return nullptr;
  return &m_tabletop_hand_meshes[hand];
}

void OpenXRManager::DestroyTabletopHandMeshes()
{
  if (m_simultaneous_hands_controllers_active && m_session != XR_NULL_HANDLE &&
      m_xrPauseSimultaneousHandsAndControllersTrackingMETA)
  {
    XrSimultaneousHandsAndControllersTrackingPauseInfoMETA pause_info{
        XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_PAUSE_INFO_META};
    m_xrPauseSimultaneousHandsAndControllersTrackingMETA(m_session, &pause_info);
  }
  m_simultaneous_hands_controllers_active = false;

  for (size_t hand = 0; hand < m_tabletop_hand_trackers.size(); ++hand)
  {
    if (m_tabletop_hand_trackers[hand] != XR_NULL_HANDLE && m_xrDestroyHandTrackerEXT)
      m_xrDestroyHandTrackerEXT(m_tabletop_hand_trackers[hand]);
    m_tabletop_hand_trackers[hand] = XR_NULL_HANDLE;
    m_tabletop_hand_meshes[hand] = {};
  }
}

bool OpenXRManager::InitializeTabletopHandMeshes()
{
  if (m_session == XR_NULL_HANDLE || !m_xrCreateHandTrackerEXT || !m_xrDestroyHandTrackerEXT ||
      !m_xrGetHandMeshFB)
  {
    return false;
  }

  DestroyTabletopHandMeshes();
  bool any_mesh = false;

  for (uint32_t hand = 0; hand < 2; ++hand)
  {
    XrHandTrackerCreateInfoEXT tracker_info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
    tracker_info.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
    tracker_info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;

    XrResult result =
        m_xrCreateHandTrackerEXT(m_session, &tracker_info, &m_tabletop_hand_trackers[hand]);
    if (XR_FAILED(result))
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: xrCreateHandTrackerEXT({}) failed ({}).", hand,
                   static_cast<int>(result));
      m_tabletop_hand_trackers[hand] = XR_NULL_HANDLE;
      continue;
    }

    XrHandTrackingMeshFB query{XR_TYPE_HAND_TRACKING_MESH_FB};
    result = m_xrGetHandMeshFB(m_tabletop_hand_trackers[hand], &query);
    if (XR_FAILED(result) || query.jointCountOutput == 0 || query.vertexCountOutput == 0 ||
        query.indexCountOutput == 0)
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: hand mesh size query for hand {} failed ({}) j={} v={} i={}.",
                   hand, static_cast<int>(result), query.jointCountOutput,
                   query.vertexCountOutput, query.indexCountOutput);
      m_xrDestroyHandTrackerEXT(m_tabletop_hand_trackers[hand]);
      m_tabletop_hand_trackers[hand] = XR_NULL_HANDLE;
      continue;
    }

    TabletopHandMesh& data = m_tabletop_hand_meshes[hand];
    data.joint_bind_poses.resize(query.jointCountOutput);
    data.joint_radii.resize(query.jointCountOutput);
    data.joint_parents.resize(query.jointCountOutput);
    data.vertex_positions.resize(query.vertexCountOutput);
    data.vertex_normals.resize(query.vertexCountOutput);
    data.vertex_uvs.resize(query.vertexCountOutput);
    data.vertex_blend_indices.resize(query.vertexCountOutput);
    data.vertex_blend_weights.resize(query.vertexCountOutput);
    data.indices.resize(query.indexCountOutput);

    XrHandTrackingMeshFB mesh{XR_TYPE_HAND_TRACKING_MESH_FB};
    mesh.jointCapacityInput = static_cast<uint32_t>(data.joint_bind_poses.size());
    mesh.jointBindPoses = data.joint_bind_poses.data();
    mesh.jointRadii = data.joint_radii.data();
    mesh.jointParents = data.joint_parents.data();
    mesh.vertexCapacityInput = static_cast<uint32_t>(data.vertex_positions.size());
    mesh.vertexPositions = data.vertex_positions.data();
    mesh.vertexNormals = data.vertex_normals.data();
    mesh.vertexUVs = data.vertex_uvs.data();
    mesh.vertexBlendIndices = data.vertex_blend_indices.data();
    mesh.vertexBlendWeights = data.vertex_blend_weights.data();
    mesh.indexCapacityInput = static_cast<uint32_t>(data.indices.size());
    mesh.indices = data.indices.data();

    result = m_xrGetHandMeshFB(m_tabletop_hand_trackers[hand], &mesh);
    if (XR_FAILED(result))
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: xrGetHandMeshFB({}) failed ({}).", hand,
                   static_cast<int>(result));
      data = {};
      m_xrDestroyHandTrackerEXT(m_tabletop_hand_trackers[hand]);
      m_tabletop_hand_trackers[hand] = XR_NULL_HANDLE;
      continue;
    }

    data.valid = mesh.jointCountOutput > 0 && mesh.vertexCountOutput > 0 && mesh.indexCountOutput > 0;
    if (data.valid)
    {
      any_mesh = true;
      INFO_LOG_FMT(OPENXR, "OpenXR: runtime hand mesh {} ready: {} vertices, {} indices, {} joints.",
                   hand == 0 ? "left" : "right", mesh.vertexCountOutput,
                   mesh.indexCountOutput, mesh.jointCountOutput);
    }

    // Keep the tracker alive for true optical hand tracking. XR_META_simultaneous_hands_and_controllers
    // remains paused, so Quest can naturally switch between controller input and hand tracking.
    // When optical joints are inactive, the renderer falls back to the controller-driven Valve/FK
    // hand pose; when they become valid, those runtime joints take priority.
  }

  return any_mesh;
}

bool OpenXRManager::InitializeInputActions()
{
  if (m_input_action_set != XR_NULL_HANDLE)
    return true;

  if (m_instance == XR_NULL_HANDLE || m_session == XR_NULL_HANDLE)
    return false;

  auto to_path = [this](const char* path) -> XrPath {
    XrPath xr_path = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(m_instance, path, &xr_path)))
      return XR_NULL_PATH;
    return xr_path;
  };

  m_input_hand_paths[0] = to_path("/user/hand/left");
  m_input_hand_paths[1] = to_path("/user/hand/right");
  if (m_input_hand_paths[0] == XR_NULL_PATH || m_input_hand_paths[1] == XR_NULL_PATH)
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Failed to create hand subaction paths.");
    return false;
  }

  XrActionSetCreateInfo action_set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  CopyOpenXRName(action_set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "dolphin_input");
  CopyOpenXRName(action_set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE,
                 "Dolphin Input");
  action_set_info.priority = 0;

  XrResult result = xrCreateActionSet(m_instance, &action_set_info, &m_input_action_set);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrCreateActionSet failed ({}).", static_cast<int>(result));
    return false;
  }

  const auto create_action = [this](XrAction* action, const char* name, const char* localized_name,
                                    XrActionType type) -> bool {
    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    CopyOpenXRName(action_info.actionName, XR_MAX_ACTION_NAME_SIZE, name);
    CopyOpenXRName(action_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
                   localized_name);
    action_info.actionType = type;
    action_info.countSubactionPaths = static_cast<uint32_t>(m_input_hand_paths.size());
    action_info.subactionPaths = m_input_hand_paths.data();

    const XrResult create_result = xrCreateAction(m_input_action_set, &action_info, action);
    if (XR_FAILED(create_result))
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: xrCreateAction('{}') failed ({}).", name,
                    static_cast<int>(create_result));
      return false;
    }
    return true;
  };

  if (!create_action(&m_action_primary_click, "primary_click", "Primary Button",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_secondary_click, "secondary_click", "Secondary Button",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_menu_click, "menu_click", "Menu Button",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_thumbstick_click, "thumbstick_click", "Thumbstick Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trigger_click, "trigger_click", "Trigger Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_squeeze_click, "squeeze_click", "Squeeze Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trigger_value, "trigger_value", "Trigger Value",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_squeeze_value, "squeeze_value", "Squeeze Value",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_squeeze_force, "squeeze_force", "Squeeze Force",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_thumbstick_x, "thumbstick_x", "Thumbstick X",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_thumbstick_y, "thumbstick_y", "Thumbstick Y",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_aim_pose, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT) ||
      !create_action(&m_action_grip_pose, "grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT) ||
      !create_action(&m_action_haptic, "haptic", "Haptic Output",
                     XR_ACTION_TYPE_VIBRATION_OUTPUT))
  {
    DestroyInputActions();
    return false;
  }

  struct BindingDef
  {
    XrAction action = XR_NULL_HANDLE;
    const char* path = nullptr;
  };

  const auto suggest_bindings = [this, &to_path](const char* profile,
                                                 std::initializer_list<BindingDef> defs) {
    const XrPath profile_path = to_path(profile);
    if (profile_path == XR_NULL_PATH)
      return;

    std::vector<XrActionSuggestedBinding> bindings;
    bindings.reserve(defs.size());
    for (const auto& def : defs)
    {
      if (def.action == XR_NULL_HANDLE || def.path == nullptr)
        continue;
      const XrPath binding_path = to_path(def.path);
      if (binding_path == XR_NULL_PATH)
        continue;
      bindings.push_back({def.action, binding_path});
    }

    if (bindings.empty())
      return;

    XrInteractionProfileSuggestedBinding suggested{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = profile_path;
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    const XrResult suggest_result = xrSuggestInteractionProfileBindings(m_instance, &suggested);
#if defined(ANDROID)
    __android_log_print(ANDROID_LOG_INFO, "ACVRBindings", "profile=%s result=%d bindings=%u",
                        profile, static_cast<int>(suggest_result),
                        static_cast<unsigned>(bindings.size()));
#endif
    if (XR_FAILED(suggest_result))
    {
      WARN_LOG_FMT(OPENXR,
                   "OpenXR: xrSuggestInteractionProfileBindings('{}') failed ({}). "
                   "This profile may not be supported by the active runtime.",
                   profile, static_cast<int>(suggest_result));
    }
    else
    {
      INFO_LOG_FMT(OPENXR, "OpenXR: Suggested {} bindings for profile '{}'.",
                   bindings.size(), profile);
    }
  };

  suggest_bindings("/interaction_profiles/khr/simple_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/select/click"},
                       {m_action_primary_click, "/user/hand/right/input/select/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_menu_click, "/user/hand/right/input/menu/click"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  // Oculus Touch: NO trigger/click or squeeze/click in spec (only trigger/value + squeeze/value).
  suggest_bindings("/interaction_profiles/oculus/touch_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/x/click"},
                       {m_action_secondary_click, "/user/hand/left/input/y/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_menu_click, "/user/hand/right/input/system/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  // Meta Touch Plus profiles are core-promoted on current Quest runtimes. Do not gate binding
  // suggestions on XR_META_touch_controller_plus: Horizon OS may expose the promoted profile
  // without advertising/enabling the legacy extension name. Unsupported profiles simply cause
  // xrSuggestInteractionProfileBindings to fail harmlessly inside suggest_bindings().
  suggest_bindings("/interaction_profiles/meta/touch_plus_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/x/click"},
                       {m_action_secondary_click, "/user/hand/left/input/y/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_menu_click, "/user/hand/right/input/system/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  // Meta Touch Controller Plus (Quest 3): NO trigger/click or squeeze/click.
  // Same extension gates both touch_plus_controller and touch_controller_plus.
  suggest_bindings("/interaction_profiles/meta/touch_controller_plus",
                   {
                       {m_action_primary_click, "/user/hand/left/input/x/click"},
                       {m_action_secondary_click, "/user/hand/left/input/y/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_menu_click, "/user/hand/right/input/system/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  // Meta Quest 2 controller (1.1 core profile): NO trigger/click or squeeze/click.
  // Available on 1.1 runtimes without extension; will gracefully fail on 1.0.
  suggest_bindings("/interaction_profiles/meta/touch_controller_quest_2",
                   {
                       {m_action_primary_click, "/user/hand/left/input/x/click"},
                       {m_action_secondary_click, "/user/hand/left/input/y/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_menu_click, "/user/hand/right/input/system/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });
  // Valve Index: has trigger/click but NOT squeeze/click (has squeeze/value + squeeze/force).
  suggest_bindings("/interaction_profiles/valve/index_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/a/click"},
                       {m_action_secondary_click, "/user/hand/left/input/b/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_trigger_click, "/user/hand/left/input/trigger/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_squeeze_force, "/user/hand/left/input/squeeze/force"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_trigger_click, "/user/hand/right/input/trigger/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_squeeze_force, "/user/hand/right/input/squeeze/force"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  // ByteDance Pico controllers require XR_BD_controller_interaction extension.
  if (IsExtensionEnabled(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME))
  {
  suggest_bindings("/interaction_profiles/bytedance/pico4_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/x/click"},
                       {m_action_secondary_click, "/user/hand/left/input/y/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_trigger_click, "/user/hand/left/input/trigger/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/left/input/squeeze/click"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_menu_click, "/user/hand/right/input/system/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_trigger_click, "/user/hand/right/input/trigger/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/right/input/squeeze/click"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  suggest_bindings("/interaction_profiles/bytedance/pico_neo3_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/x/click"},
                       {m_action_secondary_click, "/user/hand/left/input/y/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_trigger_click, "/user/hand/left/input/trigger/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/left/input/squeeze/click"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_menu_click, "/user/hand/right/input/system/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_trigger_click, "/user/hand/right/input/trigger/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/right/input/squeeze/click"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });
  }  // XR_BD_controller_interaction

  suggest_bindings("/interaction_profiles/microsoft/motion_controller",
                   {
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/left/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_menu_click, "/user/hand/right/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/right/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  suggest_bindings("/interaction_profiles/htc/vive_controller",
                   {
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/trackpad/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/trackpad/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/trackpad/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/left/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_menu_click, "/user/hand/right/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/trackpad/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/trackpad/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/trackpad/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/right/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  const XrActionSet action_set = m_input_action_set;
  attach_info.countActionSets = 1;
  attach_info.actionSets = &action_set;
  result = xrAttachSessionActionSets(m_session, &attach_info);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrAttachSessionActionSets failed ({}).", static_cast<int>(result));
    DestroyInputActions();
    return false;
  }
  INFO_LOG_FMT(OPENXR, "OpenXR: xrAttachSessionActionSets succeeded.");

  auto create_action_space = [this](XrAction action, XrPath subaction_path, XrSpace* out_space,
                                    const char* label) {
    if (action == XR_NULL_HANDLE || subaction_path == XR_NULL_PATH)
      return;

    XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    space_info.action = action;
    space_info.subactionPath = subaction_path;
    space_info.poseInActionSpace = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
    const XrResult space_result = xrCreateActionSpace(m_session, &space_info, out_space);
    if (XR_FAILED(space_result))
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: xrCreateActionSpace('{}') failed ({}).", label,
                   static_cast<int>(space_result));
    }
  };

  for (size_t hand = 0; hand < m_input_hand_paths.size(); ++hand)
  {
    create_action_space(m_action_aim_pose, m_input_hand_paths[hand], &m_aim_spaces[hand], "aim");
    create_action_space(m_action_grip_pose, m_input_hand_paths[hand], &m_grip_spaces[hand],
                        "grip");
  }

  INFO_LOG_FMT(OPENXR, "OpenXR: Input action system initialized — "
                        "action set 'dolphin_input' with 14 actions, spaces created for both hands.");
  return true;
}

void OpenXRManager::DestroyInputActions()
{
  for (auto& space : m_aim_spaces)
  {
    if (space != XR_NULL_HANDLE)
      xrDestroySpace(space);
    space = XR_NULL_HANDLE;
  }
  for (auto& space : m_grip_spaces)
  {
    if (space != XR_NULL_HANDLE)
      xrDestroySpace(space);
    space = XR_NULL_HANDLE;
  }

  if (m_input_action_set != XR_NULL_HANDLE)
  {
    xrDestroyActionSet(m_input_action_set);
    m_input_action_set = XR_NULL_HANDLE;
  }

  m_input_hand_paths = {XR_NULL_PATH, XR_NULL_PATH};
  m_action_primary_click = XR_NULL_HANDLE;
  m_action_secondary_click = XR_NULL_HANDLE;
  m_action_menu_click = XR_NULL_HANDLE;
  m_action_thumbstick_click = XR_NULL_HANDLE;
  m_action_trigger_click = XR_NULL_HANDLE;
  m_action_squeeze_click = XR_NULL_HANDLE;
  m_action_trigger_value = XR_NULL_HANDLE;
  m_action_squeeze_value = XR_NULL_HANDLE;
  m_action_squeeze_force = XR_NULL_HANDLE;
  m_action_thumbstick_x = XR_NULL_HANDLE;
  m_action_thumbstick_y = XR_NULL_HANDLE;
  m_action_aim_pose = XR_NULL_HANDLE;
  m_action_grip_pose = XR_NULL_HANDLE;
  m_action_haptic = XR_NULL_HANDLE;
  m_haptics_active = {false, false};
}

void OpenXRManager::ResetInputActionsState()
{
  m_haptics_active = {false, false};
  m_tabletop_grab_active = false;
  Common::VR::OpenXRInputState::Reset();
}

void OpenXRManager::UpdateTabletopManipulation(
    std::array<Common::VR::OpenXRControllerState, 2>* controllers)
{
  if (!controllers)
    return;

  if (!IsTabletopModeActive())
  {
    // Runtime camera toggling should not destroy the user's tabletop placement. Stop an active
    // two-hand grab while classic view is selected, but preserve translation/yaw/scale so the
    // same diorama comes back when the right stick is clicked again.
    m_tabletop_grab_active = false;
    return;
  }

  auto& left = (*controllers)[0];
  auto& right = (*controllers)[1];
  const bool grabbing = left.squeeze_button && right.squeeze_button && left.grip_pose.valid &&
                        right.grip_pose.valid;
  if (!grabbing)
  {
    m_tabletop_grab_active = false;
    return;
  }

  const std::array<float, 3> midpoint = {
      0.5f * (left.grip_pose.position[0] + right.grip_pose.position[0]),
      0.5f * (left.grip_pose.position[1] + right.grip_pose.position[1]),
      0.5f * (left.grip_pose.position[2] + right.grip_pose.position[2])};
  const float hand_dx = right.grip_pose.position[0] - left.grip_pose.position[0];
  const float hand_dy = right.grip_pose.position[1] - left.grip_pose.position[1];
  const float hand_dz = right.grip_pose.position[2] - left.grip_pose.position[2];
  const float hand_distance =
      std::sqrt(hand_dx * hand_dx + hand_dy * hand_dy + hand_dz * hand_dz);
  const float hand_yaw = std::atan2(hand_dz, hand_dx);

  if (!m_tabletop_grab_active)
  {
    m_tabletop_grab_active = true;
    m_tabletop_grab_start_midpoint = midpoint;
    m_tabletop_grab_start_hand_yaw_rad = hand_yaw;
    m_tabletop_grab_start_hand_distance_m = std::max(hand_distance, 0.08f);
    m_tabletop_grab_start_offset_m = m_tabletop_user_offset_m;
    m_tabletop_grab_start_yaw_rad = m_tabletop_user_yaw_rad;
    m_tabletop_grab_start_scale = m_tabletop_user_scale;
  }
  else
  {
    for (size_t axis = 0; axis < 3; ++axis)
    {
      m_tabletop_user_offset_m[axis] =
          m_tabletop_grab_start_offset_m[axis] + midpoint[axis] - m_tabletop_grab_start_midpoint[axis];
    }

    constexpr float PI = 3.14159265358979323846f;
    float yaw_delta = hand_yaw - m_tabletop_grab_start_hand_yaw_rad;
    while (yaw_delta > PI)
      yaw_delta -= 2.0f * PI;
    while (yaw_delta < -PI)
      yaw_delta += 2.0f * PI;
    m_tabletop_user_yaw_rad = m_tabletop_grab_start_yaw_rad + yaw_delta;

    // Moving the hands apart zooms in (larger board), moving them together zooms out.
    // UnitsPerMeter is inverse physical scale, hence the division by the distance ratio.
    const float distance_ratio = hand_distance / m_tabletop_grab_start_hand_distance_m;
    if (distance_ratio > 0.05f)
    {
      m_tabletop_user_scale =
          std::clamp(m_tabletop_grab_start_scale / distance_ratio, 0.20f, 5.0f);
    }
  }

  // The two-grip gesture is a VR manipulation mode, not a GameCube input chord. Keep tracking
  // poses intact but neutralize gameplay controls until the board is released.
  for (auto& controller : *controllers)
  {
    controller.primary_button = false;
    controller.secondary_button = false;
    controller.menu_button = false;
    controller.thumbstick_button = false;
    controller.trigger_button = false;
    controller.squeeze_button = false;
    controller.trigger_value = 0.0f;
    controller.squeeze_value = 0.0f;
    controller.squeeze_force = 0.0f;
    controller.thumbstick_x = 0.0f;
    controller.thumbstick_y = 0.0f;
  }
}

void OpenXRManager::UpdateHaptics()
{
  if (!m_session_running || m_session == XR_NULL_HANDLE || m_action_haptic == XR_NULL_HANDLE)
    return;

  const auto haptics = Common::VR::OpenXRInputState::GetHaptics();

  // Re-send short pulses while active to approximate continuous rumble.
  constexpr XrDuration vibration_duration_ns = 50'000'000;

  for (size_t hand = 0; hand < m_input_hand_paths.size(); ++hand)
  {
    const XrPath hand_path = m_input_hand_paths[hand];
    if (hand_path == XR_NULL_PATH)
      continue;

    const float amplitude = std::clamp(haptics.amplitude[hand], 0.0f, 1.0f);

    XrHapticActionInfo action_info{XR_TYPE_HAPTIC_ACTION_INFO};
    action_info.action = m_action_haptic;
    action_info.subactionPath = hand_path;

    if (amplitude > 0.001f)
    {
      XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
      vibration.amplitude = amplitude;
      vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
      vibration.duration = vibration_duration_ns;

      xrApplyHapticFeedback(
          m_session, &action_info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
      m_haptics_active[hand] = true;
    }
    else if (m_haptics_active[hand])
    {
      xrStopHapticFeedback(m_session, &action_info);
      m_haptics_active[hand] = false;
    }
  }
}

XrTime OpenXRManager::GetInputSampleTime()
{
  const XrTime display_time = m_frame_state.predictedDisplayTime;
#if defined(_WIN32)
  if (!IsExtensionEnabled(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME))
    return display_time;
  if (!m_pfn_convert_now_to_time)
  {
    xrGetInstanceProcAddr(m_instance, "xrConvertWin32PerformanceCounterToTimeKHR",
                          &m_pfn_convert_now_to_time);
  }
  if (m_pfn_convert_now_to_time)
  {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    XrTime now_time = 0;
    if (XR_SUCCEEDED(reinterpret_cast<PFN_xrConvertWin32PerformanceCounterToTimeKHR>(
            m_pfn_convert_now_to_time)(m_instance, &qpc, &now_time)) &&
        now_time > 0)
    {
      return std::min(display_time, now_time);
    }
  }
#elif defined(ANDROID)
  if (!IsExtensionEnabled(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME))
    return display_time;
  if (!m_pfn_convert_now_to_time)
  {
    xrGetInstanceProcAddr(m_instance, "xrConvertTimespecTimeToTimeKHR",
                          &m_pfn_convert_now_to_time);
  }
  if (m_pfn_convert_now_to_time)
  {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    XrTime now_time = 0;
    if (XR_SUCCEEDED(reinterpret_cast<PFN_xrConvertTimespecTimeToTimeKHR>(
            m_pfn_convert_now_to_time)(m_instance, &ts, &now_time)) &&
        now_time > 0)
    {
      return std::min(display_time, now_time);
    }
  }
#endif
  return display_time;
}

void OpenXRManager::UpdateInputActions()
{
  if (!m_session_running || m_session == XR_NULL_HANDLE || m_input_action_set == XR_NULL_HANDLE)
  {
    ResetInputActionsState();
    return;
  }

  const XrActiveActionSet active_action_set{m_input_action_set, XR_NULL_PATH};
  XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
  sync_info.countActiveActionSets = 1;
  sync_info.activeActionSets = &active_action_set;
  const XrResult sync_result = xrSyncActions(m_session, &sync_info);
  if (sync_result == XR_SESSION_NOT_FOCUSED)
  {
    // XR_SESSION_NOT_FOCUSED is a success code but means input is inactive.
    // Don't reset state — keep previous connected status to avoid flicker in UI.
    return;
  }
  if (XR_FAILED(sync_result))
  {
    ResetInputActionsState();
    return;
  }

  std::array<Common::VR::OpenXRControllerState, 2> controllers{};

  // Rendering-coupled consumers (Controller Anchor) need poses at the predicted display
  // time so anchored elements stay glued to the rendered frame; pure input consumers (the
  // Wii pointer, motion) want measured poses at "now" — prediction extrapolates fast
  // controller motion several frames ahead and sprays the aim ray.
  const XrTime input_time = GetInputSampleTime();

  const auto locate_space_state = [this](XrSpace space, XrTime time,
                                         Common::VR::OpenXRPoseState* pose_state,
                                         Common::VR::OpenXRVelocityState* velocity_state) {
    if (space == XR_NULL_HANDLE || m_reference_space == XR_NULL_HANDLE)
      return;

    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    location.next = &velocity;

    if (XR_FAILED(xrLocateSpace(space, m_reference_space, time, &location)))
    {
      return;
    }

    constexpr XrSpaceLocationFlags required_flags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                                    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    pose_state->valid = (location.locationFlags & required_flags) == required_flags;
    if (pose_state->valid)
    {
      pose_state->position = {location.pose.position.x, location.pose.position.y,
                              location.pose.position.z};
      pose_state->orientation = {location.pose.orientation.x, location.pose.orientation.y,
                                 location.pose.orientation.z, location.pose.orientation.w};
    }

    if (velocity_state)
    {
      velocity_state->linear_valid =
          (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
      if (velocity_state->linear_valid)
      {
        velocity_state->linear = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                  velocity.linearVelocity.z};
      }

      velocity_state->angular_valid =
          (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
      if (velocity_state->angular_valid)
      {
        velocity_state->angular = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                   velocity.angularVelocity.z};
      }
    }
  };

  static uint64_t s_action_diag_counter = 0;
  static std::array<float, 2> s_peak_trigger{};
  static std::array<float, 2> s_peak_squeeze{};
  static std::array<bool, 2> s_peak_primary{};
  static std::array<bool, 2> s_peak_secondary{};
  const bool log_action_diag = (++s_action_diag_counter % 300) == 1;

  for (size_t hand = 0; hand < controllers.size(); ++hand)
  {
    const XrPath hand_path = m_input_hand_paths[hand];
    auto& controller = controllers[hand];

    XrInteractionProfileState interaction_profile_state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (hand_path != XR_NULL_PATH &&
        XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_session, hand_path, &interaction_profile_state)) &&
        interaction_profile_state.interactionProfile != m_logged_interaction_profiles[hand])
    {
      m_logged_interaction_profiles[hand] = interaction_profile_state.interactionProfile;
      const std::string profile_string =
          PathToString(m_instance, interaction_profile_state.interactionProfile);
      INFO_LOG_FMT(OPENXR, "OpenXR: Hand {} interaction profile: {}",
                   hand == 0 ? "left" : "right",
                   profile_string.empty() ? "<none>" : profile_string);
    }

    bool action_seen = false;
    bool trigger_action_active = false;
    bool squeeze_action_active = false;
    bool aim_action_active = false;
    bool grip_action_active = false;
    const auto get_boolean = [this, hand_path, &action_seen](XrAction action) -> bool {
      XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
      get_info.action = action;
      get_info.subactionPath = hand_path;
      XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
      if (XR_FAILED(xrGetActionStateBoolean(m_session, &get_info, &state)))
        return false;
      action_seen |= (state.isActive == XR_TRUE);
      return state.currentState == XR_TRUE;
    };

    const auto get_float = [this, hand_path, &action_seen](XrAction action,
                                                           bool* out_active = nullptr) -> float {
      XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
      get_info.action = action;
      get_info.subactionPath = hand_path;
      XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
      if (XR_FAILED(xrGetActionStateFloat(m_session, &get_info, &state)))
      {
        if (out_active)
          *out_active = false;
        return 0.0f;
      }
      const bool active = state.isActive == XR_TRUE;
      if (out_active)
        *out_active = active;
      action_seen |= active;
      return state.currentState;
    };

    const auto get_pose_active = [this, hand_path](XrAction action) -> bool {
      XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
      get_info.action = action;
      get_info.subactionPath = hand_path;
      XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
      return XR_SUCCEEDED(xrGetActionStatePose(m_session, &get_info, &state)) &&
             state.isActive == XR_TRUE;
    };

    controller.primary_button = get_boolean(m_action_primary_click);
    controller.secondary_button = get_boolean(m_action_secondary_click);
    controller.menu_button = get_boolean(m_action_menu_click);
    controller.thumbstick_button = get_boolean(m_action_thumbstick_click);

    const bool trigger_click = get_boolean(m_action_trigger_click);
    const bool squeeze_click = get_boolean(m_action_squeeze_click);

    controller.trigger_value =
        std::clamp(get_float(m_action_trigger_value, &trigger_action_active), 0.0f, 1.0f);
    controller.squeeze_value =
        std::clamp(get_float(m_action_squeeze_value, &squeeze_action_active), 0.0f, 1.0f);
    controller.squeeze_force = std::clamp(get_float(m_action_squeeze_force), 0.0f, 1.0f);
    controller.thumbstick_x = std::clamp(get_float(m_action_thumbstick_x), -1.0f, 1.0f);
    controller.thumbstick_y = std::clamp(get_float(m_action_thumbstick_y), -1.0f, 1.0f);
    aim_action_active = get_pose_active(m_action_aim_pose);
    grip_action_active = get_pose_active(m_action_grip_pose);
    action_seen |= aim_action_active || grip_action_active;

    locate_space_state(m_aim_spaces[hand], m_frame_state.predictedDisplayTime,
                       &controller.aim_pose, nullptr);
    // Match Meta's XrInput sample: action-space poses are located at the frame's predicted display
    // time. Querying Touch grip space at a converted wall-clock "now" can leave an otherwise active
    // pose with no valid location on Quest.
    locate_space_state(m_grip_spaces[hand], m_frame_state.predictedDisplayTime,
                       &controller.grip_pose, &controller.grip_velocity);

    // Quest 3 can optically track the real hand while Touch controllers remain active. Feed the
    // runtime skeleton to the hand renderer when available; this gives real per-finger motion
    // instead of approximating every finger from trigger/grip values.
    controller.hand_joints_valid = false;
    if (m_xrLocateHandJointsEXT && m_reference_space != XR_NULL_HANDLE &&
        m_tabletop_hand_trackers[hand] != XR_NULL_HANDLE)
    {
      std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> xr_joints{};
      XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
      locations.jointCount = static_cast<uint32_t>(xr_joints.size());
      locations.jointLocations = xr_joints.data();
      XrHandJointsLocateInfoEXT locate_info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
      locate_info.baseSpace = m_reference_space;
      locate_info.time = input_time;

      const XrResult hand_result =
          m_xrLocateHandJointsEXT(m_tabletop_hand_trackers[hand], &locate_info, &locations);
      if (XR_SUCCEEDED(hand_result) && locations.isActive == XR_TRUE)
      {
        size_t valid_joint_count = 0;
        for (size_t joint = 0; joint < xr_joints.size(); ++joint)
        {
          const auto& src = xr_joints[joint];
          auto& dst = controller.hand_joints[joint];
          const XrSpaceLocationFlags required =
              XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
          dst.valid = (src.locationFlags & required) == required;
          if (!dst.valid)
            continue;

          dst.position = {src.pose.position.x, src.pose.position.y, src.pose.position.z};
          dst.orientation = {src.pose.orientation.x, src.pose.orientation.y,
                             src.pose.orientation.z, src.pose.orientation.w};
          ++valid_joint_count;
        }
        controller.hand_joints_valid = valid_joint_count >= 20;
      }
    }

    // Learn the physical Touch grip -> optical wrist transform whenever both tracking sources are
    // available. This is substantially more accurate than a hard-coded Quest-controller offset and
    // remains cached for the rest of the XR session when optical hand tracking is later disabled.
    if (controller.hand_joints_valid && controller.grip_pose.valid &&
        controller.hand_joints[XR_HAND_JOINT_WRIST_EXT].valid)
    {
      const auto& grip = controller.grip_pose;
      const auto& wrist = controller.hand_joints[XR_HAND_JOINT_WRIST_EXT];
      const XrQuaternionf grip_q{grip.orientation[0], grip.orientation[1], grip.orientation[2],
                                 grip.orientation[3]};
      const XrQuaternionf inv_grip{-grip_q.x, -grip_q.y, -grip_q.z, grip_q.w};
      const XrQuaternionf wrist_q{wrist.orientation[0], wrist.orientation[1],
                                  wrist.orientation[2], wrist.orientation[3]};
      const XrQuaternionf relative_q = MultiplyQuaternions(inv_grip, wrist_q);
      const XrQuaternionf delta_q{wrist.position[0] - grip.position[0],
                                  wrist.position[1] - grip.position[1],
                                  wrist.position[2] - grip.position[2], 0.0f};
      const XrQuaternionf relative_p_q =
          MultiplyQuaternions(MultiplyQuaternions(inv_grip, delta_q), grip_q);

      Common::VR::OpenXRPoseState sample;
      sample.valid = true;
      sample.position = {relative_p_q.x, relative_p_q.y, relative_p_q.z};
      sample.orientation = {relative_q.x, relative_q.y, relative_q.z, relative_q.w};

      if (!m_touch_wrist_calibration_valid[hand])
      {
        m_touch_wrist_from_grip[hand] = sample;
        m_touch_wrist_calibration_valid[hand] = true;
      }
      else
      {
        // Low-pass the optical sample because simultaneous hand+controller tracking can jitter when
        // fingers are partially hidden by the Touch controller. Position uses a simple EMA;
        // orientation uses hemisphere-corrected normalized linear interpolation.
        constexpr float CALIBRATION_BLEND = 0.08f;
        auto& filtered = m_touch_wrist_from_grip[hand];
        for (size_t axis = 0; axis < 3; ++axis)
        {
          filtered.position[axis] +=
              (sample.position[axis] - filtered.position[axis]) * CALIBRATION_BLEND;
        }

        float dot = 0.0f;
        for (size_t axis = 0; axis < 4; ++axis)
          dot += filtered.orientation[axis] * sample.orientation[axis];
        const float sign = dot < 0.0f ? -1.0f : 1.0f;
        float length_sq = 0.0f;
        for (size_t axis = 0; axis < 4; ++axis)
        {
          filtered.orientation[axis] +=
              (sample.orientation[axis] * sign - filtered.orientation[axis]) * CALIBRATION_BLEND;
          length_sq += filtered.orientation[axis] * filtered.orientation[axis];
        }
        if (length_sq > 1.0e-8f)
        {
          const float inv_length = 1.0f / std::sqrt(length_sq);
          for (float& component : filtered.orientation)
            component *= inv_length;
        }
        filtered.valid = true;
      }
    }

    controller.hand_wrist_from_grip_valid = m_touch_wrist_calibration_valid[hand];
    if (controller.hand_wrist_from_grip_valid)
      controller.hand_wrist_from_grip = m_touch_wrist_from_grip[hand];

    // Absolute pointing target for the emulated Wii Remote IR: where the aim ray meets
    // the virtual screen the renderer is actually displaying. Uses a dedicated measured
    // "now" locate of the aim pose — the display-time aim above stays reserved for the
    // Controller Anchor, which must match the rendered frame.
    Common::VR::OpenXRPoseState input_aim;
    locate_space_state(m_aim_spaces[hand], input_time, &input_aim, nullptr);
    ComputeVirtualScreenHit(input_aim.valid ? input_aim : controller.aim_pose,
                            &controller.screen_hit);

    controller.trigger_button = trigger_click || controller.trigger_value > 0.5f;
    controller.squeeze_button =
        squeeze_click || std::max(controller.squeeze_value, controller.squeeze_force) > 0.5f;
    // Visual fallback hands must still articulate on runtimes that expose only digital click
    // states for a control. Quest Touch normally supplies analog values, but maxing with clicks
    // makes the fallback deterministic when system hand tracking is disabled or bindings vary.
    controller.hand_trigger_value =
        std::max(controller.trigger_value, trigger_click ? 1.0f : 0.0f);
    controller.hand_squeeze_value =
        std::max({controller.squeeze_value, controller.squeeze_force,
                  squeeze_click ? 1.0f : 0.0f});
    controller.hand_thumb_pressed =
        controller.primary_button || controller.secondary_button || controller.thumbstick_button;
    controller.connected = action_seen || controller.aim_pose.valid || controller.grip_pose.valid;
    s_peak_trigger[hand] = std::max(s_peak_trigger[hand], controller.hand_trigger_value);
    s_peak_squeeze[hand] = std::max(s_peak_squeeze[hand], controller.hand_squeeze_value);
    s_peak_primary[hand] = s_peak_primary[hand] || controller.primary_button;
    s_peak_secondary[hand] = s_peak_secondary[hand] || controller.secondary_button;
#if defined(ANDROID)
    if (log_action_diag)
    {
      const std::string profile = PathToString(m_instance, interaction_profile_state.interactionProfile);
      __android_log_print(
          ANDROID_LOG_INFO, "ACVRActionState",
          "%s profile=%s trigActive=%d squeezeActive=%d aimActive=%d gripActive=%d aimValid=%d gripValid=%d peakTrig=%.2f peakGrip=%.2f peakP=%d peakS=%d",
          hand == 0 ? "L" : "R", profile.empty() ? "<null>" : profile.c_str(),
          trigger_action_active ? 1 : 0, squeeze_action_active ? 1 : 0,
          aim_action_active ? 1 : 0, grip_action_active ? 1 : 0,
          controller.aim_pose.valid ? 1 : 0, controller.grip_pose.valid ? 1 : 0,
          s_peak_trigger[hand], s_peak_squeeze[hand], s_peak_primary[hand] ? 1 : 0,
          s_peak_secondary[hand] ? 1 : 0);
      s_peak_trigger[hand] = 0.0f;
      s_peak_squeeze[hand] = 0.0f;
      s_peak_primary[hand] = false;
      s_peak_secondary[hand] = false;
    }
#endif
  }

  // Right Touch thumbstick click switches between the room-anchored tabletop and the normal VR
  // camera without touching the saved INI. Consume the click so it never reaches the emulated
  // GameCube controller on the same frame.
  const bool right_stick_down = controllers[1].connected && controllers[1].thumbstick_button;
  if (right_stick_down && !m_tabletop_toggle_right_stick_was_down)
  {
    const bool enable_tabletop = !m_tabletop_runtime_enabled.load(std::memory_order_acquire);
    m_tabletop_runtime_enabled.store(enable_tabletop, std::memory_order_release);
    m_tabletop_reanchor_requested.store(true, std::memory_order_release);
    m_tabletop_grab_active = false;
    Core::System::GetInstance().GetGeometryShaderManager().InvalidateVRHeadPose();
    OSD::AddMessage(enable_tabletop ? "Mode plateau" : "Camera VR",
                    OSD::Duration::NORMAL);
  }
  m_tabletop_toggle_right_stick_was_down = right_stick_down;
  controllers[1].thumbstick_button = false;

  // Throttled periodic diagnostic log (~every 5 seconds at 60fps).
  static uint64_t s_sync_log_counter = 0;
  if ((++s_sync_log_counter % 300) == 1)
  {
    INFO_LOG_FMT(
        OPENXR,
        "OpenXR input: sync={}, focused={}, L(conn={} grip={} joints={} calib={} trig={:.2f} squeeze={:.2f} A={} B={}) R(conn={} grip={} joints={} calib={} trig={:.2f} squeeze={:.2f} A={} B={})",
        static_cast<int>(sync_result), m_session_focused.load(), controllers[0].connected,
        controllers[0].grip_pose.valid, controllers[0].hand_joints_valid,
        controllers[0].hand_wrist_from_grip_valid, controllers[0].hand_trigger_value,
        controllers[0].hand_squeeze_value, controllers[0].primary_button,
        controllers[0].secondary_button, controllers[1].connected,
        controllers[1].grip_pose.valid, controllers[1].hand_joints_valid,
        controllers[1].hand_wrist_from_grip_valid, controllers[1].hand_trigger_value,
        controllers[1].hand_squeeze_value, controllers[1].primary_button,
        controllers[1].secondary_button);
#if defined(ANDROID)
    __android_log_print(
        ANDROID_LOG_INFO, "ACVRInput",
        "L(conn=%d gripPose=%d trig=%.2f squeeze=%.2f X/A=%d Y/B=%d) R(conn=%d gripPose=%d trig=%.2f squeeze=%.2f A=%d B=%d)",
        controllers[0].connected ? 1 : 0, controllers[0].grip_pose.valid ? 1 : 0,
        controllers[0].hand_trigger_value, controllers[0].hand_squeeze_value,
        controllers[0].primary_button ? 1 : 0, controllers[0].secondary_button ? 1 : 0,
        controllers[1].connected ? 1 : 0, controllers[1].grip_pose.valid ? 1 : 0,
        controllers[1].hand_trigger_value, controllers[1].hand_squeeze_value,
        controllers[1].primary_button ? 1 : 0, controllers[1].secondary_button ? 1 : 0);
#endif
  }

  // Provide HMD head orientation for IR pointer reference direction.
  // Use left eye orientation as a proxy for head center (negligible difference from averaged).
  Common::VR::OpenXRPoseState head_pose;
  if (m_eye_views[0].pose.orientation.w != 0.0f || m_eye_views[0].pose.orientation.x != 0.0f ||
      m_eye_views[0].pose.orientation.y != 0.0f || m_eye_views[0].pose.orientation.z != 0.0f)
  {
    head_pose.valid = true;
    head_pose.orientation = {m_eye_views[0].pose.orientation.x,
                             m_eye_views[0].pose.orientation.y,
                             m_eye_views[0].pose.orientation.z,
                             m_eye_views[0].pose.orientation.w};
    head_pose.position = {m_eye_views[0].pose.position.x,
                          m_eye_views[0].pose.position.y,
                          m_eye_views[0].pose.position.z};
  }

  std::array<std::string, 2> profile_strings;
  for (size_t hand = 0; hand < 2; ++hand)
    profile_strings[hand] = PathToString(m_instance, m_logged_interaction_profiles[hand]);

  UpdateTabletopManipulation(&controllers);

  Common::VR::OpenXRInputState::SetControllers(controllers, true, head_pose,
                                               profile_strings, m_session_focused,
                                               input_time);

  UpdateHaptics();
}

bool OpenXRManager::CreateReferenceSpace()
{
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  // Identity pose: origin at (0,0,0), no rotation.
  space_info.poseInReferenceSpace = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};

  const OpenXRReferenceSpaceMode reference_space_mode = g_ActiveConfig.vr_reference_space_mode;
  const XrReferenceSpaceType requested_type =
      reference_space_mode == OpenXRReferenceSpaceMode::Local ? XR_REFERENCE_SPACE_TYPE_LOCAL :
                                                                XR_REFERENCE_SPACE_TYPE_STAGE;

  space_info.referenceSpaceType = requested_type;
  XrResult result = xrCreateReferenceSpace(m_session, &space_info, &m_reference_space);
  if (XR_FAILED(result) && requested_type == XR_REFERENCE_SPACE_TYPE_STAGE)
  {
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: STAGE reference space unavailable for default VR position mode; "
                 "falling back to LOCAL.");
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    result = xrCreateReferenceSpace(m_session, &space_info, &m_reference_space);
  }

  if (XR_FAILED(result))
  {
    char result_string[XR_MAX_RESULT_STRING_SIZE]{};
    xrResultToString(m_instance, result, result_string);
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrCreateReferenceSpace({}) failed: {}",
                  ReferenceSpaceTypeName(space_info.referenceSpaceType), result_string);
    return false;
  }

  m_reference_space_type = space_info.referenceSpaceType;
  m_home_set = false;
  m_home_position = {0.f, 0.f, 0.f};
  INFO_LOG_FMT(OPENXR, "OpenXR: Created {} reference space.",
               ReferenceSpaceTypeName(m_reference_space_type));
  return true;
}

bool OpenXRManager::PollEvents()
{
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};

  while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
  {
    switch (event.type)
    {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
    {
      const auto& ev = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
      HandleSessionStateChange(ev.state);
      break;
    }
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      WARN_LOG_FMT(OPENXR, "OpenXR: Instance loss pending — stopping VR.");
      m_exit_render_loop = true;
      ResetInputActionsState();
      return false;

    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      INFO_LOG_FMT(OPENXR, "OpenXR: Reference space change pending.");
      break;

    default:
      break;
    }

    // Reset for next poll.
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }

  return !m_exit_render_loop;
}

void OpenXRManager::HandleSessionStateChange(XrSessionState new_state)
{
  const char* state_name = "UNKNOWN";
  switch (new_state)
  {
  case XR_SESSION_STATE_UNKNOWN: state_name = "UNKNOWN"; break;
  case XR_SESSION_STATE_IDLE: state_name = "IDLE"; break;
  case XR_SESSION_STATE_READY: state_name = "READY"; break;
  case XR_SESSION_STATE_SYNCHRONIZED: state_name = "SYNCHRONIZED"; break;
  case XR_SESSION_STATE_VISIBLE: state_name = "VISIBLE"; break;
  case XR_SESSION_STATE_FOCUSED: state_name = "FOCUSED"; break;
  case XR_SESSION_STATE_STOPPING: state_name = "STOPPING"; break;
  case XR_SESSION_STATE_LOSS_PENDING: state_name = "LOSS_PENDING"; break;
  case XR_SESSION_STATE_EXITING: state_name = "EXITING"; break;
  default: break;
  }
  INFO_LOG_FMT(OPENXR, "OpenXR: Session state -> {} ({})", state_name, static_cast<int>(new_state));
  m_session_state = new_state;

  switch (new_state)
  {
  case XR_SESSION_STATE_READY:
  {
    XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
    begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (XR_SUCCEEDED(xrBeginSession(m_session, &begin_info)))
    {
      m_session_running = true;
      INFO_LOG_FMT(OPENXR, "OpenXR: Session running.");
#if defined(ANDROID)
      // Quest 3/3S expose the extra sustained CPU level through the manifest
      // CPU-for-GPU trade hint. Request sustained high when the session starts,
      // then re-issue after FOCUSED below for runtimes that defer perf changes.
      RequestAndroidHighPerformanceLevel();
#endif
    }
    else
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: xrBeginSession failed.");
    }
    break;
  }
  case XR_SESSION_STATE_SYNCHRONIZED:
    m_session_focused = false;
    break;

  case XR_SESSION_STATE_VISIBLE:
    m_session_focused = false;
    break;

  case XR_SESSION_STATE_FOCUSED:
    m_session_focused = true;
    INFO_LOG_FMT(OPENXR, "OpenXR: Session FOCUSED — controller input is now active.");
#if defined(ANDROID)
    // Meta Quest may ignore perf settings before the session is FOCUSED. Re-issue
    // here so the sustained-high request is applied to the active immersive app.
    RequestAndroidHighPerformanceLevel();
#endif
    break;

  case XR_SESSION_STATE_STOPPING:
  {
    const XrResult end_result = xrEndSession(m_session);
    if (XR_FAILED(end_result))
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: xrEndSession failed in STOPPING state ({}).",
                   static_cast<int>(end_result));
    }
    m_session_running = false;
    m_session_focused = false;
    ResetInputActionsState();
    INFO_LOG_FMT(OPENXR, "OpenXR: Session stopped.");
    break;
  }

  case XR_SESSION_STATE_LOSS_PENDING:
  case XR_SESSION_STATE_EXITING:
    m_exit_render_loop = true;
    m_session_running = false;
    m_session_focused = false;
    ResetInputActionsState();
    break;

  default:
    break;
  }
}

bool OpenXRManager::WaitFrame()
{
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  m_frame_state = {XR_TYPE_FRAME_STATE};
  XR_CHECK(xrWaitFrame(m_session, &wait_info, &m_frame_state));

  // Cross-thread snapshots: LocateViews/blit gating on the video thread read these
  // while the pacing thread owns m_frame_state itself.
  m_predicted_display_time_snapshot.store(m_frame_state.predictedDisplayTime,
                                          std::memory_order_release);
  m_predicted_display_period_snapshot.store(m_frame_state.predictedDisplayPeriod,
                                            std::memory_order_release);
  m_should_render_snapshot.store(m_frame_state.shouldRender == XR_TRUE,
                                 std::memory_order_release);

  if (m_startup_display_refresh_rate_hz <= 0.0f && m_frame_state.predictedDisplayPeriod > 0)
  {
    constexpr double ns_per_second = 1000000000.0;
    SetStartupDisplayRefreshRate(
        static_cast<float>(ns_per_second /
                           static_cast<double>(m_frame_state.predictedDisplayPeriod)),
        "first xrWaitFrame predicted display period");
  }

  if (m_last_predicted_display_time != 0 &&
      m_frame_state.predictedDisplayTime > m_last_predicted_display_time)
  {
    const XrTime delta = m_frame_state.predictedDisplayTime - m_last_predicted_display_time;
    constexpr double ns_to_ms = 1.0 / 1000000.0;
    const double delta_ms = static_cast<double>(delta) * ns_to_ms;
    if (delta_ms > 0.5 && delta_ms < 50.0)
    {
      const double prev = m_estimated_display_period_ms.load(std::memory_order_relaxed);
      m_estimated_display_period_ms.store(
          prev <= 0.0 ? delta_ms : (prev * 0.75) + (delta_ms * 0.25), std::memory_order_release);
    }
  }

  m_last_predicted_display_time = m_frame_state.predictedDisplayTime;
  UpdateInputActions();
  return true;
}

bool OpenXRManager::BeginFrame()
{
  if (m_swapchain && !m_swapchain->WaitForPendingFrameFinalization("before xrBeginFrame"))
    return false;

  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  {
    std::unique_lock<std::mutex> queue_lock;
    if (m_swapchain)
      queue_lock = m_swapchain->AcquireGraphicsQueueLock();
    const XrResult begin_result = xrBeginFrame(m_session, &begin_info);
    if (XR_FAILED(begin_result))
    {
      char buf[XR_MAX_RESULT_STRING_SIZE]{};
      xrResultToString(m_instance, begin_result, buf);
      ERROR_LOG_FMT(OPENXR, "OpenXR: xrBeginFrame failed: {}", buf);
      return false;
    }
  }
  return true;
}

bool OpenXRManager::SupportsAlphaBlend() const
{
  return std::find(m_supported_blend_modes.begin(), m_supported_blend_modes.end(),
                   XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) != m_supported_blend_modes.end();
}

bool OpenXRManager::IsFBPassthroughUsable() const
{
  return m_xrCreatePassthroughFB != nullptr && m_system_supports_fb_passthrough;
}

bool OpenXRManager::SupportsPassthrough() const
{
  return IsFBPassthroughUsable() || SupportsAlphaBlend();
}

XrEnvironmentBlendMode OpenXRManager::GetActiveBlendMode() const
{
  // XR_FB_passthrough composites its own layer behind an OPAQUE projection layer, so
  // ALPHA_BLEND is only the fallback for runtimes without the extension.
  if (IsTabletopModeActive() && g_ActiveConfig.VRPassthroughEnabled() &&
      !IsFBPassthroughUsable() && SupportsAlphaBlend())
    return XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
  return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
}

XrCompositionLayerFlags OpenXRManager::GetProjectionLayerExtraFlags() const
{
  // Per-pixel alpha lets the compositor reveal whatever sits behind the projection layer
  // (the FB passthrough layer, or the real world in an ALPHA_BLEND environment).
  if (IsTabletopModeActive() && g_ActiveConfig.VRPassthroughEnabled() && SupportsPassthrough())
  {
    return XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
           XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  }
  return 0;
}

void OpenXRManager::UpdateFBPassthrough(bool enable)
{
  if (!IsFBPassthroughUsable() || m_session == XR_NULL_HANDLE)
    return;

  if (enable && m_fb_passthrough == XR_NULL_HANDLE)
  {
    if (m_fb_passthrough_create_failed)
      return;

    XrPassthroughCreateInfoFB create_info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    create_info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    XrResult result = m_xrCreatePassthroughFB(m_session, &create_info, &m_fb_passthrough);
    if (XR_FAILED(result))
    {
      ERROR_LOG_FMT(OPENXR,
                    "OpenXR: xrCreatePassthroughFB failed ({}). Check that passthrough is "
                    "allowed in the headset settings (and 'Passthrough over Meta Horizon "
                    "Link' for Link).",
                    static_cast<int>(result));
      m_fb_passthrough = XR_NULL_HANDLE;
      m_fb_passthrough_create_failed = true;
      OSD::AddMessage("Passthrough disabled: the OpenXR runtime rejected passthrough creation.",
                      OSD::Duration::VERY_LONG);
      Config::SetBaseOrCurrent(Config::GFX_VR_PASSTHROUGH, false);
      g_ActiveConfig.vr_passthrough = false;
      return;
    }

    XrPassthroughLayerCreateInfoFB layer_info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    layer_info.passthrough = m_fb_passthrough;
    layer_info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    layer_info.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    result = m_xrCreatePassthroughLayerFB(m_session, &layer_info, &m_fb_passthrough_layer);
    if (XR_FAILED(result))
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: xrCreatePassthroughLayerFB failed ({}).",
                    static_cast<int>(result));
      m_xrDestroyPassthroughFB(m_fb_passthrough);
      m_fb_passthrough = XR_NULL_HANDLE;
      m_fb_passthrough_layer = XR_NULL_HANDLE;
      m_fb_passthrough_create_failed = true;
      OSD::AddMessage("Passthrough disabled: the OpenXR runtime rejected the passthrough layer.",
                      OSD::Duration::VERY_LONG);
      Config::SetBaseOrCurrent(Config::GFX_VR_PASSTHROUGH, false);
      g_ActiveConfig.vr_passthrough = false;
      return;
    }

    m_fb_passthrough_running = true;
    INFO_LOG_FMT(OPENXR, "OpenXR: XR_FB_passthrough feed started.");
    return;
  }

  if (m_fb_passthrough == XR_NULL_HANDLE || m_fb_passthrough_layer == XR_NULL_HANDLE)
    return;

  if (enable && !m_fb_passthrough_running)
  {
    const XrResult start_result = m_xrPassthroughStartFB(m_fb_passthrough);
    const XrResult resume_result = m_xrPassthroughLayerResumeFB(m_fb_passthrough_layer);
    m_fb_passthrough_running = XR_SUCCEEDED(start_result) && XR_SUCCEEDED(resume_result);
    if (m_fb_passthrough_running)
    {
      INFO_LOG_FMT(OPENXR, "OpenXR: XR_FB_passthrough feed resumed.");
    }
    else
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: Failed to resume XR_FB_passthrough (start={} resume={}).",
                   static_cast<int>(start_result), static_cast<int>(resume_result));
      if (!m_fb_passthrough_create_failed)
      {
        m_fb_passthrough_create_failed = true;
        OSD::AddMessage("Passthrough disabled: the OpenXR runtime stopped passthrough support.",
                        OSD::Duration::VERY_LONG);
        Config::SetBaseOrCurrent(Config::GFX_VR_PASSTHROUGH, false);
        g_ActiveConfig.vr_passthrough = false;
      }
    }
  }
  else if (!enable && m_fb_passthrough_running)
  {
    m_xrPassthroughLayerPauseFB(m_fb_passthrough_layer);
    m_xrPassthroughPauseFB(m_fb_passthrough);
    m_fb_passthrough_running = false;
    INFO_LOG_FMT(OPENXR, "OpenXR: XR_FB_passthrough feed paused.");
  }
}

void OpenXRManager::DestroyFBPassthrough()
{
  if (m_fb_passthrough_layer != XR_NULL_HANDLE && m_xrDestroyPassthroughLayerFB != nullptr)
    m_xrDestroyPassthroughLayerFB(m_fb_passthrough_layer);
  m_fb_passthrough_layer = XR_NULL_HANDLE;

  if (m_fb_passthrough != XR_NULL_HANDLE && m_xrDestroyPassthroughFB != nullptr)
    m_xrDestroyPassthroughFB(m_fb_passthrough);
  m_fb_passthrough = XR_NULL_HANDLE;

  m_fb_passthrough_running = false;
  m_fb_passthrough_create_failed = false;
}

bool OpenXRManager::EndFrame(const std::vector<XrCompositionLayerBaseHeader*>& layers)
{
  return EndFrameDetached(m_frame_state.predictedDisplayTime, GetActiveBlendMode(),
                          m_frame_state.shouldRender == XR_TRUE, layers);
}

bool OpenXRManager::EndFrameDetached(XrTime display_time,
                                     XrEnvironmentBlendMode environment_blend_mode,
                                     bool should_render,
                                     const std::vector<XrCompositionLayerBaseHeader*>& layers,
                                     bool lock_graphics_queue)
{
  // Keep the FB passthrough feed in sync with the Passthrough setting. Gated on a
  // registered swapchain so the controller-binding utility session never starts it.
  UpdateFBPassthrough(IsTabletopModeActive() && g_ActiveConfig.VRPassthroughEnabled() &&
                      m_swapchain != nullptr);

  // The passthrough layer composites first (behind the projection layer), replacing the
  // black void with the camera feed wherever the projection layer's alpha is 0.
  std::vector<XrCompositionLayerBaseHeader*> submit_layers;
  submit_layers.reserve(layers.size() + 1);
  if (m_fb_passthrough_running && m_fb_passthrough_layer != XR_NULL_HANDLE)
  {
    m_fb_passthrough_composition_layer = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    m_fb_passthrough_composition_layer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    m_fb_passthrough_composition_layer.space = XR_NULL_HANDLE;
    m_fb_passthrough_composition_layer.layerHandle = m_fb_passthrough_layer;
    submit_layers.push_back(
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_fb_passthrough_composition_layer));
  }
  submit_layers.insert(submit_layers.end(), layers.begin(), layers.end());

  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = display_time;
  end_info.environmentBlendMode = environment_blend_mode;

  // Only submit layers when the runtime requests rendering; otherwise submit 0 layers
  // (this handles the VISIBLE/SYNCHRONIZED states correctly).
  if (should_render)
  {
    end_info.layerCount = static_cast<uint32_t>(submit_layers.size());
    end_info.layers = submit_layers.data();
  }

  {
    std::unique_lock<std::mutex> queue_lock;
    if (lock_graphics_queue && m_swapchain)
      queue_lock = m_swapchain->AcquireGraphicsQueueLock();
    XR_CHECK(xrEndFrame(m_session, &end_info));
  }
  return true;
}

bool OpenXRManager::LocateViews()
{
  // Runs on the video/emu thread. With the pacing thread active, m_frame_state belongs
  // to that thread — use the cross-thread snapshot instead, one display period ahead
  // (the frame being rendered now will reach the compositor no earlier than that).
  const XrTime snapshot_time = m_predicted_display_time_snapshot.load(std::memory_order_acquire);
  if (snapshot_time == 0)
    return false;  // No xrWaitFrame yet this session.
  const XrTime display_time =
      IsFrameThreadActive() ?
          snapshot_time + m_predicted_display_period_snapshot.load(std::memory_order_acquire) :
          snapshot_time;

  XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate_info.displayTime = display_time;
  locate_info.space = m_reference_space;

  XrViewState view_state{XR_TYPE_VIEW_STATE};
  uint32_t view_count = static_cast<uint32_t>(m_views.size());
  m_views.fill({XR_TYPE_VIEW});

  XR_CHECK(xrLocateViews(m_session, &locate_info, &view_state,
                          view_count, &view_count, m_views.data()));
  m_view_state_flags = view_state.viewStateFlags;

  for (uint32_t i = 0; i < view_count; ++i)
  {
    m_eye_views[i].pose = m_views[i].pose;
    m_eye_views[i].fov = m_views[i].fov;
  }

  // Diagnostic: log raw xrLocateViews output every ~2 seconds.  We want to know if the
  // OpenXR runtime is actually returning different positions per eye (real IPD) or if
  // it's collapsing both eyes to the same point.  Real IPD ~ 0.060-0.070 m for adults.
  if (view_count >= 2)
  {
    static int s_locate_views_log_counter = 0;
    if ((s_locate_views_log_counter++ % 180) == 0)
    {
      const XrVector3f& p0 = m_views[0].pose.position;
      const XrVector3f& p1 = m_views[1].pose.position;
      const XrQuaternionf& o0 = m_views[0].pose.orientation;
      const XrQuaternionf& o1 = m_views[1].pose.orientation;
      const float dx = p1.x - p0.x;
      const float dy = p1.y - p0.y;
      const float dz = p1.z - p0.z;
      const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
      const XrFovf& f0 = m_views[0].fov;
      const XrFovf& f1 = m_views[1].fov;
      INFO_LOG_FMT(
          OPENXR,
          "VR_IPD_DBG: view_flags=0x{:x} | L_pos=({:.5f},{:.5f},{:.5f}) "
          "R_pos=({:.5f},{:.5f},{:.5f}) delta=({:.5f},{:.5f},{:.5f}) ipd={:.5f} m | "
          "L_quat=({:.4f},{:.4f},{:.4f},{:.4f}) R_quat=({:.4f},{:.4f},{:.4f},{:.4f}) | "
          "L_fov(L={:.4f},R={:.4f},U={:.4f},D={:.4f}) "
          "R_fov(L={:.4f},R={:.4f},U={:.4f},D={:.4f})",
          static_cast<uint32_t>(view_state.viewStateFlags),
          p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, dx, dy, dz, ipd,
          o0.x, o0.y, o0.z, o0.w, o1.x, o1.y, o1.z, o1.w,
          f0.angleLeft, f0.angleRight, f0.angleUp, f0.angleDown,
          f1.angleLeft, f1.angleRight, f1.angleUp, f1.angleDown);
    }
  }

  // Seed the render/submit pose snapshots on the very first locate so SubmitFrame has
  // a valid pose even before any VR draw has refreshed the GS cache.
  if (m_rendered_eye_views[0].pose.orientation.w == 0.0f &&
      m_rendered_eye_views[0].pose.orientation.x == 0.0f &&
      m_rendered_eye_views[0].pose.orientation.y == 0.0f &&
      m_rendered_eye_views[0].pose.orientation.z == 0.0f)
  {
    RecordRenderedEyeViews();
  }

  if (m_recenter_requested.exchange(false, std::memory_order_acq_rel) && view_count >= 2)
  {
    m_home_position.x = 0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
    m_home_position.y = 0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
    m_home_position.z = 0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);
    m_home_set = true;
    // Re-place the flat panel in front of the newly recentered head pose.
    m_flat_screen_pose_valid = false;
    INFO_LOG_FMT(OPENXR, "OpenXR: Recentered home position to ({:.4f},{:.4f},{:.4f})",
                 m_home_position.x, m_home_position.y, m_home_position.z);
  }

  return true;
}

void OpenXRManager::RequestRecenter()
{
  m_recenter_requested.store(true, std::memory_order_release);
}

XrPosef OpenXRManager::GetFlatScreenPose() const
{
  const float distance = g_ActiveConfig.vr_screen_distance;

  // Fall back to a world-origin panel until a real head pose is available. A zero orientation
  // means xrLocateViews has not produced a valid pose yet; don't cache that.
  const XrQuaternionf& q = m_eye_views[0].pose.orientation;
  const bool have_pose = (q.x != 0.f || q.y != 0.f || q.z != 0.f || q.w != 0.f);
  if (!have_pose)
    return {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, -distance}};

  if (m_flat_screen_pose_valid)
    return m_flat_screen_pose;

  // Head center and yaw-only heading, so the panel sits in front of the user, upright and
  // level (no pitch/roll), matching PPSSPP's flat-screen placement.
  const XrVector3f& p0 = m_eye_views[0].pose.position;
  const XrVector3f& p1 = m_eye_views[1].pose.position;
  const XrVector3f center{0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y), 0.5f * (p0.z + p1.z)};
  const float yaw =
      std::atan2(2.f * (q.x * q.z + q.w * q.y), 1.f - 2.f * (q.x * q.x + q.y * q.y));

  XrPosef pose{};
  pose.orientation = {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
  pose.position = {center.x - std::sin(yaw) * distance, center.y,
                   center.z - std::cos(yaw) * distance};
  m_flat_screen_pose = pose;
  m_flat_screen_pose_valid = true;
  return m_flat_screen_pose;
}

bool OpenXRManager::SubmitFlatQuadFrame(XrSwapchain swapchain, uint32_t width, uint32_t height)
{
  if (swapchain == XR_NULL_HANDLE || width == 0 || height == 0)
    return IsFrameThreadActive() ? true : EndFrame({});

  const float height_m = g_ActiveConfig.vr_screen_size;
  const float aspect =
      m_flat_screen_aspect > 0.f ? m_flat_screen_aspect : static_cast<float>(width) / height;

  m_flat_quad_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  m_flat_quad_layer.layerFlags = 0;
  m_flat_quad_layer.space = m_reference_space;
  m_flat_quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  m_flat_quad_layer.pose = GetFlatScreenPose();
  m_flat_quad_layer.size = {height_m * aspect, height_m};
  m_flat_quad_layer.subImage.swapchain = swapchain;
  m_flat_quad_layer.subImage.imageArrayIndex = 0;
  m_flat_quad_layer.subImage.imageRect.offset = {0, 0};
  m_flat_quad_layer.subImage.imageRect.extent = {static_cast<int32_t>(width),
                                                 static_cast<int32_t>(height)};

  if (IsFrameThreadActive())
  {
    PublishQuadFrame(m_flat_quad_layer);
    return true;
  }

  const std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_flat_quad_layer)};
  return EndFrame(layers);
}

bool IOpenXRSwapchain::SubmitFlatFrame()
{
  if (!g_openxr)
    return false;
  return g_openxr->SubmitFlatQuadFrame(GetFlatSwapchain(), GetEyeWidth(), GetEyeHeight());
}

void OpenXRManager::RecordRenderedEyeViews()
{
  m_rendered_eye_views = GetTrackingAdjustedEyeViews();
  if (g_ActiveConfig.vr_tracking_mode == OpenXRTrackingMode::None)
    m_submitted_eye_views = m_eye_views;
  else
    m_submitted_eye_views = m_rendered_eye_views;
}

void OpenXRManager::StampXFBPose(uint32_t xfb_addr)
{
  // Runs at the XFB copy in FIFO order: the just-copied XFB is the completed frame,
  // and m_submitted_eye_views still holds the pose its draws used — the next frame's
  // first draw (RecordRenderedEyeViews via the GS cache refresh) is what overwrites it.
  XFBPoseStamp* slot = nullptr;
  for (auto& stamp : m_xfb_pose_stamps)
  {
    if (stamp.serial != 0 && stamp.xfb_addr == xfb_addr)
    {
      slot = &stamp;
      break;
    }
  }
  if (!slot)
  {
    slot = &m_xfb_pose_stamps[m_xfb_pose_stamp_next];
    m_xfb_pose_stamp_next = (m_xfb_pose_stamp_next + 1) % m_xfb_pose_stamps.size();
  }
  slot->xfb_addr = xfb_addr;
  slot->serial = ++m_xfb_pose_stamp_serial;
  slot->views = m_submitted_eye_views;
}

void OpenXRManager::SelectPresentPoseForXFB(uint32_t xfb_addr)
{
  // Exact address match first: a VI duplicate present of an older XFB must keep that
  // XFB's own pose even after a newer copy stamped a different buffer. Fall back to
  // the newest stamp (stitched hybrid XFBs can present an address no single copy
  // used); with no stamps at all GetPresentEyeViews falls through to the live
  // snapshot, which is the pre-stamping behavior.
  const XFBPoseStamp* match = nullptr;
  const XFBPoseStamp* newest = nullptr;
  for (const auto& stamp : m_xfb_pose_stamps)
  {
    if (stamp.serial == 0)
      continue;
    if (stamp.xfb_addr == xfb_addr)
      match = &stamp;
    if (!newest || stamp.serial > newest->serial)
      newest = &stamp;
  }
  if (!match)
    match = newest;
  if (match)
  {
    m_present_eye_views = match->views;
    m_present_eye_views_valid = true;
  }
}

void OpenXRManager::EnsureHomePositionFromCurrentViews() const
{
  if (m_home_set)
    return;

  const float center_x = 0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
  const float center_y = 0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
  const float center_z = 0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);

  switch (g_ActiveConfig.vr_reference_space_mode)
  {
  case OpenXRReferenceSpaceMode::Stage:
    // In play-space center mode, Dolphin's VR origin follows the OpenXR reference-space origin.
    // With STAGE space this is the runtime's play area center; with fallback LOCAL space it is
    // the runtime-provided local origin.
    m_home_position = {0.f, 0.f, 0.f};
    m_home_set = true;
    INFO_LOG_FMT(OPENXR, "OpenXR: Home position set to reference-space origin ({})",
                 ReferenceSpaceTypeName(m_reference_space_type));
    return;

  case OpenXRReferenceSpaceMode::StageHeight:
    if (!m_session_focused || (m_view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
        (m_reference_space_type == XR_REFERENCE_SPACE_TYPE_STAGE && std::fabs(center_y) < 0.05f))
    {
      return;
    }
    m_home_position = {0.f, center_y, 0.f};
    m_home_set = true;
    INFO_LOG_FMT(OPENXR,
                 "OpenXR: Home position set to reference-space origin with headset height "
                 "({:.4f}) ({})",
                 m_home_position.y, ReferenceSpaceTypeName(m_reference_space_type));
    return;

  case OpenXRReferenceSpaceMode::Local:
    break;
  }

  if (!m_session_focused || (m_view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0)
    return;

  m_home_position = {center_x, center_y, center_z};
  m_home_set = true;
  INFO_LOG_FMT(OPENXR, "OpenXR: Home position set to initial headset position ({:.4f},{:.4f},{:.4f})",
               m_home_position.x, m_home_position.y, m_home_position.z);
}

std::array<XREyeView, 2> OpenXRManager::GetTrackingAdjustedEyeViews() const
{
  EnsureHomePositionFromCurrentViews();

  std::array<XREyeView, 2> eye_views = m_eye_views;
  if (g_ActiveConfig.vr_tracking_mode == OpenXRTrackingMode::Full6DoF)
    return eye_views;

  const float center_x = 0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
  const float center_y = 0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
  const float center_z = 0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);

  if (g_ActiveConfig.vr_tracking_mode == OpenXRTrackingMode::Rotation3DoF)
  {
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      eye_views[eye].pose.position.x =
          m_home_position.x + (m_eye_views[eye].pose.position.x - center_x);
      eye_views[eye].pose.position.y =
          m_home_position.y + (m_eye_views[eye].pose.position.y - center_y);
      eye_views[eye].pose.position.z =
          m_home_position.z + (m_eye_views[eye].pose.position.z - center_z);
    }
    return eye_views;
  }

  const float dx = m_eye_views[1].pose.position.x - m_eye_views[0].pose.position.x;
  const float dy = m_eye_views[1].pose.position.y - m_eye_views[0].pose.position.y;
  const float dz = m_eye_views[1].pose.position.z - m_eye_views[0].pose.position.z;
  const float half_ipd = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);

  eye_views[0].pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
  eye_views[1].pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
  eye_views[0].pose.position = {m_home_position.x - half_ipd, m_home_position.y,
                                m_home_position.z};
  eye_views[1].pose.position = {m_home_position.x + half_ipd, m_home_position.y,
                                m_home_position.z};
  return eye_views;
}

namespace
{
constexpr std::array<float, 9> CAMERA_ANCHOR_IDENTITY{1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                                      0.0f, 0.0f, 0.0f, 1.0f};

// Rebuild a right-handed orthonormal basis from the columns of a row-major 3x3
// (Gram-Schmidt via cross products, X column authoritative). Removes the element
// matrix's scale and repairs drift from per-entry smoothing. Returns false and
// leaves the matrix untouched when a column is degenerate.
bool OrthonormalizeAnchorRotation(std::array<float, 9>& m)
{
  float xx = m[0], xy = m[3], xz = m[6];
  const float yx = m[1], yy = m[4], yz = m[7];
  const float x_len = std::sqrt(xx * xx + xy * xy + xz * xz);
  if (x_len < 1e-5f)
    return false;
  xx /= x_len;
  xy /= x_len;
  xz /= x_len;
  // z = x cross y, then y = z cross x.
  float zx = xy * yz - xz * yy;
  float zy = xz * yx - xx * yz;
  float zz = xx * yy - xy * yx;
  const float z_len = std::sqrt(zx * zx + zy * zy + zz * zz);
  if (z_len < 1e-5f)
    return false;
  zx /= z_len;
  zy /= z_len;
  zz /= z_len;
  const float nyx = zy * xz - zz * xy;
  const float nyy = zz * xx - zx * xz;
  const float nyz = zx * xy - zy * xx;
  m = {xx, nyx, zx, xy, nyy, zy, xz, nyz, zz};
  return true;
}
}  // namespace

void OpenXRManager::SetPendingCameraAnchor(float x, float y, float z,
                                           const std::array<float, 9>& rotation,
                                           float units_per_meter)
{
  // First anchor draw of the frame wins; later matches (e.g. a second character
  // sharing the element signature) are ignored for determinism.
  if (m_camera_anchor_pending_valid)
    return;
  m_camera_anchor_pending_valid = true;
  m_camera_anchor_pending = {x, y, z};
  m_camera_anchor_pending_rotation = rotation;
  m_camera_anchor_pending_upm = units_per_meter > 0.0f ? units_per_meter : 0.0f;
}

float OpenXRManager::GetEffectiveUnitsPerMeter() const
{
  float upm = m_camera_anchor_upm > 0.0f ? m_camera_anchor_upm : g_ActiveConfig.vr_units_per_meter;
  if (IsTabletopModeActive())
    upm *= g_ActiveConfig.vr_tabletop_scale * m_tabletop_user_scale;
  return upm;
}

float OpenXRManager::GetTabletopUIPhysicalScale() const
{
  if (!IsTabletopModeActive())
    return 1.0f;

  // World physical size is inverse to UnitsPerMeter. Make UI follow that zoom with a square-root
  // response: a 50% smaller board yields about a 29% smaller UI, close to the requested half-strength
  // coupling while preserving readability. 6x is the original tabletop reference size.
  constexpr float REFERENCE_TABLETOP_SCALE = 6.0f;
  const float tabletop_scale =
      std::max(g_ActiveConfig.vr_tabletop_scale * m_tabletop_user_scale, 0.05f);
  const float ui_scale = std::sqrt(REFERENCE_TABLETOP_SCALE / tabletop_scale);
  return std::clamp(ui_scale, 0.50f, 1.50f);
}

float OpenXRManager::GetVirtualScreenDistanceMeters() const
{
  const float configured_distance = std::max(g_ActiveConfig.vr_screen_distance, 0.05f);
  if (!IsTabletopModeActive())
    return configured_distance;

  // In tabletop the synthetic screen is expressed relative to the board origin, not the user's
  // head. A long 1.5 m game-screen distance therefore drives dialogue down/away with the pitched
  // diorama. Compress it heavily: the stock 1.5 m setting becomes 0.25 m in board-local Z.
  return std::clamp(configured_distance / 6.0f, 0.18f, 0.45f);
}

float OpenXRManager::GetVirtualScreenVerticalOffsetMeters() const
{
  if (!IsTabletopModeActive())
    return 0.0f;

  // Lift dialogue/menu geometry in the tabletop's local Y before the rig pitch is applied. With
  // the default -45 degree pitch and 0.25 m local-Z distance this puts the panel centre roughly
  // 20 cm above the physical board instead of below/behind it.
  return 0.55f;
}

bool OpenXRManager::GetTabletopOcclusionPlane(TabletopOcclusionPlane* out_plane) const
{
  // Do not guess a plane before Animal Crossing's anchored camera has been captured. The old
  // fallback used tabletop local Y=0 directly, even though the renderer first transforms world
  // geometry into the anchored Camera2 basis; that made "above/below" disagree with the image.
  if (!out_plane || !IsTabletopModeActive() || !m_home_set ||
      !m_ac_tabletop_camera_anchor_valid)
  {
    return false;
  }

  constexpr float DEG_TO_RAD = 0.01745329252f;
  constexpr float AC_ACRE_SIZE = 640.0f;
  constexpr float PLAYER_EYE_HEIGHT = 33.0f;
  constexpr float FIELD_HALF_EXTENT_GAME = AC_ACRE_SIZE * 1.5f;  // native 3x3 acre field

  const float pitch = g_ActiveConfig.vr_tabletop_pitch * DEG_TO_RAD;
  const float half_pitch = pitch * 0.5f;
  const XrQuaternionf pitch_quat{std::sin(half_pitch), 0.0f, 0.0f,
                                 std::cos(half_pitch)};
  const float half_yaw = -0.5f * m_tabletop_user_yaw_rad;
  const XrQuaternionf yaw_quat{0.0f, std::sin(half_yaw), 0.0f, std::cos(half_yaw)};
  const XrQuaternionf tabletop_quat = MultiplyQuaternions(yaw_quat, pitch_quat);

  const auto add = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::array<float, 3>{a[0] + b[0], a[1] + b[1], a[2] + b[2]};
  };
  const auto sub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  };
  const auto mul = [](const std::array<float, 3>& v, float s) {
    return std::array<float, 3>{v[0] * s, v[1] * s, v[2] * s};
  };
  const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::array<float, 3>{a[1] * b[2] - a[2] * b[1],
                                a[2] * b[0] - a[0] * b[2],
                                a[0] * b[1] - a[1] * b[0]};
  };
  const auto normalize = [](std::array<float, 3> v) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1.0e-6f)
    {
      v[0] /= len;
      v[1] /= len;
      v[2] /= len;
    }
    return v;
  };
  const auto rotate = [](const XrQuaternionf& q,
                         const std::array<float, 3>& v) -> std::array<float, 3> {
    const XrQuaternionf vq{v[0], v[1], v[2], 0.0f};
    const XrQuaternionf qc{-q.x, -q.y, -q.z, q.w};
    const XrQuaternionf r = MultiplyQuaternions(MultiplyQuaternions(q, vq), qc);
    return {r.x, r.y, r.z};
  };

  // Rebuild exactly the same anchored Camera2 basis used by
  // GetAnimalCrossingRuntimeViewTransform(). Its rows map Animal Crossing world vectors into the
  // stabilized game-view frame that GetEyeProjectionRows() subsequently maps into the room.
  const auto forward = normalize(sub(m_ac_tabletop_anchor_center, m_ac_tabletop_anchor_eye));
  const auto right = normalize(cross(forward, m_ac_tabletop_anchor_up));
  const auto real_up = normalize(cross(right, forward));
  const std::array<std::array<float, 3>, 3> anchor_basis = {
      right, real_up,
      std::array<float, 3>{-forward[0], -forward[1], -forward[2]}};

  // The camera centre follows player.eye.position (33 units above the player's ground point).
  // Keep the occlusion footprint centred on the loaded 3x3 acre grid rather than on the camera
  // itself. m_ac_tabletop_anchor_local_* preserves that relationship through the eased acre swap.
  const std::array<float, 3> field_center_world = {
      m_ac_tabletop_anchor_center[0] - m_ac_tabletop_anchor_local_x,
      m_ac_tabletop_anchor_center[1] - PLAYER_EYE_HEIGHT,
      m_ac_tabletop_anchor_center[2] - m_ac_tabletop_anchor_local_z};
  const std::array<float, 3> field_delta = sub(field_center_world, m_ac_tabletop_anchor_eye);
  const std::array<float, 3> field_center_anchor = {
      dot(anchor_basis[0], field_delta), dot(anchor_basis[1], field_delta),
      dot(anchor_basis[2], field_delta)};

  // Columns of anchor_basis are the world X/Y/Z axes expressed in stabilized game-view space.
  // Transform those axes through the tabletop rig as well. In particular, this is what makes the
  // occlusion normal match the visually level ground after the game's built-in camera pitch has
  // been cancelled; using tabletop local +Y was the main sign/orientation bug in the old code.
  const std::array<float, 3> anchor_world_x = {anchor_basis[0][0], anchor_basis[1][0],
                                                anchor_basis[2][0]};
  const std::array<float, 3> anchor_world_y = {anchor_basis[0][1], anchor_basis[1][1],
                                                anchor_basis[2][1]};
  const std::array<float, 3> anchor_world_z = {anchor_basis[0][2], anchor_basis[1][2],
                                                anchor_basis[2][2]};

  const float camera_height =
      g_ActiveConfig.vr_enable_camera_height ? g_ActiveConfig.vr_camera_height : 0.0f;
  const float camera_forward =
      g_ActiveConfig.vr_enable_camera_forward ? g_ActiveConfig.vr_camera_forward : 0.0f;
  const std::array<float, 3> tabletop_origin_room = {
      m_home_position.x + m_tabletop_user_offset_m[0],
      m_home_position.y + m_tabletop_user_offset_m[1] - g_ActiveConfig.vr_tabletop_height -
          camera_height,
      m_home_position.z + m_tabletop_user_offset_m[2] - g_ActiveConfig.vr_tabletop_distance +
          camera_forward};

  const float upm = std::max(GetEffectiveUnitsPerMeter(), 1.0f);
  out_plane->center =
      add(tabletop_origin_room, rotate(tabletop_quat, mul(field_center_anchor, 1.0f / upm)));
  out_plane->axis_x = normalize(rotate(tabletop_quat, anchor_world_x));
  out_plane->normal = normalize(rotate(tabletop_quat, anchor_world_y));
  out_plane->axis_z = normalize(rotate(tabletop_quat, anchor_world_z));
  out_plane->half_extent_x_m = FIELD_HALF_EXTENT_GAME / upm;
  out_plane->half_extent_z_m = FIELD_HALF_EXTENT_GAME / upm;
  return true;
}

void OpenXRManager::CommitCameraAnchorFrame()
{
  // Frames the last target survives without a matching draw before the camera
  // glides back to the default position (rides out transient culling).
  constexpr int HOLD_FRAMES = 15;

  if (m_camera_anchor_pending_valid)
  {
    m_camera_anchor_target = m_camera_anchor_pending;
    m_camera_anchor_target_rotation = m_camera_anchor_pending_rotation;
    if (!OrthonormalizeAnchorRotation(m_camera_anchor_target_rotation))
      m_camera_anchor_target_rotation = CAMERA_ANCHOR_IDENTITY;
    m_camera_anchor_target_upm = m_camera_anchor_pending_upm;
    m_camera_anchor_has_target = true;
    m_camera_anchor_missing_frames = 0;
  }
  else if (m_camera_anchor_has_target && ++m_camera_anchor_missing_frames > HOLD_FRAMES)
  {
    m_camera_anchor_has_target = false;
    m_camera_anchor_target = {};
    m_camera_anchor_target_rotation = CAMERA_ANCHOR_IDENTITY;
    m_camera_anchor_target_upm = 0.0f;
  }
  m_camera_anchor_pending_valid = false;

  // Exponential smoothing: fraction of the previous position kept each frame
  // (0 = hard lock to the element, which transmits every animation bob to the head).
  const float k = std::clamp(g_ActiveConfig.vr_camera_anchor_smoothing, 0.0f, 0.95f);
  for (size_t i = 0; i < 3; ++i)
  {
    m_camera_anchor_position[i] =
        k * m_camera_anchor_position[i] + (1.0f - k) * m_camera_anchor_target[i];
  }

  // Rotation: per-entry blend toward the target, then re-orthonormalize. For the small
  // per-frame deltas smoothing produces this behaves like a quaternion nlerp; if the
  // blend passes through a degenerate configuration (target ~180 degrees away), snap.
  for (size_t i = 0; i < 9; ++i)
  {
    m_camera_anchor_rotation[i] =
        k * m_camera_anchor_rotation[i] + (1.0f - k) * m_camera_anchor_target_rotation[i];
  }
  if (!OrthonormalizeAnchorRotation(m_camera_anchor_rotation))
    m_camera_anchor_rotation = m_camera_anchor_target_rotation;

  // World scale: glide toward the anchor's own scale while engaged, and back to the global
  // setting on release. Once it lands back on the global value the scale disengages entirely
  // (m_camera_anchor_upm = 0) so the global slider stays immediate when no anchor scale is set.
  const float global_upm = std::max(g_ActiveConfig.vr_units_per_meter, 0.0001f);
  if (m_camera_anchor_target_upm > 0.0f)
  {
    if (m_camera_anchor_upm <= 0.0f)
      m_camera_anchor_upm = global_upm;  // engage from the scale currently on screen
    m_camera_anchor_upm = k * m_camera_anchor_upm + (1.0f - k) * m_camera_anchor_target_upm;
  }
  else if (m_camera_anchor_upm > 0.0f)
  {
    m_camera_anchor_upm = k * m_camera_anchor_upm + (1.0f - k) * global_upm;
    if (std::abs(m_camera_anchor_upm - global_upm) <= 0.001f * global_upm)
      m_camera_anchor_upm = 0.0f;
  }

  bool rotation_active = false;
  for (size_t i = 0; i < 9; ++i)
  {
    if (std::abs(m_camera_anchor_rotation[i] - CAMERA_ANCHOR_IDENTITY[i]) > 1e-4f)
    {
      rotation_active = true;
      break;
    }
  }
  m_camera_anchor_rotation_active = rotation_active;

  if (!m_camera_anchor_has_target)
  {
    // Snap the tail of the return glide so a released anchor leaves exactly zero offset.
    const float d2 = m_camera_anchor_position[0] * m_camera_anchor_position[0] +
                     m_camera_anchor_position[1] * m_camera_anchor_position[1] +
                     m_camera_anchor_position[2] * m_camera_anchor_position[2];
    if (d2 < 1e-6f)
      m_camera_anchor_position = {};
    if (!rotation_active)
      m_camera_anchor_rotation = CAMERA_ANCHOR_IDENTITY;
  }
}

bool OpenXRManager::GetControllerAnchorViewPose(int hand, float units_per_meter,
                                                std::array<float, 3>* out_position,
                                                std::array<float, 9>* out_rotation)
{
  if (hand < 0 || hand > 1 || !out_position)
    return false;

  // Under the head-pose lock the eye projection is frozen for the whole game frame, so
  // the hand pose must be too — a fresher hand than head would jitter against the
  // world. Without the lock the GS refetches per draw and so do we.
  const bool lock = g_ActiveConfig.VRLockHeadPosePerFrame();
  if (lock && m_controller_anchor_cache_valid[hand] &&
      std::abs(m_controller_anchor_cache_upm[hand] - units_per_meter) <= 0.0001f)
  {
    *out_position = m_controller_anchor_cache[hand];
    if (out_rotation)
      *out_rotation = m_controller_anchor_cache_rot[hand];
    return true;
  }

  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  const Common::VR::OpenXRPoseState& aim = snapshot.controllers[hand].aim_pose;
  std::array<float, 9> rot{};
  if (!MapAimPoseToGameView(aim, units_per_meter, out_position, &rot))
    return false;
  if (out_rotation)
    *out_rotation = rot;

  if (lock)
  {
    m_controller_anchor_cache_valid[hand] = true;
    m_controller_anchor_cache[hand] = *out_position;
    m_controller_anchor_cache_rot[hand] = rot;
    m_controller_anchor_cache_upm[hand] = units_per_meter;
  }
  return true;
}

bool OpenXRManager::MapAimPoseToGameView(const Common::VR::OpenXRPoseState& aim,
                                         float units_per_meter,
                                         std::array<float, 3>* out_position,
                                         std::array<float, 9>* out_rotation) const
{
  if (!aim.valid || !out_position)
    return false;

  // Tracking-mode adjustment via the eye-center delta: adjusted_center + (raw − raw_center)
  // reproduces GetTrackingAdjustedEyeViews() for every tracking mode without duplicating
  // its switch (exact identity in Full6DoF) and keeps the hand attached to a pinned head
  // in the 3DoF/no-tracking modes.
  const std::array<XREyeView, 2> adjusted = GetTrackingAdjustedEyeViews();
  const float raw_cx = 0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
  const float raw_cy = 0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
  const float raw_cz = 0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);
  const float adj_cx = 0.5f * (adjusted[0].pose.position.x + adjusted[1].pose.position.x);
  const float adj_cy = 0.5f * (adjusted[0].pose.position.y + adjusted[1].pose.position.y);
  const float adj_cz = 0.5f * (adjusted[0].pose.position.z + adjusted[1].pose.position.z);
  const float cx = adj_cx + (aim.position[0] - raw_cx);
  const float cy = adj_cy + (aim.position[1] - raw_cy);
  const float cz = adj_cz + (aim.position[2] - raw_cz);

  // Replay of the GetEyeProjectionRows eye-position chain (home offset, camera
  // height/forward, anchor rotation, anchor position) with the controller substituted
  // for the eye. Any divergence between the two shows up as the element swimming
  // against the hand under head motion — keep them in lockstep.
  const float s = std::max(units_per_meter, 0.0001f);
  float px = (cx - m_home_position.x) * s;
  float py = (cy - m_home_position.y) * s;
  float pz = (cz - m_home_position.z) * s;
  float camera_height_m =
      g_ActiveConfig.vr_enable_camera_height ? g_ActiveConfig.vr_camera_height : 0.0f;
  float camera_forward_m =
      g_ActiveConfig.vr_enable_camera_forward ? g_ActiveConfig.vr_camera_forward : 0.0f;
  if (IsTabletopModeActive())
  {
    camera_height_m += g_ActiveConfig.vr_tabletop_height;
    // Positive tabletop distance means the viewer sits farther back from the board.
    camera_forward_m -= g_ActiveConfig.vr_tabletop_distance;
  }
  py += camera_height_m * s;
  pz += -camera_forward_m * s;
  if (m_camera_anchor_rotation_active)
  {
    const std::array<float, 9>& A = m_camera_anchor_rotation;
    const float rx = px, ry = py, rz = pz;
    px = A[0] * rx + A[1] * ry + A[2] * rz;
    py = A[3] * rx + A[4] * ry + A[5] * rz;
    pz = A[6] * rx + A[7] * ry + A[8] * rz;
  }
  px += m_camera_anchor_position[0];
  py += m_camera_anchor_position[1];
  pz += m_camera_anchor_position[2];

  // Controller orientation in view space. Standard quaternion-to-matrix, columns =
  // controller-local axes in reference space (X right, Y up, -Z the aim direction);
  // directions carry over to view space unchanged — validated by the position path —
  // and the camera-anchor rig rotation applies exactly as it does to the eye offsets.
  const float qx = aim.orientation[0], qy = aim.orientation[1];
  const float qz = aim.orientation[2], qw = aim.orientation[3];
  const float x2 = 2.0f * qx * qx, y2 = 2.0f * qy * qy, z2 = 2.0f * qz * qz;
  const float xy = 2.0f * qx * qy, xz = 2.0f * qx * qz, yz = 2.0f * qy * qz;
  const float wx = 2.0f * qw * qx, wy = 2.0f * qw * qy, wz = 2.0f * qw * qz;
  std::array<float, 9> rot = {1.0f - y2 - z2, xy - wz,        xz + wy,
                              xy + wz,        1.0f - x2 - z2, yz - wx,
                              xz - wy,        yz + wx,        1.0f - x2 - y2};
  if (m_camera_anchor_rotation_active)
  {
    const std::array<float, 9>& A = m_camera_anchor_rotation;
    const std::array<float, 9> c = rot;
    for (int r = 0; r < 3; ++r)
    {
      for (int col = 0; col < 3; ++col)
      {
        rot[r * 3 + col] =
            A[r * 3 + 0] * c[0 + col] + A[r * 3 + 1] * c[3 + col] + A[r * 3 + 2] * c[6 + col];
      }
    }
  }

  *out_position = {px, py, pz};
  if (out_rotation)
    *out_rotation = rot;
  return true;
}

void OpenXRManager::ComputeVirtualScreenHit(const Common::VR::OpenXRPoseState& aim,
                                            Common::VR::OpenXRScreenHit* out_hit) const
{
  *out_hit = {};
  if (!aim.valid)
    return;

  if (g_ActiveConfig.vr_flat_screen)
  {
    // Flat panel: a world-locked quad in reference space (meters), yaw-only orientation.
    // Bring the aim ray into quad-local space (quad plane is z=0, +z toward the viewer).
    const XrPosef quad = GetFlatScreenPose();
    const float yaw = 2.0f * std::atan2(quad.orientation.y, quad.orientation.w);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const auto to_quad_local = [&](float x, float y, float z) -> std::array<float, 3> {
      // Inverse yaw rotation about Y.
      return {cy * x - sy * z, y, sy * x + cy * z};
    };

    const std::array<float, 3> p = to_quad_local(aim.position[0] - quad.position.x,
                                                 aim.position[1] - quad.position.y,
                                                 aim.position[2] - quad.position.z);
    // Aim forward = orientation * (0,0,-1).
    const float qx = aim.orientation[0], qy = aim.orientation[1];
    const float qz = aim.orientation[2], qw = aim.orientation[3];
    const float fx = -(2.0f * (qx * qz + qw * qy));
    const float fy = -(2.0f * (qy * qz - qw * qx));
    const float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
    const std::array<float, 3> d = to_quad_local(fx, fy, fz);

    if (std::abs(d[2]) < 1e-6f)
      return;
    const float t = -p[2] / d[2];
    if (t <= 0.0f)
      return;

    const float half_h = std::max(g_ActiveConfig.vr_screen_size * 0.5f, 1e-4f);
    const float aspect = m_flat_screen_aspect > 0.0f ? m_flat_screen_aspect : (16.0f / 9.0f);
    const float half_w = half_h * aspect;
    out_hit->valid = true;
    out_hit->u = (p[0] + t * d[0]) / half_w;
    out_hit->v = (p[1] + t * d[1]) / half_h;
    // Perpendicular distance, not ray length: rotating the controller must not change
    // the emulated sensor-bar distance (ray length grows toward the screen corners).
    out_hit->distance_m = p[2];
    return;
  }

  // Immersive mode: the ortho virtual screen lives in game view space at z = -distance,
  // extents {half_h * 16/9, half_h} (see GeometryShaderManager's cvr_screen constants).
  // Map the aim pose through the same chain the eye takes so the hit matches what is drawn.
  const float upm = std::max(GetEffectiveUnitsPerMeter(), 0.0001f);
  std::array<float, 3> pos{};
  std::array<float, 9> rot{};
  if (!MapAimPoseToGameView(aim, upm, &pos, &rot))
    return;

  // Aim forward in view space = -third column of the rotation.
  const float dx = -rot[2], dy = -rot[5], dz = -rot[8];
  if (std::abs(dz) < 1e-6f)
    return;

  const float ui_scale = GetTabletopUIPhysicalScale();
  const float dist = upm * GetVirtualScreenDistanceMeters();
  const float vertical_offset = upm * GetVirtualScreenVerticalOffsetMeters();
  const float half_h =
      std::max(upm * g_ActiveConfig.vr_screen_size * ui_scale * 0.5f, 1e-4f);
  const float half_w = half_h * (16.0f / 9.0f);

  const float t = (-dist - pos[2]) / dz;
  if (t <= 0.0f)
    return;

  out_hit->valid = true;
  out_hit->u = (pos[0] + t * dx) / half_w;
  out_hit->v = (pos[1] + t * dy - vertical_offset) / half_h;
  // Perpendicular distance to the screen plane (see the flat-mode note above).
  out_hit->distance_m = (pos[2] + dist) / upm;
}

void OpenXRManager::InvalidateControllerAnchorCache()
{
  m_controller_anchor_cache_valid = {false, false};
}

bool OpenXRManager::GetAnimalCrossingRuntimeViewTransform(
    std::array<float, 12>* out_transform) const
{
  if (!out_transform)
    return false;

  *out_transform = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f};

  if (m_tabletop_reanchor_requested.exchange(false, std::memory_order_acq_rel))
  {
    m_ac_tabletop_camera_anchor_valid = false;
    m_ac_tabletop_transition_active = false;
    m_ac_tabletop_pending_block_frames = 0;
  }

  const bool tabletop_active = IsTabletopModeActive();
  if (!tabletop_active || SConfig::GetInstance().GetGameID() != "GAFE01")
  {
    m_ac_tabletop_camera_anchor_valid = false;
    m_ac_tabletop_transition_active = false;
    // Keep the already discovered Camera2 address across the runtime camera switch. Re-entering
    // tabletop is then immediate and cannot trigger another 24 MiB MEM1 scan.
    return false;
  }

  // GAFE01 GAME_PLAY embeds Camera2 at +0x1B88. Camera2 is 0x138 bytes; the layout is
  // documented by ACreTeam/ac-decomp. We locate the live structure instead of hardcoding the
  // REL BSS address, because the forest REL's BSS load address is runtime-assigned.
  constexpr uint32_t MEM1_BASE = 0x80000000;
  constexpr size_t MEM1_SIZE = 0x01800000;
  constexpr uint32_t CAMERA_SIZE = 0x138;
  constexpr uint32_t CAMERA_EYE = 0x00;
  constexpr uint32_t CAMERA_CENTER = 0x0C;
  constexpr uint32_t CAMERA_UP = 0x18;
  constexpr uint32_t CAMERA_FOV = 0x24;
  constexpr uint32_t CAMERA_NEAR = 0x2C;
  constexpr uint32_t CAMERA_FAR = 0x30;
  constexpr uint32_t CAMERA_SCALE = 0x34;
  constexpr uint32_t CAMERA_MAIN_INDEX = 0x60;
  constexpr uint32_t GAME_PLAY_CAMERA = 0x1B88;
  constexpr uint32_t GAME_PLAY_SCENE_ID = 0x00E0;
  constexpr uint32_t GAME_PLAY_ACTOR_INFO = 0x1DA8;
  constexpr uint32_t GAME_PLAY_PLAYER_COUNT = 0x1DC4;
  constexpr uint32_t GAME_PLAY_PLAYER_PTR = 0x1DC8;
  constexpr uint32_t ACTOR_PART = 0x0002;
  constexpr uint32_t AC_SCENE_COUNT = 52;
  constexpr uint32_t CAMERA2_PROCESS_NORMAL = 1;

  auto& memory = Core::System::GetInstance().GetMemory();
  const auto read_f32 = [&memory](uint32_t address) {
    return std::bit_cast<float>(memory.Read_U32(address));
  };
  const auto read_vec3 = [&read_f32](uint32_t address) {
    return std::array<float, 3>{read_f32(address), read_f32(address + 4), read_f32(address + 8)};
  };
  const auto finite_vec = [](const std::array<float, 3>& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
  };

  const auto camera_is_plausible = [&](uint32_t camera_addr, bool require_world_center) {
    if (camera_addr < MEM1_BASE + GAME_PLAY_CAMERA ||
        camera_addr + CAMERA_SIZE >= MEM1_BASE + MEM1_SIZE)
    {
      return false;
    }

    // Camera2 is embedded in GAME_PLAY. Validate a few surrounding GAME_PLAY/Actor_info fields so
    // a stale block of memory left by the train scene cannot continue masquerading as the live
    // camera after the town scene allocates a new instance.
    const uint32_t play_addr = camera_addr - GAME_PLAY_CAMERA;
    const uint32_t scene_id = memory.Read_U16(play_addr + GAME_PLAY_SCENE_ID);
    const uint32_t total_actors = memory.Read_U32(play_addr + GAME_PLAY_ACTOR_INFO);
    const uint32_t player_count = memory.Read_U32(play_addr + GAME_PLAY_PLAYER_COUNT);
    if (scene_id >= AC_SCENE_COUNT || total_actors > 512 || player_count > 1)
      return false;
    if (player_count == 1)
    {
      const uint32_t player_addr = memory.Read_U32(play_addr + GAME_PLAY_PLAYER_PTR);
      if (player_addr < MEM1_BASE || player_addr + 0x174 >= MEM1_BASE + MEM1_SIZE ||
          memory.Read_U8(player_addr + ACTOR_PART) != 3)
      {
        return false;
      }
    }

    const float fov = read_f32(camera_addr + CAMERA_FOV);
    const float near_plane = read_f32(camera_addr + CAMERA_NEAR);
    const float far_plane = read_f32(camera_addr + CAMERA_FAR);
    const float scale = read_f32(camera_addr + CAMERA_SCALE);
    const auto eye = read_vec3(camera_addr + CAMERA_EYE);
    const auto center = read_vec3(camera_addr + CAMERA_CENTER);
    const auto up = read_vec3(camera_addr + CAMERA_UP);
    const uint32_t main_index = memory.Read_U32(camera_addr + CAMERA_MAIN_INDEX);
    if (!finite_vec(eye) || !finite_vec(center) || !finite_vec(up))
      return false;
    if (!(fov > 1.0f && fov < 100.0f && near_plane > 0.1f && near_plane < 1000.0f &&
          far_plane > 1000.0f && far_plane < 10000.0f && scale > 0.1f && scale < 4.0f &&
          main_index <= 12))
      return false;
    const float up_len_sq = up[0] * up[0] + up[1] * up[1] + up[2] * up[2];
    if (up_len_sq < 0.2f || up_len_sq > 2.0f)
      return false;
    if (require_world_center && std::abs(center[0]) + std::abs(center[2]) < 50.0f)
      return false;
    return true;
  };

  const XrTime scan_now = m_predicted_display_time_snapshot.load(std::memory_order_acquire);
  if (m_ac_tabletop_camera_addr != 0 &&
      !camera_is_plausible(m_ac_tabletop_camera_addr, false))
  {
    // Camera2 can be briefly invalid during save/load and scene transitions, so don't immediately
    // restart the expensive MEM1 scan. If it stays invalid for several frames, however, assume the
    // GAME_PLAY instance was replaced (train -> town is the important case) and allow one rescan.
    ++m_ac_tabletop_camera_invalid_frames;
    m_ac_tabletop_camera_anchor_valid = false;
    m_ac_tabletop_transition_active = false;
    m_ac_tabletop_pending_block_frames = 0;
    if (m_ac_tabletop_camera_invalid_frames < 12)
      return false;

    m_ac_tabletop_camera_addr = 0;
    m_ac_tabletop_camera_invalid_frames = 0;
    m_ac_tabletop_next_camera_scan_time = scan_now;
  }

  if (m_ac_tabletop_camera_addr == 0)
  {
    m_ac_tabletop_camera_anchor_valid = false;
    if (scan_now > 0 && m_ac_tabletop_next_camera_scan_time > scan_now)
      return false;

    const uint8_t* ram = memory.GetPointerForRange(MEM1_BASE, MEM1_SIZE);
    if (!ram)
      return false;

    // Camera2::perspective.far is normally exactly 1600.0f (44 C8 00 00). Scan raw MEM1 for
    // that signature, then validate both Camera2 and its surrounding GAME_PLAY fields. Failed
    // scans are throttled to 500 ms so loading screens cannot cause a 24 MiB scan every frame.
    for (size_t offset = 0; offset + CAMERA_SIZE < MEM1_SIZE; offset += 4)
    {
      const size_t far_offset = offset + CAMERA_FAR;
      if (ram[far_offset + 0] != 0x44 || ram[far_offset + 1] != 0xC8 ||
          ram[far_offset + 2] != 0x00 || ram[far_offset + 3] != 0x00)
      {
        continue;
      }

      const uint32_t candidate = MEM1_BASE + static_cast<uint32_t>(offset);
      if (camera_is_plausible(candidate, true))
      {
        m_ac_tabletop_camera_addr = candidate;
        break;
      }
    }

    if (m_ac_tabletop_camera_addr == 0)
    {
      if (scan_now > 0)
        m_ac_tabletop_next_camera_scan_time = scan_now + 500'000'000;
      return false;
    }

    m_ac_tabletop_camera_invalid_frames = 0;
    m_ac_tabletop_next_camera_scan_time = 0;
  }

  const auto eye = read_vec3(m_ac_tabletop_camera_addr + CAMERA_EYE);
  const auto center = read_vec3(m_ac_tabletop_camera_addr + CAMERA_CENTER);
  const auto up = read_vec3(m_ac_tabletop_camera_addr + CAMERA_UP);
  const uint32_t play_addr = m_ac_tabletop_camera_addr - GAME_PLAY_CAMERA;
  const int scene_id = static_cast<int>(memory.Read_U16(play_addr + GAME_PLAY_SCENE_ID));
  const uint32_t camera_main_index = memory.Read_U32(m_ac_tabletop_camera_addr + CAMERA_MAIN_INDEX);

  // Never carry a stable Camera2 basis across Animal Crossing scenes. In particular, the train
  // arrival uses a different camera setup from SCENE_FG; retaining that basis is what rotated the
  // town tabletop for some first-time users.
  if (m_ac_tabletop_scene_id != scene_id)
  {
    m_ac_tabletop_scene_id = scene_id;
    m_ac_tabletop_stable_basis_valid = false;
    m_ac_tabletop_camera_anchor_valid = false;
    m_ac_tabletop_transition_active = false;
    m_ac_tabletop_pending_block_frames = 0;
  }

  // Init_Camera2 briefly resets the center to the origin during scene transitions. Drop the old
  // anchor there and capture a fresh one once the new scene has a real camera.
  if (!finite_vec(eye) || !finite_vec(center) || !finite_vec(up) ||
      std::abs(center[0]) + std::abs(center[2]) < 10.0f)
  {
    ++m_ac_tabletop_camera_invalid_frames;
    m_ac_tabletop_camera_anchor_valid = false;
    m_ac_tabletop_transition_active = false;
    m_ac_tabletop_pending_block_frames = 0;
    if (m_ac_tabletop_camera_invalid_frames >= 30)
    {
      // A long invalid window generally means GAME_PLAY was replaced by a scene load. Forget the
      // stale address, but let the throttled scanner find the new one instead of scanning each frame.
      m_ac_tabletop_camera_addr = 0;
      m_ac_tabletop_camera_invalid_frames = 0;
      m_ac_tabletop_next_camera_scan_time = scan_now;
    }
    return false;
  }
  m_ac_tabletop_camera_invalid_frames = 0;

  // An acre is 16 x 40 = 640 game units. A transition must replace the old acre in the exact
  // same tabletop slot, without inheriting the transient zoom/pitch/offset of Animal Crossing's
  // scrolling camera. Debounce the block decision for a few rendered frames, then move only the
  // ANCHOR'S world origin by an exact acre multiple. Keeping the original eye/center separation
  // and up vector removes the small scale/tilt wobble that came from recapturing the live camera.
  constexpr float AC_ACRE_SIZE = 640.0f;
  constexpr int AC_BLOCK_CONFIRM_FRAMES = 4;
  const int current_block_x = static_cast<int>(std::floor(center[0] / AC_ACRE_SIZE));
  const int current_block_z = static_cast<int>(std::floor(center[2] / AC_ACRE_SIZE));

  // A new scene may begin with a DEMO/DOOR camera. Use it only as a temporary anchor; as soon as
  // Camera2 returns to NORMAL, recapture once and freeze that normal gameplay basis for the scene.
  if (!m_ac_tabletop_stable_basis_valid && m_ac_tabletop_camera_anchor_valid &&
      camera_main_index == CAMERA2_PROCESS_NORMAL)
  {
    m_ac_tabletop_camera_anchor_valid = false;
    m_ac_tabletop_transition_active = false;
  }

  if (!m_ac_tabletop_camera_anchor_valid)
  {
    m_ac_tabletop_anchor_center = center;
    if (m_ac_tabletop_stable_basis_valid)
    {
      // Re-entering tabletop after the classic VR camera (or recovering after a temporary Camera2
      // invalidation) must reuse the original stable camera basis. Only recenter the anchor at the
      // current game position; do not adopt the live eye/up vectors because dialogue and scripted
      // cameras can be pitched/rolled/zoomed at the exact frame the user switches back.
      for (size_t axis = 0; axis < 3; ++axis)
      {
        m_ac_tabletop_anchor_eye[axis] =
            center[axis] + m_ac_tabletop_stable_eye_from_center[axis];
      }
      m_ac_tabletop_anchor_up = m_ac_tabletop_stable_up;
    }
    else
    {
      // Scripted train/door/demo cameras are allowed as temporary anchors so the scene remains
      // usable, but only NORMAL gameplay is allowed to become the persistent basis for this scene.
      m_ac_tabletop_anchor_eye = eye;
      m_ac_tabletop_anchor_up = up;
      if (camera_main_index == CAMERA2_PROCESS_NORMAL)
      {
        for (size_t axis = 0; axis < 3; ++axis)
        {
          m_ac_tabletop_stable_eye_from_center[axis] = eye[axis] - center[axis];
        }
        m_ac_tabletop_stable_up = up;
        m_ac_tabletop_stable_basis_valid = true;
      }
    }
    m_ac_tabletop_anchor_block_x = current_block_x;
    m_ac_tabletop_anchor_block_z = current_block_z;
    m_ac_tabletop_anchor_local_x =
        center[0] - (static_cast<float>(current_block_x) + 0.5f) * AC_ACRE_SIZE;
    m_ac_tabletop_anchor_local_z =
        center[2] - (static_cast<float>(current_block_z) + 0.5f) * AC_ACRE_SIZE;
    m_ac_tabletop_pending_block_x = current_block_x;
    m_ac_tabletop_pending_block_z = current_block_z;
    m_ac_tabletop_pending_block_frames = 0;
    m_ac_tabletop_camera_anchor_valid = true;
    return true;
  }

  if (current_block_x != m_ac_tabletop_anchor_block_x ||
      current_block_z != m_ac_tabletop_anchor_block_z)
  {
    if (current_block_x == m_ac_tabletop_pending_block_x &&
        current_block_z == m_ac_tabletop_pending_block_z)
    {
      ++m_ac_tabletop_pending_block_frames;
    }
    else
    {
      m_ac_tabletop_pending_block_x = current_block_x;
      m_ac_tabletop_pending_block_z = current_block_z;
      m_ac_tabletop_pending_block_frames = 1;
    }

    if (m_ac_tabletop_pending_block_frames >= AC_BLOCK_CONFIRM_FRAMES)
    {
      const float block_delta_x =
          static_cast<float>(current_block_x - m_ac_tabletop_anchor_block_x) * AC_ACRE_SIZE;
      const float block_delta_z =
          static_cast<float>(current_block_z - m_ac_tabletop_anchor_block_z) * AC_ACRE_SIZE;

      // The destination remains an exact acre-grid shift, but do not teleport the anchor there.
      // Ease it over a fraction of a second so the streamed acre visually replaces the previous
      // one instead of producing a hard pop in the diorama.
      m_ac_tabletop_transition_from_eye = m_ac_tabletop_anchor_eye;
      m_ac_tabletop_transition_from_center = m_ac_tabletop_anchor_center;
      m_ac_tabletop_transition_target_eye = m_ac_tabletop_anchor_eye;
      m_ac_tabletop_transition_target_center = m_ac_tabletop_anchor_center;
      m_ac_tabletop_transition_target_eye[0] += block_delta_x;
      m_ac_tabletop_transition_target_center[0] += block_delta_x;
      m_ac_tabletop_transition_target_eye[2] += block_delta_z;
      m_ac_tabletop_transition_target_center[2] += block_delta_z;
      m_ac_tabletop_transition_start_time =
          m_predicted_display_time_snapshot.load(std::memory_order_acquire);
      m_ac_tabletop_transition_active = m_ac_tabletop_transition_start_time > 0;

      // If there is no valid compositor timestamp yet (very early startup), finish immediately.
      if (!m_ac_tabletop_transition_active)
      {
        m_ac_tabletop_anchor_eye = m_ac_tabletop_transition_target_eye;
        m_ac_tabletop_anchor_center = m_ac_tabletop_transition_target_center;
      }

      m_ac_tabletop_anchor_block_x = current_block_x;
      m_ac_tabletop_anchor_block_z = current_block_z;
      m_ac_tabletop_pending_block_frames = 0;
    }
  }
  else
  {
    m_ac_tabletop_pending_block_x = current_block_x;
    m_ac_tabletop_pending_block_z = current_block_z;
    m_ac_tabletop_pending_block_frames = 0;
  }

  if (m_ac_tabletop_transition_active)
  {
    // XrTime is nanoseconds. 220 ms is long enough to hide the acre swap, but short enough that
    // control still feels immediate. Smoothstep gives zero velocity at both ends and avoids the
    // little kick that a linear blend produces when the transition starts/stops.
    constexpr XrDuration AC_ACRE_BLEND_DURATION = 220'000'000;
    const XrTime now = m_predicted_display_time_snapshot.load(std::memory_order_acquire);
    const XrDuration elapsed = std::max<XrDuration>(0, now - m_ac_tabletop_transition_start_time);
    const float linear_t = std::clamp(static_cast<float>(elapsed) /
                                          static_cast<float>(AC_ACRE_BLEND_DURATION),
                                      0.0f, 1.0f);
    const float eased_t = linear_t * linear_t * (3.0f - 2.0f * linear_t);
    for (size_t axis = 0; axis < 3; ++axis)
    {
      m_ac_tabletop_anchor_eye[axis] =
          m_ac_tabletop_transition_from_eye[axis] +
          (m_ac_tabletop_transition_target_eye[axis] -
           m_ac_tabletop_transition_from_eye[axis]) *
              eased_t;
      m_ac_tabletop_anchor_center[axis] =
          m_ac_tabletop_transition_from_center[axis] +
          (m_ac_tabletop_transition_target_center[axis] -
           m_ac_tabletop_transition_from_center[axis]) *
              eased_t;
    }
    if (linear_t >= 1.0f)
    {
      m_ac_tabletop_anchor_eye = m_ac_tabletop_transition_target_eye;
      m_ac_tabletop_anchor_center = m_ac_tabletop_transition_target_center;
      m_ac_tabletop_transition_active = false;
    }
  }

  const auto sub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  };
  const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::array<float, 3>{a[1] * b[2] - a[2] * b[1],
                                a[2] * b[0] - a[0] * b[2],
                                a[0] * b[1] - a[1] * b[0]};
  };
  const auto normalize = [](std::array<float, 3> v) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1.0e-5f)
    {
      v[0] /= len;
      v[1] /= len;
      v[2] /= len;
    }
    return v;
  };
  const auto make_view_basis = [&](const std::array<float, 3>& basis_eye,
                                   const std::array<float, 3>& basis_center,
                                   const std::array<float, 3>& basis_up) {
    const auto forward = normalize(sub(basis_center, basis_eye));
    const auto right = normalize(cross(forward, basis_up));
    const auto real_up = normalize(cross(right, forward));
    return std::array<std::array<float, 3>, 3>{right, real_up,
                                               std::array<float, 3>{-forward[0], -forward[1],
                                                                    -forward[2]}};
  };

  const auto anchor_basis =
      make_view_basis(m_ac_tabletop_anchor_eye, m_ac_tabletop_anchor_center,
                      m_ac_tabletop_anchor_up);
  const auto current_basis = make_view_basis(eye, center, up);

  // Current game view -> anchored game view:
  //   v_anchor = R_anchor * R_current^T * v_current + R_anchor*(eye_current-eye_anchor)
  std::array<std::array<float, 3>, 3> rotation{};
  for (size_t r = 0; r < 3; ++r)
  {
    for (size_t c = 0; c < 3; ++c)
      rotation[r][c] = dot(anchor_basis[r], current_basis[c]);
  }
  const auto eye_delta = sub(eye, m_ac_tabletop_anchor_eye);
  const std::array<float, 3> translation = {dot(anchor_basis[0], eye_delta),
                                             dot(anchor_basis[1], eye_delta),
                                             dot(anchor_basis[2], eye_delta)};

  *out_transform = {rotation[0][0], rotation[0][1], rotation[0][2], translation[0],
                    rotation[1][0], rotation[1][1], rotation[1][2], translation[1],
                    rotation[2][0], rotation[2][1], rotation[2][2], translation[2]};
  return true;
}

void OpenXRManager::GetEyeProjectionRows(
    float units_per_meter,
    std::array<std::array<float, 4>, 4>& out_proj_rows,
    std::array<std::array<float, 4>, 2>& out_z_rows) const
{
  const float s = std::max(units_per_meter, 0.0001f);
  constexpr float DEG_TO_RAD = 0.01745329252f;
  const float lean_back_rad =
      (g_ActiveConfig.vr_enable_lean_back_angle ? g_ActiveConfig.vr_lean_back_angle : 0.0f) *
      DEG_TO_RAD;
  const bool tabletop_active = IsTabletopModeActive();
  const float tabletop_pitch_rad =
      (tabletop_active ? g_ActiveConfig.vr_tabletop_pitch : 0.0f) * DEG_TO_RAD;
  const float camera_forward_m =
      g_ActiveConfig.vr_enable_camera_forward ? g_ActiveConfig.vr_camera_forward : 0.0f;
  const float camera_height_m =
      g_ActiveConfig.vr_enable_camera_height ? g_ActiveConfig.vr_camera_height : 0.0f;

  const std::array<XREyeView, 2> eye_views = GetTrackingAdjustedEyeViews();
  std::array<float, 12> ac_tabletop_view_transform{};
  const bool apply_ac_runtime_view =
      GetAnimalCrossingRuntimeViewTransform(&ac_tabletop_view_transform);

  // The tabletop is a rigid transform between Animal Crossing's camera/view space and the
  // OpenXR room. Keep this transform independent from the headset orientation: the headset then
  // moves inside a stable miniature world instead of carrying the board's pitch axis with it.
  XrQuaternionf tabletop_rig_quat{0.0f, 0.0f, 0.0f, 1.0f};
  if (tabletop_active)
  {
    const float half_pitch = 0.5f * tabletop_pitch_rad;
    const XrQuaternionf pitch_quat{std::sin(half_pitch), 0.0f, 0.0f,
                                   std::cos(half_pitch)};
    const float half_yaw = -0.5f * m_tabletop_user_yaw_rad;
    const XrQuaternionf yaw_quat{0.0f, std::sin(half_yaw), 0.0f, std::cos(half_yaw)};
    // First cancel the game's built-in camera pitch, then rotate the resulting horizontal board
    // around room-up. Quaternion multiplication applies the right operand first.
    tabletop_rig_quat = MultiplyQuaternions(yaw_quat, pitch_quat);
  }

  const auto rotate_vector = [](const XrQuaternionf& q,
                                const std::array<float, 3>& v) -> std::array<float, 3> {
    const XrQuaternionf vq{v[0], v[1], v[2], 0.0f};
    const XrQuaternionf qc{-q.x, -q.y, -q.z, q.w};
    const XrQuaternionf r = MultiplyQuaternions(MultiplyQuaternions(q, vq), qc);
    return {r.x, r.y, r.z};
  };

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    const XrFovf& fov = eye_views[eye].fov;
    const XrQuaternionf& q_xr = eye_views[eye].pose.orientation;
    const XrVector3f& eye_pos_xr = eye_views[eye].pose.position;
    XrQuaternionf q = {-q_xr.x, -q_xr.y, -q_xr.z, q_xr.w};
    if (lean_back_rad != 0.0f)
    {
      const float half_angle = 0.5f * lean_back_rad;
      const XrQuaternionf lean_back_quat = {std::sin(half_angle), 0.0f, 0.0f,
                                            std::cos(half_angle)};
      q = MultiplyQuaternions(q, lean_back_quat);
    }
    if (tabletop_active)
    {
      // q is the inverse headset orientation. The board transform belongs on its right:
      //   room -> eye = inverse(head) * tabletop_rig.
      // This is the composition that keeps a physical tabletop level while the user yaws,
      // pitches or rolls their head.
      q = MultiplyQuaternions(q, tabletop_rig_quat);
    }

    // --- Quaternion to 3x3 rotation matrix R ---
    // R transforms from eye-local frame to reference space (standard quaternion convention).
    // Columns of R are the eye's local axes expressed in reference-space coordinates.
    // To go reference→local we use R^T, but it's never needed explicitly: see below.
    const float x2 = 2.0f * q.x * q.x, y2 = 2.0f * q.y * q.y, z2 = 2.0f * q.z * q.z;
    const float xy = 2.0f * q.x * q.y, xz = 2.0f * q.x * q.z, yz = 2.0f * q.y * q.z;
    const float wx = 2.0f * q.w * q.x, wy = 2.0f * q.w * q.y, wz = 2.0f * q.w * q.z;

    // R rows (R[row][col]):
    // R[0] = { 1-y2-z2,  xy+wz,   xz-wy  }
    // R[1] = { xy-wz,    1-x2-z2, yz+wx   }
    // R[2] = { xz+wy,    yz-wx,   1-x2-y2 }
    //
    // R^T maps reference-space positions into eye-local coords:
    //   eye_local = R^T * (viewPos - eye_pos_game)
    //
    // clip_x = P_row0 · eye_local = P_row0 · R^T · d
    //        = Σ_j d_j · (Σ_i P_i · R[j][i])
    //        = (R * P_col0) · d
    //
    // So combined_row = R * proj_col  (NOT R^T * proj_col — that would be wrong).

    // R matrix elements (row, col)
    const float r00 = 1.0f - y2 - z2, r01 = xy + wz, r02 = xz - wy;
    const float r10 = xy - wz, r11 = 1.0f - x2 - z2, r12 = yz + wx;
    const float r20 = xz + wy, r21 = yz - wx, r22 = 1.0f - x2 - y2;

    // Camera anchor rotation: attach the camera rig to the anchor element's frame.
    // A's columns are the rig axes in view space, so the full eye-local -> view
    // rotation is A*R (HMD rotation composes inside the rig frame, and the user can
    // still look around freely from the element-oriented base).
    float m00 = r00, m01 = r01, m02 = r02;
    float m10 = r10, m11 = r11, m12 = r12;
    float m20 = r20, m21 = r21, m22 = r22;
    if (m_camera_anchor_rotation_active)
    {
      const std::array<float, 9>& A = m_camera_anchor_rotation;
      m00 = A[0] * r00 + A[1] * r10 + A[2] * r20;
      m01 = A[0] * r01 + A[1] * r11 + A[2] * r21;
      m02 = A[0] * r02 + A[1] * r12 + A[2] * r22;
      m10 = A[3] * r00 + A[4] * r10 + A[5] * r20;
      m11 = A[3] * r01 + A[4] * r11 + A[5] * r21;
      m12 = A[3] * r02 + A[4] * r12 + A[5] * r22;
      m20 = A[6] * r00 + A[7] * r10 + A[8] * r20;
      m21 = A[6] * r01 + A[7] * r11 + A[8] * r21;
      m22 = A[6] * r02 + A[7] * r12 + A[8] * r22;
    }

    // --- Asymmetric projection from FOV tangent angles ---
    const float tanL = tanf(fov.angleLeft);   // negative
    const float tanR_val = tanf(fov.angleRight);  // positive
    const float tanU = tanf(fov.angleUp);     // positive
    const float tanD = tanf(fov.angleDown);   // negative

    const float inv_w = 1.0f / (tanR_val - tanL);
    const float inv_h = 1.0f / (tanU - tanD);

    // Raw projection rows (before head rotation):
    //   proj_row0 = { 2*inv_w,  0,        (tanR+tanL)*inv_w }
    //   proj_row1 = { 0,        2*inv_h,  (tanU+tanD)*inv_h }
    const float p0x = 2.0f * inv_w;
    const float p0z = (tanR_val + tanL) * inv_w;
    const float p1y = 2.0f * inv_h;
    const float p1z = (tanU + tanD) * inv_h;

    // --- Bake rotation: combined = (A*R) * proj_row ---
    // combined_row0 = (A*R) * {p0x, 0, p0z}
    const float c0x = m00 * p0x + m02 * p0z;
    const float c0y = m10 * p0x + m12 * p0z;
    const float c0z = m20 * p0x + m22 * p0z;

    // combined_row1 = (A*R) * {0, p1y, p1z}
    const float c1x = m01 * p1y + m02 * p1z;
    const float c1y = m11 * p1y + m12 * p1z;
    const float c1z = m21 * p1y + m22 * p1z;

    // Eye position relative to home, in game units. In normal immersive mode this is the original
    // direct OpenXR -> game mapping. In tabletop mode, build the eye position in ROOM space first
    // and transform it through the inverse tabletop rig. Rotation and translation then describe one
    // rigid diorama, which prevents head yaw from making the board roll/tilt or climb vertically.
    float ex = 0.0f;
    float ey = 0.0f;
    float ez = 0.0f;
    if (!tabletop_active)
    {
      ex = (eye_pos_xr.x - m_home_position.x) * s;
      ey = (eye_pos_xr.y - m_home_position.y) * s + camera_height_m * s;
      ez = (eye_pos_xr.z - m_home_position.z) * s - camera_forward_m * s;
    }
    else
    {
      // Board placement in room space: its centre is TabletopDistance metres in front of the
      // initial headset position and TabletopHeight metres below it. Two-hand translation is also
      // stored in room metres, so moving the controllers maps 1:1 to moving the physical board.
      std::array<float, 3> room_eye = {
          (eye_pos_xr.x - m_home_position.x - m_tabletop_user_offset_m[0]) * s,
          (eye_pos_xr.y - m_home_position.y - m_tabletop_user_offset_m[1] +
           g_ActiveConfig.vr_tabletop_height + camera_height_m) *
              s,
          (eye_pos_xr.z - m_home_position.z - m_tabletop_user_offset_m[2] +
           g_ActiveConfig.vr_tabletop_distance - camera_forward_m) *
              s};

      // eye_game = inverse(tabletop_rig) * eye_room. Using the same rigid transform as the
      // orientation is the key to correct 6DoF: IPD, head translation and board placement can no
      // longer disagree about which axis is "up" or "forward".
      const XrQuaternionf inverse_tabletop{-tabletop_rig_quat.x, -tabletop_rig_quat.y,
                                            -tabletop_rig_quat.z, tabletop_rig_quat.w};
      const std::array<float, 3> game_eye = rotate_vector(inverse_tabletop, room_eye);
      ex = game_eye[0];
      ey = game_eye[1];
      ez = game_eye[2];
    }
    if (m_camera_anchor_rotation_active)
    {
      // The offsets above are rig-space; rotate them into view space so IPD, room
      // tracking and the fixed camera offsets stay aligned with the turned rig.
      const std::array<float, 9>& A = m_camera_anchor_rotation;
      const float rx = ex, ry = ey, rz = ez;
      ex = A[0] * rx + A[1] * ry + A[2] * rz;
      ey = A[3] * rx + A[4] * ry + A[5] * rz;
      ez = A[6] * rx + A[7] * ry + A[8] * rz;
    }
    // Camera anchor: committed element position, already in game units and game view
    // space (see CommitCameraAnchorFrame). Zero when no anchor override is active;
    // capture is gated on the enable toggle, so disabling glides the camera home.
    ex += m_camera_anchor_position[0];
    ey += m_camera_anchor_position[1];
    ez += m_camera_anchor_position[2];

    // W component: -dot(combined_xyz, eye_pos) using the ROTATED projection rows.
    // This gives the correct full view transform: P · R^T · (viewPos - eye_pos).
    std::array<float, 4> row0 = {c0x, c0y, c0z, -(c0x * ex + c0y * ey + c0z * ez)};
    std::array<float, 4> row1 = {c1x, c1y, c1z, -(c1x * ex + c1y * ey + c1z * ez)};

    // --- Z-axis row for depth/w computation ---
    // z_eye = ((A*R)^T * (viewPos - eye_pos)).z = dot((A*R)_col2, viewPos - eye_pos)
    // (A*R)_col2 = {m02, m12, m22}
    std::array<float, 4> zrow = {m02, m12, m22, -(m02 * ex + m12 * ey + m22 * ez)};

    if (apply_ac_runtime_view)
    {
      // Animal Crossing already transformed vertices by its moving GameCube camera before they
      // arrive here. Compose the inverse camera motion into every VR row so the village remains
      // room-anchored while the original camera follows the player, talks, zooms or tilts.
      const auto compose_camera_anchor = [&](std::array<float, 4> row) {
        const auto& m = ac_tabletop_view_transform;
        const float x = row[0], y = row[1], z = row[2], w = row[3];
        return std::array<float, 4>{
            x * m[0] + y * m[4] + z * m[8],
            x * m[1] + y * m[5] + z * m[9],
            x * m[2] + y * m[6] + z * m[10],
            x * m[3] + y * m[7] + z * m[11] + w};
      };
      row0 = compose_camera_anchor(row0);
      row1 = compose_camera_anchor(row1);
      zrow = compose_camera_anchor(zrow);
    }

    out_proj_rows[eye * 2 + 0] = row0;
    out_proj_rows[eye * 2 + 1] = row1;
    out_z_rows[eye] = zrow;

  }
}

void OpenXRManager::GetRawEyeProjectionRows(
    float units_per_meter,
    std::array<std::array<float, 4>, 4>& out_proj_rows) const
{
  const float s = std::max(units_per_meter, 0.0001f);
  const std::array<XREyeView, 2> eye_views = GetTrackingAdjustedEyeViews();

  // Compute head center (average of both eyes) for extracting per-eye local offset.
  const float hx = 0.5f * (eye_views[0].pose.position.x + eye_views[1].pose.position.x);
  const float hy = 0.5f * (eye_views[0].pose.position.y + eye_views[1].pose.position.y);
  const float hz = 0.5f * (eye_views[0].pose.position.z + eye_views[1].pose.position.z);

  // Get head rotation to compute R^T * (eye_world - head_center) = local eye offset.
  const XrQuaternionf& q_xr = eye_views[0].pose.orientation;
  const XrQuaternionf q = {-q_xr.x, -q_xr.y, -q_xr.z, q_xr.w};
  const float x2 = 2.0f * q.x * q.x, y2 = 2.0f * q.y * q.y, z2 = 2.0f * q.z * q.z;
  const float xy = 2.0f * q.x * q.y, xz = 2.0f * q.x * q.z, yz = 2.0f * q.y * q.z;
  const float wx = 2.0f * q.w * q.x, wy = 2.0f * q.w * q.y, wz = 2.0f * q.w * q.z;

  // R^T rows (= R columns) for transforming world offset to head-local space
  const float rt00 = 1.0f - y2 - z2, rt01 = xy - wz, rt02 = xz + wy;
  const float rt10 = xy + wz, rt11 = 1.0f - x2 - z2, rt12 = yz - wx;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    const XrFovf& fov = eye_views[eye].fov;
    const XrVector3f& eye_pos_xr = eye_views[eye].pose.position;

    // --- Asymmetric projection from FOV tangent angles (same as rotated version) ---
    const float tanL = tanf(fov.angleLeft);
    const float tanR_val = tanf(fov.angleRight);
    const float tanU = tanf(fov.angleUp);
    const float tanD = tanf(fov.angleDown);

    const float inv_w = 1.0f / (tanR_val - tanL);
    const float inv_h = 1.0f / (tanU - tanD);

    // Raw (unrotated) projection rows
    const float p0x = 2.0f * inv_w;
    const float p0z = (tanR_val + tanL) * inv_w;
    const float p1y = 2.0f * inv_h;
    const float p1z = (tanU + tanD) * inv_h;

    // Per-eye offset in head-local space: R^T * (eye_world - head_center)
    const float dx = eye_pos_xr.x - hx;
    const float dy = eye_pos_xr.y - hy;
    const float dz = eye_pos_xr.z - hz;

    const float local_ex = (rt00 * dx + rt01 * dy + rt02 * dz) * s;
    const float local_ey = (rt10 * dx + rt11 * dy + rt12 * dz) * s;

    // W component using raw (unrotated) P rows and head-local eye offset
    const float pw0 = -(p0x * local_ex + p0z * 0.0f);  // p0z * local_ez ≈ 0
    const float pw1 = -(p1y * local_ey + p1z * 0.0f);  // p1z * local_ez ≈ 0

    out_proj_rows[eye * 2 + 0] = {p0x, 0.0f, p0z, pw0};
    out_proj_rows[eye * 2 + 1] = {0.0f, p1y, p1z, pw1};
  }
}

bool OpenXRManager::IsQuestOrVirtualDesktopRuntime() const
{
  // Cached: queried per draw on the Vulkan path; the case-insensitive name scans must not
  // run hundreds of thousands of times per frame. Names are fixed after instance/system
  // init (the cache is reset where they are assigned).
  if (!m_quest_or_vd_runtime.has_value())
  {
    const bool virtual_desktop_runtime =
        Common::CaseInsensitiveContains(m_runtime_name, "virtualdesktop") ||
        Common::CaseInsensitiveContains(m_runtime_name, "virtual desktop");
    const bool quest_class_system =
        Common::CaseInsensitiveContains(m_system_name, "quest") ||
        Common::CaseInsensitiveContains(m_system_name, "oculus") ||
        Common::CaseInsensitiveContains(m_system_name, "meta");
    m_quest_or_vd_runtime = virtual_desktop_runtime || quest_class_system;
  }
  return *m_quest_or_vd_runtime;
}

}  // namespace VR

#endif  // ENABLE_VR
