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
#include <QCheckBox>
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

namespace {

constexpr float HANDLE_RADIUS = 14.0f;

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
	MsrPreviewDock *dock;
};

bool enum_scene_items_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<EnumItemsCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *name = obs_source_get_name(src);
	if (!name || !*name)
		return true;

	auto *vc = MsrVerticalCanvas::instance();
	const QString qname = QString::fromUtf8(name);

	auto *row_item = new QListWidgetItem();
	row_item->setData(Qt::UserRole, qname);
	row_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
	ctx->list->insertItem(0, row_item);

	auto *row = new QWidget(ctx->list);
	auto *hl = new QHBoxLayout(row);
	hl->setContentsMargins(2, 0, 2, 0);
	hl->setSpacing(4);

	auto *vis = new QCheckBox(row);
	vis->setToolTip(QString::fromUtf8(obs_module_text("VerticalVideoToggleTip")));
	vis->setChecked(obs_sceneitem_visible(item));
	vis->setText(QString::fromUtf8("V"));

	auto *aud = new QToolButton(row);
	aud->setCheckable(true);
	aud->setAutoRaise(true);
	aud->setText(QString::fromUtf8("A"));
	aud->setToolTip(QString::fromUtf8(obs_module_text("VerticalAudioToggleTip")));
	aud->setChecked(vc ? vc->item_audio_enabled(name) : true);
	aud->setStyleSheet(QStringLiteral(
		"QToolButton:checked { color: #6c6; font-weight: bold; }"
		"QToolButton:!checked { color: #666; }"));

	auto *lab = new QLabel(qname, row);
	hl->addWidget(vis);
	hl->addWidget(aud);
	hl->addWidget(lab, 1);

	ctx->list->setItemWidget(row_item, row);
	row_item->setSizeHint(row->sizeHint().expandedTo(QSize(0, 22)));

	QObject::connect(vis, &QCheckBox::toggled, ctx->list, [qname](bool on) {
		auto *v = MsrVerticalCanvas::instance();
		if (v)
			v->set_item_visible(qname.toStdString(), on);
	});
	QObject::connect(aud, &QToolButton::toggled, ctx->list, [qname](bool on) {
		auto *v = MsrVerticalCanvas::instance();
		if (v)
			v->set_item_audio_enabled(qname.toStdString(), on);
	});
	return true;
}

struct ItemAtPosCtx {
	const vec2 *pos;
	obs_sceneitem_t *item;
};

bool item_at_pos_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<ItemAtPosCtx *>(param);
	if (!obs_sceneitem_visible(item))
		return true;
	matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
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

static vec2 get_item_size(obs_sceneitem_t *item)
{
	vec2 size;
	obs_bounds_type bt = obs_sceneitem_get_bounds_type(item);
	if (bt != OBS_BOUNDS_NONE) {
		obs_sceneitem_get_bounds(item, &size);
	} else {
		obs_source_t *src = obs_sceneitem_get_source(item);
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		vec2 scale;
		obs_sceneitem_get_scale(item, &scale);
		size.x = float(obs_source_get_width(src) - crop.left - crop.right) * scale.x;
		size.y = float(obs_source_get_height(src) - crop.top - crop.bottom) * scale.y;
	}
	return size;
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
	setMinimumSize(90, 160);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);
	matrix4_identity(&item_to_screen_);
	matrix4_identity(&screen_to_item_);
}

MsrPreviewDisplay::~MsrPreviewDisplay()
{
	destroy_display();
}

void MsrPreviewDisplay::ensure_canvas()
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	int w = 1080, h = 1920;
	if (auto *c = MsrController::instance()) {
		auto s = c->settings();
		w = s.vertical_width;
		h = s.vertical_height;
	}
	vc->ensure((uint32_t)w, (uint32_t)h);
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
	auto *vc = MsrVerticalCanvas::instance();
	world_cx_ = vc && vc->width() ? vc->width() : 1080;
	world_cy_ = vc && vc->height() ? vc->height() : 1920;
	get_scale_and_center((int)world_cx_, (int)world_cy_, (int)cx, (int)cy, preview_x_, preview_y_, preview_scale_);
}

