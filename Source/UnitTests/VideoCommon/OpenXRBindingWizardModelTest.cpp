// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "VideoCommon/VR/OpenXRBindingWizardModel.h"

using namespace std::chrono_literals;

TEST(OpenXRBindingWizardModel, EnumeratesOrdinaryGroupsOnly)
{
  EXPECT_TRUE(VR::IsOpenXRMapperMappableGroup(ControllerEmu::GroupType::Buttons));
  EXPECT_TRUE(VR::IsOpenXRMapperMappableGroup(ControllerEmu::GroupType::Stick));
  EXPECT_TRUE(VR::IsOpenXRMapperMappableGroup(ControllerEmu::GroupType::Triggers));
  EXPECT_FALSE(VR::IsOpenXRMapperMappableGroup(ControllerEmu::GroupType::Attachments));
  EXPECT_FALSE(VR::IsOpenXRMapperMappableGroup(ControllerEmu::GroupType::Force));
  EXPECT_FALSE(VR::IsOpenXRMapperMappableGroup(ControllerEmu::GroupType::Cursor));
  EXPECT_FALSE(VR::IsOpenXRMapperMappableGroup(ControllerEmu::GroupType::IMUAccelerometer));
  EXPECT_FALSE(VR::IsOpenXRMapperMappableGroup(ControllerEmu::GroupType::IMUGyroscope));
}

TEST(OpenXRBindingWizardModel, NavigationClearAndSkip)
{
  VR::OpenXRBindingWizardModel model({"A", "B", "C"});
  EXPECT_FALSE(model.Back());
  EXPECT_TRUE(model.Skip());
  EXPECT_EQ(model.GetIndex(), 1u);
  model.Clear();
  EXPECT_TRUE(model.GetExpression().empty());
  EXPECT_TRUE(model.Back());
  EXPECT_EQ(model.GetExpression(), "A");
}

TEST(OpenXRBindingWizardModel, RequiresNeutralDebounceAfterClick)
{
  VR::OpenXRBindingWizardModel model({"A"});
  const auto start = VR::OpenXRBindingWizardModel::Clock::time_point{};
  model.BeginBinding();
  EXPECT_FALSE(model.UpdateNeutralGate(false, start));
  EXPECT_FALSE(model.UpdateNeutralGate(true, start + 10ms));
  EXPECT_FALSE(model.UpdateNeutralGate(true, start + 159ms));
  EXPECT_TRUE(model.UpdateNeutralGate(true, start + 160ms));
  EXPECT_FALSE(model.UpdateNeutralGate(true, start + 500ms));
}

TEST(OpenXRBindingWizardModel, DigitalAndAnalogExpressionsAreStaged)
{
  VR::OpenXRBindingWizardModel model({"old digital", "old analog"});
  model.AcceptBinding("`Left Button A`");
  EXPECT_TRUE(model.Select(1));
  model.AcceptBinding("`Right Trigger` > 0.5");
  EXPECT_EQ(model.GetExpressions()[0], "`Left Button A`");
  EXPECT_EQ(model.GetExpressions()[1], "`Right Trigger` > 0.5");
}

TEST(OpenXRBindingWizardModel, SelectsBindingDirectly)
{
  VR::OpenXRBindingWizardModel model({"A", "B", "C"});
  EXPECT_TRUE(model.Select(2));
  model.AcceptBinding("X");
  EXPECT_EQ(model.GetIndex(), 2u);
  EXPECT_EQ(model.GetExpressions(), (std::vector<std::string>{"A", "B", "X"}));
  EXPECT_FALSE(model.Select(3));
}

TEST(OpenXRBindingWizardModel, CancelPreservesOriginalConfigurationUntilCommit)
{
  const std::vector<std::string> original{"A", "B"};
  VR::OpenXRBindingWizardModel model(original);
  model.Clear();
  model.Cancel();
  EXPECT_EQ(model.GetResult(), VR::OpenXRBindingWizardModel::Result::Cancel);
  EXPECT_EQ(original, (std::vector<std::string>{"A", "B"}));
}

TEST(OpenXRBindingWizardModel, FinishPublishesWholeStagedSet)
{
  VR::OpenXRBindingWizardModel model({"A", "B"});
  model.AcceptBinding("X");
  EXPECT_TRUE(model.Select(1));
  model.Clear();
  model.Finish();
  EXPECT_EQ(model.GetResult(), VR::OpenXRBindingWizardModel::Result::Apply);
  EXPECT_EQ(model.GetExpressions(), (std::vector<std::string>{"X", ""}));
}
