/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-session.hpp"
#include "plugin-support.h"

#include <util/platform.h>
#include <graphics/vec2.h>

#include <ctime>
#include <cstring>
#include <cmath>
#include <vector>
#include <initializer_list>

namespace {

bool encoder_available(const char *id)
{
	const char *val = nullptr;
	int i = 0;
	while (obs_enum_encoder_types(i++, &val)) {
		if (strcmp(val, id) == 0)
			return true;
	}
	return false;
}

std::string sanitize_name(const std::string &name)
{
	std::string out;
	out.reserve(name.size());
	for (char c : name) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
		    c == '_') {
			out.push_back(c);
		} else if (c == ' ') {
			out.push_back('_');
		}
	}
	if (out.empty())
		out = "source";
	return out;
}

void ensure_directory(char *path)
{
#ifdef _WIN32
	char *backslash = strrchr(path, '\\');
	if (backslash)
		*backslash = '/';
#endif
	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(path);
		*slash = '/';
	}
#ifdef _WIN32
	if (backslash)
		*backslash = '\\';
#endif
}

void mix_source_audio(obs_source_t *source, struct audio_output_data *mixes, uint32_t mixers, size_t channels)
{
	if (!source || obs_source_removed(source))
		return;
	if (obs_source_audio_pending(source))
		return;

	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_AUDIO) == 0 && (flags & OBS_SOURCE_COMPOSITE) == 0)
		return;

	struct obs_source_audio_mix audio;
	obs_source_get_audio_mix(source, &audio);

	for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
		if ((mixers & (1u << mix_idx)) == 0)
			continue;
		for (size_t ch = 0; ch < channels; ch++) {
			float *out = mixes[mix_idx].data[ch];
			float *in = audio.output[0].data[ch];
			if (!in || !out)
				continue;
			for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
				out[i] += in[i];
				if (out[i] > 1.0f)
					out[i] = 1.0f;
				if (out[i] < -1.0f)
					out[i] = -1.0f;
			}
		}
	}
}

} // namespace

MsrSession::MsrSession(std::string source_name, MsrOrientation orientation, std::string extra_audio,
		       const MsrSettings &settings)
	: source_name_(std::move(source_name)),
	  orientation_(orientation),
	  extra_audio_name_(std::move(extra_audio)),
	  settings_(settings)
{
}

MsrSession::~MsrSession()
{
	stop();
}

bool MsrSession::audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts,
				      uint32_t mixers, struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(end_ts_in);
	auto *self = static_cast<MsrSession *>(param);
	if (!self || self->closing_.load()) {
		*out_ts = start_ts_in;
		return true;
	}

	obs_source_t *source = obs_weak_source_get_source(self->weak_source_);
	obs_source_t *extra = obs_weak_source_get_source(self->weak_extra_audio_);

	uint64_t ts = 0;
	if (source) {
		ts = obs_source_get_audio_timestamp(source);
		if (!ts && extra)
			ts = obs_source_get_audio_timestamp(extra);
	} else if (extra) {
		ts = obs_source_get_audio_timestamp(extra);
	}

	if (!ts) {
		*out_ts = start_ts_in;
		obs_source_release(source);
		obs_source_release(extra);
		return true;
	}

	if ((source && obs_source_audio_pending(source)) || (extra && obs_source_audio_pending(extra))) {
		obs_source_release(source);
		obs_source_release(extra);
		return false;
	}

	const size_t channels = audio_output_get_channels(self->audio_output_);
	mix_source_audio(source, mixes, mixers, channels);
	mix_source_audio(extra, mixes, mixers, channels);

	*out_ts = ts;
	obs_source_release(source);
	obs_source_release(extra);
	return true;
}

