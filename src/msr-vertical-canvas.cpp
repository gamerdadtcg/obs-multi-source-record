/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-vertical-canvas.hpp"
#include "plugin-support.h"

#include <obs-frontend-api.h>

MsrVerticalCanvas *MsrVerticalCanvas::instance_ = nullptr;

MsrVerticalCanvas *MsrVerticalCanvas::instance()
{
	return instance_;
}

void MsrVerticalCanvas::create()
{
	if (!instance_)
		instance_ = new MsrVerticalCanvas();
}

void MsrVerticalCanvas::destroy()
{
	delete instance_;
	instance_ = nullptr;
}

MsrVerticalCanvas::~MsrVerticalCanvas()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (canvas_) {
		/* Leave the canvas in OBS so Travis keeps his vertical scenes. */
		obs_canvas_release(canvas_);
		canvas_ = nullptr;
	}
}

obs_canvas_t *MsrVerticalCanvas::find_existing() const
{
	return obs_get_canvas_by_name(kCanvasName);
}

bool MsrVerticalCanvas::ensure_default_scene(obs_canvas_t *canvas)
{
	if (!canvas)
		return false;

	obs_source_t *existing = obs_canvas_get_channel(canvas, 0);
	if (existing) {
		obs_source_release(existing);
		return true;
	}

	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, kDefaultSceneName);
	if (!scene)
		scene = obs_canvas_scene_create(canvas, kDefaultSceneName);
	if (!scene) {
		obs_log(LOG_ERROR, "failed to create default vertical scene");
		return false;
	}

	obs_source_t *ss = obs_scene_get_source(scene);
	obs_canvas_set_channel(canvas, 0, ss);
	obs_scene_release(scene);
	obs_log(LOG_INFO, "vertical canvas default scene '%s' is program", kDefaultSceneName);
	return true;
}

bool MsrVerticalCanvas::ensure(uint32_t width, uint32_t height)
{
	if (width < 2)
		width = 1080;
	if (height < 2)
		height = 1920;
	width += (width & 1);
	height += (height & 1);

	std::lock_guard<std::mutex> lock(mutex_);

	obs_canvas_t *canvas = canvas_;
	if (!canvas) {
		canvas = find_existing();
		if (canvas) {
			canvas_ = canvas; /* take the strong ref from get_by_name */
		} else {
			/* ACTIVATE | SCENE_REF — no MIX_AUDIO so vertical does not enter main mix. */
			const int flags = (1 << 1) | (1 << 3);
			struct obs_video_info ovi = {};
			obs_get_video_info(&ovi);
			ovi.base_width = width;
			ovi.base_height = height;
			ovi.output_width = width;
			ovi.output_height = height;
			canvas = obs_frontend_add_canvas(kCanvasName, &ovi, flags);
			if (!canvas) {
				obs_log(LOG_ERROR, "obs_frontend_add_canvas('%s') failed", kCanvasName);
				return false;
			}
			canvas_ = canvas;
			obs_log(LOG_INFO, "created vertical canvas '%s' %ux%u", kCanvasName, width, height);
		}
	}

	struct obs_video_info cur = {};
	if (obs_canvas_get_video_info(canvas_, &cur) && cur.base_width == width && cur.base_height == height &&
	    obs_canvas_has_video(canvas_)) {
		if (!ensure_default_scene(canvas_))
			return false;
		width_ = width;
		height_ = height;
		return true;
	}

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	ovi.base_width = width;
	ovi.base_height = height;
	ovi.output_width = width;
	ovi.output_height = height;

	if (!obs_canvas_reset_video(canvas_, &ovi)) {
		obs_log(LOG_ERROR, "obs_canvas_reset_video failed for '%s'", kCanvasName);
		return false;
	}

	if (!ensure_default_scene(canvas_))
		return false;

	width_ = width;
	height_ = height;
	return obs_canvas_has_video(canvas_) && obs_canvas_get_video(canvas_) != nullptr;
}

obs_canvas_t *MsrVerticalCanvas::canvas()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return canvas_ ? obs_canvas_get_ref(canvas_) : nullptr;
}

video_t *MsrVerticalCanvas::video()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return canvas_ ? obs_canvas_get_video(canvas_) : nullptr;
}

obs_source_t *MsrVerticalCanvas::program_source()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!canvas_)
		return nullptr;
	return obs_canvas_get_channel(canvas_, 0);
}

bool MsrVerticalCanvas::ready() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return canvas_ && obs_canvas_has_video(canvas_);
}
