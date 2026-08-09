// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

class QHBoxLayout;

class HotkeyVR final : public MappingWidget
{
  Q_OBJECT
public:
  enum class Page
  {
    VR,
    Overrides,
  };

  explicit HotkeyVR(MappingWindow* window, Page page);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;
  void CreateMainLayout();

  Page m_page;
  QHBoxLayout* m_main_layout;
};
