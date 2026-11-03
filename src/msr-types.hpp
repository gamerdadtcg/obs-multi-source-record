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

enum class MsrAutoStartTrigger {
	Off = 0,
	Record = 1,
	Stream = 2,
	Both = 3,
};

enum class MsrSplitMode {
	Off = 0,
	ByTime = 1,
	BySize = 2,
};

enum class MsrOrientation {
	Horizontal = 0,
	Vertical = 1,
	Both = 2,
};

struct MsrSourceConfig {
	std::string source_name;
	bool enabled = false;
	/** When true, this source gets a replay buffer (independent of recording enabled). Default off. */
	bool replay_enabled = false;
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
	/** Fade duration (ms) when MSR vertical canvas switches scenes. Horizontal OBS transition unchanged. */
	int vertical_transition_ms = 300;
	/** When not Off, MSR recording follows OBS main record and/or stream start/stop. Default Off. */
	MsrAutoStartTrigger auto_start = MsrAutoStartTrigger::Off;
	/** Legacy/master: true if any source has replay_enabled (kept for status/configure compat). */
	bool replay_enabled = false;
	/** Replay buffer length in seconds (shared by all selected sources). Default 30. */
	int replay_seconds = 30;
	/** File split for MSR recordings (ffmpeg_muxer). Default Off. */
	MsrSplitMode split_mode = MsrSplitMode::Off;
	int split_time_min = 30;  /* used when ByTime */
	int split_size_mb = 1024; /* used when BySize */
	std::vector<MsrSourceConfig> sources;
	std::vector<MsrStreamDestination> destinations;
	/** Filter display name used by Exeldro Source Record (or compatible) plugin. */
	std::string source_record_filter = "Source Record";
	/** Sources whose Source Record filter is toggled by Record All Sources.
	 *  Empty = auto-discover every source that already has source_record_filter. */
	std::vector<std::string> source_record_sources;
	/** Bring-your-own X OAuth Client ID (public for Native App; not a secret). */
	std::string x_live_client_id;
	/** Cached X Livestream source id (equals stream key — never log). */
	std::string x_live_source_id;
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

inline const char *auto_start_token(MsrAutoStartTrigger t)
{
	switch (t) {
	case MsrAutoStartTrigger::Record:
		return "record";
	case MsrAutoStartTrigger::Stream:
		return "stream";
	case MsrAutoStartTrigger::Both:
		return "both";
	case MsrAutoStartTrigger::Off:
	default:
		return "off";
	}
}

inline MsrAutoStartTrigger parse_auto_start_token(const char *s, int as_int = 0)
{
	if (s && *s) {
		if (strcmp(s, "record") == 0)
			return MsrAutoStartTrigger::Record;
		if (strcmp(s, "stream") == 0)
			return MsrAutoStartTrigger::Stream;
		if (strcmp(s, "both") == 0)
			return MsrAutoStartTrigger::Both;
		if (strcmp(s, "off") == 0)
			return MsrAutoStartTrigger::Off;
	}
	if (as_int == (int)MsrAutoStartTrigger::Record)
		return MsrAutoStartTrigger::Record;
	if (as_int == (int)MsrAutoStartTrigger::Stream)
		return MsrAutoStartTrigger::Stream;
	if (as_int == (int)MsrAutoStartTrigger::Both)
		return MsrAutoStartTrigger::Both;
	return MsrAutoStartTrigger::Off;
}

inline const char *split_mode_token(MsrSplitMode m)
{
	switch (m) {
	case MsrSplitMode::ByTime:
		return "time";
	case MsrSplitMode::BySize:
		return "size";
	case MsrSplitMode::Off:
	default:
		return "off";
	}
}

inline MsrSplitMode parse_split_mode_token(const char *s, int as_int = 0)
{
	if (s && *s) {
		if (strcmp(s, "time") == 0 || strcmp(s, "by_time") == 0)
			return MsrSplitMode::ByTime;
		if (strcmp(s, "size") == 0 || strcmp(s, "by_size") == 0)
			return MsrSplitMode::BySize;
		if (strcmp(s, "off") == 0)
			return MsrSplitMode::Off;
	}
	if (as_int == (int)MsrSplitMode::ByTime)
		return MsrSplitMode::ByTime;
	if (as_int == (int)MsrSplitMode::BySize)
		return MsrSplitMode::BySize;
	return MsrSplitMode::Off;
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
