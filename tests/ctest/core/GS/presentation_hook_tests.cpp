// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSRenderer.h"

#include <gtest/gtest.h>
#include <limits>

namespace
{
	class TestTexture final : public GSTexture
	{
	public:
		TestTexture(int width, int height, bool depth = false)
		{
			m_size = GSVector2i(width, height);
			m_mipmap_levels = 1;
			m_usage = depth ? DepthStencil : RenderTarget;
			m_format = depth ? Format::DepthStencil : Format::Color;
		}

		void* GetNativeHandle() const override { return nullptr; }
		bool Update(const GSVector4i&, const void*, int, int) override { return false; }
		bool Map(GSMap&, const GSVector4i*, int) override { return false; }
		void Unmap() override {}
		void GenerateMipmap() override {}
#ifdef PCSX2_DEVBUILD
		void SetDebugName(std::string_view) override {}
#endif
	};

	struct HookState
	{
		GSTexture* output = nullptr;
		GSVector4i output_rect = GSVector4i::zero();
		bool succeeds = false;
		int calls = 0;
		u64 presentation_id = 0;
		bool is_redraw = false;
	};

	bool Upscale(void* userdata, const GSPresentationUpscaleFrame& frame, GSPresentationUpscaleOutput& output)
	{
		HookState& state = *static_cast<HookState*>(userdata);
		state.calls++;
		state.presentation_id = frame.presentation_id;
		state.is_redraw = frame.is_redraw;
		output.texture = state.output;
		output.source_rect = state.output_rect;
		return state.succeeds;
	}

	GSPresentationUpscaleFrame MakeFrame(GSTexture* texture)
	{
		return {nullptr, texture, GSVector4i(4, 8, 104, 108),
			GSVector4(4.0f / 128.0f, 8.0f / 128.0f, 104.0f / 128.0f, 108.0f / 128.0f),
			GSVector4(10.0f, 20.0f, 210.0f, 220.0f), 42, true};
	}

	void ExpectOriginalFrame(const GSPresentationUpscaleFrame& frame, const GSTexture* source)
	{
		EXPECT_EQ(frame.texture, source);
		EXPECT_TRUE(frame.source_rect.eq(GSVector4i(4, 8, 104, 108)));
		EXPECT_FLOAT_EQ(frame.source_uv.x, 4.0f / 128.0f);
		EXPECT_FLOAT_EQ(frame.source_uv.y, 8.0f / 128.0f);
		EXPECT_FLOAT_EQ(frame.source_uv.z, 104.0f / 128.0f);
		EXPECT_FLOAT_EQ(frame.source_uv.w, 108.0f / 128.0f);
	}
}

TEST(GSPresentationHook, NoHookAndFailurePreserveSource)
{
	TestTexture source(128, 128);
	TestTexture output(200, 160);
	GSPresentationUpscaleFrame frame = MakeFrame(&source);
	EXPECT_EQ(GSApplyPresentationUpscaleHook({}, frame), GSPresentationUpscaleStatus::NotCalled);
	ExpectOriginalFrame(frame, &source);

	HookState state{&output, GSVector4i(0, 0, 200, 160), false};
	const GSPresentationUpscaleHooks hooks{&state, Upscale, nullptr};
	EXPECT_EQ(GSApplyPresentationUpscaleHook(hooks, frame), GSPresentationUpscaleStatus::Failed);
	EXPECT_EQ(state.calls, 1);
	EXPECT_EQ(state.presentation_id, 42u);
	EXPECT_TRUE(state.is_redraw);
	ExpectOriginalFrame(frame, &source);
}

TEST(GSPresentationHook, ValidOutputReplacesOnlySourceAndUV)
{
	TestTexture source(128, 128);
	TestTexture output(200, 160);
	GSPresentationUpscaleFrame frame = MakeFrame(&source);
	HookState state{&output, GSVector4i(10, 20, 190, 140), true};
	const GSPresentationUpscaleHooks hooks{&state, Upscale, nullptr};

	EXPECT_EQ(GSApplyPresentationUpscaleHook(hooks, frame), GSPresentationUpscaleStatus::Applied);
	EXPECT_EQ(frame.texture, &output);
	EXPECT_TRUE(frame.source_rect.eq(state.output_rect));
	EXPECT_FLOAT_EQ(frame.source_uv.x, 0.05f);
	EXPECT_FLOAT_EQ(frame.source_uv.y, 0.125f);
	EXPECT_FLOAT_EQ(frame.source_uv.z, 0.95f);
	EXPECT_FLOAT_EQ(frame.source_uv.w, 0.875f);
	EXPECT_FLOAT_EQ(frame.destination_rect.x, 10.0f);
	EXPECT_FLOAT_EQ(frame.destination_rect.w, 220.0f);
	EXPECT_EQ(frame.presentation_id, 42u);
	EXPECT_TRUE(frame.is_redraw);
}

TEST(GSPresentationHook, InvalidOutputsFallBack)
{
	TestTexture source(128, 128);
	TestTexture output(200, 160);
	TestTexture depth(200, 160, true);
	HookState state{&output, GSVector4i(0, 0, 200, 160), true};
	const GSPresentationUpscaleHooks hooks{&state, Upscale, nullptr};

	const auto expect_failure = [&]() {
		GSPresentationUpscaleFrame frame = MakeFrame(&source);
		EXPECT_EQ(GSApplyPresentationUpscaleHook(hooks, frame), GSPresentationUpscaleStatus::Failed);
		ExpectOriginalFrame(frame, &source);
	};
	state.output = nullptr;
	expect_failure();
	state.output = &source;
	expect_failure();
	state.output = &depth;
	expect_failure();
	state.output = &output;
	for (const GSVector4i rect : {GSVector4i(-1, 0, 10, 10), GSVector4i(0, 0, 201, 160),
			 GSVector4i(0, 0, 200, 161), GSVector4i(10, 0, 10, 1)})
	{
		state.output_rect = rect;
		expect_failure();
	}
}

TEST(GSPresentationHook, InvalidInputDoesNotCallHook)
{
	TestTexture source(128, 128);
	HookState state;
	const GSPresentationUpscaleHooks hooks{&state, Upscale, nullptr};
	GSPresentationUpscaleFrame frame = MakeFrame(&source);
	frame.source_rect = GSVector4i(-1, 0, 100, 100);
	EXPECT_EQ(GSApplyPresentationUpscaleHook(hooks, frame), GSPresentationUpscaleStatus::NotCalled);

	frame = MakeFrame(&source);
	frame.destination_rect.z = frame.destination_rect.x;
	EXPECT_EQ(GSApplyPresentationUpscaleHook(hooks, frame), GSPresentationUpscaleStatus::NotCalled);
	frame.destination_rect.z = std::numeric_limits<float>::quiet_NaN();
	EXPECT_EQ(GSApplyPresentationUpscaleHook(hooks, frame), GSPresentationUpscaleStatus::NotCalled);
	EXPECT_EQ(state.calls, 0);
}
