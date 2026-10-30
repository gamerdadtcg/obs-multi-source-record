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
#include <vector>
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
	obs_scene_t *program_scene();   /* strong scene ref; caller releases */
	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	bool ready() const;

	std::vector<std::string> list_scenes();
	bool set_program_scene(const std::string &scene_name);
	bool create_scene(const std::string &scene_name);
	/** Add an existing OBS source into the program vertical scene. Returns false if missing. */
	bool add_existing_source(const std::string &source_name);
	/** Fit selected scene item (by source name) to the vertical canvas. */
	bool fit_source(const std::string &source_name);
	bool set_item_visible(const std::string &source_name, bool visible);
	bool reorder_source(const std::string &source_name, bool move_up);

private:
	MsrVerticalCanvas() = default;
	~MsrVerticalCanvas();

	obs_canvas_t *find_existing() const;
	bool ensure_default_scene(obs_canvas_t *canvas);
	obs_sceneitem_t *find_item_by_name(obs_scene_t *scene, const std::string &source_name);

	mutable std::mutex mutex_;
	obs_canvas_t *canvas_ = nullptr; /* strong */
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	static MsrVerticalCanvas *instance_;
};
