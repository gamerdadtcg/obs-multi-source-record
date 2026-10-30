/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-controller.hpp"
#include "plugin-support.h"
#include "obs-websocket-api.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/config-file.h>

#include <fstream>
#include <sstream>
#include <string>

MsrController *MsrController::instance_ = nullptr;

MsrController *MsrController::instance()
{
	return instance_;
}

void MsrController::create()
{
	if (!instance_)
		instance_ = new MsrController();
}

void MsrController::destroy()
{
	delete instance_;
	instance_ = nullptr;
}

MsrController::~MsrController()
{
	unregister_hotkey();
	stop_all();
}

std::string MsrController::settings_path() const
{
	char *base = obs_module_config_path(nullptr);
	if (base) {
		os_mkdirs(base);
		bfree(base);
	}

	char *profile = obs_frontend_get_current_profile();
	std::string name = "settings";
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

void MsrController::load_settings()
{
	std::lock_guard<std::mutex> lock(mutex_);
	settings_ = MsrSettings{};

	/* Prefer last OBS recording path as default folder. */
	config_t *profile_cfg = obs_frontend_get_profile_config();
	if (profile_cfg) {
		const char *mode = config_get_string(profile_cfg, "Output", "Mode");
		bool adv = mode && (strcmp(mode, "Advanced") == 0 || strcmp(mode, "advanced") == 0);
		const char *path = nullptr;
		if (adv)
			path = config_get_string(profile_cfg, "AdvOut", "RecFilePath");
		if (!path || !*path)
			path = config_get_string(profile_cfg, "SimpleOutput", "FilePath");
		if (path && *path)
			settings_.output_folder = path;
	}

	std::ifstream in(settings_path());
	if (!in)
		return;

	std::stringstream buf;
	buf << in.rdbuf();
	std::string json = buf.str();
	/* Strip UTF-8 BOM (common when settings are written from PowerShell). */
	if (json.size() >= 3 && (unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB &&
	    (unsigned char)json[2] == 0xBF)
		json.erase(0, 3);
	obs_data_t *data = obs_data_create_from_json(json.c_str());
	if (!data) {
		obs_log(LOG_WARNING, "failed to parse settings JSON at %s", settings_path().c_str());
		return;
	}

	const char *folder = obs_data_get_string(data, "output_folder");
	if (folder && *folder)
		settings_.output_folder = folder;
	const char *pattern = obs_data_get_string(data, "filename_pattern");
	if (pattern && *pattern)
		settings_.filename_pattern = pattern;
	const char *container = obs_data_get_string(data, "container");
	if (container && *container)
		settings_.container = container;
	const char *encoder = obs_data_get_string(data, "video_encoder");
	if (encoder && *encoder)
		settings_.video_encoder = encoder;
	if (obs_data_has_user_value(data, "video_bitrate_kbps"))
		settings_.video_bitrate_kbps = (int)obs_data_get_int(data, "video_bitrate_kbps");
	if (obs_data_has_user_value(data, "audio_bitrate_kbps"))
		settings_.audio_bitrate_kbps = (int)obs_data_get_int(data, "audio_bitrate_kbps");
	if (obs_data_has_user_value(data, "vertical_width"))
		settings_.vertical_width = (int)obs_data_get_int(data, "vertical_width");
	if (obs_data_has_user_value(data, "vertical_height"))
		settings_.vertical_height = (int)obs_data_get_int(data, "vertical_height");

	obs_data_array_t *arr = obs_data_get_array(data, "sources");
	if (arr) {
		const size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			MsrSourceConfig cfg;
			cfg.source_name = obs_data_get_string(item, "source_name");
			cfg.enabled = obs_data_get_bool(item, "enabled");
			cfg.orientation = static_cast<MsrOrientation>(obs_data_get_int(item, "orientation"));
			cfg.extra_audio_source = obs_data_get_string(item, "extra_audio_source");
			if (cfg.extra_audio_source == cfg.source_name)
				cfg.extra_audio_source.clear();
			if (!cfg.source_name.empty())
				settings_.sources.push_back(cfg);
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(data);
}

void MsrController::save_settings()
{
	std::lock_guard<std::mutex> lock(mutex_);
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "output_folder", settings_.output_folder.c_str());
	obs_data_set_string(data, "filename_pattern", settings_.filename_pattern.c_str());
	obs_data_set_string(data, "container", settings_.container.c_str());
	obs_data_set_string(data, "video_encoder", settings_.video_encoder.c_str());
	obs_data_set_int(data, "video_bitrate_kbps", settings_.video_bitrate_kbps);
	obs_data_set_int(data, "audio_bitrate_kbps", settings_.audio_bitrate_kbps);
	obs_data_set_int(data, "vertical_width", settings_.vertical_width);
	obs_data_set_int(data, "vertical_height", settings_.vertical_height);

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &cfg : settings_.sources) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "source_name", cfg.source_name.c_str());
		obs_data_set_bool(item, "enabled", cfg.enabled);
		obs_data_set_int(item, "orientation", static_cast<int>(cfg.orientation));
		obs_data_set_string(item, "extra_audio_source", cfg.extra_audio_source.c_str());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "sources", arr);
	obs_data_array_release(arr);

	const char *json = obs_data_get_json(data);
	std::ofstream out(settings_path(), std::ios::trunc);
	if (out && json)
		out << json;
	obs_data_release(data);
}

