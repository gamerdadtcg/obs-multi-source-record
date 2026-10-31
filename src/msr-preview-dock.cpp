/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

Interactive preview patterns adapted from OBS Studio / Aitum Vertical Canvas
(GPL-2.0) — simplified for MSR's linked vertical canvas.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-preview-dock.hpp"
#include "msr-vertical-canvas.hpp"
#include "msr-controller.hpp"
#include "plugin-support.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QEvent>
#include <QTimer>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QWindow>
#include <QSizePolicy>
#include <QSize>
#include <QGuiApplication>
#include <QSignalBlocker>
#include <QToolButton>
#include <QMenu>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <cmath>
#include <algorithm>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <objc/objc.h>
#else
#include <obs-nix-platform.h>
#endif


struct MsrSelHandleCtx {
	const vec2 *pos;
	obs_sceneitem_t *item;
	MsrItemHandle handle;
	MsrPreviewDisplay *self;
};

bool msr_selected_handle_cb(obs_scene_t *, obs_sceneitem_t *it, void *p)
{
	auto *c = static_cast<MsrSelHandleCtx *>(p);
	if (!obs_sceneitem_selected(it) || obs_sceneitem_locked(it))
		return true;
	MsrItemHandle h = c->self->handle_at_pos(it, *c->pos);
	if (h != MsrItemHandle::None) {
		c->item = it;
		c->handle = h;
	}
	return true;
}

namespace {

constexpr float HANDLE_RADIUS = 7.0f;       /* half-size in logical screen px (drawn larger) */
constexpr float HANDLE_SEL_RADIUS = 12.0f;  /* hit radius in logical screen px */
constexpr float DRAG_THRESHOLD = 5.0f;
constexpr float SNAP_DISTANCE = 12.0f;

inline QSize pixel_size(QWidget *w)
{
	return w->size() * w->devicePixelRatioF();
}

inline void get_scale_and_center(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	const double windowAspect = double(windowCX) / double(windowCY);
	const double baseAspect = double(baseCX) / double(baseCY);
	int newCX, newCY;
	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}
	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

struct EnumItemsCtx {
	QListWidget *list;
};

bool enum_scene_items_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<EnumItemsCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *name = obs_source_get_name(src);
	if (!name || !*name)
		return true;
	/* Names only — vertical V/A toggles live in the main OBS Sources list. */
	auto *row = new QListWidgetItem(QString::fromUtf8(name));
	row->setData(Qt::UserRole, QString::fromUtf8(name));
	row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
	ctx->list->insertItem(0, row);
	return true;
}

struct ItemAtPosCtx {
	const vec2 *pos;
	obs_sceneitem_t *item;
};

/* Cropped source base size in source pixels (before item scale / bounds). */
static void get_cropped_source_size(obs_sceneitem_t *item, float &cx, float &cy)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	cx = float(obs_source_get_width(src));
	cy = float(obs_source_get_height(src));
	cx -= float(crop.left + crop.right);
	cy -= float(crop.top + crop.bottom);
	if (cx < 1.0f)
		cx = 1.0f;
	if (cy < 1.0f)
		cy = 1.0f;
}

/*
 * Transform mapping the unit square (0,0)-(1,1) onto the *visible source content*.
 * Uses draw_transform × cropped source size — the same mapping OBS uses to place
 * the source pixels (honors crop, SCALE_INNER/OUTER fit, rotation, and scale).
 * Do NOT use bare box_transform for letterboxed bounds: that is the full 1080×1920
 * bounds rect, not the fitted cam.
 */
static void get_selection_box_transform(obs_sceneitem_t *item, matrix4 *out)
{
	float cx, cy;
	get_cropped_source_size(item, cx, cy);

	/* Same composition OBS uses when building transforms: out = draw * S(cx,cy).
	 * Unit square then maps onto the visible source quad (crop + bounds fit + rot). */
	matrix4 draw;
	obs_sceneitem_get_draw_transform(item, &draw);
	matrix4_scale3f(out, &draw, cx, cy, 1.0f);
}

/* World-space size of the visible content (tight box). */
static vec2 get_item_size(obs_sceneitem_t *item)
{
	vec2 size;
	matrix4 sel;
	get_selection_box_transform(item, &sel);
	/* Unit-square edges → world length (handles rotation). */
	vec3 o, x, y;
	vec3_set(&o, 0.0f, 0.0f, 0.0f);
	vec3_set(&x, 1.0f, 0.0f, 0.0f);
	vec3_set(&y, 0.0f, 1.0f, 0.0f);
	vec3_transform(&o, &o, &sel);
	vec3_transform(&x, &x, &sel);
	vec3_transform(&y, &y, &sel);
	size.x = fabsf(x.x - o.x);
	size.y = fabsf(y.y - o.y);
	/* Prefer edge lengths for rotated items */
	const float wlen = sqrtf((x.x - o.x) * (x.x - o.x) + (x.y - o.y) * (x.y - o.y));
	const float hlen = sqrtf((y.x - o.x) * (y.x - o.x) + (y.y - o.y) * (y.y - o.y));
	if (wlen > 1.0f)
		size.x = wlen;
	if (hlen > 1.0f)
		size.y = hlen;
	if (size.x < 1.0f)
		size.x = 1.0f;
	if (size.y < 1.0f)
		size.y = 1.0f;
	return size;
}

bool item_at_pos_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<ItemAtPosCtx *>(param);
	if (!obs_sceneitem_visible(item))
		return true;
	matrix4 box;
	get_selection_box_transform(item, &box);
	vec3 pos3, tpos;
	vec3_set(&pos3, ctx->pos->x, ctx->pos->y, 0.0f);
	matrix4 inv;
	matrix4_inv(&inv, &box);
	vec3_transform(&tpos, &pos3, &inv);
	if (tpos.x >= 0.0f && tpos.x <= 1.0f && tpos.y >= 0.0f && tpos.y <= 1.0f) {
		ctx->item = item;
		/* Keep going so the topmost item wins (enum is bottom→top). */
	}
	return true;
}


