/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-session.hpp"
#include "msr-vertical-canvas.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <callback/signal.h>

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

} // namespace

MsrSession::MsrSession(std::string source_name, MsrOrientation orientation, std::string extra_audio,
		       const MsrSettings &settings, bool use_vertical_canvas, MsrSessionKind kind)
	: source_name_(std::move(source_name)),
	  orientation_(orientation),
	  extra_audio_name_(std::move(extra_audio)),
	  settings_(settings),
	  use_vertical_canvas_(use_vertical_canvas),
	  kind_(kind)
{
}

MsrSession::~MsrSession()
{
	stop();
}

void MsrSession::activate_source(obs_source_t *source)
{
	if (!source)
		return;
	obs_source_inc_active(source);
	obs_source_inc_showing(source);
}

void MsrSession::deactivate_sources()
{
	if (capture_cb_source_ && activated_source_) {
		obs_source_remove_audio_capture_callback(activated_source_, audio_capture_callback, this);
		capture_cb_source_ = false;
	}
	if (capture_cb_extra_ && activated_extra_) {
		obs_source_remove_audio_capture_callback(activated_extra_, audio_capture_callback, this);
		capture_cb_extra_ = false;
	}
	if (activated_source_) {
		obs_source_dec_showing(activated_source_);
		obs_source_dec_active(activated_source_);
		obs_source_release(activated_source_);
		activated_source_ = nullptr;
	}
	if (activated_extra_) {
		obs_source_dec_showing(activated_extra_);
		obs_source_dec_active(activated_extra_);
		obs_source_release(activated_extra_);
		activated_extra_ = nullptr;
	}
}

void MsrSession::audio_capture_callback(void *param, obs_source_t *source, const struct audio_data *audio_data,
					bool muted)
{
	auto *self = static_cast<MsrSession *>(param);
	if (!self || self->closing_.load() || !audio_data)
		return;
	self->push_captured_audio(source, audio_data, muted);
}

void MsrSession::push_captured_audio(obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	if (muted || !audio_data->frames || paused_.load())
		return;

	std::lock_guard<std::mutex> lock(audio_mutex_);
	have_capture_audio_ = true;
	/* Extra must be a different source — same-pointer would mean we registered
	 * the capture callback twice and every buffer would be appended twice
	 * (half-speed / chunk-stutter; seen on Game Capture ISO). */
	if (activated_extra_ && source == activated_extra_ && activated_extra_ != activated_source_)
		fifo_extra_.push_planar(audio_data);
	else
		fifo_main_.push_planar(audio_data);
}

bool MsrSession::pop_captured_audio(struct audio_output_data *mixes, uint32_t mixers, size_t channels)
{
	std::lock_guard<std::mutex> lock(audio_mutex_);
	const size_t need = AUDIO_OUTPUT_FRAMES;
	bool any = false;
	/* Consume a full period from each FIFO that is ready. Do not block one
	 * FIFO on the other — that reintroduced silence holes when mic/game
	 * capture burst sizes differed. */
	if (fifo_main_.size >= need) {
		fifo_main_.pop_mixed_into(mixes, mixers, channels, false);
		any = true;
	}
	if (fifo_extra_.size >= need) {
		fifo_extra_.pop_mixed_into(mixes, mixers, channels, false);
		any = true;
	}
	return any;
}

bool MsrSession::audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts,
				      uint32_t mixers, struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(end_ts_in);
	auto *self = static_cast<MsrSession *>(param);
	/* Lock private audio_output to its own clock. Returning source capture
	 * timestamps caused ~263-sample silence holes every 1024 frames. */
	*out_ts = start_ts_in;
	if (!self || self->closing_.load())
		return true;

	const size_t channels = audio_output_get_channels(self->audio_output_);
	self->pop_captured_audio(mixes, mixers, channels);
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

