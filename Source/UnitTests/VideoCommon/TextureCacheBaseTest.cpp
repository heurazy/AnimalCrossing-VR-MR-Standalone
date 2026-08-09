// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureInfo.h"

TEST(TextureInfo, ParsesTextureHashFromDumpFilename)
{
  const auto hash =
      TextureInfo::ParseTextureHash("tex1_512x256_m_adf692a6f5270302_c5cbf6cb131b2b80_9.png");
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(*hash, 0xadf692a6f5270302ULL);

  const auto non_mipmapped_hash =
      TextureInfo::ParseTextureHash("tex1_128x128_017fce12e592f4da_14.png");
  ASSERT_TRUE(non_mipmapped_hash.has_value());
  EXPECT_EQ(*non_mipmapped_hash, 0x017fce12e592f4daULL);
}

TEST(TextureInfo, RejectsInvalidTextureNames)
{
  EXPECT_FALSE(TextureInfo::ParseTextureHash("texture_512x256_adf692a6f5270302_9.png"));
  EXPECT_FALSE(TextureInfo::ParseTextureHash("tex1_512x256_m_not-a-hash_9.png"));
  EXPECT_FALSE(TextureInfo::ParseTextureHash("tex1_512x256_adf692a6f52703020_9.png"));
}

TEST(TextureCacheBase, MetroidPrime1ThermalSourceCandidateMatchesStereoColorEfbCopy)
{
  EXPECT_TRUE(TextureCacheBase::IsMetroidPrime1ThermalStereoSourceCandidate(
      640, 448, 2, false, false));
  EXPECT_TRUE(TextureCacheBase::IsMetroidPrime1ThermalStereoSourceCandidate(
      640, 448, 3, false, false));
}

TEST(TextureCacheBase, MetroidPrime1ThermalSourceCandidateRejectsWrongCopies)
{
  EXPECT_FALSE(TextureCacheBase::IsMetroidPrime1ThermalStereoSourceCandidate(
      640, 448, 1, false, false));
  EXPECT_FALSE(TextureCacheBase::IsMetroidPrime1ThermalStereoSourceCandidate(
      320, 224, 2, false, false));
  EXPECT_FALSE(TextureCacheBase::IsMetroidPrime1ThermalStereoSourceCandidate(
      640, 448, 2, true, false));
  EXPECT_FALSE(TextureCacheBase::IsMetroidPrime1ThermalStereoSourceCandidate(
      640, 448, 2, false, true));
}
