/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/
#pragma once

#include <obs.h>
#include <obs-module.h>
#include <obs-module.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

/* Private audio-only sources for Source Record. These do not use or alter
 * OBS mixer tracks; the capture callback forwards only the chosen source. */
class MsrSourceRecordAudio {
public:
	static MsrSourceRecordAudio &instance();
	static void register_source();

	std::string prepare(const std::string &iso_source, const std::string &audio_source);
	void stop();

private:
	struct RelayContext {
		obs_source_t *source = nullptr;
	};
	struct Relay {
		obs_source_t *captured = nullptr;
		obs_source_t *relay = nullptr;
	};

	static const struct obs_source_info relay_info;
	static void *relay_create(obs_data_t *settings, obs_source_t *source);
	static void relay_destroy(void *data);
	static uint32_t relay_flags(void *data);
	static void capture_callback(void *param, obs_source_t *source, const struct audio_data *audio_data,
				     bool muted);

	std::mutex mutex_;
	std::map<std::string, Relay> relays_;
};
