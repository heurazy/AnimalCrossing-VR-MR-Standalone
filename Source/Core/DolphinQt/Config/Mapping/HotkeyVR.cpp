// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/Mapping/HotkeyVR.h"

#include <initializer_list>

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>

#include "Core/HotkeyManager.h"

HotkeyVR::HotkeyVR(MappingWindow* window, Page page) : MappingWidget(window), m_page(page)
{
  CreateMainLayout();
}

void HotkeyVR::CreateMainLayout()
{
  m_main_layout = new QHBoxLayout();

  if (m_page == Page::Overrides)
  {
    m_main_layout->addWidget(
        CreateGroupBox(tr("Shader"), HotkeyManagerEmu::GetHotkeyGroup(HKGP_VR_SHADER)));
    m_main_layout->addWidget(
        CreateGroupBox(tr("Texture"), HotkeyManagerEmu::GetHotkeyGroup(HKGP_VR_TEXTURE)));
    setLayout(m_main_layout);
    return;
  }

  const auto* const vr_hotkeys = HotkeyManagerEmu::GetHotkeyGroup(HKGP_VR);
  const auto create_vr_group_box = [this, vr_hotkeys](const QString& name,
                                                      std::initializer_list<Hotkey> hotkeys) {
    auto* const group_box = new QGroupBox(name);
    auto* const form_layout = new QFormLayout;
    group_box->setLayout(form_layout);

    for (const Hotkey hotkey : hotkeys)
    {
      const size_t index = static_cast<size_t>(hotkey - HK_VR_TOGGLE_OPENXR);
      CreateControl(vr_hotkeys->controls.at(index).get(), form_layout, true);
    }

    return group_box;
  };

  m_main_layout->addWidget(
      create_vr_group_box(tr("VR"), {HK_VR_TOGGLE_OPENXR, HK_VR_DECREASE_UNITS_PER_METER,
                                     HK_VR_INCREASE_UNITS_PER_METER, HK_VR_TOGGLE_DONT_CLEAR_SCREEN,
                                     HK_VR_TOGGLE_FORCE_VBI, HK_VR_TOGGLE_REMOVE_CINEMATIC_BARS,
                                     HK_VR_TOGGLE_CONTROLLER_ANCHOR}));

  auto* const camera_column = new QWidget;
  auto* const camera_layout = new QVBoxLayout(camera_column);
  camera_layout->addWidget(create_vr_group_box(
      tr("Camera"),
      {HK_VR_RESET_POSITION, HK_VR_DECREASE_LEAN_BACK_ANGLE, HK_VR_INCREASE_LEAN_BACK_ANGLE,
       HK_VR_TOGGLE_ENABLE_CAMERA_FORWARD, HK_VR_DECREASE_CAMERA_FORWARD,
       HK_VR_INCREASE_CAMERA_FORWARD, HK_VR_TOGGLE_ENABLE_CAMERA_HEIGHT,
       HK_VR_DECREASE_CAMERA_HEIGHT, HK_VR_INCREASE_CAMERA_HEIGHT, HK_VR_TOGGLE_CAMERA_ANCHOR}));
  camera_layout->addWidget(create_vr_group_box(
      tr("Virtual Screen"),
      {HK_VR_TOGGLE_VIRTUAL_SCREEN, HK_VR_DECREASE_SCREEN_DISTANCE, HK_VR_INCREASE_SCREEN_DISTANCE,
       HK_VR_DECREASE_SCREEN_SIZE, HK_VR_INCREASE_SCREEN_SIZE, HK_VR_DECREASE_SCREEN_CURVATURE,
       HK_VR_INCREASE_SCREEN_CURVATURE}));
  camera_layout->addStretch();
  m_main_layout->addWidget(camera_column);

  setLayout(m_main_layout);
}

InputConfig* HotkeyVR::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}

void HotkeyVR::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

void HotkeyVR::SaveSettings()
{
  HotkeyManagerEmu::GetConfig()->SaveConfig();
}
