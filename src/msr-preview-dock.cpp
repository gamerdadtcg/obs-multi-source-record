/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-preview-dock.hpp"
#include "msr-vertical-canvas.hpp"
#include "msr-controller.hpp"
#include "plugin-support.h"

#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QWindow>
#include <QScreen>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <objc/objc.h>
#else
#include <obs-nix-platform.h>
#ifdef ENABLE_WAYLAND
#include <QGuiApplication>
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
#include <qpa/qplatformnativeinterface.h>
#endif
#endif
#endif

namespace {

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

} // namespace

MsrPreviewDock::MsrPreviewDock(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumSize(120, 200);
}

MsrPreviewDock::~MsrPreviewDock()
{
	destroy_display();
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
}

void MsrPreviewDock::create_display()
{
	if (display_)
		return;
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
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		info.window.id = (uint32_t)winId();
		info.window.display = obs_get_nix_platform_display();
		break;
#ifdef ENABLE_WAYLAND
	case OBS_NIX_PLATFORM_WAYLAND: {
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
		QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
		info.window.display = native->nativeResourceForWindow("surface", windowHandle());
#else
		info.window.display = (void *)winId();
#endif
		if (!info.window.display) {
			obs_log(LOG_WARNING, "vertical preview: wayland surface missing");
			return;
		}
		break;
	}
#endif
	default:
		obs_log(LOG_WARNING, "vertical preview: unsupported nix platform");
		return;
	}
#endif

	display_ = obs_display_create(&info, 0xFF4C4C4C);
	if (!display_) {
		obs_log(LOG_WARNING, "vertical preview display create failed");
		return;
	}
	obs_display_add_draw_callback(display_, draw_preview, this);
}

void MsrPreviewDock::destroy_display()
{
	if (!display_)
		return;
	obs_display_remove_draw_callback(display_, draw_preview, this);
	obs_display_destroy(display_);
	display_ = nullptr;
}

void MsrPreviewDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	create_display();
}

void MsrPreviewDock::hideEvent(QHideEvent *event)
{
	destroy_display();
	QWidget::hideEvent(event);
}

void MsrPreviewDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!display_) {
		create_display();
		return;
	}
	QSize size = pixel_size(this);
	obs_display_resize(display_, (uint32_t)size.width(), (uint32_t)size.height());
}

void MsrPreviewDock::paintEvent(QPaintEvent *)
{
	create_display();
}

QPaintEngine *MsrPreviewDock::paintEngine() const
{
	return nullptr;
}

void MsrPreviewDock::draw_preview(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(param);
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc || !vc->ready())
		return;

	obs_canvas_t *canvas = vc->canvas();
	if (!canvas)
		return;

	uint32_t sourceCX = vc->width();
	uint32_t sourceCY = vc->height();
	if (sourceCX < 1)
		sourceCX = 1;
	if (sourceCY < 1)
		sourceCY = 1;

	int x, y;
	float scale;
	get_scale_and_center((int)sourceCX, (int)sourceCY, (int)cx, (int)cy, x, y, scale);
	const float newCX = scale * float(sourceCX);
	const float newCY = scale * float(sourceCY);

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(-float(x), newCX + float(x), -float(y), newCY + float(y), -100.0f, 100.0f);
	gs_reset_viewport();

	const bool previous = gs_set_linear_srgb(true);
	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, (int)newCX, (int)newCY);
	obs_canvas_render(canvas);
	gs_set_linear_srgb(previous);

	gs_projection_pop();
	gs_viewport_pop();

	obs_canvas_release(canvas);
}
