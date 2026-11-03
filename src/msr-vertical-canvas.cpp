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
#include <obs-module.h>
#include <util/platform.h>
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
	detach_frontend_transition();
	detach_main_signals();
	audio_mix_.shutdown();
	std::lock_guard<std::mutex> lock(mutex_);
	if (layouts_) {
		obs_data_release(layouts_);
		layouts_ = nullptr;
	}
	if (transition_) {
		if (canvas_)
			obs_canvas_set_channel(canvas_, 0, nullptr);
		obs_source_release(transition_);
		transition_ = nullptr;
	}
	if (canvas_) {
		/* Leave the canvas in OBS so Travis keeps his vertical scenes. */
		obs_canvas_release(canvas_);
		canvas_ = nullptr;
	}
}

obs_canvas_t *MsrVerticalCanvas::find_existing() const
{
	obs_canvas_t *c = obs_get_canvas_by_name(kCanvasName);
	if (c)
		return c;
	/* Prior sessions sometimes renamed a duplicate to "MSR Vertical 2". */
	return obs_get_canvas_by_name("MSR Vertical 2");
}

bool MsrVerticalCanvas::ensure_default_scene(obs_canvas_t *canvas)
{
	if (!canvas)
		return false;

	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, kDefaultSceneName);
	if (!scene)
		scene = obs_canvas_scene_create(canvas, kDefaultSceneName);
	if (!scene) {
		obs_log(LOG_ERROR, "failed to create default vertical scene");
		return false;
	}

	if (!ensure_transition_locked()) {
		obs_scene_release(scene);
		return false;
	}
	/* Never hard-cut an active follow to the empty default — that killed fades
	 * when ensure() ran on every SCENE_CHANGED / preview refresh. */
	if (followed_name_.empty()) {
		obs_source_t *ss = obs_scene_get_source(scene);
		show_scene_locked(ss, false);
		followed_name_ = kDefaultSceneName;
		obs_log(LOG_INFO, "vertical canvas default scene '%s' is program (fade ready)",
			kDefaultSceneName);
	}
	obs_scene_release(scene);
	return true;
}

bool MsrVerticalCanvas::ensure(uint32_t width, uint32_t height)
{
	if (width < 2)
		width = 1080;
	if (height < 2)
		height = 1920;
	/* Vertical canvas is always portrait (phone). Never keep main 1920x1080. */
	if (width >= height) {
		width = 1080;
		height = 1920;
	}
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
	/* Channel 0 may be the fade transition — resolve the followed scene by name. */
	if (!followed_name_.empty()) {
		obs_scene_t *by_name = obs_canvas_get_scene_by_name(canvas_, followed_name_.c_str());
		if (by_name)
			return by_name;
	}
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
	if (!ensure_transition_locked()) {
		obs_scene_release(scene);
		return false;
	}
	const bool animate = (!followed_name_.empty() && followed_name_ != scene_name);
	show_scene_locked(obs_scene_get_source(scene), animate);
	followed_name_ = scene_name;
	obs_scene_release(scene);
	obs_log(LOG_INFO, "vertical program scene set to '%s' (fade=%s %dms)", scene_name.c_str(),
		animate ? "yes" : "no", transition_ms_);
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
			obs_source_t *vsrc = obs_scene_get_source(vert_scene);
			const char *vname = vsrc ? obs_source_get_name(vsrc) : nullptr;
			bool restored = false;
			if (vname && *vname)
				restored = apply_saved_transform(added, vname);
			if (!restored) {
				/* H-hide on main must not hide the vertical twin. */
				const bool hide_h = sceneitem_hide_horizontal(mitem);
				obs_sceneitem_set_visible(added, hide_h ? true : obs_sceneitem_visible(mitem));
			} else {
				/* Still force visible when main is H-hidden so vertical twin shows. */
				const bool hide_h = sceneitem_hide_horizontal(mitem);
				if (hide_h)
					obs_sceneitem_set_visible(added, true);
			}
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
		signal_handler_disconnect(sh, "item_visible", on_main_item_visible, this);
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
	signal_handler_connect(sh, "item_visible", on_main_item_visible, this);
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
	const bool ok = follow_scene_named(name, true);
	obs_source_release(main_src);
	return ok;
}