obs_sceneitem_t *first_selected_item(obs_scene_t *scene)
{
	if (!scene)
		return nullptr;
	struct Ctx {
		obs_sceneitem_t *item;
	} ctx{nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *it, void *p) {
			auto *c = static_cast<Ctx *>(p);
			if (obs_sceneitem_selected(it))
				c->item = it;
			return true;
		},
		&ctx);
	return ctx.item;
}

} // namespace

/* ---- MsrPreviewDisplay -------------------------------------------------- */

MsrPreviewDisplay::MsrPreviewDisplay(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumSize(135, 240); /* 9:16 phone proportions */
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	matrix4_identity(&item_to_screen_);
	matrix4_identity(&screen_to_item_);
}

MsrPreviewDisplay::~MsrPreviewDisplay()
{
	destroy_display();
}

int MsrPreviewDisplay::heightForWidth(int w) const
{
	if (w < 1)
		w = 1;
	return int(float(w) * 16.0f / 9.0f + 0.5f);
}

MsrPhoneFrame::MsrPhoneFrame(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setMinimumSize(150, 280);
	/* Dark letter/pillarbox host around the centered phone rect. */
	setStyleSheet("background-color: #141416;");
	display_ = new MsrPreviewDisplay(this);
	/* Do not stylesheet the native OBS display widget — that can crash Qt/OBS. Border is drawn in GS. */
}

void MsrPhoneFrame::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!display_)
		return;
	const int W = width();
	const int H = height();
	if (W < 2 || H < 2)
		return;
	/* Largest centered 9:16 phone rect inside the dock area. */
	int ph = H;
	int pw = int(float(ph) * 9.0f / 16.0f + 0.5f);
	if (pw > W) {
		pw = W;
		ph = int(float(pw) * 16.0f / 9.0f + 0.5f);
	}
	if (pw < 2)
		pw = 2;
	if (ph < 2)
		ph = 2;
	const int x = (W - pw) / 2;
	const int y = (H - ph) / 2;
	display_->setGeometry(x, y, pw, ph);
}

void MsrPreviewDisplay::ensure_canvas()
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	/* Always portrait phone canvas — never inherit main 1920x1080 OVI. */
	int w = 1080, h = 1920;
	if (auto *c = MsrController::instance()) {
		auto s = c->settings();
		if (s.vertical_width > 1 && s.vertical_height > 1 && s.vertical_width < s.vertical_height) {
			w = s.vertical_width;
			h = s.vertical_height;
		}
	}
	vc->ensure((uint32_t)w, (uint32_t)h);
	/* Re-force if something left the canvas landscape. */
	if (vc->width() >= vc->height() || vc->width() == 0 || vc->height() == 0)
		vc->ensure(1080, 1920);
	vc->follow_main_scene();
}

void MsrPreviewDisplay::create_display()
{
	if (display_)
		return;
	if (!windowHandle())
		winId();
	if (!windowHandle() || !windowHandle()->isExposed())
		return;

	ensure_canvas();

	QSize size = pixel_size(this);
	if (size.width() < 2 || size.height() < 2)
		return;

	gs_init_data info = {};
	info.cx = (uint32_t)size.width();
	info.cy = (uint32_t)size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

#ifdef _WIN32
	info.window.hwnd = (HWND)winId();
#elif defined(__APPLE__)
	info.window.view = (id)winId();
#else
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL) {
		obs_log(LOG_WARNING, "vertical preview: unsupported nix platform");
		return;
	}
	info.window.id = (uint32_t)winId();
	info.window.display = obs_get_nix_platform_display();
#endif

	display_ = obs_display_create(&info, 0xFF1A1A1A);
	if (!display_) {
		obs_log(LOG_WARNING, "vertical preview display create failed");
		return;
	}
	obs_display_add_draw_callback(display_, draw_preview, this);
	obs_log(LOG_INFO, "vertical preview display created %dx%d", size.width(), size.height());
}

void MsrPreviewDisplay::destroy_display()
{
	if (!display_)
		return;
	obs_display_remove_draw_callback(display_, draw_preview, this);
	obs_display_destroy(display_);
	display_ = nullptr;
}

void MsrPreviewDisplay::refresh_display()
{
	if (!display_)
		create_display();
	else {
		QSize size = pixel_size(this);
		if (size.width() >= 2 && size.height() >= 2)
			obs_display_resize(display_, (uint32_t)size.width(), (uint32_t)size.height());
	}
}

void MsrPreviewDisplay::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	ensure_canvas();
	QTimer::singleShot(0, this, [this]() { create_display(); });
	QTimer::singleShot(150, this, [this]() {
		if (!display_)
			create_display();
	});
}

void MsrPreviewDisplay::hideEvent(QHideEvent *event)
{
	destroy_display();
	QWidget::hideEvent(event);
}

void MsrPreviewDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!display_) {
		create_display();
		return;
	}
	QSize size = pixel_size(this);
	obs_display_resize(display_, (uint32_t)size.width(), (uint32_t)size.height());
}

void MsrPreviewDisplay::paintEvent(QPaintEvent *)
{
	create_display();
}

QPaintEngine *MsrPreviewDisplay::paintEngine() const
{
	return nullptr;
}

void MsrPreviewDisplay::update_preview_layout(uint32_t cx, uint32_t cy)
{
	/* Projection/scale MUST use the vertical canvas base (phone), never main OVI. */
	auto *vc = MsrVerticalCanvas::instance();
	uint32_t w = 1080;
	uint32_t h = 1920;
	if (vc && vc->width() > 1 && vc->height() > 1 && vc->width() < vc->height()) {
		w = vc->width();
		h = vc->height();
	}
	world_cx_ = w;
	world_cy_ = h;
	get_scale_and_center((int)world_cx_, (int)world_cy_, (int)cx, (int)cy, preview_x_, preview_y_, preview_scale_);
}

