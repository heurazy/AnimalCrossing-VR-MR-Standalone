// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class ControlReference;

namespace VR
{
enum class OpenXRUtilitySessionState : uint8_t
{
  Idle,
  Starting,
  Running,
  ApplyPending,
  Applied,
  Cancelled,
  Failed,
};

enum class OpenXRUtilitySessionFailure : uint8_t
{
  None,
  RuntimeUnavailable,
  VulkanUnavailable,
  SessionBusy,
  InitializationFailed,
  SessionLost,
};

enum class OpenXRUtilitySessionTargetType : uint8_t
{
  WiiRemote,
  GameCubeController,
  Hotkeys,
};

struct OpenXRUtilitySessionTarget
{
  OpenXRUtilitySessionTargetType type = OpenXRUtilitySessionTargetType::WiiRemote;
  int port = -1;
};

struct OpenXRPendingBinding
{
  ControlReference* reference = nullptr;
  std::string expression;
};

struct OpenXRPendingBindings
{
  OpenXRUtilitySessionTarget target;
  std::string default_device;
  std::vector<OpenXRPendingBinding> bindings;
};

// Owns a small Vulkan-backed OpenXR session used only while emulation is stopped. The session
// displays an interactive controller-binding quad and feeds the regular OpenXR ControllerInterface
// device. Configuration changes are staged on the XR thread and handed to the UI thread atomically.
class OpenXRUtilitySession final
{
public:
  OpenXRUtilitySession();
  ~OpenXRUtilitySession();

  OpenXRUtilitySession(const OpenXRUtilitySession&) = delete;
  OpenXRUtilitySession& operator=(const OpenXRUtilitySession&) = delete;

  bool Start(OpenXRUtilitySessionTarget target);
  bool Start(int wiimote_port);
  void RequestStop();
  void Stop();

  OpenXRUtilitySessionState GetState() const;
  OpenXRUtilitySessionFailure GetFailureReason() const;
  std::string GetFailureMessage() const;

  OpenXRPendingBindings TakePendingBindings();
  void MarkApplied();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  std::atomic<OpenXRUtilitySessionState> m_state{OpenXRUtilitySessionState::Idle};
  std::atomic<OpenXRUtilitySessionFailure> m_failure{OpenXRUtilitySessionFailure::None};
  mutable std::mutex m_result_mutex;
  std::string m_failure_message;
  OpenXRPendingBindings m_pending_bindings;
};
}  // namespace VR

#endif  // ENABLE_VR