bool MsrVerticalCanvas::follow_scene_named(const std::string &scene_name, bool animate)
{
	if (scene_name.empty())
		return false;
	if (following_.exchange(true))
		return false; /* re-entrant */

	obs_source_t *main_src = obs_get_source_by_name(scene_name.c_str());
	if (!main_src) {
		following_.store(false);
		return false;
	}
	obs_scene_t *main_scene = obs_scene_from_source(main_src);
	if (!main_scene) {
		obs_source_release(main_src);
		following_.store(false);
		return false;
	}

	bool ok = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		ok = follow_scene_named_locked(scene_name, main_scene, animate);
	}

	attach_main_signals(main_src);
	obs_source_release(main_src);
	if (ok) {
		apply_horizontal_hides();
		rebuild_audio_mix();
	}
	following_.store(false);
	return ok;
}

bool MsrVerticalCanvas::follow_scene_named_locked(const std::string &scene_name, obs_scene_t *main_scene,
						 bool animate)
{
	if (!canvas_ || !main_scene)
		return false;

	obs_scene_t *vert = obs_canvas_get_scene_by_name(canvas_, scene_name.c_str());
	if (!vert)
		vert = obs_canvas_scene_create(canvas_, scene_name.c_str());
	if (!vert) {
		obs_log(LOG_ERROR, "vertical follow: failed to create scene '%s'", scene_name.c_str());
		return false;
	}

	sync_from_main(main_scene, vert);
	if (!ensure_transition_locked()) {
		obs_scene_release(vert);
		return false;
	}

	const bool same = (!followed_name_.empty() && followed_name_ == scene_name);
	if (same) {
		/* Already showing this scene — sync only; do not restart/cancel a fade. */
		obs_scene_release(vert);
		return true;
	}

	const bool do_anim = animate && !followed_name_.empty() && transition_ms_ > 0;
	show_scene_locked(obs_scene_get_source(vert), do_anim);
	followed_name_ = scene_name;
	obs_log(LOG_INFO, "vertical canvas following main scene '%s' (fade=%s %dms)", scene_name.c_str(),
		do_anim ? "yes" : "no", transition_ms_);
	obs_scene_release(vert);
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


bool MsrVerticalCanvas::set_item_layout(const std::string &source_name, float x, float y, float bounds_w,
				       float bounds_h, obs_bounds_type bounds)
{
	obs_scene_t *scene = program_scene();
	if (!scene)
		return false;
	obs_sceneitem_t *item = find_item_by_name(scene, source_name);
	if (!item) {
		obs_scene_release(scene);
		return false;
	}
	obs_sceneitem_set_bounds_type(item, bounds);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
	vec2 b;
	vec2_set(&b, bounds_w, bounds_h);
	obs_sceneitem_set_bounds(item, &b);
	vec2 pos;
	vec2_set(&pos, x, y);
	obs_sceneitem_set_pos(item, &pos);
	obs_scene_release(scene);
	obs_log(LOG_INFO, "vertical layout '%s' -> (%.0f,%.0f) %.0fx%.0f bounds=%d", source_name.c_str(), x, y,
		bounds_w, bounds_h, (int)bounds);
	persist_current_scene_layout();
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
	persist_current_scene_layout();
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

bool MsrVerticalCanvas::stretch_source(const std::string &source_name)
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
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
	vec2 bounds;
	vec2_set(&bounds, (float)cw, (float)ch);
	obs_sceneitem_set_bounds(item, &bounds);
	vec2 pos;
	vec2_set(&pos, 0.0f, 0.0f);
	obs_sceneitem_set_pos(item, &pos);
	obs_scene_release(scene);
	persist_current_scene_layout();
	return true;
}

bool MsrVerticalCanvas::center_source(const std::string &source_name)
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
	vec2 bounds;
	obs_sceneitem_get_bounds(item, &bounds);
	if (bounds.x < 1.0f || bounds.y < 1.0f) {
		obs_source_t *src = obs_sceneitem_get_source(item);
		vec2 scale;
		obs_sceneitem_get_scale(item, &scale);
		bounds.x = float(obs_source_get_width(src)) * scale.x;
		bounds.y = float(obs_source_get_height(src)) * scale.y;
	}
	vec2 pos;
	vec2_set(&pos, ((float)cw - bounds.x) * 0.5f, ((float)ch - bounds.y) * 0.5f);
	obs_sceneitem_set_pos(item, &pos);
	obs_scene_release(scene);
	persist_current_scene_layout();
	return true;
}

