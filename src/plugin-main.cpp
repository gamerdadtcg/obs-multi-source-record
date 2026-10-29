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

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGED || event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		if (auto *c = MsrController::instance()) {
			c->load_settings();
			if (dock_widget)
				QMetaObject::invokeMethod(dock_widget, "refresh_sources", Qt::QueuedConnection);
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		if (auto *c = MsrController::instance())
			c->stop_all();
	}
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "loading Multi Source Record %s", PLUGIN_VERSION);

	MsrController::create();
	MsrController::instance()->load_settings();
	MsrController::instance()->register_hotkey();

	obs_frontend_add_event_callback(frontend_event, nullptr);

	QMainWindow *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dock_widget = new MsrDock(main);
	obs_frontend_add_dock_by_id("MultiSourceRecordDock", obs_module_text("DockTitle"), dock_widget);

	return true;
}

void obs_module_post_load(void)
{
	if (auto *c = MsrController::instance())
		c->register_websocket();
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	obs_frontend_remove_dock("MultiSourceRecordDock");
	dock_widget = nullptr;
	MsrController::destroy();
	obs_log(LOG_INFO, "unloaded Multi Source Record");
}
