// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef ENABLE_VULKAN
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include <gtest/gtest.h>

TEST(GSVulkanFeatureChain, RestoresCallerChainForFailureFallbackAndRecreation)
{
	VkBaseOutStructure first{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, nullptr};
	VkBaseOutStructure last{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, nullptr};
	first.pNext = &last;

	const auto create_device = [&](bool fail) {
		VkBaseOutStructure base{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, nullptr};
		Vulkan::ScopedPNextLink link;
		link.Append(&last, &base);
		EXPECT_EQ(first.pNext, &last);
		EXPECT_EQ(last.pNext, &base);
		const VkResult result = fail ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
		link.Reset();
		EXPECT_EQ(first.pNext, &last);
		EXPECT_EQ(last.pNext, nullptr);
		if (result != VK_SUCCESS)
		{
			EXPECT_EQ(base.pNext, nullptr); // Fallback uses only the base chain.
			return VK_SUCCESS;
		}
		return result;
	};

	EXPECT_EQ(create_device(true), VK_SUCCESS);
	EXPECT_EQ(create_device(false), VK_SUCCESS);
	EXPECT_EQ(create_device(false), VK_SUCCESS);
	EXPECT_EQ(first.pNext, &last);
	EXPECT_EQ(last.pNext, nullptr);

	{
		VkBaseOutStructure base{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, nullptr};
		Vulkan::ScopedPNextLink link;
		link.Append(&last, &base);
	}
	EXPECT_EQ(last.pNext, nullptr);
}
#endif