void MsrPreviewDisplay::preview_to_world(float px, float py, vec2 &out) const
{
	const float dpr = (float)devicePixelRatioF();
	px *= dpr;
	py *= dpr;
	out.x = (px - (float)preview_x_) / preview_scale_;
	out.y = (py - (float)preview_y_) / preview_scale_;
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

	gs_viewport_push();
	gs_projection_push();

	const bool previous = gs_set_linear_srgb(true);
	gs_ortho(0.0f, float(self->world_cx_), 0.0f, float(self->world_cy_), -100.0f, 100.0f);
	gs_set_viewport(self->preview_x_, self->preview_y_, newCX, newCY);

	obs_source_video_render(program);
	self->draw_selection_overlay();

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();

	obs_source_release(program);
}

bool MsrPreviewDisplay::draw_selected_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(param);
	if (!obs_sceneitem_selected(item) || !obs_sceneitem_visible(item))
		return true;

	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	gs_matrix_push();
	gs_matrix_mul(&boxTransform);

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	gs_vertex2f(1.0f, 0.0f);
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

	/* Corner handles */
	const float hs = 0.06f;
	auto draw_handle = [&](float hx, float hy) {
		gs_render_start(true);
		gs_vertex2f(hx - hs, hy - hs);
		gs_vertex2f(hx - hs, hy + hs);
		gs_vertex2f(hx + hs, hy + hs);
		gs_vertex2f(hx + hs, hy - hs);
		gs_vertex2f(hx - hs, hy - hs);
		gs_vertbuffer_t *hbuf = gs_render_save();
		vec4 hc;
		vec4_set(&hc, 1.0f, 1.0f, 1.0f, 1.0f);
		gs_effect_set_vec4(color, &hc);
		while (gs_effect_loop(solid, "Solid")) {
			gs_load_vertexbuffer(hbuf);
			gs_draw(GS_LINESTRIP, 0, 0);
		}
		gs_vertexbuffer_destroy(hbuf);
	};
	draw_handle(0.0f, 0.0f);
	draw_handle(0.5f, 0.0f);
	draw_handle(1.0f, 0.0f);
	draw_handle(0.0f, 0.5f);
	draw_handle(1.0f, 0.5f);
	draw_handle(0.0f, 1.0f);
	draw_handle(0.5f, 1.0f);
	draw_handle(1.0f, 1.0f);

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
	obs_sceneitem_get_box_transform(item, &box);
	const float radius = HANDLE_RADIUS / (preview_scale_ > 0.001f ? preview_scale_ : 1.0f);

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
	for (const auto &p : probes) {
		vec3 corner, world;
		vec3_set(&corner, p.u, p.v, 0.0f);
		vec3_transform(&world, &corner, &box);
		float dx = world.x - pos.x;
		float dy = world.y - pos.y;
		float d = dx * dx + dy * dy;
		if (d <= best_d) {
			best_d = d;
			best = p.h;
		}
	}
	return best;
}

void MsrPreviewDisplay::move_selected(const vec2 &pos)
{
	if (!stretch_item_)
		return;
	vec2 delta;
	vec2_sub(&delta, &pos, &last_pos_);
	vec2 cur;
	obs_sceneitem_get_pos(stretch_item_, &cur);
	cur.x = std::round(cur.x + delta.x);
	cur.y = std::round(cur.y + delta.y);
	obs_sceneitem_set_pos(stretch_item_, &cur);
	last_pos_ = pos;
}