bool MsrVerticalCanvas::reset_source_transform(const std::string &source_name)
{
	return fit_source(source_name);
}

bool MsrVerticalCanvas::item_visible(const std::string &source_name)
{
	obs_scene_t *scene = program_scene();
	if (!scene)
		return false;
	obs_sceneitem_t *item = find_item_by_name(scene, source_name);
	if (!item) {
		obs_scene_release(scene);
		return false;
	}
	const bool vis = obs_sceneitem_visible(item);
	obs_scene_release(scene);
	return vis;
}

bool MsrVerticalCanvas::select_source(const std::string &source_name, bool exclusive)
{
	obs_scene_t *scene = program_scene();
	if (!scene)
		return false;
	if (exclusive) {
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *it, void *) {
				obs_sceneitem_select(it, false);
				return true;
			},
			nullptr);
	}
	obs_sceneitem_t *item = find_item_by_name(scene, source_name);
	if (!item) {
		obs_scene_release(scene);
		return false;
	}
	obs_sceneitem_select(item, true);
	obs_scene_release(scene);
	return true;
}

bool MsrVerticalCanvas::select_source_uuid(const std::string &uuid, bool exclusive)
{
	if (uuid.empty())
		return false;
	obs_scene_t *scene = program_scene();
	if (!scene)
		return false;
	if (exclusive) {
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *it, void *) {
				obs_sceneitem_select(it, false);
				return true;
			},
			nullptr);
	}
	obs_sceneitem_t *item = find_item_by_uuid(scene, uuid.c_str());
	if (!item) {
		obs_scene_release(scene);
		return false;
	}
	obs_sceneitem_select(item, true);
	obs_scene_release(scene);
	return true;
}

void MsrVerticalCanvas::clear_selection()
{
	obs_scene_t *scene = program_scene();
	if (!scene)
		return;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *it, void *) {
			obs_sceneitem_select(it, false);
			return true;
		},
		nullptr);
	obs_scene_release(scene);
}

std::string MsrVerticalCanvas::selected_source_name()
{
	std::string name;
	obs_scene_t *scene = program_scene();
	if (!scene)
		return name;
	struct Ctx {
		std::string *out;
	} ctx{&name};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *it, void *p) {
			auto *c = static_cast<Ctx *>(p);
			if (obs_sceneitem_selected(it)) {
				const char *n = obs_source_get_name(obs_sceneitem_get_source(it));
				if (n)
					*c->out = n;
			}
			return true;
		},
		&ctx);
	obs_scene_release(scene);
	return name;
}

bool MsrVerticalCanvas::nudge_selected(float dx, float dy)
{
	obs_scene_t *scene = program_scene();
	if (!scene)
		return false;
	bool any = false;
	struct Ctx {
		float dx, dy;
		bool *any;
	} ctx{dx, dy, &any};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *it, void *p) {
			auto *c = static_cast<Ctx *>(p);
			if (!obs_sceneitem_selected(it) || obs_sceneitem_locked(it))
				return true;
			vec2 pos;
			obs_sceneitem_get_pos(it, &pos);
			pos.x = std::round(pos.x + c->dx);
			pos.y = std::round(pos.y + c->dy);
			obs_sceneitem_set_pos(it, &pos);
			*c->any = true;
			return true;
		},
		&ctx);
	obs_scene_release(scene);
	return any;
}



void MsrVerticalCanvas::on_main_item_visible(void *data, calldata_t *cd)
{
	auto *self = static_cast<MsrVerticalCanvas *>(data);
	if (!self || self->applying_hide_horizontal_.load())
		return;
	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(cd, "item");
	if (!item || !sceneitem_hide_horizontal(item))
		return;
	const bool visible = calldata_bool(cd, "visible");
	if (!visible)
		return;
	/* Sticky H: keep main hidden while the flag is set. */
	self->applying_hide_horizontal_.store(true);
	obs_sceneitem_set_visible(item, false);
	self->applying_hide_horizontal_.store(false);
}

