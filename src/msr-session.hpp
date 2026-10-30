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

enum class MsrSessionKind {
	Record = 0,
	Replay = 1,
};

class MsrSession {
public:
	MsrSession(std::string source_name, MsrOrientation orientation, std::string extra_audio,
		   const MsrSettings &settings, bool use_vertical_canvas = false,
		   MsrSessionKind kind = MsrSessionKind::Record);
	~MsrSession();

	MsrSession(const MsrSession &) = delete;
	MsrSession &operator=(const MsrSession &) = delete;

	bool start();
	void stop();
	bool active() const { return active_.load(); }
	bool pause();
	bool resume();
	bool paused() const { return paused_.load(); }
	bool add_chapter(const std::string &name);
	bool save_replay();

	const std::string &source_name() const { return source_name_; }
	MsrOrientation orientation() const { return orientation_; }
	MsrSessionKind kind() const { return kind_; }
	const std::string &output_path() const { return output_path_; }
	std::string chapters_path() const;

private:
	struct SampleFifo {
		static constexpr size_t kChannels = 8;
		static constexpr size_t kCapacity = 48000; /* 1s @ 48k */
		std::vector<float> data[kChannels];
		size_t channels = 0;
		size_t read_pos = 0;
		size_t write_pos = 0;
		size_t size = 0;

		void clear()
		{
			for (size_t ch = 0; ch < kChannels; ch++)
				data[ch].clear();
			channels = read_pos = write_pos = size = 0;
		}

		void ensure()
		{
			if (!data[0].empty())
				return;
			for (size_t ch = 0; ch < kChannels; ch++)
				data[ch].assign(kCapacity, 0.0f);
		}

		void push_planar(const struct audio_data *audio_data)
		{
			size_t ch_count = 0;
			for (size_t ch = 0; ch < MAX_AV_PLANES; ch++) {
				if (audio_data->data[ch])
					ch_count = ch + 1;
			}
			if (ch_count == 0 || !audio_data->frames)
				return;
			ensure();
			/* Always store stereo. Downmix mono / 2.1 / 5.1 / 7.1 so
			 * application-audio capture (often >2ch) is not left with
			 * silent surround planes or a mono-in-left-only image. */
			channels = 2;

			for (uint32_t i = 0; i < audio_data->frames; i++) {
				if (size >= kCapacity) {
					read_pos = (read_pos + 1) % kCapacity;
					size--;
				}
				auto sample = [&](size_t ch) -> float {
					if (ch >= ch_count)
						return 0.0f;
					const float *in = (const float *)audio_data->data[ch];
					return in ? in[i] : 0.0f;
				};
				float l = 0.0f, r = 0.0f;
				if (ch_count == 1) {
					l = r = sample(0);
				} else if (ch_count == 2) {
					l = sample(0);
					r = sample(1);
				} else {
					/* Treat as surround: FL FR FC LFE ... */
					l = sample(0);
					r = sample(1);
					const float fc = sample(2) * 0.707f;
					l += fc;
					r += fc;
					if (ch_count >= 6) {
						l += sample(4) * 0.707f; /* BL/SL */
						r += sample(5) * 0.707f;
					}
					if (ch_count >= 8) {
						l += sample(6) * 0.5f;
						r += sample(7) * 0.5f;
					}
				}
				if (l > 1.0f)
					l = 1.0f;
				if (l < -1.0f)
					l = -1.0f;
				if (r > 1.0f)
					r = 1.0f;
				if (r < -1.0f)
					r = -1.0f;
				data[0][write_pos] = l;
				data[1][write_pos] = r;
				write_pos = (write_pos + 1) % kCapacity;
				size++;
			}
		}

		bool pop_mixed_into(struct audio_output_data *mixes, uint32_t mixers, size_t out_channels,
				    bool drain_partial)
		{
			if (size < AUDIO_OUTPUT_FRAMES) {
				if (!drain_partial || size == 0)
					return false;
			}
			const size_t frames = size < AUDIO_OUTPUT_FRAMES ? size : AUDIO_OUTPUT_FRAMES;
			const size_t use_ch = channels < out_channels ? channels : out_channels;
			for (size_t i = 0; i < frames; i++) {
				for (size_t ch = 0; ch < use_ch; ch++) {
					float s = data[ch][read_pos];
					for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
						if ((mixers & (1u << mix_idx)) == 0)
							continue;
						float *out = mixes[mix_idx].data[ch];
						if (!out)
							continue;
						out[i] += s;
						if (out[i] > 1.0f)
							out[i] = 1.0f;
						if (out[i] < -1.0f)
							out[i] = -1.0f;
					}
				}
				read_pos = (read_pos + 1) % kCapacity;
				size--;
			}
			return frames > 0;
		}
	};

	bool setup_video();
	bool setup_video_from_canvas();
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

	void push_captured_audio(obs_source_t *source, const struct audio_data *audio_data, bool muted);
	bool pop_captured_audio(struct audio_output_data *mixes, uint32_t mixers, size_t channels);
	double media_time_sec() const;
	void write_chapters_sidecar() const;
	bool try_native_add_chapter(const std::string &name);
	void connect_file_changed_signal();
	void on_file_split(const char *next_file);
	static void file_changed_signal(void *data, calldata_t *cd);

	std::string source_name_;
	MsrOrientation orientation_;
	std::string extra_audio_name_;
	MsrSettings settings_;
	bool use_vertical_canvas_ = false;
	MsrSessionKind kind_ = MsrSessionKind::Record;
	std::string output_path_;
	std::string base_output_path_;
	std::vector<std::string> split_parts_;
	std::vector<double> part_start_sec_;
	int current_part_index_ = 0;

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
	std::atomic<bool> paused_{false};
	uint64_t start_ns_ = 0;

	struct ChapterMark {
		std::string name;
		double time_sec = 0.0;
		int part_index = 0;
		double time_in_part_sec = 0.0;
	};
	std::vector<ChapterMark> chapters_;

	std::mutex audio_mutex_;
	SampleFifo fifo_main_;
	SampleFifo fifo_extra_;
	bool have_capture_audio_ = false;
};