void MsrPreviewDisplay::preview_to_world(float px, float py, vec2 &out) const
{
	/* Same formula as OBSBasicPreview::GetMouseEventPos:
	 * preview_x_/y_ and preview_scale_ are in device pixels; qt pos is logical. */
	const float dpr = (float)devicePixelRatioF();
	if (dpr < 0.001f || preview_scale_ < 0.001f) {
		out.x = 0.0f;
		out.y = 0.0f;
		return;
	}
	const float scale = dpr / preview_scale_;
	out.x = (px - float(preview_x_) / dpr) * scale;
	out.y = (py - float(preview_y_) / dpr) * scale;
}

vec2 MsrPreviewDisplay::get_mouse_event_pos(const QPointF &qt) const
{
	vec2 pos;
	preview_to_world((float)qt.x(), (float)qt.y(), pos);
	pos.x = std::round(pos.x);
	pos.y = std::round(pos.y);
	return pos;
}

void MsrPreviewDisplay::draw_preview(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MsrPreviewDisplay *>(param);
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc || !vc->ready())
		return;

	obs_source_t *program = vc->program_source();
	if (!program)
		return;

	self->update_preview_layout(cx, cy);
	int newCX = (int)(self->preview_scale_ * float(self->world_cx_));
	int newCY = (int)(self->preview_scale_ * float(self->world_cy_));
	if (newCX < 1)
		newCX = 1;
	if (newCY < 1)
		newCY = 1;

	/* Dark letter/pillarbox bars behind the phone frame. */
	vec4 clear_color;
	vec4_set(&clear_color, 0.08f, 0.08f, 0.09f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	gs_viewport_push();
	gs_projection_push();

	const bool previous = gs_set_linear_srgb(true);
	/* Ortho matches vertical canvas base (1080x1920), not main 1920x1080. */
	gs_ortho(0.0f, float(self->world_cx_), 0.0f, float(self->world_cy_), -100.0f, 100.0f);
	gs_set_viewport(self->preview_x_, self->preview_y_, newCX, newCY);

	obs_source_video_render(program);
	self->draw_selection_overlay();

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();

	/* Thin border around the portrait phone area. */
	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, float(cx), 0.0f, float(cy), -100.0f, 100.0f);
	gs_set_viewport(0, 0, (int)cx, (int)cy);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	vec4 border;
	vec4_set(&border, 0.78f, 0.78f, 0.84f, 1.0f);
	gs_effect_set_vec4(color, &border);

	const float x0 = float(self->preview_x_);
	const float y0 = float(self->preview_y_);
	const float x1 = float(self->preview_x_ + newCX);
	const float y1 = float(self->preview_y_ + newCY);

	gs_render_start(true);
	gs_vertex2f(x0, y0);
	gs_vertex2f(x1, y0);
	gs_vertex2f(x1, y1);
	gs_vertex2f(x0, y1);
	gs_vertex2f(x0, y0);
	gs_vertbuffer_t *border_buf = gs_render_save();
	while (gs_effect_loop(solid, "Solid")) {
		gs_load_vertexbuffer(border_buf);
		gs_draw(GS_LINESTRIP, 0, 0);
	}
	gs_vertexbuffer_destroy(border_buf);
	gs_load_vertexbuffer(nullptr);

	gs_projection_pop();
	gs_viewport_pop();

	obs_source_release(program);
}

bool MsrPreviewDisplay::draw_selected_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (!obs_sceneitem_selected(item) || !obs_sceneitem_visible(item))
		return true;

	/* Draw in source-pixel space under draw_transform — identical to how OBS places the item. */
	float cx, cy;
	get_cropped_source_size(item, cx, cy);
	matrix4 draw;
	obs_sceneitem_get_draw_transform(item, &draw);

	gs_matrix_push();
	gs_matrix_mul(&draw);

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, cy);
	gs_vertex2f(cx, cy);
	gs_vertex2f(cx, 0.0f);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertbuffer_t *box = gs_render_save();

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	vec4 outline;
	vec4_set(&outline, 1.0f, 0.18f, 0.18f, 1.0f);
	gs_effect_set_vec4(color, &outline);

	while (gs_effect_loop(solid, "Solid")) {
		gs_load_vertexbuffer(box);
		gs_draw(GS_LINESTRIP, 0, 0);
	}

	/* Handles sized in screen px so they stay visible at any zoom. */
	auto *self = static_cast<MsrPreviewDisplay *>(param);
	vec2 content = get_item_size(item);
	const float dpr = self ? float(self->devicePixelRatioF()) : 1.0f;
	const float ps = (self && self->preview_scale_ > 0.001f) ? self->preview_scale_ : 1.0f;
	const float half_world = HANDLE_RADIUS * (dpr > 0.001f ? dpr : 1.0f) / ps;
	float hx = half_world * (cx / (fabsf(content.x) > 1.0f ? fabsf(content.x) : 1.0f));
	float hy = half_world * (cy / (fabsf(content.y) > 1.0f ? fabsf(content.y) : 1.0f));
	if (hx < 1.0f)
		hx = 1.0f;
	if (hy < 1.0f)
		hy = 1.0f;
	if (hx > cx * 0.12f)
		hx = cx * 0.12f;
	if (hy > cy * 0.12f)
		hy = cy * 0.12f;
	auto draw_handle = [&](float px, float py) {
		gs_render_start(true);
		gs_vertex2f(px - hx, py - hy);
		gs_vertex2f(px + hx, py - hy);
		gs_vertex2f(px - hx, py + hy);
		gs_vertex2f(px + hx, py + hy);
		gs_vertbuffer_t *hbuf = gs_render_save();
		vec4 hc;
		vec4_set(&hc, 1.0f, 1.0f, 1.0f, 1.0f);
		gs_effect_set_vec4(color, &hc);
		while (gs_effect_loop(solid, "Solid")) {
			gs_load_vertexbuffer(hbuf);
			gs_draw(GS_TRISTRIP, 0, 0);
		}
		gs_vertexbuffer_destroy(hbuf);
	};
	draw_handle(0.0f, 0.0f);
	draw_handle(cx * 0.5f, 0.0f);
	draw_handle(cx, 0.0f);
	draw_handle(0.0f, cy * 0.5f);
	draw_handle(cx, cy * 0.5f);
	draw_handle(0.0f, cy);
	draw_handle(cx * 0.5f, cy);
	draw_handle(cx, cy);

	gs_vertexbuffer_destroy(box);
	gs_matrix_pop();
	return true;
}

