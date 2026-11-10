// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSRenderer.h"

#include <gtest/gtest.h>

TEST(GSTemporalObservation, GuestDepthTestSurvivesHardwareOptimization)
{
	GIFRegTEST guest{};
	guest.ZTE = 1;
	guest.ZTST = ZTST_GEQUAL;
	GIFRegTEST optimized = guest;
	optimized.ZTST = ZTST_ALWAYS;

	GSTemporalDrawObservation observation;
	observation.SetDepthTests(guest, optimized.ZTST);
	EXPECT_TRUE(observation.depth_test_enabled);
	EXPECT_EQ(observation.gs_depth_test, ZTST_GEQUAL);
	EXPECT_EQ(observation.hardware_depth_test, ZTST_ALWAYS);

	guest.ZTE = 0;
	guest.ZTST = ZTST_GREATER;
	observation.SetDepthTests(guest, ZTST_ALWAYS);
	EXPECT_FALSE(observation.depth_test_enabled);
	EXPECT_EQ(observation.gs_depth_test, ZTST_GREATER);
	EXPECT_EQ(observation.hardware_depth_test, ZTST_ALWAYS);
}
