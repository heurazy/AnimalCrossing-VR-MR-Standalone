// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/MetroidElementClassifier.h"

namespace
{
MetroidProjectionMetrics MakePerspective(float hfov, float vfov, float zfar, u32 sequence = 0)
{
  MetroidProjectionMetrics metrics;
  metrics.perspective = true;
  metrics.projection_sequence = sequence;
  metrics.hfov = hfov;
  metrics.vfov = vfov;
  metrics.znear = 1.0f;
  metrics.zfar = zfar;
  return metrics;
}
}  // namespace

TEST(MetroidElementClassifier, ParsesProfileAndLayerNames)
{
  EXPECT_EQ(MetroidElementProfileFromString("metroid_prime_1_gc"),
            MetroidElementProfile::Prime1GC);
  EXPECT_EQ(MetroidElementProfileFromString("Metroid Prime Trilogy Auto"),
            MetroidElementProfile::TrilogyAuto);
  EXPECT_EQ(MetroidElementLayerFromString("METROID_SCAN_HOLOGRAM"),
            MetroidElementLayer::ScanHologram);
  EXPECT_EQ(MetroidElementLayerFromString("Dark Visor HUD"), MetroidElementLayer::DarkVisorHUD);
}

TEST(MetroidElementClassifier, Prime1CompactHudProjectionMatchesHydraHudFamily)
{
  MetroidElementClassifier classifier;

  const MetroidElementLayer layer = classifier.Classify(
      MetroidElementProfile::Prime1GC, MakePerspective(0.08f, 0.06f, 4.096f));

  EXPECT_EQ(layer, MetroidElementLayer::ScanText);
}

TEST(MetroidElementClassifier, Prime2WideHudSequenceIsStateful)
{
  MetroidElementClassifier classifier;

  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime2GC,
                                MakePerspective(82.43f, 65.0f, 4096.0f, 1)),
            MetroidElementLayer::Helmet);
  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime2GC,
                                MakePerspective(82.43f, 65.0f, 4096.0f, 2)),
            MetroidElementLayer::HUD);

  classifier.ResetFrame();
  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime2GC,
                                MakePerspective(82.43f, 65.0f, 4096.0f, 1)),
            MetroidElementLayer::Helmet);
}

TEST(MetroidElementClassifier, Prime3HudAndTrilogyAutoClassifyHudLayers)
{
  MetroidElementClassifier classifier;

  EXPECT_EQ(classifier.Classify(MetroidElementProfile::Prime3,
                                MakePerspective(80.55f, 65.0f, 4096.0f, 1)),
            MetroidElementLayer::HUD);

  MetroidElementClassifier trilogy_classifier;
  EXPECT_EQ(trilogy_classifier.Classify(MetroidElementProfile::TrilogyAuto,
                                        MakePerspective(80.55f, 65.0f, 4096.0f, 1)),
            MetroidElementLayer::HUD);
}
