/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

Private vertical audio mix: sums per-source capture from the vertical
program scene, excluding sources with msr_vertical_audio=false.
Used by vertical recording so main/horizontal audio
is unchanged.
*/
#pragma once

#include <obs.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

class MsrVerticalAudioMix {
public:
	static constexpr const char *kSettingKey = "msr_vertical_audio";

	MsrVerticalAudioMix() = default;
	~MsrVerticalAudioMix();

	MsrVerticalAudioMix(const MsrVerticalAudioMix &) = delete;
	MsrVerticalAudioMix &operator=(const MsrVerticalAudioMix &) = delete;

	audio_t *audio();
	void shutdown();
	void rebuild(obs_scene_t *vert_scene);

	static bool source_audio_enabled(obs_source_t *source);
	static void set_source_audio_enabled(obs_source_t *source, bool enabled);

private:
	struct SampleFifo {
		static constexpr size_t kChannels = 2;
		static constexpr size_t kCapacity = 48000;
		std::vector<float> data[kChannels];
		size_t read_pos = 0;
		size_t write_pos = 0;
		size_t size = 0;

		void clear();
		void ensure();
		void push_planar(const struct audio_data *audio_data);
		bool has_frames() const { return size >= AUDIO_OUTPUT_FRAMES; }
		void mix_pop_into(struct audio_output_data *mixes, uint32_t mixers, size_t out_channels);
	};

	struct Cap {
		obs_source_t *source = nullptr;
		SampleFifo fifo;
	};

	void clear_captures_locked();
	static bool audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts,
					 uint32_t mixers, struct audio_output_data *mixes);
	static void audio_capture_callback(void *param, obs_source_t *source, const struct audio_data *audio_data,
					   bool muted);

	std::mutex mutex_;
	audio_t *audio_output_ = nullptr;
	std::vector<Cap> caps_;
	std::atomic<bool> closing_{false};
};