bool MsrSession::setup_video_from_canvas()
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc) {
		obs_log(LOG_ERROR, "vertical canvas manager missing");
		return false;
	}
	if (!vc->ensure((uint32_t)settings_.vertical_width, (uint32_t)settings_.vertical_height)) {
		obs_log(LOG_ERROR, "failed to ensure vertical canvas");
		return false;
	}

	video_output_ = vc->video();
	if (!video_output_) {
		obs_log(LOG_ERROR, "vertical canvas has no video");
		return false;
	}
	width_ = vc->width();
	height_ = vc->height();

	obs_source_t *prog = vc->program_source();
	if (prog) {
		activate_source(prog);
		activated_source_ = prog;
		weak_source_ = obs_source_get_weak_source(prog);
	} else {
		obs_log(LOG_WARNING, "vertical canvas has no program source yet");
	}

	obs_log(LOG_INFO, "using vertical canvas video %ux%u", width_, height_);
	return true;
}

bool MsrSession::setup_video()
{
	obs_source_t *source = obs_get_source_by_name(source_name_.c_str());
	if (!source) {
		obs_log(LOG_ERROR, "source '%s' not found", source_name_.c_str());
		return false;
	}

	/* Activate before querying size — monitor/game capture report 0x0 while inactive. */
	activate_source(source);
	activated_source_ = source; /* takes ownership of the get_source ref */

	uint32_t src_w = 0, src_h = 0;
	for (int attempt = 0; attempt < 30; attempt++) {
		src_w = obs_source_get_width(source);
		src_h = obs_source_get_height(source);
		if (src_w > 0 && src_h > 0)
			break;
		os_sleep_ms(10);
	}

	if (src_w == 0 || src_h == 0) {
		obs_log(LOG_ERROR, "source '%s' has zero size after activate (is capture available?)",
			source_name_.c_str());
		deactivate_sources();
		return false;
	}
	src_w += (src_w & 1);
	src_h += (src_h & 1);

	weak_source_ = obs_source_get_weak_source(source);

	view_ = obs_view_create();
	if (!view_) {
		deactivate_sources();
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
			deactivate_sources();
			return false;
		}

		obs_scene_t *scene = obs_scene_from_source(private_scene_);
		obs_sceneitem_t *item = obs_scene_add(scene, source);

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

	if (!video_output_) {
		obs_log(LOG_ERROR, "failed to create video output for '%s'", source_name_.c_str());
		deactivate_sources();
		return false;
	}
	return true;
}

