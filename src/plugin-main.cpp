/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QMainWindow>
#include <QAction>
#include <QDockWidget>
#include <QMetaObject>

#include "plugin-support.h"
#include "msr-controller.hpp"
#include "msr-dock.hpp"
#include "msr-preview-dock.hpp"
#include "msr-vertical-canvas.hpp"
#include "msr-sources-inject.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Record multiple sources to separate files with each source's own audio";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Multi Source Record";
}

static MsrDock *dock_widget = nullptr;
static MsrPreviewDock *preview_dock = nullptr;

static void frontend_event(enum obs_frontend_event event, void *)
{
	auto *c = MsrController::instance();
	if (!c)
		return;

	if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGED || event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		c->load_settings();
		if (dock_widget)
			QMetaObject::invokeMethod(dock_widget, "refresh_sources", Qt::QueuedConnection);
		if (auto *vc = MsrVerticalCanvas::instance()) {
			auto s = c->settings();
			vc->ensure((uint32_t)s.vertical_width, (uint32_t)s.vertical_height);
			vc->set_transition_ms(s.vertical_transition_ms);
			vc->follow_main_scene();
		}
		if (preview_dock)
			QMetaObject::invokeMethod(preview_dock, "refresh_all", Qt::QueuedConnection);
		/* Start replay buffers if enabled in saved settings (safe; no OBS stream). */
		c->sync_replays_to_settings();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		if (auto *vc = MsrVerticalCanvas::instance()) {
			auto s = c->settings();
			vc->ensure((uint32_t)s.vertical_width, (uint32_t)s.vertical_height);
			vc->set_transition_ms(s.vertical_transition_ms);
			vc->follow_main_scene();
		}
		if (preview_dock)
			QMetaObject::invokeMethod(preview_dock, "on_main_scene_changed", Qt::QueuedConnection);
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		c->on_obs_recording_started();
		if (dock_widget)
			QMetaObject::invokeMethod(dock_widget, "update_recording_state", Qt::QueuedConnection,
						  Q_ARG(bool, c->recording()));
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		c->on_obs_recording_stopped();
		if (dock_widget)
			QMetaObject::invokeMethod(dock_widget, "update_recording_state", Qt::QueuedConnection,
						  Q_ARG(bool, c->recording()));
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		/* Follow OBS stream start for auto-start only — never call StartStream ourselves. */
		c->on_obs_streaming_started();
		if (dock_widget)
			QMetaObject::invokeMethod(dock_widget, "update_recording_state", Qt::QueuedConnection,
						  Q_ARG(bool, c->recording()));
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		c->on_obs_streaming_stopped();
		if (dock_widget)
			QMetaObject::invokeMethod(dock_widget, "update_recording_state", Qt::QueuedConnection,
						  Q_ARG(bool, c->recording()));
	} else if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		c->stop_replays();
		c->stop_streams();
		c->stop_all();
	}
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "loading Multi Source Record %s", PLUGIN_VERSION);

	MsrController::create();
	MsrVerticalCanvas::create();
	MsrController::instance()->load_settings();
	MsrController::instance()->register_hotkey();

	obs_frontend_add_event_callback(frontend_event, nullptr);

	QMainWindow *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dock_widget = new MsrDock(main);
	obs_frontend_add_dock_by_id("MultiSourceRecordDock", obs_module_text("DockTitle"), dock_widget);

	preview_dock = new MsrPreviewDock(main);
	obs_frontend_add_dock_by_id("MultiSourceRecordVerticalPreview", obs_module_text("VerticalPreviewDockTitle"),
				    preview_dock);

	MsrSourcesInject::create();

	return true;
}

void obs_module_post_load(void)
{
	if (auto *c = MsrController::instance()) {
		c->register_websocket();
		c->sync_replays_to_settings();
	}
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	obs_frontend_remove_dock("MultiSourceRecordVerticalPreview");
	preview_dock = nullptr;
	obs_frontend_remove_dock("MultiSourceRecordDock");
	dock_widget = nullptr;
	MsrSourcesInject::destroy();
	MsrController::destroy();
	MsrVerticalCanvas::destroy();
	obs_log(LOG_INFO, "unloaded Multi Source Record");
}