std::string MsrSession::build_output_path() const
{
	std::time_t now = std::time(nullptr);
	std::tm tm_buf{};
#ifdef _WIN32
	localtime_s(&tm_buf, &now);
#else
	localtime_r(&now, &tm_buf);
#endif
	char date_buf[32];
	char time_buf[32];
	std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_buf);
	std::strftime(time_buf, sizeof(time_buf), "%H-%M-%S", &tm_buf);

	std::string pattern = settings_.filename_pattern;
	const std::string src = sanitize_name(source_name_);
	const std::string ori = orientation_token(orientation_);

	auto replace_all = [](std::string &s, const std::string &from, const std::string &to) {
		size_t pos = 0;
		while ((pos = s.find(from, pos)) != std::string::npos) {
			s.replace(pos, from.size(), to);
			pos += to.size();
		}
	};
	replace_all(pattern, "{source}", src);
	replace_all(pattern, "{orientation}", ori);
	replace_all(pattern, "{date}", date_buf);
	replace_all(pattern, "{time}", time_buf);

	const char *ext = (settings_.container == "mp4") ? "mp4" : "mkv";
	std::string folder = settings_.output_folder;
	if (folder.empty()) {
#ifdef _WIN32
		folder = "C:/Users/Public/Videos";
#else
		folder = "/tmp";
#endif
	}
	while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\'))
		folder.pop_back();

	return folder + "/" + pattern + "." + ext;
}

const char *MsrSession::resolve_video_encoder_id() const
{
	const std::string &pref = settings_.video_encoder;
	auto try_ids = [](std::initializer_list<const char *> ids) -> const char * {
		for (const char *id : ids) {
			if (encoder_available(id))
				return id;
		}
		return nullptr;
	};

	if (pref == "nvenc") {
		const char *id = try_ids({"obs_nvenc_h264_tex", "jim_nvenc", "ffmpeg_nvenc"});
		if (id)
			return id;
	} else if (pref == "amf") {
		const char *id = try_ids({"h264_texture_amf"});
		if (id)
			return id;
	} else if (pref == "qsv") {
		const char *id = try_ids({"obs_qsv11_v2", "obs_qsv11"});
		if (id)
			return id;
	} else if (pref == "x264") {
		return "obs_x264";
	}

	const char *id = try_ids({"obs_nvenc_h264_tex", "jim_nvenc", "ffmpeg_nvenc", "h264_texture_amf", "obs_qsv11_v2",
				  "obs_qsv11", "obs_x264"});
	return id ? id : "obs_x264";
}

bool MsrSession::setup_video()
{
	obs_source_t *source = obs_get_source_by_name(source_name_.c_str());
	if (!source) {
		obs_log(LOG_ERROR, "source '%s' not found", source_name_.c_str());
		return false;
	}
	weak_source_ = obs_source_get_weak_source(source);

	uint32_t src_w = obs_source_get_width(source);
	uint32_t src_h = obs_source_get_height(source);
	if (src_w == 0 || src_h == 0) {
		obs_log(LOG_ERROR, "source '%s' has zero size", source_name_.c_str());
		obs_source_release(source);
		return false;
	}
	src_w += (src_w & 1);
	src_h += (src_h & 1);

	view_ = obs_view_create();
	if (!view_) {
		obs_source_release(source);
		return false;
	}

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);

	if (orientation_ == MsrOrientation::Vertical) {
		width_ = (uint32_t)settings_.vertical_width;
		height_ = (uint32_t)settings_.vertical_height;
		width_ += (width_ & 1);
		height_ += (height_ & 1);

		obs_data_t *scene_settings = obs_data_create();
		private_scene_ = obs_source_create_private("scene", "msr-vertical-scene", scene_settings);
		obs_data_release(scene_settings);
		if (!private_scene_) {
			obs_source_release(source);
			return false;
		}

		obs_scene_t *scene = obs_scene_from_source(private_scene_);
		obs_sceneitem_t *item = obs_scene_add(scene, source);

		/* Center-crop source to 9:16 then scale to vertical canvas. */
		const double target_aspect = (double)width_ / (double)height_;
		const double src_aspect = (double)src_w / (double)src_h;
		uint32_t crop_l = 0, crop_t = 0, crop_r = 0, crop_b = 0;
		if (src_aspect > target_aspect) {
			uint32_t new_w = (uint32_t)std::lround((double)src_h * target_aspect);
			if (new_w > src_w)
				new_w = src_w;
			crop_l = (src_w - new_w) / 2;
			crop_r = src_w - new_w - crop_l;
		} else {
			uint32_t new_h = (uint32_t)std::lround((double)src_w / target_aspect);
			if (new_h > src_h)
				new_h = src_h;
			crop_t = (src_h - new_h) / 2;
			crop_b = src_h - new_h - crop_t;
		}

		struct obs_sceneitem_crop crop = {};
		crop.left = (int)crop_l;
		crop.top = (int)crop_t;
		crop.right = (int)crop_r;
		crop.bottom = (int)crop_b;
		obs_sceneitem_set_crop(item, &crop);

		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		vec2 bounds;
		vec2_set(&bounds, (float)width_, (float)height_);
		obs_sceneitem_set_bounds(item, &bounds);
		vec2 pos;
		vec2_set(&pos, 0.0f, 0.0f);
		obs_sceneitem_set_pos(item, &pos);

		obs_view_set_source(view_, 0, private_scene_);
	} else {
		width_ = src_w;
		height_ = src_h;
		obs_view_set_source(view_, 0, source);
	}

	ovi.base_width = width_;
	ovi.base_height = height_;
	ovi.output_width = width_;
	ovi.output_height = height_;
	video_output_ = obs_view_add2(view_, &ovi);
	obs_source_release(source);

	if (!video_output_) {
		obs_log(LOG_ERROR, "failed to create video output for '%s'", source_name_.c_str());
		return false;
	}
	return true;
}