bool MsrSession::setup_audio()
{
	/* Vertical canvas record/replay: use the canvas private mix so per-source
	 * vertical audio mute applies. Main/horizontal audio is untouched. */
	if (use_vertical_canvas_) {
		auto *vc = MsrVerticalCanvas::instance();
		if (!vc) {
			obs_log(LOG_ERROR, "vertical canvas missing for audio");
			return false;
		}
		audio_output_ = vc->audio();
		if (!audio_output_) {
			obs_log(LOG_ERROR, "vertical audio mix unavailable");
			return false;
		}
		owns_audio_output_ = false;
		obs_log(LOG_INFO, "using vertical private audio mix for '%s'", source_name_.c_str());
		return true;
	}

	/* Ignore extra when blank or the same as the video source — selecting
	 * "Game Capture" as its own extra registered the callback twice and
	 * doubled every audio buffer in the FIFO. */
	if (!extra_audio_name_.empty() && extra_audio_name_ != source_name_) {
		obs_source_t *extra = obs_get_source_by_name(extra_audio_name_.c_str());
		if (extra && extra == activated_source_) {
			obs_log(LOG_WARNING, "extra audio '%s' is the same source; ignoring",
				extra_audio_name_.c_str());
			obs_source_release(extra);
		} else if (extra) {
			activate_source(extra);
			activated_extra_ = extra;
			weak_extra_audio_ = obs_source_get_weak_source(extra);
		} else {
			obs_log(LOG_WARNING, "extra audio source '%s' not found", extra_audio_name_.c_str());
		}
	} else if (!extra_audio_name_.empty() && extra_audio_name_ == source_name_) {
		obs_log(LOG_WARNING, "extra audio for '%s' points at itself; ignoring", source_name_.c_str());
	}

	if (activated_source_) {
		obs_source_add_audio_capture_callback(activated_source_, audio_capture_callback, this);
		capture_cb_source_ = true;
	}
	if (activated_extra_ && activated_extra_ != activated_source_) {
		obs_source_add_audio_capture_callback(activated_extra_, audio_capture_callback, this);
		capture_cb_extra_ = true;
	}

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

	const char *ext = (settings_.container == "mp4") ? "mp4" : "mkv";
	std::string out_name = "msr-out-" + sanitize_name(source_name_) + "-" + orientation_token(orientation_);
	obs_data_t *osettings = obs_data_create();

	if (kind_ == MsrSessionKind::Replay) {
		std::string dir = settings_.output_folder;
		if (dir.empty())
			dir = ".";
		std::vector<char> dir_buf(dir.begin(), dir.end());
		dir_buf.push_back('\0');
		os_mkdirs(dir_buf.data());
		std::string fmt = sanitize_name(source_name_) + "_" + orientation_token(orientation_) +
				  "_clip_%CCYY-%MM-%DD_%hh-%mm-%ss";
		int rb_sec = settings_.replay_seconds > 0 ? settings_.replay_seconds : 30;
		/* Cap RAM: allow ~2x bitrate-ish MB as soft max (0 = time-only). */
		int rb_mb = (settings_.video_bitrate_kbps * rb_sec) / (8 * 1024);
		if (rb_mb < 50)
			rb_mb = 50;
		if (rb_mb > 2048)
			rb_mb = 2048;
		obs_data_set_string(osettings, "directory", dir.c_str());
		obs_data_set_string(osettings, "format", fmt.c_str());
		obs_data_set_string(osettings, "extension", ext);
		obs_data_set_bool(osettings, "allow_spaces", false);
		obs_data_set_int(osettings, "max_time_sec", rb_sec);
		obs_data_set_int(osettings, "max_size_mb", rb_mb);
		out_name = "msr-replay-" + sanitize_name(source_name_) + "-" + orientation_token(orientation_);
		file_output_ = obs_output_create("replay_buffer", out_name.c_str(), osettings, nullptr);
		obs_data_release(osettings);
		if (!file_output_) {
			obs_log(LOG_ERROR, "failed to create replay_buffer output");
			return false;
		}
	} else {
		output_path_ = build_output_path();
		base_output_path_ = output_path_;
		split_parts_.clear();
		split_parts_.push_back(output_path_);
		part_start_sec_.clear();
		part_start_sec_.push_back(0.0);
		current_part_index_ = 0;
		std::vector<char> path_buf(output_path_.begin(), output_path_.end());
		path_buf.push_back('\0');
		ensure_directory(path_buf.data());

		obs_data_set_string(osettings, "path", output_path_.c_str());
		obs_data_set_string(osettings, "extension", ext);
		if (settings_.split_mode == MsrSplitMode::ByTime) {
			int sec = settings_.split_time_min > 0 ? settings_.split_time_min * 60 : 1800;
			obs_data_set_bool(osettings, "split_file", true);
			obs_data_set_int(osettings, "max_time_sec", sec);
			obs_data_set_int(osettings, "max_size_mb", 0);
		} else if (settings_.split_mode == MsrSplitMode::BySize) {
			int mb = settings_.split_size_mb > 0 ? settings_.split_size_mb : 1024;
			obs_data_set_bool(osettings, "split_file", true);
			obs_data_set_int(osettings, "max_size_mb", mb);
			obs_data_set_int(osettings, "max_time_sec", 0);
		} else {
			obs_data_set_bool(osettings, "split_file", false);
			obs_data_set_int(osettings, "max_time_sec", 0);
			obs_data_set_int(osettings, "max_size_mb", 0);
		}
		file_output_ = obs_output_create("ffmpeg_muxer", out_name.c_str(), osettings, nullptr);
		obs_data_release(osettings);
		if (!file_output_) {
			obs_log(LOG_ERROR, "failed to create file output");
			return false;
		}
		connect_file_changed_signal();
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
	paused_.store(false);
	{
		std::lock_guard<std::mutex> lock(audio_mutex_);
		fifo_main_.clear();
		fifo_extra_.clear();
		have_capture_audio_ = false;
	}

	const bool video_ok = use_vertical_canvas_ ? setup_video_from_canvas() : setup_video();
	if (!video_ok || !setup_audio() || !setup_encoders_and_output()) {
		teardown();
		return false;
	}

	if (!obs_output_start(file_output_)) {
		obs_log(LOG_ERROR, "obs_output_start failed for '%s' (%s)", source_name_.c_str(), output_path_.c_str());
		teardown();
		return false;
	}

	chapters_.clear();
	start_ns_ = os_gettime_ns();
	active_.store(true);
	if (kind_ == MsrSessionKind::Replay) {
		obs_log(LOG_INFO, "replay buffer '%s' (%s) %ds", source_name_.c_str(), orientation_token(orientation_),
			settings_.replay_seconds > 0 ? settings_.replay_seconds : 30);
	} else {
		obs_log(LOG_INFO, "recording '%s' (%s) -> %s%s", source_name_.c_str(), orientation_token(orientation_),
			output_path_.c_str(), settings_.split_mode != MsrSplitMode::Off ? " [split]" : "");
	}
	return true;
}

void MsrSession::stop()
{
	closing_.store(true);
	paused_.store(false);
	if (!active_.exchange(false) && !file_output_) {
		teardown();
		return;
	}

	if (kind_ == MsrSessionKind::Record && !chapters_.empty())
		write_chapters_sidecar();

	if (file_output_ && obs_output_active(file_output_))
		obs_output_stop(file_output_);

	start_ns_ = 0;
	teardown();
}

bool MsrSession::pause()
{
	if (!active_.load() || !file_output_)
		return false;
	if (paused_.load())
		return true;
	if (!obs_output_pause(file_output_, true)) {
		obs_log(LOG_WARNING, "obs_output_pause(true) failed for '%s'", source_name_.c_str());
		return false;
	}
	paused_.store(true);
	{
		std::lock_guard<std::mutex> lock(audio_mutex_);
		fifo_main_.clear();
		fifo_extra_.clear();
	}
	obs_log(LOG_INFO, "paused '%s' (%s)", source_name_.c_str(), orientation_token(orientation_));
	return true;
}

bool MsrSession::resume()
{
	if (!active_.load() || !file_output_)
		return false;
	if (!paused_.load())
		return true;
	if (!obs_output_pause(file_output_, false)) {
		obs_log(LOG_WARNING, "obs_output_pause(false) failed for '%s'", source_name_.c_str());
		return false;
	}
	paused_.store(false);
	obs_log(LOG_INFO, "resumed '%s' (%s)", source_name_.c_str(), orientation_token(orientation_));
	return true;
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
	} else {
		audio_output_ = nullptr; /* canvas-owned mix */
	}
	deactivate_sources();
	if (use_vertical_canvas_) {
		/* Canvas video is owned by the frontend canvas — do not remove it. */
		video_output_ = nullptr;
	} else if (view_) {
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
	{
		std::lock_guard<std::mutex> lock(audio_mutex_);
		fifo_main_.clear();
		fifo_extra_.clear();
		have_capture_audio_ = false;
	}
}

double MsrSession::media_time_sec() const
{
	if (!active_.load() || start_ns_ == 0)
		return 0.0;
	const uint64_t now = os_gettime_ns();
	uint64_t pause_off = 0;
	if (file_output_)
		pause_off = obs_output_get_pause_offset(file_output_);
	if (now <= start_ns_ + pause_off)
		return 0.0;
	return (double)(now - start_ns_ - pause_off) / 1000000000.0;
}

std::string MsrSession::chapters_path() const
{
	/* Always next to the first part so one sidecar covers the whole split set. */
	const std::string &src = !base_output_path_.empty() ? base_output_path_ : output_path_;
	if (src.empty())
		return {};
	std::string p = src;
	auto dot = p.find_last_of('.');
	if (dot != std::string::npos)
		p = p.substr(0, dot);
	return p + ".chapters.json";
}

bool MsrSession::try_native_add_chapter(const std::string &name)
{
	if (!file_output_)
		return false;
	proc_handler_t *ph = obs_output_get_proc_handler(file_output_);
	if (!ph)
		return false;
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "chapter_name", name.c_str());
	const bool ok = proc_handler_call(ph, "add_chapter", &cd);
	calldata_free(&cd);
	return ok;
}

