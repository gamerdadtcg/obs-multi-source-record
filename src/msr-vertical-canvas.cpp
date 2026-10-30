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
#include <algorithm>
#include <cmath>

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

	/* Require true portrait base AND output (1080x1920 typical). Main stays 1920x1080. */
	struct obs_video_info cur = {};
	const bool dims_ok = obs_canvas_get_video_info(canvas_, &cur) && cur.base_width == width &&
			     cur.base_height == height && cur.output_width == width && cur.output_height == height &&
			     obs_canvas_has_video(canvas_);
	if (dims_ok) {
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

	obs_log(LOG_INFO, "resetting vertical canvas '%s' to %ux%u (was base %ux%u out %ux%u)", kCanvasName, width,
		height, cur.base_width, cur.base_height, cur.output_width, cur.output_height);

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

obs_scene_t *MsrVerticalCanvas::program_scene()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!canvas_)
		return nullptr;
	obs_source_t *src = obs_canvas_get_channel(canvas_, 0);
	if (!src)
		return nullptr;
	obs_scene_t *scene = obs_scene_from_source(src);
	if (!scene) {
		obs_source_release(src);
		return nullptr;
	}
	obs_scene_t *strong = obs_scene_get_ref(scene);
	obs_source_release(src);
	return strong;
}

bool MsrVerticalCanvas::ready() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return canvas_ && obs_canvas_has_video(canvas_);
}

namespace {

struct SceneListCtx {
	std::vector<std::string> *out;
};

bool enum_scenes_cb(void *data, obs_source_t *source)
{
	auto *ctx = static_cast<SceneListCtx *>(data);
	const char *name = obs_source_get_name(source);
	if (name && *name)
		ctx->out->emplace_back(name);
	return true;
}

struct ItemFindCtx {
	const char *name;
	obs_sceneitem_t *item;
};

bool find_item_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<ItemFindCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *n = obs_source_get_name(src);
	if (n && ctx->name && strcmp(n, ctx->name) == 0) {
		ctx->item = item;
		return false;
	}
	return true;
}

} // namespace

std::vector<std::string> MsrVerticalCanvas::list_scenes()
{
	std::vector<std::string> out;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!canvas_)
		return out;
	SceneListCtx ctx{&out};
	obs_canvas_enum_scenes(canvas_, enum_scenes_cb, &ctx);
	return out;
}

bool MsrVerticalCanvas::set_program_scene(const std::string &scene_name)
{
	if (scene_name.empty())
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!canvas_)
		return false;
	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas_, scene_name.c_str());
	if (!scene)
		return false;
	obs_canvas_set_channel(canvas_, 0, obs_scene_get_source(scene));
	obs_scene_release(scene);
	obs_log(LOG_INFO, "vertical program scene set to '%s'", scene_name.c_str());
	return true;
}

bool MsrVerticalCanvas::create_scene(const std::string &scene_name)
{
	if (scene_name.empty())
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!canvas_)
		return false;
	obs_scene_t *existing = obs_canvas_get_scene_by_name(canvas_, scene_name.c_str());
	if (existing) {
		obs_scene_release(existing);
		return true;
	}
	obs_scene_t *scene = obs_canvas_scene_create(canvas_, scene_name.c_str());
	if (!scene)
		return false;
	obs_scene_release(scene);
	return true;
}

obs_sceneitem_t *MsrVerticalCanvas::find_item_by_name(obs_scene_t *scene, const std::string &source_name)
{
	if (!scene || source_name.empty())
		return nullptr;
	ItemFindCtx ctx{source_name.c_str(), nullptr};
	obs_scene_enum_items(scene, find_item_cb, &ctx);
	return ctx.item;
}

bool MsrVerticalCanvas::add_existing_source(const std::string &source_name)
{
	if (source_name.empty())
		return false;
	obs_source_t *src = obs_get_source_by_name(source_name.c_str());
	if (!src)
		return false;

	obs_scene_t *scene = program_scene();
	if (!scene) {
		obs_source_release(src);
		return false;
	}

	/* Avoid duplicate top-level items for the same source. */
	if (find_item_by_name(scene, source_name)) {
		obs_scene_release(scene);
		obs_source_release(src);
		return true;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, src);
	obs_source_release(src);
	if (!item) {
		obs_scene_release(scene);
		return false;
	}

	/* Fit to vertical canvas by default. */
	const uint32_t cw = width_ ? width_ : 1080;
	const uint32_t ch = height_ ? height_ : 1920;
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	vec2 bounds;
	vec2_set(&bounds, (float)cw, (float)ch);
	obs_sceneitem_set_bounds(item, &bounds);
	vec2 pos;
	vec2_set(&pos, 0.0f, 0.0f);
	obs_sceneitem_set_pos(item, &pos);

	obs_scene_release(scene);
	obs_log(LOG_INFO, "added source '%s' to vertical program scene", source_name.c_str());
	return true;
}

bool MsrVerticalCanvas::fit_source(const std::string &source_name)
{
	obs_scene_t *scene = program_scene();
	if (!scene)
		return false;
	obs_sceneitem_t *item = find_item_by_name(scene, source_name);
	if (!item) {
		obs_scene_release(scene);
		return false;
	}
	const uint32_t cw = width_ ? width_ : 1080;
	const uint32_t ch = height_ ? height_ : 1920;
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	vec2 bounds;
	vec2_set(&bounds, (float)cw, (float)ch);
	obs_sceneitem_set_bounds(item, &bounds);
	vec2 pos;
	vec2_set(&pos, 0.0f, 0.0f);
	obs_sceneitem_set_pos(item, &pos);
	obs_scene_release(scene);
	return true;
}

bool MsrVerticalCanvas::set_item_visible(const std::string &source_name, bool visible)
{
	obs_scene_t *scene = program_scene();
	if (!scene)
		return false;
	obs_sceneitem_t *item = find_item_by_name(scene, source_name);
	if (!item) {
		obs_scene_release(scene);
		return false;
	}
	obs_sceneitem_set_visible(item, visible);
	obs_scene_release(scene);
	return true;
}

bool MsrVerticalCanvas::reorder_source(const std::string &source_name, bool move_up)
{
	obs_scene_t *scene = program_scene();
	if (!scene)
		return false;
	obs_sceneitem_t *item = find_item_by_name(scene, source_name);
	if (!item) {
		obs_scene_release(scene);
		return false;
	}
	obs_sceneitem_set_order(item, move_up ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN);
	obs_scene_release(scene);
	return true;
}
