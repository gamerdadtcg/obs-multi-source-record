/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

enum class MsrOrientation {
	Horizontal = 0,
	Vertical = 1,
	Both = 2,
};

struct MsrSourceConfig {
	std::string source_name;
	bool enabled = false;
	MsrOrientation orientation = MsrOrientation::Horizontal;
	std::string extra_audio_source; /* optional second audio source name */
};

/** One multistream RTMP/RTMPS destination. Orientation is H or V only (not Both). */
struct MsrStreamDestination {
	std::string name;
	bool enabled = false;
	MsrOrientation orientation = MsrOrientation::Horizontal;
	std::string server_url;     /* e.g. rtmp://127.0.0.1:1935/live */
	std::string stream_key;     /* never log; mask in UI / get_settings */
	int video_bitrate_kbps = 0; /* 0 = use global settings */
	int audio_bitrate_kbps = 0; /* 0 = use global settings */
	/** Bitmask of OBS mixer tracks 1–6 (bit0=track1). Phase 5 matrix uses this. */
	uint32_t audio_track_mask = 1u;
};

struct MsrSettings {
	std::string output_folder;
	std::string filename_pattern = "{source}_{orientation}_{date}_{time}";
	std::string container = "mkv";      /* mkv | mp4 */
	std::string video_encoder = "auto"; /* auto | nvenc | amf | qsv | x264 */
	int video_bitrate_kbps = 8000;
	int audio_bitrate_kbps = 160;
	int vertical_width = 1080;
	int vertical_height = 1920;
	std::vector<MsrSourceConfig> sources;
	std::vector<MsrStreamDestination> destinations;
};

inline const char *orientation_token(MsrOrientation o)
{
	switch (o) {
	case MsrOrientation::Horizontal:
		return "horizontal";
	case MsrOrientation::Vertical:
		return "vertical";
	case MsrOrientation::Both:
		return "both";
	}
	return "horizontal";
}

inline MsrOrientation parse_orientation_token(const char *s, int as_int = 0)
{
	if (s && *s) {
		if (strcmp(s, "vertical") == 0 || strcmp(s, "Vertical") == 0)
			return MsrOrientation::Vertical;
		if (strcmp(s, "both") == 0 || strcmp(s, "Both") == 0)
			return MsrOrientation::Both;
		if (strcmp(s, "horizontal") == 0 || strcmp(s, "Horizontal") == 0)
			return MsrOrientation::Horizontal;
	}
	if (as_int == (int)MsrOrientation::Vertical)
		return MsrOrientation::Vertical;
	if (as_int == (int)MsrOrientation::Both)
		return MsrOrientation::Both;
	return MsrOrientation::Horizontal;
}

/** Mask a stream key for UI / API responses. Never log the raw key. */
inline std::string mask_stream_key(const std::string &key)
{
	if (key.empty())
		return {};
	if (key.size() <= 4)
		return "****";
	return std::string(key.size() - 4, '*') + key.substr(key.size() - 4);
}