bool MsrSession::setup_audio()
{
	if (!extra_audio_name_.empty()) {
		obs_source_t *extra = obs_get_source_by_name(extra_audio_name_.c_str());
		if (extra) {
			weak_extra_audio_ = obs_source_get_weak_source(extra);
			obs_source_release(extra);
		}
	}

	/* Capture audio DIRECTLY from the source via a private audio_output.
	 * This avoids silent files when the source is only on mixer tracks 2/3/4. */
	struct audio_output_info oi = {};
	oi.name = source_name_.c_str();
	oi.speakers = SPEAKERS_STEREO;
	oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
	oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
	oi.input_param = this;
	oi.input_callback = audio_input_callback;

	if (audio_output_open(&audio_output_, &oi) != 0) {
		obs_log(LOG_ERROR, "failed to open private audio output for '%s'", source_name_.c_str());
		return false;
	}
	owns_audio_output_ = true;
	return true;
}

bool MsrSession::setup_encoders_and_output()
{
	const char *venc_id = resolve_video_encoder_id();
	obs_data_t *vsettings = obs_data_create();
	obs_data_set_int(vsettings, "bitrate", settings_.video_bitrate_kbps);
	obs_data_set_string(vsettings, "rate_control", "CBR");
	obs_data_set_int(vsettings, "keyint_sec", 2);
	obs_data_set_string(vsettings, "preset", "quality");
	obs_data_set_string(vsettings, "preset2", "p5");

	std::string enc_name = "msr-venc-" + sanitize_name(source_name_) + "-" + orientation_token(orientation_);
	video_encoder_ = obs_video_encoder_create(venc_id, enc_name.c_str(), vsettings, nullptr);
	obs_data_release(vsettings);
	if (!video_encoder_) {
		obs_log(LOG_ERROR, "failed to create video encoder %s", venc_id);
		return false;
	}
	obs_encoder_set_video(video_encoder_, video_output_);
	obs_encoder_set_scaled_size(video_encoder_, width_, height_);

	obs_data_t *asettings = obs_data_create();
	obs_data_set_int(asettings, "bitrate", settings_.audio_bitrate_kbps);
	std::string aenc_name = "msr-aenc-" + sanitize_name(source_name_) + "-" + orientation_token(orientation_);
	audio_encoder_ = obs_audio_encoder_create("ffmpeg_aac", aenc_name.c_str(), asettings, 0, nullptr);
	obs_data_release(asettings);
	if (!audio_encoder_) {
		obs_log(LOG_ERROR, "failed to create audio encoder");
		return false;
	}
	obs_encoder_set_audio(audio_encoder_, audio_output_);

	output_path_ = build_output_path();
	std::vector<char> path_buf(output_path_.begin(), output_path_.end());
	path_buf.push_back('\0');
	ensure_directory(path_buf.data());

	obs_data_t *osettings = obs_data_create();
	obs_data_set_string(osettings, "path", output_path_.c_str());
	const char *ext = (settings_.container == "mp4") ? "mp4" : "mkv";
	obs_data_set_string(osettings, "extension", ext);

	std::string out_name = "msr-out-" + sanitize_name(source_name_) + "-" + orientation_token(orientation_);
	file_output_ = obs_output_create("ffmpeg_muxer", out_name.c_str(), osettings, nullptr);
	obs_data_release(osettings);
	if (!file_output_) {
		obs_log(LOG_ERROR, "failed to create file output");
		return false;
	}

	obs_output_set_video_encoder(file_output_, video_encoder_);
	obs_output_set_audio_encoder(file_output_, audio_encoder_, 0);
	return true;
}

