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
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

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
	detach_main_signals();
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

std::string MsrVerticalCanvas::followed_scene_name() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return followed_name_;
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
	const char *key;
	obs_sceneitem_t *item;
	bool by_uuid;
};

bool find_item_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<ItemFindCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (ctx->by_uuid) {
		const char *u = obs_source_get_uuid(src);
		if (u && ctx->key && strcmp(u, ctx->key) == 0) {
			ctx->item = item;
			return false;
		}
	} else {
		const char *n = obs_source_get_name(src);
		if (n && ctx->key && strcmp(n, ctx->key) == 0) {
			ctx->item = item;
			return false;
		}
	}
	return true;
}

struct CollectUuidCtx {
	std::unordered_set<std::string> *uuids;
	std::vector<obs_sceneitem_t *> *items;
};

bool collect_main_items_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<CollectUuidCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *u = obs_source_get_uuid(src);
	if (u && *u) {
		ctx->uuids->insert(u);
		ctx->items->push_back(item);
	}
	return true;
}

struct CollectVertCtx {
	std::vector<obs_sceneitem_t *> *items;
};

bool collect_vert_items_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<CollectVertCtx *>(param);
	ctx->items->push_back(item);
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
	followed_name_ = scene_name;
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

obs_sceneitem_t *MsrVerticalCanvas::find_item_by_uuid(obs_scene_t *scene, const char *uuid)
{
	if (!scene || !uuid || !*uuid)
		return nullptr;
	ItemFindCtx ctx{uuid, nullptr, true};
	obs_scene_enum_items(scene, find_item_cb, &ctx);
	return ctx.item;
}

obs_sceneitem_t *MsrVerticalCanvas::find_item_by_name(obs_scene_t *scene, const std::string &source_name)
{
	if (!scene || source_name.empty())
		return nullptr;
	ItemFindCtx ctx{source_name.c_str(), nullptr, false};
	obs_scene_enum_items(scene, find_item_cb, &ctx);
	return ctx.item;
}

void MsrVerticalCanvas::apply_default_vertical_transform(obs_sceneitem_t *item)
{
	if (!item)
		return;
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
	obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
}

bool MsrVerticalCanvas::sync_from_main(obs_scene_t *main_scene, obs_scene_t *vert_scene)
{
	if (!main_scene || !vert_scene)
		return false;

	std::unordered_set<std::string> main_uuids;
	std::vector<obs_sceneitem_t *> main_items;
	CollectUuidCtx cctx{&main_uuids, &main_items};
	obs_scene_enum_items(main_scene, collect_main_items_cb, &cctx);

	/* Add missing sources into the vertical scene (keep existing vertical transforms). */
	for (obs_sceneitem_t *mitem : main_items) {
		obs_source_t *src = obs_sceneitem_get_source(mitem);
		const char *uuid = obs_source_get_uuid(src);
		if (!uuid || !*uuid)
			continue;
		if (find_item_by_uuid(vert_scene, uuid))
			continue;
		obs_sceneitem_t *added = obs_scene_add(vert_scene, src);
		if (added) {
			apply_default_vertical_transform(added);
			obs_sceneitem_set_visible(added, obs_sceneitem_visible(mitem));
			const char *n = obs_source_get_name(src);
			obs_log(LOG_INFO, "vertical sync: added '%s'", n ? n : "?");
		}
	}

	/* Remove vertical-only leftovers that are no longer in the main scene. */
	std::vector<obs_sceneitem_t *> vert_items;
	CollectVertCtx vctx{&vert_items};
	obs_scene_enum_items(vert_scene, collect_vert_items_cb, &vctx);
	for (obs_sceneitem_t *vitem : vert_items) {
		obs_source_t *src = obs_sceneitem_get_source(vitem);
		const char *uuid = obs_source_get_uuid(src);
		if (!uuid || main_uuids.find(uuid) == main_uuids.end()) {
			const char *n = obs_source_get_name(src);
			obs_log(LOG_INFO, "vertical sync: removed '%s'", n ? n : "?");
			obs_sceneitem_remove(vitem);
		}
	}
	return true;
}

void MsrVerticalCanvas::detach_main_signals()
{
	if (!main_signal_source_)
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(main_signal_source_);
	if (sh) {
		signal_handler_disconnect(sh, "item_add", on_main_item_add, this);
		signal_handler_disconnect(sh, "item_remove", on_main_item_remove, this);
	}
	main_signal_source_ = nullptr;
}

void MsrVerticalCanvas::attach_main_signals(obs_source_t *main_scene_source)
{
	detach_main_signals();
	if (!main_scene_source)
		return;
	main_signal_source_ = main_scene_source;
	signal_handler_t *sh = obs_source_get_signal_handler(main_scene_source);
	if (!sh)
		return;
	signal_handler_connect(sh, "item_add", on_main_item_add, this);
	signal_handler_connect(sh, "item_remove", on_main_item_remove, this);
}

void MsrVerticalCanvas::on_main_item_add(void *data, calldata_t *)
{
	auto *self = static_cast<MsrVerticalCanvas *>(data);
	if (self)
		self->follow_main_scene();
}

void MsrVerticalCanvas::on_main_item_remove(void *data, calldata_t *)
{
	auto *self = static_cast<MsrVerticalCanvas *>(data);
	if (self)
		self->follow_main_scene();
}

bool MsrVerticalCanvas::follow_main_scene()
{
	obs_source_t *main_src = obs_frontend_get_current_scene();
	if (!main_src)
		return false;

	const char *name = obs_source_get_name(main_src);
	if (!name || !*name) {
		obs_source_release(main_src);
		return false;
	}

	obs_scene_t *main_scene = obs_scene_from_source(main_src);
	if (!main_scene) {
		obs_source_release(main_src);
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!canvas_) {
			obs_source_release(main_src);
			return false;
		}

		obs_scene_t *vert = obs_canvas_get_scene_by_name(canvas_, name);
		if (!vert)
			vert = obs_canvas_scene_create(canvas_, name);
		if (!vert) {
			obs_log(LOG_ERROR, "vertical follow: failed to create scene '%s'", name);
			obs_source_release(main_src);
			return false;
		}

		sync_from_main(main_scene, vert);
		obs_canvas_set_channel(canvas_, 0, obs_scene_get_source(vert));
		followed_name_ = name;
		obs_scene_release(vert);
	}

	attach_main_signals(main_src);
	obs_log(LOG_INFO, "vertical canvas following main scene '%s'", name);
	obs_source_release(main_src);
	return true;
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

	apply_default_vertical_transform(item);
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
	apply_default_vertical_transform(item);
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