MsrSettings MsrController::settings() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return settings_;
}

void MsrController::set_settings(const MsrSettings &s)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		settings_ = s;
	}
	save_settings();
}

bool MsrController::recording() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &s : sessions_) {
		if (s && s->active())
			return true;
	}
	return false;
}

std::vector<std::string> MsrController::active_outputs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<std::string> out;
	for (const auto &s : sessions_) {
		if (s && s->active())
			out.push_back(s->output_path());
	}
	return out;
}

void MsrController::set_state_callback(StateCallback cb)
{
	std::lock_guard<std::mutex> lock(mutex_);
	state_cb_ = std::move(cb);
}

void MsrController::notify_state()
{
	StateCallback cb;
	bool rec;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		cb = state_cb_;
		rec = false;
		for (const auto &s : sessions_) {
			if (s && s->active()) {
				rec = true;
				break;
			}
		}
	}
	if (cb)
		cb(rec);
}

bool MsrController::start_all()
{
	stop_all();

	MsrSettings snap;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		snap = settings_;
	}

	if (snap.output_folder.empty()) {
		obs_log(LOG_ERROR, "output folder is empty");
		return false;
	}

	bool any = false;
	std::vector<std::unique_ptr<MsrSession>> started;
	for (const auto &cfg : snap.sources) {
		if (!cfg.enabled || cfg.source_name.empty())
			continue;

		auto launch = [&](MsrOrientation ori) {
			auto session = std::make_unique<MsrSession>(cfg.source_name, ori, cfg.extra_audio_source, snap);
			if (session->start()) {
				started.push_back(std::move(session));
				any = true;
			}
		};

		if (cfg.orientation == MsrOrientation::Both) {
			launch(MsrOrientation::Horizontal);
			launch(MsrOrientation::Vertical);
		} else {
			launch(cfg.orientation);
		}
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		sessions_ = std::move(started);
	}
	if (!any) {
		size_t enabled = 0;
		for (const auto &cfg : snap.sources) {
			if (cfg.enabled && !cfg.source_name.empty())
				enabled++;
		}
		obs_log(LOG_ERROR, "start produced no outputs (%zu enabled source(s) configured)", enabled);
	}
	notify_state();
	return any;
}

void MsrController::stop_all()
{
	std::vector<std::unique_ptr<MsrSession>> to_stop;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		to_stop.swap(sessions_);
	}
	for (auto &s : to_stop) {
		if (s)
			s->stop();
	}
	notify_state();
}

bool MsrController::toggle()
{
	if (recording()) {
		stop_all();
		return false;
	}
	return start_all();
}

bool MsrController::paused() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	bool any = false;
	for (const auto &s : sessions_) {
		if (s && s->active()) {
			if (!s->paused())
				return false;
			any = true;
		}
	}
	return any;
}

bool MsrController::pause_all()
{
	std::vector<MsrSession *> targets;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &s : sessions_) {
			if (s && s->active())
				targets.push_back(s.get());
		}
	}
	bool ok = false;
	for (auto *s : targets) {
		if (s->pause())
			ok = true;
	}
	notify_state();
	return ok;
}

bool MsrController::resume_all()
{
	std::vector<MsrSession *> targets;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &s : sessions_) {
			if (s && s->active())
				targets.push_back(s.get());
		}
	}
	bool ok = false;
	for (auto *s : targets) {
		if (s->resume())
			ok = true;
	}
	notify_state();
	return ok;
}

