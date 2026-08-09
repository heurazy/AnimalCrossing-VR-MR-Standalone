// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/RuntimeElementMatcher.h"
#include "VideoCommon/ShaderHunter.h"

namespace
{
ShaderHunter::RuntimeElementSignature MakePerspectiveSignature()
{
  ShaderHunter::RuntimeElementSignature signature;
  signature.valid = true;
  signature.perspective = true;
  signature.use_projection = true;
  signature.use_viewport = true;
  signature.use_scissor = true;
  signature.use_render_state = true;
  signature.perspective_hfov_x100 = 8;
  signature.perspective_vfov_x100 = 6;
  signature.perspective_near_x1000 = 1;
  signature.perspective_far_x100 = 409;
  signature.viewport_x = 662;
  signature.viewport_y = 566;
  signature.viewport_width = 320;
  signature.viewport_height = 224;
  signature.scissor_left = 342;
  signature.scissor_top = 342;
  signature.scissor_right = 981;
  signature.scissor_bottom = 789;
  signature.alpha_test_hex = 0x007f0000;
  signature.ztest = true;
  signature.zupdate = false;
  signature.zfunc = 3;
  signature.blend_color_update = true;
  signature.blend_alpha_update = true;
  return signature;
}
}  // namespace

TEST(RuntimeElementMatcher, PerspectiveProjectionAllowsOneQuantizationStep)
{
  const auto pattern = MakePerspectiveSignature();
  auto candidate = pattern;
  candidate.perspective_hfov_x100 += 1;
  candidate.perspective_vfov_x100 -= 1;
  candidate.perspective_near_x1000 += 1;
  candidate.perspective_far_x100 -= 1;

  EXPECT_TRUE(RuntimeElementMatcher::Matches(pattern, candidate));
}

TEST(RuntimeElementMatcher, PerspectiveProjectionRejectsLargerDifferences)
{
  const auto pattern = MakePerspectiveSignature();
  auto candidate = pattern;
  candidate.perspective_hfov_x100 += 2;

  EXPECT_FALSE(RuntimeElementMatcher::Matches(pattern, candidate));
}

TEST(RuntimeElementMatcher, NonProjectionGroupsRemainExact)
{
  const auto pattern = MakePerspectiveSignature();
  auto candidate = pattern;
  candidate.viewport_width += 1;

  EXPECT_FALSE(RuntimeElementMatcher::Matches(pattern, candidate));
}