void MsrPreviewDisplay::draw_selection_overlay()
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	obs_scene_t *scene = vc->program_scene();
	if (!scene)
		return;
	gs_load_vertexbuffer(nullptr);
	obs_scene_enum_items(scene, draw_selected_cb, this);
	obs_scene_release(scene);
}

obs_sceneitem_t *MsrPreviewDisplay::item_at_pos(const vec2 &pos)
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return nullptr;
	obs_scene_t *scene = vc->program_scene();
	if (!scene)
		return nullptr;
	ItemAtPosCtx ctx{&pos, nullptr};
	obs_scene_enum_items(scene, item_at_pos_cb, &ctx);
	obs_scene_release(scene);
	return ctx.item;
}

MsrItemHandle MsrPreviewDisplay::handle_at_pos(obs_sceneitem_t *item, const vec2 &pos)
{
	if (!item || !obs_sceneitem_selected(item))
		return MsrItemHandle::None;

	matrix4 box;
	get_selection_box_transform(item, &box);
	/* HANDLE_SEL_RADIUS is logical screen px; world radius matches GetMouseEventPos mapping. */
	const float dpr = float(devicePixelRatioF());
	const float ps = preview_scale_ > 0.001f ? preview_scale_ : 1.0f;
	const float radius = HANDLE_SEL_RADIUS * (dpr > 0.001f ? dpr : 1.0f) / ps;

	struct Probe {
		float u, v;
		MsrItemHandle h;
	};
	const Probe probes[] = {
		{0.0f, 0.0f, MsrItemHandle::TopLeft},      {0.5f, 0.0f, MsrItemHandle::TopCenter},
		{1.0f, 0.0f, MsrItemHandle::TopRight},     {0.0f, 0.5f, MsrItemHandle::CenterLeft},
		{1.0f, 0.5f, MsrItemHandle::CenterRight},  {0.0f, 1.0f, MsrItemHandle::BottomLeft},
		{0.5f, 1.0f, MsrItemHandle::BottomCenter}, {1.0f, 1.0f, MsrItemHandle::BottomRight},
	};

	MsrItemHandle best = MsrItemHandle::None;
	float best_d = radius * radius;
	for (const auto &pr : probes) {
		vec3 corner, world;
		vec3_set(&corner, pr.u, pr.v, 0.0f);
		vec3_transform(&world, &corner, &box);
		float dx = world.x - pos.x;
		float dy = world.y - pos.y;
		float d = dx * dx + dy * dy;
		if (d <= best_d) {
			best_d = d;
			best = pr.h;
		}
	}
	return best;
}

void MsrPreviewDisplay::prepare_stretch(obs_sceneitem_t *item, MsrItemHandle handle)
{
	stretch_item_ = item;
	stretch_handle_ = handle;
	if (!item || handle == MsrItemHandle::None)
		return;

	stretch_start_size_ = get_item_size(item);
	obs_sceneitem_get_pos(item, &stretch_start_pos_);
	obs_sceneitem_get_crop(item, &start_crop_);

	matrix4 boxTransform;
	get_selection_box_transform(item, &boxTransform);
	vec3 itemUL;
	vec3_set(&itemUL, 0.0f, 0.0f, 0.0f);
	vec3_transform(&itemUL, &itemUL, &boxTransform);
	const float itemRot = obs_sceneitem_get_rot(item);

	matrix4_identity(&item_to_screen_);
	matrix4_rotate_aa4f(&item_to_screen_, &item_to_screen_, 0.0f, 0.0f, 1.0f, RAD(itemRot));
	matrix4_translate3f(&item_to_screen_, &item_to_screen_, itemUL.x, itemUL.y, 0.0f);

	matrix4_identity(&screen_to_item_);
	matrix4_translate3f(&screen_to_item_, &screen_to_item_, -itemUL.x, -itemUL.y, 0.0f);
	matrix4_rotate_aa4f(&screen_to_item_, &screen_to_item_, 0.0f, 0.0f, 1.0f, RAD(-itemRot));
}

void MsrPreviewDisplay::clamp_aspect(vec3 &tl, vec3 &br, vec2 &size, const vec2 &base_size) const
{
	if (base_size.x < 1.0f || base_size.y < 1.0f)
		return;
	const float baseAspect = base_size.x / base_size.y;
	const float aspect = size.x / (size.y != 0.0f ? size.y : 1.0f);
	const uint32_t stretchFlags = (uint32_t)stretch_handle_;
	const float startCX = stretch_start_size_.x;
	const float startCY = stretch_start_size_.y;

	if (stretch_handle_ == MsrItemHandle::TopLeft || stretch_handle_ == MsrItemHandle::TopRight ||
	    stretch_handle_ == MsrItemHandle::BottomLeft || stretch_handle_ == MsrItemHandle::BottomRight) {
		if (fabsf(aspect) < fabsf(baseAspect))
			size.x = size.y * baseAspect;
		else
			size.y = size.x / baseAspect;
	} else if (stretch_handle_ == MsrItemHandle::TopCenter || stretch_handle_ == MsrItemHandle::BottomCenter) {
		/* Edge: grow X symmetrically so aspect holds without drift. */
		size.x = size.y * baseAspect;
		tl.x = (startCX - size.x) * 0.5f;
		br.x = tl.x + size.x;
	} else if (stretch_handle_ == MsrItemHandle::CenterLeft || stretch_handle_ == MsrItemHandle::CenterRight) {
		size.y = size.x / baseAspect;
		tl.y = (startCY - size.y) * 0.5f;
		br.y = tl.y + size.y;
	}

	size.x = std::round(size.x);
	size.y = std::round(size.y);

	if (stretchFlags & (uint32_t)MsrItemHandle::Left)
		tl.x = br.x - size.x;
	else if (stretchFlags & (uint32_t)MsrItemHandle::Right)
		br.x = tl.x + size.x;

	if (stretchFlags & (uint32_t)MsrItemHandle::Top)
		tl.y = br.y - size.y;
	else if (stretchFlags & (uint32_t)MsrItemHandle::Bottom)
		br.y = tl.y + size.y;
}

