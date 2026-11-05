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
#include "msr-stream-session.hpp"

#include <obs-hotkey.h>

#include <cstdint>
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
	bool pause_all();
	bool resume_all();
	bool toggle_pause();
	bool paused() const;
	bool add_chapter(const std::string &name);

	bool start_replays();
	void stop_replays();
	bool save_replays();
	bool replaying() const;

	/** Toggle/enable Exeldro Source Record filters on configured sources (not MSR file outputs). */
	bool source_recording() const;
	bool set_source_recording(bool enabled);
	bool toggle_source_recording();
	/** Stamp Source Record filters with date + 12h AM/PM filename pattern. */
	size_t apply_source_record_filename_formats();
	/** Push Different Audio / audio_source onto each target Source Record filter. */
	size_t apply_source_record_audio_settings();
	std::vector<std::string> source_record_targets() const;
	/** Apply replay_enabled from current settings (start or stop buffers). */
	void sync_replays_to_settings();

	/** OBS frontend auto-start hooks (never starts OBS streaming itself). */
	void on_obs_recording_started();
	void on_obs_recording_stopped();
	void on_obs_streaming_started();
	void on_obs_streaming_stopped();

	bool start_streams();
	void stop_streams();
	bool toggle_streams();
	bool streaming() const;
	/** Minimum obs_output total frames across active MSR stream sessions. */
	uint64_t stream_total_frames_min() const;
	std::vector<std::string> active_stream_names() const;

	std::vector<std::string> active_outputs() const;

	using StateCallback = std::function<void(bool recording)>;
	void set_state_callback(StateCallback cb);
	void set_source_record_state_callback(StateCallback cb);

	void register_hotkey();
	void unregister_hotkey();
	void register_websocket();

private:
	MsrController() = default;
	~MsrController();

	static void hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void pause_hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void chapter_hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void stream_hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void replay_hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void source_record_hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

	static void ws_start(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_stop(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_toggle(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_status(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_pause(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_resume(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_toggle_pause(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_add_chapter(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_get_settings(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_configure(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_start_stream(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_stop_stream(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_toggle_stream(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_save_replay(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_start_replay(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_stop_replay(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_source_record_status(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_start_source_record(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_stop_source_record(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_toggle_source_record(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_vertical_add_source(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_vertical_status(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_vertical_set_item(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_vertical_select(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_vertical_fit(obs_data_t *request_data, obs_data_t *response_data, void *priv);
	static void ws_vertical_set_layout(obs_data_t *request_data, obs_data_t *response_data, void *priv);

	std::string settings_path() const;
	void notify_state();
	void notify_source_record_state();

	mutable std::mutex mutex_;
	MsrSettings settings_;
	std::vector<std::unique_ptr<MsrSession>> sessions_;
	std::vector<std::unique_ptr<MsrSession>> replay_sessions_;
	std::vector<std::unique_ptr<MsrStreamSession>> stream_sessions_;
	obs_hotkey_id hotkey_id_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id pause_hotkey_id_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id chapter_hotkey_id_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id stream_hotkey_id_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id replay_hotkey_id_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id source_record_hotkey_id_ = OBS_INVALID_HOTKEY_ID;
	bool auto_started_from_record_ = false;
	bool auto_started_from_stream_ = false;
	void *ws_vendor_ = nullptr;
	StateCallback state_cb_;
	StateCallback source_record_state_cb_;
	static MsrController *instance_;
};
