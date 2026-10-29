/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "msr-types.hpp"
#include "msr-session.hpp"

#include <obs-hotkey.h>

#include <memory>
#include <mutex>
#include <vector>
#include <functional>
#include <string>

class MsrController {
public:
	static MsrController *instance();
	static void create();
	static void destroy();

	void load_settings();
	void save_settings();
	MsrSettings settings() const;
	void set_settings(const MsrSettings &s);

	bool start_all();
	void stop_all();
	bool toggle();
	bool recording() const;

	std::vector<std::string> active_outputs() const;

	using StateCallback = std::function<void(bool recording)>;
	void set_state_callback(StateCallback cb);

	void register_hotkey();
	void unregister_hotkey();
	void register_websocket();

private:
	MsrController() = default;
	~MsrController();

	static void hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

	static void ws_start(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_stop(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_toggle(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_status(obs_data_t *request_data, obs_data_t *response_data, void *priv);

	std::string settings_path() const;
	void notify_state();

	mutable std::mutex mutex_;
	MsrSettings settings_;
	std::vector<std::unique_ptr<MsrSession>> sessions_;
	obs_hotkey_id hotkey_id_ = OBS_INVALID_HOTKEY_ID;
	void *ws_vendor_ = nullptr;
	StateCallback state_cb_;
	static MsrController *instance_;
};