vec3 MsrPreviewDisplay::snap_offset(const vec3 &tl, const vec3 &br) const
{
	vec3 clampOffset;
	vec3_zero(&clampOffset);
	if (!snap_enabled_)
		return clampOffset;

	const float dprSnap = float(devicePixelRatioF());
	const float clampDist = SNAP_DISTANCE * (dprSnap > 0.001f ? dprSnap : 1.0f) /
				(preview_scale_ > 0.001f ? preview_scale_ : 1.0f);
	const float screenW = float(world_cx_);
	const float screenH = float(world_cy_);
	const float centerX = br.x - (br.x - tl.x) / 2.0f;
	const float centerY = br.y - (br.y - tl.y) / 2.0f;

	auto edge = [&](float moveAxis, float checkAxis, float &offset) {
		const float dist = fabsf(checkAxis - moveAxis);
		if (dist < clampDist && fabsf(offset) < 0.001f)
			offset = checkAxis - moveAxis;
	};

	edge(tl.x, 0.0f, clampOffset.x);
	edge(br.x, screenW, clampOffset.x);
	edge(tl.y, 0.0f, clampOffset.y);
	edge(br.y, screenH, clampOffset.y);

	if (fabsf(screenW / 2.0f - centerX) < clampDist && fabsf(clampOffset.x) < 0.001f)
		clampOffset.x = screenW / 2.0f - centerX;
	if (fabsf(screenH / 2.0f - centerY) < clampDist && fabsf(clampOffset.y) < 0.001f)
		clampOffset.y = screenH / 2.0f - centerY;

	return clampOffset;
}

void MsrPreviewDisplay::update_hover_cursor(const vec2 &world)
{
	auto *vc = MsrVerticalCanvas::instance();
	obs_scene_t *scene = vc ? vc->program_scene() : nullptr;
	obs_sceneitem_t *selected = scene ? first_selected_item(scene) : nullptr;
	if (scene)
		obs_scene_release(scene);

	MsrItemHandle h = MsrItemHandle::None;
	if (selected && !obs_sceneitem_locked(selected))
		h = handle_at_pos(selected, world);

	const uint32_t flags = (uint32_t)h;
	if (h == MsrItemHandle::None) {
		/* Any press moves the locked selection, so show move cursor. */
		if (selected && !obs_sceneitem_locked(selected))
			setCursor(Qt::SizeAllCursor);
		else
			unsetCursor();
		return;
	}
	const bool corner = (flags & (flags - 1)) != 0;
	if ((flags & (uint32_t)MsrItemHandle::Left && flags & (uint32_t)MsrItemHandle::Top) ||
	    (flags & (uint32_t)MsrItemHandle::Right && flags & (uint32_t)MsrItemHandle::Bottom))
		setCursor(Qt::SizeFDiagCursor);
	else if ((flags & (uint32_t)MsrItemHandle::Left && flags & (uint32_t)MsrItemHandle::Bottom) ||
		 (flags & (uint32_t)MsrItemHandle::Right && flags & (uint32_t)MsrItemHandle::Top))
		setCursor(Qt::SizeBDiagCursor);
	else if (flags & (uint32_t)MsrItemHandle::Left || flags & (uint32_t)MsrItemHandle::Right)
		setCursor(Qt::SizeHorCursor);
	else if (flags & (uint32_t)MsrItemHandle::Top || flags & (uint32_t)MsrItemHandle::Bottom)
		setCursor(Qt::SizeVerCursor);
	else if (corner)
		setCursor(Qt::SizeFDiagCursor);
	else
		setCursor(Qt::SizeAllCursor);
}

void MsrPreviewDisplay::move_selected(const vec2 &pos)
{
	if (!stretch_item_)
		return;

	vec2 offset, moveOffset;
	vec2_sub(&offset, &pos, &start_pos_);
	vec2_sub(&moveOffset, &offset, &last_move_offset_);

	if (!(QGuiApplication::keyboardModifiers() & Qt::ControlModifier) && snap_enabled_) {
		vec2 size = get_item_size(stretch_item_);
		vec2 cur;
		obs_sceneitem_get_pos(stretch_item_, &cur);
		vec3 tl, br;
		vec3_set(&tl, cur.x + moveOffset.x, cur.y + moveOffset.y, 0.0f);
		vec3_set(&br, tl.x + size.x, tl.y + size.y, 0.0f);
		vec3 snap = snap_offset(tl, br);
		moveOffset.x += snap.x;
		moveOffset.y += snap.y;
	}

	vec2_add(&last_move_offset_, &last_move_offset_, &moveOffset);

	vec2 cur;
	obs_sceneitem_get_pos(stretch_item_, &cur);
	cur.x = std::round(cur.x + moveOffset.x);
	cur.y = std::round(cur.y + moveOffset.y);
	obs_sceneitem_set_pos(stretch_item_, &cur);
	last_pos_ = pos;
}