bool MsrVerticalCanvas::sceneitem_hide_horizontal(obs_sceneitem_t *item)
{
	if (!item)
		return false;
	obs_data_t *ps = obs_sceneitem_get_private_settings(item);
	if (!ps)
		return false;
	bool hide = false;
	if (obs_data_has_user_value(ps, kHideHorizontalKey))
		hide = obs_data_get_bool(ps, kHideHorizontalKey);
	obs_data_release(ps);
	return hide;
}

void MsrVerticalCanvas::sceneitem_set_hide_horizontal(obs_sceneitem_t *item, bool hide)
{
	if (!item)
		return;
	obs_data_t *ps = obs_sceneitem_get_private_settings(item);
	if (!ps)
		return;
	obs_data_set_bool(ps, kHideHorizontalKey, hide);
	obs_data_release(ps);
}

bool MsrVerticalCanvas::set_item_hide_horizontal(const std::string &source_name, bool hide)
{
	if (source_name.empty())
		return false;
	obs_source_t *main_src = obs_frontend_get_current_scene();
	if (!main_src)
		return false;
	obs_scene_t *main_scene = obs_scene_from_source(main_src);
	obs_sceneitem_t *item = main_scene ? find_item_by_name(main_scene, source_name) : nullptr;
	if (!item) {
		obs_source_release(main_src);
		return false;
	}
	sceneitem_set_hide_horizontal(item, hide);
	applying_hide_horizontal_.store(true);
	obs_sceneitem_set_visible(item, !hide);
	applying_hide_horizontal_.store(false);
	obs_source_release(main_src);
	if (hide) {
		/* Vertical-only intent: show on vertical when hiding from main. V can still hide later. */
		obs_scene_t *vscene = program_scene();
		if (vscene) {
			obs_sceneitem_t *vitem = find_item_by_name(vscene, source_name);
			if (vitem)
				obs_sceneitem_set_visible(vitem, true);
			obs_scene_release(vscene);
		}
	}
	obs_log(LOG_INFO, "horizontal hide %s for '%s'", hide ? "on" : "off", source_name.c_str());
	return true;
}

bool MsrVerticalCanvas::item_hide_horizontal(const std::string &source_name)
{
	if (source_name.empty())
		return false;
	obs_source_t *main_src = obs_frontend_get_current_scene();
	if (!main_src)
		return false;
	obs_scene_t *main_scene = obs_scene_from_source(main_src);
	obs_sceneitem_t *item = main_scene ? find_item_by_name(main_scene, source_name) : nullptr;
	const bool hide = item ? sceneitem_hide_horizontal(item) : false;
	obs_source_release(main_src);
	return hide;
}

void MsrVerticalCanvas::apply_horizontal_hides()
{
	obs_source_t *main_src = obs_frontend_get_current_scene();
	if (!main_src)
		return;
	obs_scene_t *main_scene = obs_scene_from_source(main_src);
	if (!main_scene) {
		obs_source_release(main_src);
		return;
	}
	applying_hide_horizontal_.store(true);
	obs_scene_enum_items(
		main_scene,
		[](obs_scene_t *, obs_sceneitem_t *it, void *) {
			if (sceneitem_hide_horizontal(it) && obs_sceneitem_visible(it))
				obs_sceneitem_set_visible(it, false);
			return true;
		},
		nullptr);
	applying_hide_horizontal_.store(false);
	obs_source_release(main_src);
}


bool MsrVerticalCanvas::set_item_audio_enabled(const std::string &source_name, bool enabled)
{
	obs_source_t *src = obs_get_source_by_name(source_name.c_str());
	if (!src)
		return false;
	MsrVerticalAudioMix::set_source_audio_enabled(src, enabled);
	obs_source_release(src);
	rebuild_audio_mix();
	obs_log(LOG_INFO, "vertical audio %s for '%s'", enabled ? "enabled" : "muted", source_name.c_str());
	return true;
}

bool MsrVerticalCanvas::item_audio_enabled(const std::string &source_name)
{
	obs_source_t *src = obs_get_source_by_name(source_name.c_str());
	if (!src)
		return true;
	const bool en = MsrVerticalAudioMix::source_audio_enabled(src);
	obs_source_release(src);
	return en;
}

audio_t *MsrVerticalCanvas::audio()
{
	audio_t *a = audio_mix_.audio();
	rebuild_audio_mix();
	return a;
}

