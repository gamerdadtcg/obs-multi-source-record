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

#include <obs.h>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

class MsrSession {
public:
	MsrSession(std::string source_name, MsrOrientation orientation, std::string extra_audio,
		   const MsrSettings &settings);
	~MsrSession();

	MsrSession(const MsrSession &) = delete;
	MsrSession &operator=(const MsrSession &) = delete;

	bool start();
	void stop();
	bool active() const { return active_.load(); }

	const std::string &source_name() const { return source_name_; }
	MsrOrientation orientation() const { return orientation_; }
	const std::string &output_path() const { return output_path_; }

private:
	bool setup_video();
	bool setup_audio();
	bool setup_encoders_and_output();
	void teardown();
	std::string build_output_path() const;
	const char *resolve_video_encoder_id() const;
	void activate_source(obs_source_t *source);
	void deactivate_sources();

	static bool audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts,
					 uint32_t mixers, struct audio_output_data *mixes);
	static void audio_capture_callback(void *param, obs_source_t *source, const struct audio_data *audio_data,
					   bool muted);

	void push_captured_audio(const struct audio_data *audio_data, bool muted);
	bool pop_captured_audio(struct audio_output_data *mixes, uint32_t mixers, size_t channels, uint64_t *out_ts);

	std::string source_name_;
	MsrOrientation orientation_;
	std::string extra_audio_name_;
	MsrSettings settings_;
	std::string output_path_;

	obs_weak_source_t *weak_source_ = nullptr;
	obs_weak_source_t *weak_extra_audio_ = nullptr;
	obs_source_t *activated_source_ = nullptr;
	obs_source_t *activated_extra_ = nullptr;
	bool capture_cb_source_ = false;
	bool capture_cb_extra_ = false;
	obs_view_t *view_ = nullptr;
	obs_source_t *private_scene_ = nullptr;
	video_t *video_output_ = nullptr;
	audio_t *audio_output_ = nullptr;
	bool owns_audio_output_ = false;
	obs_encoder_t *video_encoder_ = nullptr;
	obs_encoder_t *audio_encoder_ = nullptr;
	obs_output_t *file_output_ = nullptr;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	std::atomic<bool> active_{false};
	std::atomic<bool> closing_{false};

	std::mutex audio_mutex_;
	struct CapturedChunk {
		uint64_t timestamp = 0;
		size_t frames = 0;
		size_t channels = 0;
		std::vector<float> planar; /* frames * channels interleaved as ch0..chN per frame planar blocks */
	};
	std::vector<CapturedChunk> audio_queue_;
	static constexpr size_t kMaxQueuedChunks = 64;
};
