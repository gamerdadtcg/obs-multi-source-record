/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-stream-session.hpp"
#include "msr-vertical-canvas.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <cstring>

namespace {

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
		out = "dest";
	return out;
}

} // namespace

MsrStreamSession::MsrStreamSession(MsrStreamDestination dest, const MsrSettings &settings)
	: dest_(std::move(dest)),
	  settings_(settings)
{
	if (dest_.orientation == MsrOrientation::Both)
		dest_.orientation = MsrOrientation::Horizontal;
}

MsrStreamSession::~MsrStreamSession()
{
	stop();
}

size_t MsrStreamSession::first_mixer_index(uint32_t mask)
{
	if (mask == 0)
		return 0;
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (mask & (1u << i))
			return i;
	}
	return 0;
}

const char *MsrStreamSession::resolve_video_encoder_id() const
{
	/* Multistream always uses software x264 for reliable RTMP start across GPUs.
	 * Recording still honors the dock encoder setting separately. */
	(void)settings_;
	return "obs_x264";
}

bool MsrStreamSession::setup_video()
{
	if (dest_.orientation == MsrOrientation::Vertical) {
		auto *vc = MsrVerticalCanvas::instance();
		if (!vc) {
			obs_log(LOG_ERROR, "vertical canvas manager missing (stream '%s')", dest_.name.c_str());
			return false;
		}
		if (!vc->ensure((uint32_t)settings_.vertical_width, (uint32_t)settings_.vertical_height)) {
			obs_log(LOG_ERROR, "failed to ensure vertical canvas for stream '%s'", dest_.name.c_str());
			return false;
		}
		video_output_ = vc->video();
		if (!video_output_) {
			obs_log(LOG_ERROR, "vertical canvas has no video for stream '%s'", dest_.name.c_str());
			return false;
		}
		width_ = vc->width();
		height_ = vc->height();

		obs_source_t *prog = vc->program_source();
		if (prog) {
			obs_source_inc_active(prog);
			obs_source_inc_showing(prog);
			activated_program_ = prog;
		}
	} else {
		video_output_ = obs_get_video();
		if (!video_output_) {
			obs_log(LOG_ERROR, "main video missing for stream '%s'", dest_.name.c_str());
			return false;
		}
		struct obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		width_ = ovi.output_width;
		height_ = ovi.output_height;
	}

	/* Destinations always take OBS main mixer tracks (Advanced Audio Properties
	 * tracks 1–6 via obs_get_audio()). Private vertical mix stays for vertical
	 * recording only — not multistream. */
	audio_output_ = obs_get_audio();
	if (!audio_output_) {
		obs_log(LOG_ERROR, "main audio missing for stream '%s'", dest_.name.c_str());
		return false;
	}
	obs_log(LOG_INFO, "stream '%s' using OBS main audio mixer tracks", dest_.name.c_str());
	return true;
}

