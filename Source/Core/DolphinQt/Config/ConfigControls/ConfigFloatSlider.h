// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QLabel>

#include "DolphinQt/Config/ConfigControls/ConfigControl.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipSlider.h"

#include "Common/Config/ConfigInfo.h"

// Automatically converts an int slider into a float one.
// Do not read the int values or ranges directly from it.
class ConfigFloatSlider final : public ConfigControl<ToolTipSlider>
{
  Q_OBJECT
public:
  // Linear: value = step * position + minimum (uniform spacing).
  // Exponential: value grows geometrically across the slider, giving fine control near the minimum
  // and coarse (large) values near the maximum. Requires minimum > 0.
  enum class ScaleMode
  {
    Linear,
    Exponential,
  };

  ConfigFloatSlider(float minimum, float maximum, const Config::Info<float>& setting, float step,
                    Config::Layer* layer = nullptr, ScaleMode scale = ScaleMode::Linear,
                    Config::Layer* fallback_layer = nullptr);
  void Update(int value);

  // Returns the adjusted float value
  float GetValue() const;

protected:
  void OnConfigChanged() override;

private:
  // Map between the underlying integer slider position and the exposed float value, honoring
  // the configured scale mode.
  float PositionToValue(int position) const;
  int ValueToPosition(float value) const;

  float m_minimum;
  float m_maximum;
  float m_step;
  ScaleMode m_scale;
  const Config::Info<float> m_setting;
};

class ConfigFloatLabel final : public QLabel
{
public:
  ConfigFloatLabel(const QString& text, ConfigFloatSlider* widget);
};