void MsrPreviewDisplay::stretch_selected(const vec2 &pos)
{
	if (!stretch_item_ || stretch_handle_ == MsrItemHandle::None)
		return;

	const uint32_t stretchFlags = (uint32_t)stretch_handle_;
	const bool shiftDown = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;

	vec3 tl, br, pos3;
	vec3_zero(&tl);
	vec3_set(&br, stretch_start_size_.x, stretch_start_size_.y, 0.0f);

	vec3_set(&pos3, pos.x, pos.y, 0.0f);
	vec3_transform(&pos3, &pos3, &screen_to_item_);

	if (stretchFlags & (uint32_t)MsrItemHandle::Left)
		tl.x = pos3.x;
	else if (stretchFlags & (uint32_t)MsrItemHandle::Right)
		br.x = pos3.x;
	if (stretchFlags & (uint32_t)MsrItemHandle::Top)
		tl.y = pos3.y;
	else if (stretchFlags & (uint32_t)MsrItemHandle::Bottom)
		br.y = pos3.y;

	obs_source_t *source = obs_sceneitem_get_source(stretch_item_);
	const uint32_t source_cx = obs_source_get_width(source);
	const uint32_t source_cy = obs_source_get_height(source);
	if (!source_cx || !source_cy)
		return;

	vec2 baseSize;
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(stretch_item_, &crop);
	vec2_set(&baseSize, float(source_cx) - float(crop.left + crop.right),
		 float(source_cy) - float(crop.top + crop.bottom));
	if (baseSize.x < 1.0f)
		baseSize.x = 1.0f;
	if (baseSize.y < 1.0f)
		baseSize.y = 1.0f;

	vec2 size;
	vec2_set(&size, br.x - tl.x, br.y - tl.y);

	/* Aspect by default; Shift = free stretch (OBS scale-mode behavior). */
	if (!shiftDown)
		clamp_aspect(tl, br, size, baseSize);

	if (tl.x > br.x)
		std::swap(tl.x, br.x);
	if (tl.y > br.y)
		std::swap(tl.y, br.y);
	vec2_abs(&size, &size);
	if (size.x < 2.0f)
		size.x = 2.0f;
	if (size.y < 2.0f)
		size.y = 2.0f;

	obs_sceneitem_defer_update_begin(stretch_item_);
	/*
	 * Always leave bounds mode so the visible content box is what we resize.
	 * Aspect (default): uniform scale (scale_x == scale_y), opposite corner fixed
	 * via clamp_aspect + TOP|LEFT alignment at the new tl.
	 * Shift: free STRETCH bounds sized to the dragged rect.
	 */
	obs_sceneitem_set_bounds_alignment(stretch_item_, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
	obs_sceneitem_set_alignment(stretch_item_, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
	if (shiftDown) {
		obs_sceneitem_set_bounds_type(stretch_item_, OBS_BOUNDS_STRETCH);
		vec2 zero_scale;
		vec2_set(&zero_scale, 1.0f, 1.0f);
		obs_sceneitem_set_scale(stretch_item_, &zero_scale);
		obs_sceneitem_set_bounds(stretch_item_, &size);
	} else {
		vec2 scale;
		vec2_div(&scale, &size, &baseSize);
		const float s = (fabsf(scale.x) + fabsf(scale.y)) * 0.5f;
		if (s < 0.0001f)
			scale.x = scale.y = 0.0001f;
		else
			scale.x = scale.y = s;
		obs_sceneitem_set_bounds_type(stretch_item_, OBS_BOUNDS_NONE);
		vec2 empty;
		vec2_zero(&empty);
		obs_sceneitem_set_bounds(stretch_item_, &empty);
		obs_sceneitem_set_scale(stretch_item_, &scale);
	}

	/* tl in item space → canvas; with TOP|LEFT this is the drawn origin. */
	vec3 newPos3;
	vec3_set(&newPos3, tl.x, tl.y, 0.0f);
	vec3_transform(&newPos3, &newPos3, &item_to_screen_);
	vec2 newPos;
	vec2_set(&newPos, std::round(newPos3.x), std::round(newPos3.y));
	obs_sceneitem_set_pos(stretch_item_, &newPos);
	obs_sceneitem_defer_update_end(stretch_item_);
}

void MsrPreviewDisplay::crop_selected(const vec2 &pos)
{
	if (!stretch_item_ || stretch_handle_ == MsrItemHandle::None)
		return;
	obs_source_t *src = obs_sceneitem_get_source(stretch_item_);
	const float sw = float(obs_source_get_width(src));
	const float sh = float(obs_source_get_height(src));
	if (sw < 1.0f || sh < 1.0f)
		return;

	vec2 size = stretch_start_size_;
	float dx = pos.x - start_pos_.x;
	float dy = pos.y - start_pos_.y;
	const uint32_t flags = (uint32_t)stretch_handle_;

	obs_sceneitem_crop crop = start_crop_;
	auto clamp_crop = [&](int &v, float maxv) {
		if (v < 0)
			v = 0;
		if ((float)v > maxv - 2.0f)
			v = (int)(maxv - 2.0f);
	};

	if (flags & (uint32_t)MsrItemHandle::Left) {
		int add = (int)std::round(dx * (sw / (size.x > 1 ? size.x : 1)));
		crop.left = start_crop_.left + add;
		clamp_crop(crop.left, sw - crop.right);
	}
	if (flags & (uint32_t)MsrItemHandle::Right) {
		int add = (int)std::round(-dx * (sw / (size.x > 1 ? size.x : 1)));
		crop.right = start_crop_.right + add;
		clamp_crop(crop.right, sw - crop.left);
	}
	if (flags & (uint32_t)MsrItemHandle::Top) {
		int add = (int)std::round(dy * (sh / (size.y > 1 ? size.y : 1)));
		crop.top = start_crop_.top + add;
		clamp_crop(crop.top, sh - crop.bottom);
	}
	if (flags & (uint32_t)MsrItemHandle::Bottom) {
		int add = (int)std::round(-dy * (sh / (size.y > 1 ? size.y : 1)));
		crop.bottom = start_crop_.bottom + add;
		clamp_crop(crop.bottom, sh - crop.top);
	}
	obs_sceneitem_set_crop(stretch_item_, &crop);
}

void MsrPreviewDisplay::mousePressEvent(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	const QPointF qt = event->position();
#else
	const QPointF qt = event->localPos();
#endif
	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	setFocus(Qt::MouseFocusReason);

	QSize ps = pixel_size(this);
	update_preview_layout((uint32_t)ps.width(), (uint32_t)ps.height());

	vec2 world = get_mouse_event_pos(qt);
	start_pos_ = world;
	last_pos_ = world;
	vec2_zero(&last_move_offset_);
	mouse_down_ = true;
	mouse_moved_ = false;
	mouse_over_items_ = false;
	cropping_ = (QGuiApplication::keyboardModifiers() & Qt::AltModifier);

	auto *vc = MsrVerticalCanvas::instance();
	obs_scene_t *scene = vc ? vc->program_scene() : nullptr;
	if (!scene) {
		event->accept();
		return;
	}

	/*
	 * Selection is locked to the dock source list. Preview clicks never
	 * select, switch, or clear — they only drag/resize the current item.
	 */
	obs_sceneitem_t *selected = first_selected_item(scene);
	MsrItemHandle handle = MsrItemHandle::None;
	if (selected && !obs_sceneitem_locked(selected)) {
		handle = handle_at_pos(selected, world);
		stretch_item_ = selected;
		mouse_over_items_ = true;
		prepare_stretch(stretch_item_, handle);
		update_hover_cursor(world);
	} else {
		stretch_item_ = nullptr;
		stretch_handle_ = MsrItemHandle::None;
		unsetCursor();
	}

	obs_scene_release(scene);
	event->accept();
}

void MsrPreviewDisplay::mouseMoveEvent(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	const QPointF qt = event->position();
#else
	const QPointF qt = event->localPos();
#endif
	QSize ps = pixel_size(this);
	update_preview_layout((uint32_t)ps.width(), (uint32_t)ps.height());
	vec2 world = get_mouse_event_pos(qt);

	if (!mouse_down_) {
		update_hover_cursor(world);
		event->accept();
		return;
	}

	if (!mouse_moved_) {
		const float dx = world.x - start_pos_.x;
		const float dy = world.y - start_pos_.y;
		if (dx * dx + dy * dy < DRAG_THRESHOLD * DRAG_THRESHOLD) {
			event->accept();
			return;
		}
		mouse_moved_ = true;
	}

	if (stretch_handle_ != MsrItemHandle::None && stretch_item_) {
		if (cropping_)
			crop_selected(world);
		else
			stretch_selected(world);
	} else if (stretch_item_) {
		if (cursor().shape() != Qt::SizeAllCursor)
			setCursor(Qt::SizeAllCursor);
		move_selected(world);
	}
	event->accept();
}

void MsrPreviewDisplay::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		mouse_down_ = false;
		mouse_moved_ = false;
		mouse_over_items_ = false;
		stretch_item_ = nullptr;
		stretch_handle_ = MsrItemHandle::None;
		cropping_ = false;
		vec2_zero(&last_move_offset_);
		unsetCursor();
	}
	event->accept();
}

