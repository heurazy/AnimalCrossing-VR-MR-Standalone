// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <QObject>
#include <QPointer>

namespace VR
{
#if defined(ENABLE_VR) && defined(HAS_VULKAN)
class OpenXRUtilitySession;
#endif
}  // namespace VR

class MappingWindow;
class QPushButton;
class QTimer;

class OpenXRWiimoteConfigSessionController final : public QObject
{
  Q_OBJECT

public:
  enum class TargetType
  {
    WiiRemote,
    GameCubeController,
    Hotkeys,
  };

  OpenXRWiimoteConfigSessionController(MappingWindow* window, int port, TargetType target_type);
  ~OpenXRWiimoteConfigSessionController() override;

  QPushButton* GetButton() const;

private:
  void Start();
  void Stop();
  void ShutdownSession();
  void PollState();
  void FinishSession();

  QPointer<MappingWindow> m_window;
  const int m_port;
  const TargetType m_target_type;
  QPointer<QPushButton> m_button;
  QTimer* const m_status_timer;
#if defined(ENABLE_VR) && defined(HAS_VULKAN)
  std::unique_ptr<VR::OpenXRUtilitySession> m_session;
#endif
};