bool MsrController::toggle_pause()
{
	if (!recording())
		return false;
	if (paused()) {
		resume_all();
		return false;
	}
	pause_all();
	return true;
}

void MsrController::hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	auto *self = static_cast<MsrController *>(data);
	if (self)
		self->toggle();
}

void MsrController::pause_hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	auto *self = static_cast<MsrController *>(data);
	if (self)
		self->toggle_pause();
}

void MsrController::register_hotkey()
{
	if (hotkey_id_ == OBS_INVALID_HOTKEY_ID) {
		hotkey_id_ = obs_hotkey_register_frontend("msr.toggle", "Multi Source Record: Start/Stop",
							  hotkey_pressed, this);
	}
	if (pause_hotkey_id_ == OBS_INVALID_HOTKEY_ID) {
		pause_hotkey_id_ = obs_hotkey_register_frontend(
			"msr.pause", "Multi Source Record: Pause/Resume", pause_hotkey_pressed, this);
	}
}

void MsrController::unregister_hotkey()
{
	if (hotkey_id_ != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(hotkey_id_);
		hotkey_id_ = OBS_INVALID_HOTKEY_ID;
	}
	if (pause_hotkey_id_ != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(pause_hotkey_id_);
		pause_hotkey_id_ = OBS_INVALID_HOTKEY_ID;
	}
}

void MsrController::ws_start(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	auto *self = static_cast<MsrController *>(priv);
	bool ok = self && self->start_all();
	obs_data_set_bool(response_data, "success", ok);
	obs_data_set_bool(response_data, "recording", self && self->recording());
	obs_data_set_bool(response_data, "paused", self && self->paused());
}

void MsrController::ws_stop(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	auto *self = static_cast<MsrController *>(priv);
	if (self)
		self->stop_all();
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_bool(response_data, "recording", false);
	obs_data_set_bool(response_data, "paused", false);
}

void MsrController::ws_toggle(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	auto *self = static_cast<MsrController *>(priv);
	bool rec = self && self->toggle();
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_bool(response_data, "recording", rec);
	obs_data_set_bool(response_data, "paused", self && self->paused());
}

void MsrController::ws_pause(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	auto *self = static_cast<MsrController *>(priv);
	bool ok = self && self->pause_all();
	obs_data_set_bool(response_data, "success", ok);
	obs_data_set_bool(response_data, "recording", self && self->recording());
	obs_data_set_bool(response_data, "paused", self && self->paused());
}

void MsrController::ws_resume(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	auto *self = static_cast<MsrController *>(priv);
	bool ok = self && self->resume_all();
	obs_data_set_bool(response_data, "success", ok);
	obs_data_set_bool(response_data, "recording", self && self->recording());
	obs_data_set_bool(response_data, "paused", self && self->paused());
}

void MsrController::ws_toggle_pause(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	auto *self = static_cast<MsrController *>(priv);
	bool now_paused = self && self->toggle_pause();
	obs_data_set_bool(response_data, "success", self && self->recording());
	obs_data_set_bool(response_data, "recording", self && self->recording());
	obs_data_set_bool(response_data, "paused", now_paused || (self && self->paused()));
}

void MsrController::ws_status(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	auto *self = static_cast<MsrController *>(priv);
	bool rec = self && self->recording();
	bool paused = self && self->paused();
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_bool(response_data, "recording", rec);
	obs_data_set_bool(response_data, "paused", paused);
	obs_data_array_t *arr = obs_data_array_create();
	if (self) {
		for (const auto &path : self->active_outputs()) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "path", path.c_str());
			obs_data_array_push_back(arr, item);
			obs_data_release(item);
		}
	}
	obs_data_set_array(response_data, "outputs", arr);
	obs_data_array_release(arr);
}

