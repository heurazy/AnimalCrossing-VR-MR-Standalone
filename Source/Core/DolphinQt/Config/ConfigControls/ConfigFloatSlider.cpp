// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include <QPointer>

#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"

ConfigFloatSlider::ConfigFloatSlider(float minimum, float maximum,
                                     const Config::Info<float>& setting, float step,
                                     Config::Layer* layer, ScaleMode scale,
                                     Config::Layer* fallback_layer)
    : ConfigControl(Qt::Horizontal, setting.GetLocation(), layer, fallback_layer),
      m_minimum(minimum), m_maximum(maximum), m_step(step), m_scale(scale), m_setting(setting)
{
  const float range = maximum - minimum;
  const int steps = std::round(range / step);
  const int interval = std::round(range / steps);

  setMinimum(0);
  setMaximum(steps);
  setTickInterval(interval);
  setValue(ValueToPosition(ReadValue(setting)));

  connect(this, &ConfigFloatSlider::valueChanged, this, &ConfigFloatSlider::Update);
}

void ConfigFloatSlider::Update(int value)
{
  SaveValue(m_setting, PositionToValue(value));
}

float ConfigFloatSlider::GetValue() const
{
  return PositionToValue(value());
}

void ConfigFloatSlider::OnConfigChanged()
{
  setValue(ValueToPosition(ReadValue(m_setting)));
}

float ConfigFloatSlider::PositionToValue(int position) const
{
  if (m_scale == ScaleMode::Exponential)
  {
    const int steps = maximum();
    if (steps <= 0 || m_minimum <= 0.f)
      return m_minimum;
    const double t = static_cast<double>(position) / steps;
    return static_cast<float>(m_minimum * std::pow(m_maximum / m_minimum, t));
  }

  return (m_step * position) + m_minimum;
}

int ConfigFloatSlider::ValueToPosition(float value) const
{
  if (m_scale == ScaleMode::Exponential)
  {
    const int steps = maximum();
    if (steps <= 0 || m_minimum <= 0.f)
      return 0;
    const double clamped = std::clamp(value, m_minimum, m_maximum);
    const double t = std::log(clamped / m_minimum) / std::log(m_maximum / m_minimum);
    return static_cast<int>(std::lround(t * steps));
  }

  return static_cast<int>(std::lround((value - m_minimum) / m_step));
}

ConfigFloatLabel::ConfigFloatLabel(const QString& text, ConfigFloatSlider* widget) : QLabel(text)
{
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, [this, widget = QPointer{widget}] {
    // Label shares font changes with ConfigFloatSlider to mark game ini settings.
    if (widget)
      setFont(widget->font());
  });
}