void MsrVerticalCanvas::rebuild_audio_mix()
{
	/* Ensure mix is open so rebuild can attach captures. */
	(void)audio_mix_.audio();
	obs_scene_t *scene = program_scene();
	audio_mix_.rebuild(scene);
	if (scene)
		obs_scene_release(scene);
}

void MsrVerticalCanvas::set_transition_ms(int ms)
{
	if (ms < 0)
		ms = 0;
	if (ms > 5000)
		ms = 5000;
	std::lock_guard<std::mutex> lock(mutex_);
	transition_ms_ = ms;
	obs_log(LOG_INFO, "vertical fade duration set to %d ms", transition_ms_);
}

int MsrVerticalCanvas::transition_ms() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return transition_ms_;
}

bool MsrVerticalCanvas::ensure_transition_locked()
{
	if (!canvas_)
		return false;
	if (transition_) {
		obs_source_t *ch0 = obs_canvas_get_channel(canvas_, 0);
		if (ch0 != transition_)
			obs_canvas_set_channel(canvas_, 0, transition_);
		if (ch0)
			obs_source_release(ch0);
		obs_transition_set_size(transition_, width_ ? width_ : 1080, height_ ? height_ : 1920);
		return true;
	}

	transition_ = obs_source_create_private("fade_transition", "MSR Vertical Fade", nullptr);
	if (!transition_) {
		obs_log(LOG_ERROR, "failed to create vertical fade_transition");
		return false;
	}
	obs_transition_set_size(transition_, width_ ? width_ : 1080, height_ ? height_ : 1920);
	obs_transition_set_alignment(transition_, OBS_ALIGN_CENTER);
	obs_transition_set_scale_type(transition_, OBS_TRANSITION_SCALE_ASPECT);
	obs_canvas_set_channel(canvas_, 0, transition_);
	obs_log(LOG_INFO, "vertical canvas using private fade_transition (%d ms)", transition_ms_);
	return true;
}

void MsrVerticalCanvas::show_scene_locked(obs_source_t *scene_source, bool animate)
{
	if (!scene_source || !transition_)
		return;
	if (!animate || transition_ms_ <= 0) {
		obs_transition_set(transition_, scene_source);
		return;
	}
	obs_log(LOG_INFO, "vertical fade_transition_start -> '%s' (%d ms)", obs_source_get_name(scene_source),
		transition_ms_);
	obs_transition_start(transition_, OBS_TRANSITION_MODE_AUTO, (uint32_t)transition_ms_, scene_source);
}

std::string MsrVerticalCanvas::layouts_path() const
{
	char *base = obs_module_config_path(nullptr);
	if (base) {
		os_mkdirs(base);
		bfree(base);
	}
	char *profile = obs_frontend_get_current_profile();
	std::string name = "vertical-layouts";
	if (profile && *profile) {
		name += "-";
		for (const char *p = profile; *p; ++p) {
			char c = *p;
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
			    c == '_')
				name.push_back(c);
			else
				name.push_back('_');
		}
	}
	bfree(profile);
	name += ".json";
	char *path = obs_module_config_path(name.c_str());
	std::string out = path ? path : name;
	bfree(path);
	return out;
}

obs_data_t *MsrVerticalCanvas::layouts_root()
{
	if (!layouts_) {
		layouts_ = obs_data_create();
		obs_data_t *scenes = obs_data_create();
		obs_data_set_obj(layouts_, "scenes", scenes);
		obs_data_release(scenes);
	}
	return layouts_;
}

void MsrVerticalCanvas::load_vertical_layouts()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (layouts_) {
		obs_data_release(layouts_);
		layouts_ = nullptr;
	}
	const std::string path = layouts_path();
	layouts_ = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!layouts_) {
		layouts_ = obs_data_create();
		obs_data_t *scenes = obs_data_create();
		obs_data_set_obj(layouts_, "scenes", scenes);
		obs_data_release(scenes);
		obs_log(LOG_INFO, "vertical layouts: new file (%s)", path.c_str());
		return;
	}
	if (!obs_data_get_obj(layouts_, "scenes")) {
		obs_data_t *scenes = obs_data_create();
		obs_data_set_obj(layouts_, "scenes", scenes);
		obs_data_release(scenes);
	}
	obs_log(LOG_INFO, "vertical layouts loaded from %s", path.c_str());
}