bool MsrStreamSession::setup_encoders_and_output()
{
	const int vbr = dest_.video_bitrate_kbps > 0 ? dest_.video_bitrate_kbps : settings_.video_bitrate_kbps;
	const int abr = dest_.audio_bitrate_kbps > 0 ? dest_.audio_bitrate_kbps : settings_.audio_bitrate_kbps;
	const char *venc_id = resolve_video_encoder_id();

	obs_data_t *vsettings = obs_data_create();
	obs_data_set_int(vsettings, "bitrate", vbr);
	obs_data_set_string(vsettings, "rate_control", "CBR");
	/* X Live Studio: H.264 Main/High, keyint ~3s. Repeat SPS/PPS so late
	 * preview joiners (source tile) can decode instead of staying on logo. */
	const bool x_host = dest_.server_url.find("live.x.com") != std::string::npos ||
			  dest_.server_url.find("live.twitter.com") != std::string::npos;
	obs_data_set_int(vsettings, "keyint_sec", x_host ? 3 : 2);
	obs_data_set_string(vsettings, "preset", "veryfast");
	obs_data_set_string(vsettings, "tune", "zerolatency");
	obs_data_set_string(vsettings, "profile", "high");
	obs_data_set_int(vsettings, "bf", 0);
	obs_data_set_string(vsettings, "x264opts", "repeat-headers=1:aud=1:nal-hrd=cbr:force-cfr=1");
	obs_data_set_string(vsettings, "preset2", "p5");
	if (x_host && dest_.orientation == MsrOrientation::Vertical) {
		obs_log(LOG_WARNING,
			"stream '%s': X Live Studio recommends 1920x1080 16:9; vertical "
			"%dx%d may show the X logo in the source preview even while "
			"frames are flowing — check Encoder bitrate meters / private test",
			dest_.name.c_str(), width_, height_);
	}

	const std::string tag = sanitize_name(dest_.name) + "-" + orientation_token(dest_.orientation);
	std::string enc_name = "msr-stream-venc-" + tag;
	video_encoder_ = obs_video_encoder_create(venc_id, enc_name.c_str(), vsettings, nullptr);
	if (!video_encoder_ && strcmp(venc_id, "obs_x264") != 0) {
		obs_log(LOG_WARNING, "stream encoder %s unavailable; falling back to x264", venc_id);
		venc_id = "obs_x264";
		video_encoder_ = obs_video_encoder_create(venc_id, enc_name.c_str(), vsettings, nullptr);
	}
	obs_data_release(vsettings);
	if (!video_encoder_) {
		obs_log(LOG_ERROR, "failed to create stream video encoder %s for '%s'", venc_id, dest_.name.c_str());
		return false;
	}
	obs_encoder_set_video(video_encoder_, video_output_);
	obs_encoder_set_scaled_size(video_encoder_, width_, height_);

	/* RTMP is single-audio-track: use the first checked OBS main mixer track. */
	uint32_t mask = dest_.audio_track_mask ? dest_.audio_track_mask : 1u;
	const size_t mix = first_mixer_index(mask);
	obs_data_t *asettings = obs_data_create();
	obs_data_set_int(asettings, "bitrate", abr);
	std::string aenc_name = "msr-stream-aenc-" + tag + "-t" + std::to_string(mix + 1);
	obs_encoder_t *aenc =
		obs_audio_encoder_create("ffmpeg_aac", aenc_name.c_str(), asettings, mix, nullptr);
	obs_data_release(asettings);
	if (!aenc) {
		obs_log(LOG_ERROR, "failed to create stream audio encoder track %zu for '%s'", mix + 1,
			dest_.name.c_str());
		return false;
	}
	obs_encoder_set_audio(aenc, audio_output_);
	audio_encoders_.push_back(aenc);

	/* rtmp_output has OBS_OUTPUT_SERVICE — server/key must live on an attached
	 * rtmp_custom service. Settings alone make obs_output_start fail immediately
	 * with no RTMP connect attempt. */
	obs_data_t *ssettings = obs_data_create();
	obs_data_set_string(ssettings, "server", dest_.server_url.c_str());
	obs_data_set_string(ssettings, "key", dest_.stream_key.c_str());
	obs_data_set_bool(ssettings, "use_auth", false);
	std::string svc_name = "msr-stream-svc-" + tag;
	service_ = obs_service_create("rtmp_custom", svc_name.c_str(), ssettings, nullptr);
	obs_data_release(ssettings);
	if (!service_) {
		obs_log(LOG_ERROR, "failed to create rtmp_custom service for '%s'", dest_.name.c_str());
		return false;
	}

	std::string out_name = "msr-stream-out-" + tag;
	rtmp_output_ = obs_output_create("rtmp_output", out_name.c_str(), nullptr, nullptr);
	if (!rtmp_output_) {
		obs_log(LOG_ERROR, "failed to create rtmp_output for '%s'", dest_.name.c_str());
		return false;
	}

	obs_output_set_service(rtmp_output_, service_);
	obs_output_set_video_encoder(rtmp_output_, video_encoder_);
	for (size_t i = 0; i < audio_encoders_.size(); i++)
		obs_output_set_audio_encoder(rtmp_output_, audio_encoders_[i], i);
	obs_log(LOG_INFO, "stream '%s' OBS audio track %zu (mask=0x%x)", dest_.name.c_str(), mix + 1,
		(unsigned)mask);
	return true;
}

uint64_t MsrStreamSession::total_frames() const
{
	if (!rtmp_output_)
		return 0;
	return (uint64_t)obs_output_get_total_frames(rtmp_output_);
}

void MsrStreamSession::clear_audio_encoders()
{
	for (obs_encoder_t *aenc : audio_encoders_) {
		if (aenc)
			obs_encoder_release(aenc);
	}
	audio_encoders_.clear();
}

bool MsrStreamSession::start()
{
	if (active_.load())
		return true;
	if (dest_.server_url.empty()) {
		obs_log(LOG_ERROR, "stream '%s' has empty server URL", dest_.name.c_str());
		return false;
	}

	if (!setup_video() || !setup_encoders_and_output()) {
		teardown();
		return false;
	}

	if (!obs_output_start(rtmp_output_)) {
		const char *err = obs_output_get_last_error(rtmp_output_);
		/* Never log the stream key — only server + dest name + last_error. */
		obs_log(LOG_ERROR, "obs_output_start failed for stream '%s' -> %s (key masked)%s%s", dest_.name.c_str(),
			dest_.server_url.c_str(), err && *err ? ": " : "", err ? err : "");
		teardown();
		return false;
	}

	active_.store(true);
	obs_log(LOG_INFO, "streaming '%s' (%s) -> %s (key masked)", dest_.name.c_str(),
		orientation_token(dest_.orientation), dest_.server_url.c_str());
	return true;
}

void MsrStreamSession::stop()
{
	if (!active_.exchange(false) && !rtmp_output_) {
		teardown();
		return;
	}
	if (rtmp_output_ && obs_output_active(rtmp_output_))
		obs_output_stop(rtmp_output_);
	teardown();
}

void MsrStreamSession::teardown()
{
	if (rtmp_output_) {
		if (obs_output_active(rtmp_output_))
			obs_output_force_stop(rtmp_output_);
		obs_output_set_service(rtmp_output_, nullptr);
		obs_output_release(rtmp_output_);
		rtmp_output_ = nullptr;
	}
	if (service_) {
		obs_service_release(service_);
		service_ = nullptr;
	}
	clear_audio_encoders();
	if (video_encoder_) {
		obs_encoder_release(video_encoder_);
		video_encoder_ = nullptr;
	}
	video_output_ = nullptr;
	audio_output_ = nullptr;
	if (activated_program_) {
		obs_source_dec_showing(activated_program_);
		obs_source_dec_active(activated_program_);
		obs_source_release(activated_program_);
		activated_program_ = nullptr;
	}
	active_.store(false);
}
