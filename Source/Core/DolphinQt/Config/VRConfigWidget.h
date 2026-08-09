// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <QWidget>

#include "Common/CommonTypes.h"

class QTabWidget;

namespace Config
{
class Layer;
}

class VRConfigWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit VRConfigWidget(std::string game_id, std::optional<u16> revision = std::nullopt,
                          QWidget* parent = nullptr);
  ~VRConfigWidget() override;

private:
  void CreateWidgets();
  void SaveSettings();
  void ReloadSettings();
  void RefreshEditorTabs();

  std::string GetLocalINIPath() const;
  std::string GetLayerState() const;

  const std::string m_game_id;
  const std::optional<u16> m_revision;
  std::unique_ptr<Config::Layer> m_layer;
  std::unique_ptr<Config::Layer> m_global_layer;
  std::string m_saved_layer_state;

  QTabWidget* m_default_tab = nullptr;
  QTabWidget* m_local_tab = nullptr;
  int m_prev_tab_index = 0;
};