void MsrVerticalCanvas::save_vertical_layouts()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!layouts_)
		return;
	const std::string path = layouts_path();
	if (!obs_data_save_json_pretty_safe(layouts_, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "vertical layouts save failed: %s", path.c_str());
	else
		obs_log(LOG_INFO, "vertical layouts saved to %s", path.c_str());
}

void MsrVerticalCanvas::capture_item_to_layouts(obs_sceneitem_t *item, const std::string &scene_name)
{
	if (!item || scene_name.empty())
		return;
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *uuid = obs_source_get_uuid(src);
	if (!uuid || !*uuid)
		return;

	obs_data_t *root = layouts_root();
	obs_data_t *scenes = obs_data_get_obj(root, "scenes");
	if (!scenes)
		return;
	obs_data_t *scene_obj = obs_data_get_obj(scenes, scene_name.c_str());
	if (!scene_obj) {
		scene_obj = obs_data_create();
		obs_data_set_obj(scenes, scene_name.c_str(), scene_obj);
	}

	obs_data_t *row = obs_data_create();
	vec2 pos, scale, bounds;
	obs_sceneitem_get_pos(item, &pos);
	obs_sceneitem_get_scale(item, &scale);
	obs_sceneitem_get_bounds(item, &bounds);
	obs_data_set_double(row, "pos_x", pos.x);
	obs_data_set_double(row, "pos_y", pos.y);
	obs_data_set_double(row, "scale_x", scale.x);
	obs_data_set_double(row, "scale_y", scale.y);
	obs_data_set_double(row, "bounds_x", bounds.x);
	obs_data_set_double(row, "bounds_y", bounds.y);
	obs_data_set_int(row, "bounds_type", (int)obs_sceneitem_get_bounds_type(item));
	obs_data_set_int(row, "bounds_align", (int)obs_sceneitem_get_bounds_alignment(item));
	obs_data_set_int(row, "align", (int)obs_sceneitem_get_alignment(item));
	obs_data_set_double(row, "rot", obs_sceneitem_get_rot(item));
	obs_data_set_bool(row, "visible", obs_sceneitem_visible(item));
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	obs_data_set_int(row, "crop_left", crop.left);
	obs_data_set_int(row, "crop_top", crop.top);
	obs_data_set_int(row, "crop_right", crop.right);
	obs_data_set_int(row, "crop_bottom", crop.bottom);
	const char *n = obs_source_get_name(src);
	if (n)
		obs_data_set_string(row, "name", n);

	obs_data_set_obj(scene_obj, uuid, row);
	obs_data_release(row);
	obs_data_release(scene_obj);
	obs_data_release(scenes);
}

void MsrVerticalCanvas::apply_item_from_data(obs_sceneitem_t *item, obs_data_t *row)
{
	if (!item || !row)
		return;
	if (obs_data_has_user_value(row, "bounds_type"))
		obs_sceneitem_set_bounds_type(item, (obs_bounds_type)obs_data_get_int(row, "bounds_type"));
	if (obs_data_has_user_value(row, "bounds_align"))
		obs_sceneitem_set_bounds_alignment(item, (uint32_t)obs_data_get_int(row, "bounds_align"));
	if (obs_data_has_user_value(row, "align"))
		obs_sceneitem_set_alignment(item, (uint32_t)obs_data_get_int(row, "align"));
	if (obs_data_has_user_value(row, "bounds_x") || obs_data_has_user_value(row, "bounds_y")) {
		vec2 b;
		vec2_set(&b, (float)obs_data_get_double(row, "bounds_x"), (float)obs_data_get_double(row, "bounds_y"));
		obs_sceneitem_set_bounds(item, &b);
	}
	if (obs_data_has_user_value(row, "pos_x") || obs_data_has_user_value(row, "pos_y")) {
		vec2 pos;
		vec2_set(&pos, (float)obs_data_get_double(row, "pos_x"), (float)obs_data_get_double(row, "pos_y"));
		obs_sceneitem_set_pos(item, &pos);
	}
	if (obs_data_has_user_value(row, "scale_x") || obs_data_has_user_value(row, "scale_y")) {
		vec2 scale;
		vec2_set(&scale, (float)obs_data_get_double(row, "scale_x"), (float)obs_data_get_double(row, "scale_y"));
		obs_sceneitem_set_scale(item, &scale);
	}
	if (obs_data_has_user_value(row, "rot"))
		obs_sceneitem_set_rot(item, (float)obs_data_get_double(row, "rot"));
	if (obs_data_has_user_value(row, "visible"))
		obs_sceneitem_set_visible(item, obs_data_get_bool(row, "visible"));
	if (obs_data_has_user_value(row, "crop_left") || obs_data_has_user_value(row, "crop_top") ||
	    obs_data_has_user_value(row, "crop_right") || obs_data_has_user_value(row, "crop_bottom")) {
		obs_sceneitem_crop crop = {};
		crop.left = (int)obs_data_get_int(row, "crop_left");
		crop.top = (int)obs_data_get_int(row, "crop_top");
		crop.right = (int)obs_data_get_int(row, "crop_right");
		crop.bottom = (int)obs_data_get_int(row, "crop_bottom");
		obs_sceneitem_set_crop(item, &crop);
	}
}

