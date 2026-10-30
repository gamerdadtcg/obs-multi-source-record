/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>
#include <string>
#include <mutex>

/** Owns/ensures the MSR Vertical obs_canvas (OBS 31+ multi-canvas). */
class MsrVerticalCanvas {
public:
	static constexpr const char *kCanvasName = "MSR Vertical";
	static constexpr const char *kDefaultSceneName = "Vertical";

	static MsrVerticalCanvas *instance();
	static void create();
	static void destroy();

	/** Create or refresh the vertical canvas at the given resolution. Safe to call repeatedly. */
	bool ensure(uint32_t width, uint32_t height);

	obs_canvas_t *canvas();
	video_t *video();
	obs_source_t *program_source(); /* strong ref; caller releases */
	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	bool ready() const;

private:
	MsrVerticalCanvas() = default;
	~MsrVerticalCanvas();

	obs_canvas_t *find_existing() const;
	bool ensure_default_scene(obs_canvas_t *canvas);

	mutable std::mutex mutex_;
	obs_canvas_t *canvas_ = nullptr; /* strong */
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	static MsrVerticalCanvas *instance_;
};
