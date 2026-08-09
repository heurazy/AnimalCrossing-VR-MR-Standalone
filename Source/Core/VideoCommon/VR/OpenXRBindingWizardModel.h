// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"

namespace VR
{
inline bool IsOpenXRMapperMappableGroup(ControllerEmu::GroupType type)
{
  switch (type)
  {
  case ControllerEmu::GroupType::Attachments:
  case ControllerEmu::GroupType::Force:
  case ControllerEmu::GroupType::Tilt:
  case ControllerEmu::GroupType::Cursor:
  case ControllerEmu::GroupType::Shake:
  case ControllerEmu::GroupType::IMUAccelerometer:
  case ControllerEmu::GroupType::IMUGyroscope:
  case ControllerEmu::GroupType::IMUCursor:
  case ControllerEmu::GroupType::IRPassthrough:
    return false;
  default:
    return true;
  }
}

// UI-independent staged state for the immersive mapper. No controller configuration is changed
// until the owner consumes Expressions() after Finish().
class OpenXRBindingWizardModel
{
public:
  enum class Result
  {
    None,
    Apply,
    Cancel,
  };

  using Clock = std::chrono::steady_clock;

  explicit OpenXRBindingWizardModel(std::vector<std::string> expressions)
      : m_expressions(std::move(expressions))
  {
  }

  bool IsValid() const { return !m_expressions.empty(); }
  size_t GetCount() const { return m_expressions.size(); }
  size_t GetIndex() const { return m_index; }
  const std::string& GetExpression() const { return m_expressions.at(m_index); }
  const std::vector<std::string>& GetExpressions() const { return m_expressions; }
  Result GetResult() const { return m_result; }

  bool Select(size_t index)
  {
    if (index >= m_expressions.size())
      return false;
    m_index = index;
    return true;
  }

  bool Back()
  {
    if (m_index == 0)
      return false;
    --m_index;
    return true;
  }

  bool Next()
  {
    if (m_index + 1 >= m_expressions.size())
      return false;
    ++m_index;
    return true;
  }

  bool Skip() { return Next(); }
  void Clear() { m_expressions.at(m_index).clear(); }

  void BeginBinding()
  {
    m_waiting_for_neutral = true;
    m_neutral_since.reset();
  }

  // Returns true exactly once after every input has remained neutral for the debounce period.
  bool UpdateNeutralGate(bool neutral, Clock::time_point now)
  {
    if (!m_waiting_for_neutral)
      return false;
    if (!neutral)
    {
      m_neutral_since.reset();
      return false;
    }
    if (!m_neutral_since)
    {
      m_neutral_since = now;
      return false;
    }
    if (now - *m_neutral_since < std::chrono::milliseconds(150))
      return false;
    m_waiting_for_neutral = false;
    m_neutral_since.reset();
    return true;
  }

  void AcceptBinding(std::string expression) { m_expressions.at(m_index) = std::move(expression); }

  void Finish() { m_result = Result::Apply; }
  void Cancel() { m_result = Result::Cancel; }

private:
  std::vector<std::string> m_expressions;
  size_t m_index = 0;
  bool m_waiting_for_neutral = false;
  std::optional<Clock::time_point> m_neutral_since;
  Result m_result = Result::None;
};
}  // namespace VR
