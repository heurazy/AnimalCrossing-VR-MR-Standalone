// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/Mapping/OpenXRWiimoteConfigSessionController.h"

#include <utility>

#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/Settings.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#if defined(ENABLE_VR) && defined(HAS_VULKAN)
#include "VideoCommon/VR/OpenXRUtilitySession.h"

namespace
{
VR::OpenXRUtilitySessionTargetType ToUtilityTargetType(
    OpenXRWiimoteConfigSessionController::TargetType type)
{
  switch (type)
  {
  case OpenXRWiimoteConfigSessionController::TargetType::WiiRemote:
    return VR::OpenXRUtilitySessionTargetType::WiiRemote;
  case OpenXRWiimoteConfigSessionController::TargetType::GameCubeController:
    return VR::OpenXRUtilitySessionTargetType::GameCubeController;
  case OpenXRWiimoteConfigSessionController::TargetType::Hotkeys:
    return VR::OpenXRUtilitySessionTargetType::Hotkeys;
  }
  return VR::OpenXRUtilitySessionTargetType::WiiRemote;
}
}  // namespace
#endif

OpenXRWiimoteConfigSessionController::OpenXRWiimoteConfigSessionController(MappingWindow* window,
                                                                           int port,
                                                                           TargetType target_type)
    : QObject(window), m_window(window), m_port(port), m_target_type(target_type),
      m_button(new QPushButton(tr("Configure in VR"), window)), m_status_timer(new QTimer(this))
{
  m_button->setToolTip(
      tr("Open an immersive OpenXR controller binding wizard. Emulation must be stopped."));
  m_status_timer->setInterval(100);

  connect(m_button, &QPushButton::clicked, this, &OpenXRWiimoteConfigSessionController::Start);
  connect(m_status_timer, &QTimer::timeout, this, &OpenXRWiimoteConfigSessionController::PollState);
  connect(window, &QDialog::finished, this, &OpenXRWiimoteConfigSessionController::Stop);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state != Core::State::Uninitialized)
      Stop();
  });
}

OpenXRWiimoteConfigSessionController::~OpenXRWiimoteConfigSessionController()
{
  // The button is reparented into the Device group box by its layout. That group box may already
  // have destroyed the button by the time QObject destroys this sibling controller. Only shut down
  // the native session here; UI restoration belongs to Stop(), while the dialog is still alive.
  m_status_timer->stop();
  ShutdownSession();
}

QPushButton* OpenXRWiimoteConfigSessionController::GetButton() const
{
  return m_button.data();
}

void OpenXRWiimoteConfigSessionController::Start()
{
#if defined(ENABLE_VR) && defined(HAS_VULKAN)
  if (m_session)
    return;

  if (Core::GetState(Core::System::GetInstance()) != Core::State::Uninitialized)
  {
    QMessageBox::warning(m_window, tr("OpenXR Controller Mapper"),
                         tr("Stop emulation before opening the controller mapper."));
    return;
  }

  m_session = std::make_unique<VR::OpenXRUtilitySession>();
  const VR::OpenXRUtilitySessionTarget target{ToUtilityTargetType(m_target_type), m_port};
  if (!m_session->Start(target))
  {
    QMessageBox::critical(m_window, tr("OpenXR Controller Mapper"),
                          QString::fromStdString(m_session->GetFailureMessage()));
    m_session.reset();
    return;
  }

  m_button->setEnabled(false);
  m_button->setText(tr("Starting VR mapper..."));
  m_window->setEnabled(false);
  m_status_timer->start();
#else
  QMessageBox::critical(m_window, tr("OpenXR Controller Mapper"),
                        tr("This build does not include OpenXR and Vulkan support."));
#endif
}

void OpenXRWiimoteConfigSessionController::Stop()
{
  m_status_timer->stop();
  ShutdownSession();

  if (m_button)
  {
    m_button->setEnabled(true);
    m_button->setText(tr("Configure in VR"));
  }
  if (m_window)
    m_window->setEnabled(true);
}

void OpenXRWiimoteConfigSessionController::ShutdownSession()
{
#if defined(ENABLE_VR) && defined(HAS_VULKAN)
  if (m_session)
  {
    m_session->Stop();
    m_session.reset();
  }
#endif
}

void OpenXRWiimoteConfigSessionController::PollState()
{
#if defined(ENABLE_VR) && defined(HAS_VULKAN)
  if (!m_session)
    return;

  switch (m_session->GetState())
  {
  case VR::OpenXRUtilitySessionState::Idle:
    Stop();
    break;
  case VR::OpenXRUtilitySessionState::Starting:
    m_button->setText(tr("Starting VR mapper..."));
    break;
  case VR::OpenXRUtilitySessionState::Running:
    m_button->setText(tr("VR mapper running..."));
    break;
  case VR::OpenXRUtilitySessionState::ApplyPending:
    FinishSession();
    break;
  case VR::OpenXRUtilitySessionState::Applied:
  case VR::OpenXRUtilitySessionState::Cancelled:
    Stop();
    break;
  case VR::OpenXRUtilitySessionState::Failed:
  {
    const QString message = QString::fromStdString(m_session->GetFailureMessage());
    Stop();
    QMessageBox::critical(m_window, tr("OpenXR Controller Mapper"), message);
    break;
  }
  }
#endif
}

void OpenXRWiimoteConfigSessionController::FinishSession()
{
#if defined(ENABLE_VR) && defined(HAS_VULKAN)
  VR::OpenXRPendingBindings pending = m_session->TakePendingBindings();
  auto* controller = m_window->GetController();
  const auto expected_target = ToUtilityTargetType(m_target_type);
  if (!controller || pending.target.type != expected_target || pending.target.port != m_port)
  {
    Stop();
    QMessageBox::critical(m_window, tr("OpenXR Controller Mapper"),
                          tr("The selected controller changed before the bindings were applied."));
    return;
  }

  {
    const auto lock = controller->GetStateLock();
    ciface::Core::DeviceQualifier device;
    device.FromString(pending.default_device);
    controller->SetDefaultDevice(device);
    for (auto& binding : pending.bindings)
    {
      if (binding.reference)
        binding.reference->SetExpression(std::move(binding.expression));
    }
    controller->UpdateReferences(g_controller_interface);
  }

  emit m_window->ConfigChanged();
  emit m_window->Save();
  m_session->MarkApplied();
  Stop();
#endif
}