void MsrSession::write_chapters_sidecar() const
{
	const std::string path = chapters_path();
	if (path.empty())
		return;

	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "output", output_path_.c_str());
	obs_data_set_string(root, "base_output", base_output_path_.c_str());
	obs_data_set_string(root, "source", source_name_.c_str());
	obs_data_set_string(root, "orientation", orientation_token(orientation_));
	obs_data_set_string(root, "split_mode", split_mode_token(settings_.split_mode));
	obs_data_set_int(root, "current_part", current_part_index_);
	{
		obs_data_array_t *parts = obs_data_array_create();
		for (size_t i = 0; i < split_parts_.size(); i++) {
			obs_data_t *pitem = obs_data_create();
			obs_data_set_string(pitem, "path", split_parts_[i].c_str());
			obs_data_set_int(pitem, "index", (int)i);
			if (i < part_start_sec_.size())
				obs_data_set_double(pitem, "start_sec", part_start_sec_[i]);
			obs_data_array_push_back(parts, pitem);
			obs_data_release(pitem);
		}
		obs_data_set_array(root, "parts", parts);
		obs_data_array_release(parts);
	}
	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &ch : chapters_) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", ch.name.c_str());
		obs_data_set_double(item, "time_sec", ch.time_sec);
		obs_data_set_int(item, "part_index", ch.part_index);
		obs_data_set_double(item, "time_in_part_sec", ch.time_in_part_sec);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "chapters", arr);
	obs_data_array_release(arr);

	if (!obs_data_save_json_pretty_safe(root, path.c_str(), "tmp", "bak")) {
		obs_log(LOG_WARNING, "failed to write chapters sidecar %s", path.c_str());
	}
	obs_data_release(root);
}