void MsrPreviewDisplay::stretch_selected(const vec2 &pos)
{
	if (!stretch_item_ || stretch_handle_ == MsrItemHandle::None)
		return;

	const uint32_t flags = (uint32_t)stretch_handle_;
	vec2 size = stretch_start_size_;
	vec2 p = stretch_start_pos_;

	float dx = pos.x - start_pos_.x;
	float dy = pos.y - start_pos_.y;

	if (flags & (uint32_t)MsrItemHandle::Left) {
		p.x = stretch_start_pos_.x + dx;
		size.x = stretch_start_size_.x - dx;
	} else if (flags & (uint32_t)MsrItemHandle::Right) {
		size.x = stretch_start_size_.x + dx;
	}
	if (flags & (uint32_t)MsrItemHandle::Top) {
		p.y = stretch_start_pos_.y + dy;
		size.y = stretch_start_size_.y - dy;
	} else if (flags & (uint32_t)MsrItemHandle::Bottom) {
		size.y = stretch_start_size_.y + dy;
	}

	if (size.x < 2.0f)
		size.x = 2.0f;
	if (size.y < 2.0f)
		size.y = 2.0f;

	obs_sceneitem_set_bounds_type(stretch_item_, OBS_BOUNDS_STRETCH);
	obs_sceneitem_set_bounds_alignment(stretch_item_, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
	obs_sceneitem_set_alignment(stretch_item_, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
	obs_sceneitem_set_bounds(stretch_item_, &size);
	obs_sceneitem_set_pos(stretch_item_, &p);
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

	vec2 size = get_item_size(stretch_item_);
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

	QSize ps = pixel_size(this);
	update_preview_layout((uint32_t)ps.width(), (uint32_t)ps.height());

	vec2 world;
	preview_to_world((float)qt.x(), (float)qt.y(), world);
	start_pos_ = world;
	last_pos_ = world;
	mouse_down_ = true;
	mouse_moved_ = false;
	cropping_ = (QGuiApplication::keyboardModifiers() & Qt::AltModifier);

	/* Clear selection then select clicked item (unless shift). */
	auto *vc = MsrVerticalCanvas::instance();
	obs_scene_t *scene = vc ? vc->program_scene() : nullptr;
	if (scene) {
		const bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
		if (!shift) {
			struct ClearCtx {
			} clear;
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *it, void *) {
					obs_sceneitem_select(it, false);
					return true;
				},
				&clear);
		}
		obs_sceneitem_t *hit = item_at_pos(world);
		if (hit)
			obs_sceneitem_select(hit, true);

		const bool locked = hit && obs_sceneitem_locked(hit);
		stretch_item_ = (hit && !locked) ? hit : nullptr;
		stretch_handle_ = stretch_item_ ? handle_at_pos(stretch_item_, world) : MsrItemHandle::None;
		if (stretch_item_) {
			stretch_start_size_ = get_item_size(stretch_item_);
			obs_sceneitem_get_pos(stretch_item_, &stretch_start_pos_);
			obs_sceneitem_get_crop(stretch_item_, &start_crop_);
			if (stretch_handle_ != MsrItemHandle::None)
				setCursor(Qt::SizeFDiagCursor);
			else
				setCursor(Qt::SizeAllCursor);
		}
		if (hit) {
			const char *n = obs_source_get_name(obs_sceneitem_get_source(hit));
			if (n)
				emit selection_changed(QString::fromUtf8(n));
		} else {
			emit selection_changed(QString());
		}
		obs_scene_release(scene);
	}
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
	vec2 world;
	preview_to_world((float)qt.x(), (float)qt.y(), world);

	if (!mouse_down_) {
		obs_sceneitem_t *hit = item_at_pos(world);
		MsrItemHandle h = hit ? handle_at_pos(hit, world) : MsrItemHandle::None;
		if (h != MsrItemHandle::None)
			setCursor(Qt::SizeFDiagCursor);
		else if (hit && obs_sceneitem_selected(hit))
			setCursor(Qt::SizeAllCursor);
		else
			unsetCursor();
		event->accept();
		return;
	}

	mouse_moved_ = true;
	if (stretch_handle_ != MsrItemHandle::None) {
		if (cropping_)
			crop_selected(world);
		else
			stretch_selected(world);
	} else if (stretch_item_) {
		move_selected(world);
	}
	event->accept();
}

void MsrPreviewDisplay::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		mouse_down_ = false;
		stretch_item_ = nullptr;
		stretch_handle_ = MsrItemHandle::None;
		cropping_ = false;
		unsetCursor();
	}
	event->accept();
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

	display_ = new MsrPreviewDisplay(this);
	root->addWidget(display_, 1);

	hint_label_ = new QLabel(QString::fromUtf8(obs_module_text("VerticalPreviewHint")), this);
	hint_label_->setWordWrap(true);
	root->addWidget(hint_label_);

	root->addWidget(new QLabel(QString::fromUtf8(obs_module_text("VerticalSources")), this));
	source_list_ = new QListWidget(this);
	source_list_->setMinimumHeight(80);
	source_list_->setMaximumHeight(180);
	root->addWidget(source_list_);
	connect(source_list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *, QListWidgetItem *) {
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
		w = s.vertical_width;
		h = s.vertical_height;
	}
	vc->ensure((uint32_t)w, (uint32_t)h);
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
	EnumItemsCtx ctx{source_list_, this};
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
	if (!vc || name.isEmpty())
		return;
	vc->select_source(name.toStdString(), true);
}

void MsrPreviewDock::on_preview_selection_changed(const QString &name)
{
	if (!source_list_ || updating_ui_)
		return;
	updating_ui_ = true;
	if (name.isEmpty()) {
		source_list_->clearSelection();
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