void MsrController::ws_get_settings(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	UNUSED_PARAMETER(request_data);
	auto *self = static_cast<MsrController *>(priv);
	if (!self) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	MsrSettings s = self->settings();
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_string(response_data, "output_folder", s.output_folder.c_str());
	obs_data_set_string(response_data, "filename_pattern", s.filename_pattern.c_str());
	obs_data_set_string(response_data, "container", s.container.c_str());
	obs_data_set_string(response_data, "video_encoder", s.video_encoder.c_str());
	obs_data_set_int(response_data, "video_bitrate_kbps", s.video_bitrate_kbps);
	obs_data_set_int(response_data, "audio_bitrate_kbps", s.audio_bitrate_kbps);
	obs_data_set_int(response_data, "vertical_width", s.vertical_width);
	obs_data_set_int(response_data, "vertical_height", s.vertical_height);
	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &cfg : s.sources) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "source_name", cfg.source_name.c_str());
		obs_data_set_bool(item, "enabled", cfg.enabled);
		obs_data_set_int(item, "orientation", static_cast<int>(cfg.orientation));
		obs_data_set_string(item, "extra_audio_source", cfg.extra_audio_source.c_str());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(response_data, "sources", arr);
	obs_data_array_release(arr);
}

void MsrController::ws_configure(obs_data_t *request_data, obs_data_t *response_data, void *priv)
{
	auto *self = static_cast<MsrController *>(priv);
	if (!self || !request_data) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	MsrSettings s = self->settings();
	if (obs_data_has_user_value(request_data, "output_folder"))
		s.output_folder = obs_data_get_string(request_data, "output_folder");
	if (obs_data_has_user_value(request_data, "filename_pattern")) {
		const char *p = obs_data_get_string(request_data, "filename_pattern");
		if (p && *p)
			s.filename_pattern = p;
	}
	if (obs_data_has_user_value(request_data, "container")) {
		const char *c = obs_data_get_string(request_data, "container");
		if (c && *c)
			s.container = c;
	}
	if (obs_data_has_user_value(request_data, "video_encoder")) {
		const char *e = obs_data_get_string(request_data, "video_encoder");
		if (e && *e)
			s.video_encoder = e;
	}
	if (obs_data_has_user_value(request_data, "video_bitrate_kbps"))
		s.video_bitrate_kbps = (int)obs_data_get_int(request_data, "video_bitrate_kbps");
	if (obs_data_has_user_value(request_data, "audio_bitrate_kbps"))
		s.audio_bitrate_kbps = (int)obs_data_get_int(request_data, "audio_bitrate_kbps");
	if (obs_data_has_user_value(request_data, "vertical_width"))
		s.vertical_width = (int)obs_data_get_int(request_data, "vertical_width");
	if (obs_data_has_user_value(request_data, "vertical_height"))
		s.vertical_height = (int)obs_data_get_int(request_data, "vertical_height");

	obs_data_array_t *arr = obs_data_get_array(request_data, "sources");
	if (arr) {
		s.sources.clear();
		const size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			MsrSourceConfig cfg;
			cfg.source_name = obs_data_get_string(item, "source_name");
			cfg.enabled = obs_data_get_bool(item, "enabled");
			cfg.orientation = static_cast<MsrOrientation>(obs_data_get_int(item, "orientation"));
			cfg.extra_audio_source = obs_data_get_string(item, "extra_audio_source");
			if (cfg.extra_audio_source == cfg.source_name)
				cfg.extra_audio_source.clear();
			if (!cfg.source_name.empty())
				s.sources.push_back(cfg);
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}

	self->set_settings(s);
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_bool(response_data, "recording", self->recording());
}

void MsrController::register_websocket()
{
	ws_vendor_ = obs_websocket_register_vendor("obs-multi-source-record");
	if (!ws_vendor_) {
		obs_log(LOG_WARNING, "obs-websocket not available; Stream Deck vendor requests disabled");
		return;
	}
	obs_websocket_vendor_register_request(ws_vendor_, "start", ws_start, this);
	obs_websocket_vendor_register_request(ws_vendor_, "stop", ws_stop, this);
	obs_websocket_vendor_register_request(ws_vendor_, "toggle", ws_toggle, this);
	obs_websocket_vendor_register_request(ws_vendor_, "status", ws_status, this);
	obs_websocket_vendor_register_request(ws_vendor_, "pause", ws_pause, this);
	obs_websocket_vendor_register_request(ws_vendor_, "resume", ws_resume, this);
	obs_websocket_vendor_register_request(ws_vendor_, "toggle_pause", ws_toggle_pause, this);
	obs_websocket_vendor_register_request(ws_vendor_, "get_settings", ws_get_settings, this);
	obs_websocket_vendor_register_request(ws_vendor_, "configure", ws_configure, this);
	obs_log(LOG_INFO, "registered obs-websocket vendor obs-multi-source-record");
}
