// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "kzgs.h"
#include "kzgs_internal.h"

#include "GS/GS.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{
	alignas(16) u8 s_regs[0x2000];
	std::atomic<int> s_validation_errors{0};
	std::atomic<bool> s_validation_seen{false};
	std::atomic<bool> s_validation_unavailable{false};

	void Log(int, const char* message)
	{
		if (std::strstr(message, "Inserted device layer \"VK_LAYER_KHRONOS_validation\""))
			s_validation_seen = true;
		if (std::strstr(message, "validation/debug layers requested but are unavailable"))
			s_validation_unavailable = true;
		if (std::strstr(message, "SYNC-HAZARD") || std::strstr(message, "Validation Error") ||
			std::strstr(message, "Present source was not prepared") ||
			(std::strstr(message, "VUID-") && !std::strstr(message, "VUID-VkDeviceCreateInfo-enabledLayerCount-12384")))
		{
			std::fprintf(stderr, "%s\n", message);
			++s_validation_errors;
		}
	}

	void SetPriv(u32 offset, u64 value)
	{
		std::memcpy(s_regs + offset, &value, sizeof(value));
	}

	void SetupDisplay()
	{
		std::memset(s_regs, 0, sizeof(s_regs));
		SetPriv(0x0000, 1ull | (1ull << 2) | (1ull << 5) | (0xffull << 8));
		SetPriv(0x0010, 4ull | (32ull << 3) | (2ull << 13));
		SetPriv(0x0020, 1ull);
		SetPriv(0x0070, 10ull << 9);
		SetPriv(0x0080, 636ull | (50ull << 12) | (3ull << 23) | (2559ull << 32) | (447ull << 44));
	}

	u64 XYZ(int x, int y)
	{
		return static_cast<u64>(static_cast<u16>(x << 4)) |
			(static_cast<u64>(static_cast<u16>(y << 4)) << 16) | (0xffffffffull << 32);
	}

	std::vector<u8> Scene()
	{
		std::vector<u64> words;
		const auto write = [&](u64 reg, u64 value) {
			words.push_back(value);
			words.push_back(reg);
		};
		write(0x4c, 10ull << 16);                  // FRAME_1
		write(0x4e, 140ull | (1ull << 32));       // ZBUF_1: no depth writes
		write(0x18, 0);                           // XYOFFSET_1
		write(0x40, (639ull << 16) | (447ull << 48));
		write(0x47, (1ull << 16) | (2ull << 17)); // Guest GEQUAL, always passing at maximum Z
		write(0x1a, 1);
		write(0x46, 1);
		write(0x00, 6);
		write(0x01, 0x3f8000008000ff40ull);
		write(0x05, XYZ(0, 0));
		write(0x05, XYZ(640, 448));
		write(0x00, 6);
		write(0x01, 0x3f80000080ff0020ull);
		write(0x05, XYZ(64, 64));
		write(0x05, XYZ(256, 192));

		const u64 tag_lo = (words.size() / 2) | (1ull << 15) | (1ull << 60);
		const u64 tag_hi = 0xe;
		std::vector<u8> packet(16 + words.size() * sizeof(u64));
		std::memcpy(packet.data(), &tag_lo, sizeof(tag_lo));
		std::memcpy(packet.data() + 8, &tag_hi, sizeof(tag_hi));
		std::memcpy(packet.data() + 16, words.data(), words.size() * sizeof(u64));
		return packet;
	}

	struct HookState
	{
		bool succeeds = true;
		bool invalid_layout = false;
		bool invalid_clear = false;
		bool valid_clear = false;
		int calls = 0;
		int completions = 0;
		int completed_frames = 0;
		bool succeeded = false;
		bool used = false;
		bool redraw = false;
		bool saw_non_sample_layout = false;
		bool presented_sample_layout = false;
		bool presented_after_processing = false;
		GSVector2i input_size{0, 0};
		GSVector4i input_rect = GSVector4i::zero();
		GSVector4 destination = GSVector4::zero();
		std::unique_ptr<GSTexture> output;
	};

	bool Upscale(void* userdata, const GSPresentationUpscaleFrame& frame, GSPresentationUpscaleOutput& output)
	{
		auto& state = *static_cast<HookState*>(userdata);
		++state.calls;
		state.redraw = frame.is_redraw;
		state.input_size = frame.texture->GetSize();
		state.input_rect = frame.source_rect;
		state.destination = frame.destination_rect;
		if (!state.succeeds)
			return false;

		const int width = static_cast<int>(std::ceil(frame.destination_rect.z - frame.destination_rect.x));
		const int height = static_cast<int>(std::ceil(frame.destination_rect.w - frame.destination_rect.y));
		if (width <= frame.source_rect.width() || height <= frame.source_rect.height())
			return false;

		if (state.invalid_layout || state.invalid_clear)
		{
			state.output.reset(state.invalid_clear ?
				frame.device->FetchSurface(GSTexture::ShaderWriteTexture, width, height, 1, GSTexture::Format::Color, false, false) :
				frame.device->CreateShaderWriteTarget(width, height, GSTexture::Format::Color, false));
			if (!state.output)
				return false;
			if (state.invalid_clear)
			{
				static_cast<GSDeviceVK*>(frame.device)->EndRenderPass();
				static_cast<GSTextureVK*>(state.output.get())->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			}
			state.output->SetState(state.invalid_clear ? GSTexture::State::Cleared : GSTexture::State::Dirty);
		}
		else
		{
			state.output.reset(frame.device->CreateRenderTarget(width, height, GSTexture::Format::Color, false));
			if (!state.output)
				return false;
			frame.device->StretchRect(frame.texture, frame.source_uv, state.output.get(),
				GSVector4(0, 0, width, height), ShaderConvert::COPY, Biln);
			if (state.valid_clear)
				frame.device->ClearRenderTarget(state.output.get(), 0xff5090e0u);
		}
		if (frame.device->GetRenderAPI() == RenderAPI::Vulkan)
		{
			state.saw_non_sample_layout =
				static_cast<GSTextureVK*>(state.output.get())->GetLayout() != GSTextureVK::Layout::ShaderReadOnly;
		}
		output.texture = state.output.get();
		output.source_rect = GSVector4i(0, 0, width, height);
		return true;
	}

	void Presented(void* userdata, u64, bool succeeded, bool used)
	{
		auto& state = *static_cast<HookState*>(userdata);
		++state.completions;
		state.succeeded = succeeded;
		state.used = used;
	}

	void PresentedFrame(void* userdata, const GSPresentationUpscaleFrame& frame)
	{
		auto& state = *static_cast<HookState*>(userdata);
		++state.completed_frames;
		state.presented_after_processing = (frame.texture != state.output.get());
		if (frame.device->GetRenderAPI() == RenderAPI::Vulkan)
		{
			state.presented_sample_layout =
				static_cast<GSTextureVK*>(frame.texture)->GetLayout() == GSTextureVK::Layout::ShaderReadOnly;
		}
	}

	struct ObservationState
	{
		int presents = 0;
		int optimized_guest_depth = 0;
		GSVector2i size{0, 0};
		GSVector4i source = GSVector4i::zero();
		GSVector4 destination = GSVector4::zero();
	};

	void ObservePresent(void* userdata, const GSTemporalPresentObservation& observation)
	{
		auto& state = *static_cast<ObservationState*>(userdata);
		++state.presents;
		state.size = observation.texture->GetSize();
		state.source = observation.source_rect;
		state.destination = observation.destination_rect;
	}

	void ObserveDraw(void* userdata, const GSTemporalDrawObservation& observation)
	{
		auto& state = *static_cast<ObservationState*>(userdata);
		if (observation.depth_test_enabled && observation.gs_depth_test == ZTST_GEQUAL &&
			observation.hardware_depth_test == ZTST_ALWAYS)
		{
			++state.optimized_guest_depth;
		}
	}

	void SetHooks(HookState* hook, ObservationState* observation)
	{
		GSPresentationUpscaleHooks upscale;
		if (hook)
			upscale = {hook, Upscale, Presented, PresentedFrame};
		GSSetPresentationUpscaleHooks(upscale);
		GSTemporalObservationHooks temporal;
		if (observation)
		{
			temporal.userdata = observation;
			temporal.draw = ObserveDraw;
			temporal.present = ObservePresent;
		}
		GSSetTemporalObservationHooks(temporal);
	}

	class PresentationIntegration : public ::testing::Test
	{
	protected:
		HWND window = nullptr;
		bool opened = false;
		HookState hook;
		ObservationState observation;

		void SetUp() override
		{
			s_validation_errors = 0;
			s_validation_seen = false;
			s_validation_unavailable = false;
			SetupDisplay();
			window = CreateWindowExW(0, L"STATIC", L"PCSX2 presentation review", WS_POPUP,
				0, 0, 1003, 757, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
			ASSERT_NE(window, nullptr);
			kzgsSetLogCallback(Log);
		}

		void Open(KzgsRenderer renderer)
		{
			KzgsConfig config;
			config.renderer = renderer;
			config.useWarp = (renderer != KzgsRenderer::Vulkan);
			config.vsync = false;
			config.debugDevice = (renderer == KzgsRenderer::Vulkan);
			config.bilinearPresent = true;
			config.sharpPresent = true;
			config.crop[0] = 7;
			config.crop[1] = 5;
			config.crop[2] = 11;
			config.crop[3] = 3;
			std::string error;
			opened = kzgsOpen(kz::host::NativeWindow::win32(window), config, s_regs, &error);
			ASSERT_TRUE(opened) << error;
			kzgs::RunOnGSThread([&]() { SetHooks(nullptr, &observation); });
		}

		void Frame()
		{
			const std::vector<u8> packet = Scene();
			kzgsGifTransfer(3, packet.data(), static_cast<u32>(packet.size() / 16));
			kzgsVsync(0);
			kzgsSync();
		}

		std::vector<u8> ReadFrame()
		{
			std::vector<u8> pixels;
			int width = 0, height = 0;
			EXPECT_TRUE(kzgsReadback(pixels, width, height, 1003, 757));
			EXPECT_EQ(width, 1003);
			EXPECT_EQ(height, 757);
			return pixels;
		}

		void TearDown() override
		{
			if (opened)
			{
				kzgs::RunOnGSThread([&]() {
					SetHooks(nullptr, nullptr);
					hook.output.reset();
					GSDeviceVK::SetIntegrationHooks({});
				});
				kzgsClose();
			}
			if (window)
				DestroyWindow(window);
			if (s_validation_seen)
				EXPECT_FALSE(s_validation_unavailable);
			EXPECT_EQ(s_validation_errors.load(), 0);
		}
	};

	void ExpectRectEqual(const GSVector4& a, const GSVector4& b)
	{
		EXPECT_FLOAT_EQ(a.x, b.x);
		EXPECT_FLOAT_EQ(a.y, b.y);
		EXPECT_FLOAT_EQ(a.z, b.z);
		EXPECT_FLOAT_EQ(a.w, b.w);
	}

	PFN_vkQueuePresentKHR s_real_queue_present = nullptr;
	VkResult VKAPI_CALL PresentOutOfDateOnce(VkQueue queue, const VkPresentInfoKHR* info)
	{
		const VkResult result = s_real_queue_present(queue, info);
		return (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ? VK_ERROR_OUT_OF_DATE_KHR : result;
	}

	VkResult VKAPI_CALL AcquireTimeout(VkDevice, VkSwapchainKHR, u64, VkSemaphore, VkFence, u32*)
	{
		return VK_TIMEOUT;
	}

	TEST_F(PresentationIntegration, VulkanNonSampleOutputIsPreparedBeforePresentPass)
	{
		Open(KzgsRenderer::Vulkan);
		ASSERT_TRUE(opened);
		reinterpret_cast<GSPrivRegSet*>(s_regs)->PMODE.EN1 = 0;
		kzgsVsync(0);
		bool uploaded = false;
		kzgs::RunOnGSThread([&]() {
			std::vector<u32> pixels(64 * 48, 0xff5090e0u);
			std::unique_ptr<GSTexture> input(
				g_gs_device->CreateTexture(64, 48, 1, GSTexture::Format::Color));
			uploaded = input && input->Update(GSVector4i(0, 0, 64, 48), pixels.data(), 64 * sizeof(u32));
			if (!uploaded)
				return;

			GSTexture* sources[3] = {nullptr, input.get(), nullptr};
			GSVector4 source_rects[2] = {GSVector4(0, 0, 1, 1), GSVector4(0, 0, 1, 1)};
			GSVector4 destinations[3] = {GSVector4(0, 0, 64, 48), GSVector4(0, 0, 64, 48), GSVector4::zero()};
			GSRegPMODE pmode{};
			pmode.EN2 = 1;
			GSRegEXTBUF extbuf{};
			g_gs_device->Merge(sources, source_rects, destinations, GSVector2i(64, 48), pmode, extbuf, 0xff000000);
			SetHooks(&hook, &observation);
			g_gs_renderer->PresentCurrentFrame();
		});
		ASSERT_TRUE(uploaded);
		EXPECT_EQ(hook.calls, 1);
		EXPECT_TRUE(hook.saw_non_sample_layout);
		EXPECT_TRUE(hook.presented_sample_layout);
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);

		// Vulkan pre-acquires the next swapchain image after a successful present.
		kzgs::RunOnGSThread([&]() {
			const PFN_vkAcquireNextImageKHR real_acquire = vkAcquireNextImageKHR;
			vkAcquireNextImageKHR = AcquireTimeout;
			g_gs_renderer->PresentCurrentFrame();
			vkAcquireNextImageKHR = real_acquire;
		});
		ASSERT_TRUE(hook.succeeded);
		const u64 before_skipped_id = GSGetLastSuccessfulPresentId();
		const int before_skipped_frames = hook.completed_frames;
		const int before_skipped_completions = hook.completions;
		kzgs::RunOnGSThread([]() { g_gs_renderer->PresentCurrentFrame(); });
		EXPECT_FALSE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(hook.completions, before_skipped_completions + 1);
		EXPECT_EQ(hook.completed_frames, before_skipped_frames);
		EXPECT_EQ(GSGetLastSuccessfulPresentId(), before_skipped_id);
		kzgs::RunOnGSThread([]() { g_gs_renderer->PresentCurrentFrame(); });
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(GSGetLastSuccessfulPresentId(), before_skipped_id + 1);
		hook.valid_clear = true;
		kzgs::RunOnGSThread([]() { g_gs_renderer->PresentCurrentFrame(); });
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_TRUE(hook.presented_sample_layout);
		hook.valid_clear = false;

		kzgs::RunOnGSThread([&]() {
			s_real_queue_present = vkQueuePresentKHR;
			vkQueuePresentKHR = PresentOutOfDateOnce;
			g_gs_renderer->PresentCurrentFrame();
			vkQueuePresentKHR = s_real_queue_present;
		});
		ASSERT_FALSE(hook.succeeded);
		ASSERT_TRUE(SetWindowPos(window, nullptr, 0, 0, 1201, 801,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
		kzgs::RunOnGSThread([]() { g_gs_renderer->PresentCurrentFrame(); });
		EXPECT_TRUE(hook.succeeded);
		EXPECT_FALSE(hook.used);
		EXPECT_GE(observation.size.x, 1201);
		EXPECT_GE(observation.size.y, 801);
		EXPECT_TRUE(s_validation_seen.load());
	}

	TEST_F(PresentationIntegration, VulkanSharpHookFailureAndGuestDepth)
	{
		Open(KzgsRenderer::Vulkan);
		ASSERT_TRUE(opened);
		Frame();
		const ObservationState baseline = observation;
		const std::vector<u8> original = ReadFrame();
		ASSERT_FALSE(original.empty());
		EXPECT_GT(baseline.size.x, 640);
		EXPECT_GT(baseline.optimized_guest_depth, 0);

		kzgs::RunOnGSThread([&]() { SetHooks(&hook, &observation); });
		Frame();
		ASSERT_GT(hook.calls, 0);
		EXPECT_EQ(hook.input_size.x, 640);
		EXPECT_EQ(hook.input_size.y, 448);
		EXPECT_EQ(hook.input_rect.x * baseline.size.x, baseline.source.x * hook.input_size.x);
		EXPECT_EQ(hook.input_rect.y * baseline.size.y, baseline.source.y * hook.input_size.y);
		EXPECT_EQ(hook.input_rect.z * baseline.size.x, baseline.source.z * hook.input_size.x);
		EXPECT_EQ(hook.input_rect.w * baseline.size.y, baseline.source.w * hook.input_size.y);
		ExpectRectEqual(hook.destination, baseline.destination);
		EXPECT_TRUE(hook.saw_non_sample_layout);
		EXPECT_TRUE(hook.presented_sample_layout);
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_GT(hook.completed_frames, 0);
		EXPECT_EQ(ReadFrame(), original);

		kzgs::RunOnGSThread([]() {
			GSConfig.CASMode = GSCASMode::SharpenOnly;
			GSConfig.CAS_Sharpness = 50;
		});
		Frame();
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_TRUE(hook.presented_after_processing);
		EXPECT_TRUE(hook.presented_sample_layout);
		kzgs::RunOnGSThread([]() { GSConfig.CASMode = GSCASMode::Disabled; });

		hook.succeeds = false;
		Frame();
		EXPECT_TRUE(hook.succeeded);
		EXPECT_FALSE(hook.used);
		EXPECT_EQ(ReadFrame(), original);
		hook.succeeds = true;
		Frame();
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(ReadFrame(), original);
		kzgs::RunOnGSThread([]() { g_gs_renderer->PresentCurrentFrame(); });
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(hook.input_size.x, 640);

		hook.succeeds = true;
		hook.invalid_layout = true;
		Frame();
		EXPECT_TRUE(hook.succeeded);
		EXPECT_FALSE(hook.used);
		EXPECT_EQ(ReadFrame(), original);
		hook.invalid_layout = false;
		hook.invalid_clear = true;
		Frame();
		EXPECT_TRUE(hook.succeeded);
		EXPECT_FALSE(hook.used);
		EXPECT_EQ(ReadFrame(), original);
		EXPECT_TRUE(s_validation_seen.load());
		EXPECT_FALSE(s_validation_unavailable.load());
	}

	TEST_F(PresentationIntegration, D3D11SharpReadbackAndFallbackMatch)
	{
		Open(KzgsRenderer::D3D11);
		ASSERT_TRUE(opened);
		Frame();
		const std::vector<u8> original = ReadFrame();
		ASSERT_FALSE(original.empty());
		hook.succeeds = false;
		kzgs::RunOnGSThread([&]() { SetHooks(&hook, &observation); });
		Frame();
		EXPECT_GT(hook.calls, 0);
		EXPECT_TRUE(hook.succeeded);
		EXPECT_FALSE(hook.used);
		EXPECT_EQ(ReadFrame(), original);
		hook.succeeds = true;
		Frame();
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(ReadFrame(), original);
		kzgs::RunOnGSThread([]() { g_gs_renderer->PresentCurrentFrame(); });
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(hook.input_size.x, 640);
	}

	TEST_F(PresentationIntegration, D3D12SharpReadbackAndFallbackMatch)
	{
		Open(KzgsRenderer::D3D12);
		ASSERT_TRUE(opened);
		Frame();
		const std::vector<u8> original = ReadFrame();
		ASSERT_FALSE(original.empty());
		hook.succeeds = false;
		kzgs::RunOnGSThread([&]() { SetHooks(&hook, &observation); });
		Frame();
		EXPECT_GT(hook.calls, 0);
		EXPECT_TRUE(hook.succeeded);
		EXPECT_FALSE(hook.used);
		EXPECT_EQ(ReadFrame(), original);
		hook.succeeds = true;
		Frame();
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(ReadFrame(), original);
		kzgs::RunOnGSThread([]() { g_gs_renderer->PresentCurrentFrame(); });
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(hook.input_size.x, 640);
	}

	TEST_F(PresentationIntegration, VulkanSharpHookPreservesAspectAlignmentAndIntegerGeometry)
	{
		ASSERT_TRUE(SetWindowPos(window, nullptr, 0, 0, 1601, 1001,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
		Open(KzgsRenderer::Vulkan);
		ASSERT_TRUE(opened);
		const AspectRatioType previous_aspect = EmuConfig.CurrentAspectRatio;
		const float previous_custom_aspect = EmuConfig.CurrentCustomAspectRatio;
		const bool previous_integer_scaling = GSConfig.IntegerScaling;

		for (const AspectRatioType aspect : {AspectRatioType::R4_3, AspectRatioType::R16_9})
		{
			for (const bool integer_scaling : {false, true})
			{
				kzgs::RunOnGSThread([&]() {
					SetHooks(nullptr, &observation);
					EmuConfig.CurrentAspectRatio = aspect;
					EmuConfig.CurrentCustomAspectRatio = 0.0f;
					GSConfig.IntegerScaling = integer_scaling;
					GSSetDisplayAlignment(GSDisplayAlignment::RightOrBottom);
				});
				Frame();
				const ObservationState baseline = observation;
				kzgs::RunOnGSThread([&]() { SetHooks(&hook, &observation); });
				Frame();
				EXPECT_TRUE(hook.succeeded);
				EXPECT_TRUE(hook.used);
				EXPECT_EQ(hook.input_size.x, 640);
				EXPECT_EQ(hook.input_size.y, 448);
				EXPECT_EQ(hook.input_rect.x * baseline.size.x, baseline.source.x * hook.input_size.x);
				EXPECT_EQ(hook.input_rect.y * baseline.size.y, baseline.source.y * hook.input_size.y);
				EXPECT_EQ(hook.input_rect.z * baseline.size.x, baseline.source.z * hook.input_size.x);
				EXPECT_EQ(hook.input_rect.w * baseline.size.y, baseline.source.w * hook.input_size.y);
				ExpectRectEqual(hook.destination, baseline.destination);
			}
		}

		kzgs::RunOnGSThread([&]() {
			EmuConfig.CurrentAspectRatio = previous_aspect;
			EmuConfig.CurrentCustomAspectRatio = previous_custom_aspect;
			GSConfig.IntegerScaling = previous_integer_scaling;
			GSSetDisplayAlignment(GSDisplayAlignment::Center);
		});
	}

	TEST_F(PresentationIntegration, VulkanScheduledScreenshotMatchesWithoutHook)
	{
		Open(KzgsRenderer::Vulkan);
		ASSERT_TRUE(opened);
		const std::string prefix = "pcsx2-review-screenshot-" + std::to_string(GetCurrentProcessId());
		struct ScreenshotFiles
		{
			std::string baseline, hooked;
			~ScreenshotFiles()
			{
				std::remove(baseline.c_str());
				std::remove(hooked.c_str());
			}
		} files{prefix + "-baseline.png", prefix + "-hooked.png"};
		const auto read_file = [](const std::string& path) {
			std::ifstream stream(path, std::ios::binary);
			return std::vector<u8>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		};

		kzgs::RunOnGSThread([&]() {
			GSConfig.ScreenshotFormat = GSScreenshotFormat::PNG;
			GSConfig.ScreenshotSize = GSScreenshotSize::InternalResolutionUncorrected;
			g_gs_renderer->QueueSnapshot(files.baseline, 0);
		});
		Frame();
		kzgs::RunOnGSThread([]() { GSJoinSnapshotThreads(); });
		const std::vector<u8> baseline_image = read_file(files.baseline);
		ASSERT_FALSE(baseline_image.empty());
		const std::vector<u8> baseline_readback = ReadFrame();
		kzgs::RunOnGSThread([]() { g_gs_renderer->PresentCurrentFrame(); });
		EXPECT_EQ(ReadFrame(), baseline_readback);

		kzgs::RunOnGSThread([&]() {
			SetHooks(&hook, &observation);
			g_gs_renderer->QueueSnapshot(files.hooked, 0);
		});
		Frame();
		kzgs::RunOnGSThread([]() { GSJoinSnapshotThreads(); });
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(read_file(files.hooked), baseline_image);
		EXPECT_EQ(ReadFrame(), baseline_readback);
		kzgs::RunOnGSThread([&]() {
			SetHooks(nullptr, &observation);
			g_gs_renderer->PresentCurrentFrame();
		});
		EXPECT_EQ(ReadFrame(), baseline_readback);
		kzgs::RunOnGSThread([&]() { SetHooks(&hook, &observation); });
		kzgs::RunOnGSThread([]() { g_gs_renderer->PresentCurrentFrame(); });
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(hook.input_size.x, 640);
	}

	TEST_F(PresentationIntegration, VulkanOutOfDateRedrawUsesNewGeometryAndPairsCallbacks)
	{
		Open(KzgsRenderer::Vulkan);
		ASSERT_TRUE(opened);
		kzgs::RunOnGSThread([&]() { SetHooks(&hook, &observation); });
		Frame();
		ASSERT_TRUE(hook.succeeded);
		ASSERT_TRUE(hook.used);
		const int before_frames = hook.completed_frames;
		u64 before_id = 0;
		kzgs::RunOnGSThread([&]() {
			before_id = GSGetLastSuccessfulPresentId();
			s_real_queue_present = vkQueuePresentKHR;
			vkQueuePresentKHR = PresentOutOfDateOnce;
			g_gs_renderer->PresentCurrentFrame();
			vkQueuePresentKHR = s_real_queue_present;
		});
		EXPECT_TRUE(hook.redraw);
		EXPECT_FALSE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		EXPECT_EQ(hook.completed_frames, before_frames);
		EXPECT_EQ(GSGetLastSuccessfulPresentId(), before_id);

		ASSERT_TRUE(SetWindowPos(window, nullptr, 0, 0, 1201, 801,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
		GSVector2i before_size, after_size;
		kzgs::RunOnGSThread([&]() {
			before_size = g_gs_device->GetWindowSize();
			g_gs_renderer->PresentCurrentFrame();
			after_size = g_gs_device->GetWindowSize();
		});
		EXPECT_EQ(before_size.x, 1003);
		EXPECT_EQ(before_size.y, 757);
		ASSERT_EQ(after_size.x, 1201);
		ASSERT_EQ(after_size.y, 801);
		EXPECT_TRUE(hook.succeeded);
		EXPECT_FALSE(hook.used);
		EXPECT_EQ(hook.completed_frames, before_frames);
		EXPECT_EQ(GSGetLastSuccessfulPresentId(), before_id + 1);
		EXPECT_TRUE(observation.size == GSDevice::CalculateResizeTargetSize(hook.input_size, after_size.x, after_size.y));
		const GSVector4 fallback_destination = observation.destination;

		kzgs::RunOnGSThread([&]() {
			SetHooks(nullptr, &observation);
			g_gs_renderer->PresentCurrentFrame();
		});
		ExpectRectEqual(observation.destination, fallback_destination);
		kzgs::RunOnGSThread([&]() {
			SetHooks(&hook, &observation);
			g_gs_renderer->PresentCurrentFrame();
		});
		EXPECT_TRUE(hook.succeeded);
		EXPECT_TRUE(hook.used);
		ExpectRectEqual(hook.destination, fallback_destination);

		const int before_normal_frames = hook.completed_frames;
		kzgs::RunOnGSThread([&]() {
			s_real_queue_present = vkQueuePresentKHR;
			vkQueuePresentKHR = PresentOutOfDateOnce;
			g_gs_renderer->PresentCurrentFrame();
			vkQueuePresentKHR = s_real_queue_present;
		});
		ASSERT_FALSE(hook.succeeded);
		ASSERT_TRUE(SetWindowPos(window, nullptr, 0, 0, 1601, 1001,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
		Frame();
		EXPECT_TRUE(hook.succeeded);
		EXPECT_FALSE(hook.used);
		EXPECT_EQ(hook.completed_frames, before_normal_frames);
		EXPECT_TRUE(observation.size == GSDevice::CalculateResizeTargetSize(hook.input_size, 1601, 1001));
		const GSVector4 normal_fallback_destination = observation.destination;
		kzgs::RunOnGSThread([&]() {
			SetHooks(nullptr, &observation);
			g_gs_renderer->PresentCurrentFrame();
		});
		ExpectRectEqual(observation.destination, normal_fallback_destination);
		EXPECT_TRUE(s_validation_seen.load());
	}

	struct FeatureProbe
	{
		VkPhysicalDevice16BitStorageFeatures first{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
		VkPhysicalDeviceMultiviewFeatures last{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
		bool inject_failure = false;
		bool clean_on_entry = true;
		bool clean_on_created = true;
		bool clean_on_fallback = false;
		int creates = 0;
		int callbacks = 0;
		bool last_features_enabled = false;
	};

	FeatureProbe* s_feature_probe = nullptr;
	PFN_vkCreateDevice s_real_create_device = nullptr;

	VkResult VKAPI_CALL CreateDeviceWithInjectedFailure(VkPhysicalDevice physical,
		const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* device)
	{
		FeatureProbe& probe = *s_feature_probe;
		++probe.creates;
		if (probe.creates == 1)
			return VK_ERROR_FEATURE_NOT_PRESENT;
		probe.clean_on_fallback = (probe.last.pNext == nullptr && info->pNext != &probe.first);
		vkCreateDevice = s_real_create_device;
		return s_real_create_device(physical, info, allocator, device);
	}

	bool DeviceFeatures(void* userdata, VkInstance, VkPhysicalDevice, GSDeviceVK::IntegrationDeviceFeatures& requested)
	{
		auto& probe = *static_cast<FeatureProbe*>(userdata);
		probe.clean_on_entry &= (probe.first.pNext == &probe.last && probe.last.pNext == nullptr);
		requested.extension_chain = reinterpret_cast<VkBaseOutStructure*>(&probe.first);
		if (probe.inject_failure)
		{
			probe.creates = 0;
			s_feature_probe = &probe;
			s_real_create_device = vkCreateDevice;
			vkCreateDevice = CreateDeviceWithInjectedFailure;
		}
		return true;
	}

	void DeviceCreated(void* userdata, GSDeviceVK&, bool, bool, bool features_enabled)
	{
		auto& probe = *static_cast<FeatureProbe*>(userdata);
		probe.clean_on_created &= (probe.first.pNext == &probe.last && probe.last.pNext == nullptr);
		probe.last_features_enabled = features_enabled;
		++probe.callbacks;
	}

	TEST_F(PresentationIntegration, VulkanFeatureChainSurvivesSuccessFallbackAndDeviceRecreation)
	{
		Open(KzgsRenderer::Vulkan);
		ASSERT_TRUE(opened);
		FeatureProbe probe;
		probe.first.pNext = &probe.last;
		bool first = false, fallback = false, recreated = false;
		bool first_features = false, fallback_features = true, recreated_features = false;
		kzgs::RunOnGSThread([&]() {
			GSDeviceVK::IntegrationHooks hooks;
			hooks.userdata = &probe;
			hooks.device_features = DeviceFeatures;
			hooks.device_created = DeviceCreated;
			GSDeviceVK::SetIntegrationHooks(hooks);
			first = GSreopen(true, false, GSGetCurrentRenderer(), std::nullopt);
			first_features = probe.last_features_enabled;
			probe.inject_failure = true;
			fallback = GSreopen(true, false, GSGetCurrentRenderer(), std::nullopt);
			fallback_features = probe.last_features_enabled;
			probe.inject_failure = false;
			recreated = GSreopen(true, false, GSGetCurrentRenderer(), std::nullopt);
			recreated_features = probe.last_features_enabled;
			GSDeviceVK::SetIntegrationHooks({});
		});
		EXPECT_TRUE(first);
		EXPECT_TRUE(fallback);
		EXPECT_TRUE(recreated);
		EXPECT_TRUE(first_features);
		EXPECT_FALSE(fallback_features);
		EXPECT_TRUE(recreated_features);
		EXPECT_EQ(probe.callbacks, 3);
		EXPECT_EQ(probe.creates, 2);
		EXPECT_TRUE(probe.clean_on_entry);
		EXPECT_TRUE(probe.clean_on_created);
		EXPECT_TRUE(probe.clean_on_fallback);
		EXPECT_EQ(probe.first.pNext, &probe.last);
		EXPECT_EQ(probe.last.pNext, nullptr);
		EXPECT_TRUE(s_validation_seen.load());
		kzgs::RunOnGSThread([]() { SetHooks(nullptr, nullptr); });
		kzgsClose();
		opened = false;
	}
}