void MsrPreviewDisplay::leaveEvent(QEvent *event)
{
	if (!mouse_down_)
		unsetCursor();
	QWidget::leaveEvent(event);
}

void MsrPreviewDisplay::keyPressEvent(QKeyEvent *event)
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc) {
		QWidget::keyPressEvent(event);
		return;
	}
	float step = (event->modifiers() & Qt::ShiftModifier) ? 10.0f : 1.0f;
	bool handled = false;
	switch (event->key()) {
	case Qt::Key_Escape:
		vc->clear_selection();
		emit selection_changed(QString());
		handled = true;
		break;
	case Qt::Key_Left:
		handled = vc->nudge_selected(-step, 0.0f);
		break;
	case Qt::Key_Right:
		handled = vc->nudge_selected(step, 0.0f);
		break;
	case Qt::Key_Up:
		handled = vc->nudge_selected(0.0f, -step);
		break;
	case Qt::Key_Down:
		handled = vc->nudge_selected(0.0f, step);
		break;
	default:
		break;
	}
	if (handled)
		event->accept();
	else
		QWidget::keyPressEvent(event);
}

void MsrPreviewDisplay::contextMenuEvent(QContextMenuEvent *event)
{
	auto *vc = MsrVerticalCanvas::instance();
	std::string name = vc ? vc->selected_source_name() : std::string();
	if (!vc || name.empty()) {
		QWidget::contextMenuEvent(event);
		return;
	}
	QMenu menu(this);
	QAction *fit = menu.addAction(QString::fromUtf8(obs_module_text("VerticalFit")));
	QAction *stretch = menu.addAction(QString::fromUtf8(obs_module_text("VerticalStretch")));
	QAction *center = menu.addAction(QString::fromUtf8(obs_module_text("VerticalCenter")));
	QAction *reset = menu.addAction(QString::fromUtf8(obs_module_text("VerticalResetTransform")));
	QAction *picked = menu.exec(event->globalPos());
	if (picked == fit)
		vc->fit_source(name);
	else if (picked == stretch)
		vc->stretch_source(name);
	else if (picked == center)
		vc->center_source(name);
	else if (picked == reset)
		vc->reset_source_transform(name);
	event->accept();
}

/* ---- MsrPreviewDock ----------------------------------------------------- */

MsrPreviewDock::MsrPreviewDock(QWidget *parent) : QWidget(parent)
{
	build_ui();
	ensure_canvas();
	refresh_all();
}

void MsrPreviewDock::build_ui()
{
	/* Tall, narrow dock so the phone preview reads as 9:16 by default. */
	setMinimumSize(200, 420);
	resize(280, 720);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	auto *scene_row = new QHBoxLayout();
	follow_label_ = new QLabel(QString::fromUtf8(obs_module_text("VerticalFollowing")), this);
	follow_label_->setWordWrap(true);
	scene_row->addWidget(follow_label_, 1);
	auto *refresh = new QPushButton(QString::fromUtf8(obs_module_text("VerticalRefresh")), this);
	scene_row->addWidget(refresh);
	root->addLayout(scene_row);
	connect(refresh, &QPushButton::clicked, this, &MsrPreviewDock::refresh_all);

	phone_frame_ = new MsrPhoneFrame(this);
	display_ = phone_frame_->display();
	root->addWidget(phone_frame_, 5);

	hint_label_ = new QLabel(QString::fromUtf8(obs_module_text("VerticalPreviewHint")), this);
	hint_label_->setWordWrap(true);
	root->addWidget(hint_label_);

	snap_check_ = new QCheckBox(QString::fromUtf8(obs_module_text("VerticalSnap")), this);
	snap_check_->setChecked(true);
	root->addWidget(snap_check_);
	connect(snap_check_, &QCheckBox::toggled, this, &MsrPreviewDock::on_snap_toggled);
	if (display_)
		display_->set_snap_enabled(true);

	root->addWidget(new QLabel(QString::fromUtf8(obs_module_text("VerticalSources")), this));
	source_list_ = new QListWidget(this);
	source_list_->setMinimumHeight(80);
	source_list_->setMaximumHeight(180);
	root->addWidget(source_list_);
		connect(source_list_, &QListWidget::itemClicked, this, &MsrPreviewDock::on_list_item_clicked);
	connect(source_list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *, QListWidgetItem *) {
		list_row_changed_ = true;
		on_list_selection_changed();
	});
	if (display_)
		connect(display_, &MsrPreviewDisplay::selection_changed, this,
			&MsrPreviewDock::on_preview_selection_changed);

	auto *btn_row = new QHBoxLayout();
	auto *up_btn = new QPushButton(QString::fromUtf8(obs_module_text("VerticalMoveUp")), this);
	auto *down_btn = new QPushButton(QString::fromUtf8(obs_module_text("VerticalMoveDown")), this);
	auto *fit_btn = new QPushButton(QString::fromUtf8(obs_module_text("VerticalFit")), this);
	btn_row->addWidget(up_btn);
	btn_row->addWidget(down_btn);
	btn_row->addWidget(fit_btn);
	root->addLayout(btn_row);

	connect(up_btn, &QPushButton::clicked, this, &MsrPreviewDock::on_move_up);
	connect(down_btn, &QPushButton::clicked, this, &MsrPreviewDock::on_move_down);
	connect(fit_btn, &QPushButton::clicked, this, &MsrPreviewDock::on_fit_selected);
}

