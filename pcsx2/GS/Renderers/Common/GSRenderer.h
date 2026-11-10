// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSState.h"
#include <memory>
#include <string>

struct GSPresentationUpscaleFrame
{
	GSDevice* device;
	GSTexture* texture;
	GSVector4i source_rect;     // Pixel coordinates in texture.
	GSVector4 source_uv;        // Normalized source coordinates.
	GSVector4 destination_rect; // Window coordinates, using the device's origin.
	u64 presentation_id;        // Next successful-present ID; reused after failure.
	bool is_redraw;             // Re-presenting the current frame.
};

struct GSPresentationUpscaleOutput
{
	GSTexture* texture = nullptr;
	GSVector4i source_rect = GSVector4i::zero(); // Pixel coordinates in texture.
};

struct GSPresentationUpscaleHooks
{
	void* userdata = nullptr;
	bool (*upscale)(void* userdata, const GSPresentationUpscaleFrame& frame,
		GSPresentationUpscaleOutput& output) = nullptr;
	void (*presented)(void* userdata, u64 presentation_id, bool succeeded, bool used_upscaled_output) = nullptr;
};

enum class GSPresentationUpscaleStatus
{
	NotCalled,
	Failed,
	Applied
};

// Register/clear on the GS thread, or while it is stopped. The callbacks and userdata must
// remain alive until they are cleared and all in-flight callbacks have returned. Callbacks
// must not re-enter presentation or change the registration.
//
// upscale runs after merge/crop and before the present render pass. It must not modify the
// source; on success it supplies a distinct, sample-ready texture and a rectangle in pixels.
// The supplied output rectangle must have valid contents and remain valid through
// presentation and until its GPU use completes.
// presented pairs with every upscale call, including failures and skipped/device-lost
// presents. A failed present does not consume presentation_id; consumers must discard or
// reset speculative temporal history. Device destruction can occur before a pending
// completion callback; it must not use destroyed device resources. Redraws are explicitly
// identified in the frame.
void GSSetPresentationUpscaleHooks(const GSPresentationUpscaleHooks& hooks);

// Process-wide, monotonically increasing identity of the last renderer-driven present
// accepted by the graphics API (scheduled on Metal), not the queued VSync count or a
// guarantee of scanout. Starts at zero; failed/skipped presents do not advance it.
u64 GSGetLastSuccessfulPresentId();

// Applies a hook transactionally: an absent, failed, or invalid output leaves frame intact.
GSPresentationUpscaleStatus GSApplyPresentationUpscaleHook(
	const GSPresentationUpscaleHooks& hooks, GSPresentationUpscaleFrame& frame);

class GSRenderer : public GSState
{
private:
	bool Merge(int field);
	bool BeginPresentFrame(bool frame_skip);
	bool EndPresentFrame();

	u64 m_shader_time_start = 0;

	std::string m_snapshot;
	u32 m_dump_frames = 0;
	u32 m_skipped_duplicate_frames = 0;
	bool m_saving_metrics = false;

	// Tracking draw counters for idle frame detection.
	u64 m_last_draw_n = 0;
	u64 m_last_transfer_n = 0;

protected:
	GSVector2i m_real_size{0, 0};

	virtual GSTexture* GetOutput(int i, float& scale, int& y_offset) = 0;
	virtual GSTexture* GetFeedbackOutput(float& scale) { return nullptr; }

public:
	GSRenderer();
	virtual ~GSRenderer();

	virtual void Reset(bool hardware_reset) override;

	virtual void Destroy();

	virtual void UpdateRenderFixes();

	virtual void VSync(u32 field, bool registers_written, bool idle_frame);
	virtual bool CanUpscale() { return false; }
	virtual float GetUpscaleMultiplier() { return 1.0f; }
	virtual float GetTextureScaleFactor() { return 1.0f; }
	GSVector2i GetInternalResolution();
	float GetModXYOffset();

	virtual GSTexture* LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size);

	bool IsIdleFrame() const;

	bool SaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
		u32* width, u32* height, std::vector<u32>* pixels);

	void QueueSnapshot(const std::string& path, const u32 gsdump_frames);
	void StopGSDump();
	void StartSavingMetrics(u32 seconds);
	void DumpSavedMetrics();
	bool IsSavingMetrics();
	void PresentCurrentFrame();
	bool BeginCapture(std::string filename, const GSVector2i& size = GSVector2i(0, 0));
	void EndCapture();
};

extern std::unique_ptr<GSRenderer> g_gs_renderer;