bool MsrVerticalCanvas::apply_saved_transform(obs_sceneitem_t *item, const std::string &scene_name)
{
	if (!item || scene_name.empty() || !layouts_)
		return false;
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *uuid = obs_source_get_uuid(src);
	if (!uuid || !*uuid)
		return false;
	obs_data_t *scenes = obs_data_get_obj(layouts_, "scenes");
	if (!scenes)
		return false;
	obs_data_t *scene_obj = obs_data_get_obj(scenes, scene_name.c_str());
	obs_data_release(scenes);
	if (!scene_obj)
		return false;
	obs_data_t *row = obs_data_get_obj(scene_obj, uuid);
	obs_data_release(scene_obj);
	if (!row)
		return false;
	apply_item_from_data(item, row);
	obs_data_release(row);
	return true;
}

void MsrVerticalCanvas::persist_current_scene_layout()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (followed_name_.empty() || !canvas_)
		return;
	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas_, followed_name_.c_str());
	if (!scene)
		return;
	struct Ctx {
		MsrVerticalCanvas *self;
		const std::string *scene_name;
	} ctx{this, &followed_name_};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) -> bool {
			auto *c = static_cast<Ctx *>(p);
			c->self->capture_item_to_layouts(item, *c->scene_name);
			return true;
		},
		&ctx);
	obs_scene_release(scene);
	if (!layouts_)
		return;
	const std::string path = layouts_path();
	if (!obs_data_save_json_pretty_safe(layouts_, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "vertical layouts save failed: %s", path.c_str());
	else
		obs_log(LOG_INFO, "vertical layouts saved to %s", path.c_str());
}

void MsrVerticalCanvas::detach_frontend_transition()
{
	if (!frontend_transition_)
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(frontend_transition_);
	if (sh)
		signal_handler_disconnect(sh, "transition_start", on_frontend_transition_start, this);
	obs_source_release(frontend_transition_);
	frontend_transition_ = nullptr;
}

void MsrVerticalCanvas::attach_frontend_transition()
{
	obs_source_t *t = obs_frontend_get_current_transition();
	if (!t)
		return;
	if (frontend_transition_ == t) {
		obs_source_release(t);
		return;
	}
	detach_frontend_transition();
	frontend_transition_ = t; /* take get ref */
	signal_handler_t *sh = obs_source_get_signal_handler(frontend_transition_);
	if (sh)
		signal_handler_connect(sh, "transition_start", on_frontend_transition_start, this);
	obs_log(LOG_INFO, "vertical fade hooked to main transition '%s'", obs_source_get_name(t));
}

void MsrVerticalCanvas::on_frontend_transition_start(void *data, calldata_t *cd)
{
	auto *self = static_cast<MsrVerticalCanvas *>(data);
	if (!self)
		return;
	obs_source_t *transition = (obs_source_t *)calldata_ptr(cd, "source");
	if (!transition)
		transition = self->frontend_transition_;
	obs_source_t *target = nullptr;
	if (transition)
		target = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B);
	if (!target)
		target = obs_frontend_get_current_scene();
	if (!target)
		return;
	const char *name = obs_source_get_name(target);
	if (name && *name) {
		obs_log(LOG_INFO, "main transition_start -> '%s' (start vertical fade with it)", name);
		self->follow_scene_named(name, true);
	}
	obs_source_release(target);
}
