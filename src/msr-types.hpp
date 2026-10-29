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