bool MsrSession::start()
{
	if (active_.load())
		return true;

	closing_.store(false);
	if (!setup_video() || !setup_audio() || !setup_encoders_and_output()) {
		teardown();
		return false;
	}

	obs_source_t *source = obs_weak_source_get_source(weak_source_);
	if (source) {
		obs_source_inc_showing(source);
		obs_source_release(source);
	}

	if (!obs_output_start(file_output_)) {
		obs_log(LOG_ERROR, "obs_output_start failed for '%s' (%s)", source_name_.c_str(), output_path_.c_str());
		obs_source_t *s2 = obs_weak_source_get_source(weak_source_);
		if (s2) {
			obs_source_dec_showing(s2);
			obs_source_release(s2);
		}
		teardown();
		return false;
	}

	active_.store(true);
	obs_log(LOG_INFO, "recording '%s' (%s) -> %s", source_name_.c_str(), orientation_token(orientation_),
		output_path_.c_str());
	return true;
}

void MsrSession::stop()
{
	closing_.store(true);
	if (!active_.exchange(false) && !file_output_) {
		teardown();
		return;
	}

	if (file_output_ && obs_output_active(file_output_))
		obs_output_stop(file_output_);

	obs_source_t *source = obs_weak_source_get_source(weak_source_);
	if (source) {
		obs_source_dec_showing(source);
		obs_source_release(source);
	}

	teardown();
}

void MsrSession::teardown()
{
	if (file_output_) {
		if (obs_output_active(file_output_))
			obs_output_force_stop(file_output_);
		obs_output_release(file_output_);
		file_output_ = nullptr;
	}
	if (video_encoder_) {
		obs_encoder_release(video_encoder_);
		video_encoder_ = nullptr;
	}
	if (audio_encoder_) {
		obs_encoder_release(audio_encoder_);
		audio_encoder_ = nullptr;
	}
	if (owns_audio_output_ && audio_output_) {
		audio_output_close(audio_output_);
		audio_output_ = nullptr;
		owns_audio_output_ = false;
	}
	if (view_) {
		if (video_output_) {
			obs_view_remove(view_);
			video_output_ = nullptr;
		}
		obs_view_set_source(view_, 0, nullptr);
		obs_view_destroy(view_);
		view_ = nullptr;
	}
	if (private_scene_) {
		obs_source_release(private_scene_);
		private_scene_ = nullptr;
	}
	if (weak_extra_audio_) {
		obs_weak_source_release(weak_extra_audio_);
		weak_extra_audio_ = nullptr;
	}
	if (weak_source_) {
		obs_weak_source_release(weak_source_);
		weak_source_ = nullptr;
	}
}