void MsrPreviewDock::ensure_canvas()
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	int w = 1080, h = 1920;
	if (auto *c = MsrController::instance()) {
		auto s = c->settings();
		if (s.vertical_width > 1 && s.vertical_height > 1 && s.vertical_width < s.vertical_height) {
			w = s.vertical_width;
			h = s.vertical_height;
		}
	}
	vc->ensure((uint32_t)w, (uint32_t)h);
	if (vc->width() >= vc->height() || vc->width() == 0 || vc->height() == 0)
		vc->ensure(1080, 1920);
	vc->follow_main_scene();
}

void MsrPreviewDock::refresh_all()
{
	ensure_canvas();
	if (follow_label_) {
		auto *vc = MsrVerticalCanvas::instance();
		const std::string name = vc ? vc->followed_scene_name() : std::string();
		follow_label_->setText(QString::fromUtf8(obs_module_text("VerticalFollowing")).arg(
			QString::fromStdString(name.empty() ? "?" : name)));
	}
	rebuild_source_list();
	if (display_)
		display_->refresh_display();
}

void MsrPreviewDock::on_main_scene_changed()
{
	refresh_all();
}

void MsrPreviewDock::rebuild_source_list()
{
	if (!source_list_)
		return;
	updating_ui_ = true;
	const QString sel = selected_source_name();
	source_list_->clear();
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc) {
		updating_ui_ = false;
		return;
	}
	obs_scene_t *scene = vc->program_scene();
	if (!scene) {
		updating_ui_ = false;
		return;
	}
	EnumItemsCtx ctx{source_list_};
	obs_scene_enum_items(scene, enum_scene_items_cb, &ctx);
	obs_scene_release(scene);

	if (!sel.isEmpty()) {
		for (int i = 0; i < source_list_->count(); i++) {
			auto *it = source_list_->item(i);
			const QString n = it->data(Qt::UserRole).toString();
			if (n == sel || it->text() == sel) {
				source_list_->setCurrentRow(i);
				break;
			}
		}
	}
	updating_ui_ = false;
	if (hint_label_)
		hint_label_->setVisible(source_list_->count() == 0);
}

QString MsrPreviewDock::selected_source_name() const
{
	if (!source_list_)
		return {};
	auto *item = source_list_->currentItem();
	if (!item)
		return {};
	const QString role = item->data(Qt::UserRole).toString();
	return role.isEmpty() ? item->text() : role;
}

void MsrPreviewDock::on_visibility_toggled(QListWidgetItem *item)
{
	UNUSED_PARAMETER(item);
}

void MsrPreviewDock::on_list_selection_changed()
{
	if (updating_ui_)
		return;
	const QString name = selected_source_name();
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	if (name.isEmpty()) {
		vc->clear_selection();
		return;
	}
	vc->select_source(name.toStdString(), true);
}

void MsrPreviewDock::on_list_item_clicked(QListWidgetItem *item)
{
	if (updating_ui_ || !item)
		return;
	/* New row: currentItemChanged already selected. Same-row re-click: deselect. */
	if (list_row_changed_) {
		list_row_changed_ = false;
		return;
	}
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	vc->clear_selection();
	updating_ui_ = true;
	source_list_->clearSelection();
	source_list_->setCurrentItem(nullptr);
	updating_ui_ = false;
}

void MsrPreviewDock::on_preview_selection_changed(const QString &name)
{
	if (!source_list_ || updating_ui_)
		return;
	updating_ui_ = true;
	list_row_changed_ = false;
	if (name.isEmpty()) {
		source_list_->clearSelection();
		source_list_->setCurrentItem(nullptr);
	} else {
		for (int i = 0; i < source_list_->count(); i++) {
			auto *it = source_list_->item(i);
			if (it->data(Qt::UserRole).toString() == name) {
				source_list_->setCurrentRow(i);
				break;
			}
		}
	}
	updating_ui_ = false;
}

void MsrPreviewDock::on_audio_button_clicked() {}

void MsrPreviewDock::on_snap_toggled(bool checked)
{
	if (display_)
		display_->set_snap_enabled(checked);
}

void MsrPreviewDock::on_move_up()
{
	const QString name = selected_source_name();
	if (name.isEmpty())
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	vc->reorder_source(name.toStdString(), true);
	rebuild_source_list();
}

void MsrPreviewDock::on_move_down()
{
	const QString name = selected_source_name();
	if (name.isEmpty())
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	vc->reorder_source(name.toStdString(), false);
	rebuild_source_list();
}

void MsrPreviewDock::on_fit_selected()
{
	const QString name = selected_source_name();
	if (name.isEmpty())
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	vc->fit_source(name.toStdString());
	if (display_)
		display_->refresh_display();
}