bool MsrSession::add_chapter(const std::string &name)
{
	if (kind_ != MsrSessionKind::Record || !active_.load() || paused_.load())
		return false;

	std::string label = name;
	if (label.empty())
		label = "Chapter " + std::to_string(chapters_.size() + 1);

	ChapterMark mark;
	mark.name = label;
	mark.time_sec = media_time_sec();
	mark.part_index = current_part_index_;
	const double part_start = (current_part_index_ >= 0 && (size_t)current_part_index_ < part_start_sec_.size())
					  ? part_start_sec_[(size_t)current_part_index_]
					  : 0.0;
	mark.time_in_part_sec = mark.time_sec - part_start;
	if (mark.time_in_part_sec < 0.0)
		mark.time_in_part_sec = 0.0;
	chapters_.push_back(mark);

	const bool native = try_native_add_chapter(label);
	write_chapters_sidecar();
	obs_log(LOG_INFO, "chapter '%s' at %.3fs on '%s' (%s)%s", label.c_str(), mark.time_sec, source_name_.c_str(),
		orientation_token(orientation_), native ? " [native]" : " [sidecar]");
	return true;
}

void MsrSession::connect_file_changed_signal()
{
	if (!file_output_)
		return;
	signal_handler_t *sh = obs_output_get_signal_handler(file_output_);
	if (!sh)
		return;
	signal_handler_connect(sh, "file_changed", file_changed_signal, this);
}

void MsrSession::file_changed_signal(void *data, calldata_t *cd)
{
	auto *self = static_cast<MsrSession *>(data);
	if (!self || !cd)
		return;
	const char *next = calldata_string(cd, "next_file");
	self->on_file_split(next);
}

void MsrSession::on_file_split(const char *next_file)
{
	if (!next_file || !*next_file)
		return;
	const double t = media_time_sec();
	output_path_ = next_file;
	split_parts_.push_back(output_path_);
	part_start_sec_.push_back(t);
	current_part_index_ = (int)split_parts_.size() - 1;
	obs_log(LOG_INFO, "split '%s' (%s) -> part %d %s @ %.3fs", source_name_.c_str(),
		orientation_token(orientation_), current_part_index_, output_path_.c_str(), t);
	if (!chapters_.empty())
		write_chapters_sidecar();
}

bool MsrSession::save_replay()
{
	if (kind_ != MsrSessionKind::Replay || !active_.load() || !file_output_)
		return false;
	proc_handler_t *ph = obs_output_get_proc_handler(file_output_);
	if (!ph)
		return false;
	calldata_t cd;
	calldata_init(&cd);
	const bool ok = proc_handler_call(ph, "save", &cd);
	calldata_free(&cd);
	if (ok)
		obs_log(LOG_INFO, "save replay requested for '%s' (%s)", source_name_.c_str(),
			orientation_token(orientation_));
	else
		obs_log(LOG_WARNING, "save replay failed for '%s' (%s)", source_name_.c_str(),
			orientation_token(orientation_));
	return ok;
}
