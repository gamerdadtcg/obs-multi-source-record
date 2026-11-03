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
#include <atomic>

#include "msr-vertical-audio.hpp"

/**
 * Owns the MSR Vertical obs_canvas (OBS 31+).
 * Follows the main program scene (Aitum-style linked scene): same source refs,
 * independent vertical-only transforms / visibility per sceneitem.
 */
class MsrVerticalCanvas {
public:
	static constexpr const char *kCanvasName = "MSR Vertical";
	static constexpr const char *kDefaultSceneName = "Vertical";

	static MsrVerticalCanvas *instance();
	static void create();
	static void destroy();

	bool ensure(uint32_t width, uint32_t height);

	obs_canvas_t *canvas();
	video_t *video();
	obs_source_t *program_source(); /* strong ref; caller releases */
	obs_scene_t *program_scene();   /* strong scene ref; caller releases */
	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	bool ready() const;

	bool follow_main_scene();
	std::string followed_scene_name() const;
	void set_transition_ms(int ms);
	int transition_ms() const;

	std::vector<std::string> list_scenes();
	bool set_program_scene(const std::string &scene_name);
	bool create_scene(const std::string &scene_name);
	bool add_existing_source(const std::string &source_name);
	bool fit_source(const std::string &source_name);
	/** Place item in vertical canvas with bounds box (OBS_BOUNDS_*). */
	bool set_item_layout(const std::string &source_name, float x, float y, float bounds_w,
			 float bounds_h, obs_bounds_type bounds = OBS_BOUNDS_SCALE_INNER);
	bool stretch_source(const std::string &source_name);
	bool center_source(const std::string &source_name);
	bool reset_source_transform(const std::string &source_name);
	bool set_item_visible(const std::string &source_name, bool visible);
	bool item_visible(const std::string &source_name);
	bool reorder_source(const std::string &source_name, bool move_up);

	bool select_source(const std::string &source_name, bool exclusive = true);
	bool select_source_uuid(const std::string &uuid, bool exclusive = true);
	void clear_selection();
	std::string selected_source_name();
	bool nudge_selected(float dx, float dy);

	/** Vertical-only audio: mute excludes source from vertical record/stream mix. Main unchanged. */
	bool set_item_audio_enabled(const std::string &source_name, bool enabled);
	bool item_audio_enabled(const std::string &source_name);

	/** Hide from main/horizontal canvas only; vertical visibility stays independent (V toggle). */
	static constexpr const char *kHideHorizontalKey = "msr_hide_horizontal";
	bool set_item_hide_horizontal(const std::string &source_name, bool hide);
	bool item_hide_horizontal(const std::string &source_name);
	void apply_horizontal_hides();

	audio_t *audio();
	void rebuild_audio_mix();

private:
	MsrVerticalCanvas() = default;
	~MsrVerticalCanvas();

	obs_canvas_t *find_existing() const;
	bool ensure_default_scene(obs_canvas_t *canvas);
	obs_sceneitem_t *find_item_by_uuid(obs_scene_t *scene, const char *uuid);
	obs_sceneitem_t *find_item_by_name(obs_scene_t *scene, const std::string &source_name);
	void apply_default_vertical_transform(obs_sceneitem_t *item);
	bool sync_from_main(obs_scene_t *main_scene, obs_scene_t *vert_scene);
	void attach_main_signals(obs_source_t *main_scene_source);
	void detach_main_signals();
	bool ensure_transition_locked();
	void show_scene_locked(obs_source_t *scene_source, bool animate);

	static void on_main_item_add(void *data, calldata_t *cd);
	static void on_main_item_remove(void *data, calldata_t *cd);
	static void on_main_item_visible(void *data, calldata_t *cd);

	static bool sceneitem_hide_horizontal(obs_sceneitem_t *item);
	static void sceneitem_set_hide_horizontal(obs_sceneitem_t *item, bool hide);

	mutable std::mutex mutex_;
	obs_canvas_t *canvas_ = nullptr;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	std::string followed_name_;
	obs_source_t *main_signal_source_ = nullptr;
	obs_source_t *transition_ = nullptr; /* private fade_transition on canvas ch0 */
	int transition_ms_ = 300;
	MsrVerticalAudioMix audio_mix_;
	std::atomic_bool applying_hide_horizontal_{false};
	static MsrVerticalCanvas *instance_;
};
