/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman
*/
#include "msr-vertical-audio.hpp"
#include "plugin-support.h"

#include <unordered_set>
#include <cstring>

void MsrVerticalAudioMix::SampleFifo::clear()
{
	for (size_t ch = 0; ch < kChannels; ch++)
		data[ch].clear();
	read_pos = write_pos = size = 0;
}

void MsrVerticalAudioMix::SampleFifo::ensure()
{
	if (!data[0].empty())
		return;
	for (size_t ch = 0; ch < kChannels; ch++)
		data[ch].assign(kCapacity, 0.0f);
}

void MsrVerticalAudioMix::SampleFifo::push_planar(const struct audio_data *audio_data)
{
	size_t ch_count = 0;
	for (size_t ch = 0; ch < MAX_AV_PLANES; ch++) {
		if (audio_data->data[ch])
			ch_count = ch + 1;
	}
	if (ch_count == 0 || !audio_data->frames)
		return;
	ensure();
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
			l = sample(0);
			r = sample(1);
			const float fc = sample(2) * 0.707f;
			l += fc;
			r += fc;
			if (ch_count >= 6) {
				l += sample(4) * 0.707f;
				r += sample(5) * 0.707f;
			}
		}
		if (l > 1.0f)
			l = 1.0f;
		else if (l < -1.0f)
			l = -1.0f;
		if (r > 1.0f)
			r = 1.0f;
		else if (r < -1.0f)
			r = -1.0f;
		data[0][write_pos] = l;
		data[1][write_pos] = r;
		write_pos = (write_pos + 1) % kCapacity;
		size++;
	}
}

void MsrVerticalAudioMix::SampleFifo::mix_pop_into(struct audio_output_data *mixes, uint32_t mixers,
						  size_t out_channels)
{
	if (size < AUDIO_OUTPUT_FRAMES)
		return;
	const size_t use_ch = out_channels < kChannels ? out_channels : kChannels;
	for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
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
				else if (out[i] < -1.0f)
					out[i] = -1.0f;
			}
		}
		read_pos = (read_pos + 1) % kCapacity;
		size--;
	}
}

bool MsrVerticalAudioMix::source_audio_enabled(obs_source_t *source)
{
	if (!source)
		return true;
	obs_data_t *ps = obs_source_get_private_settings(source);
	if (!ps)
		return true;
	bool enabled = true;
	if (obs_data_has_user_value(ps, kSettingKey))
		enabled = obs_data_get_bool(ps, kSettingKey);
	obs_data_release(ps);
	return enabled;
}

void MsrVerticalAudioMix::set_source_audio_enabled(obs_source_t *source, bool enabled)
{
	if (!source)
		return;
	obs_data_t *ps = obs_source_get_private_settings(source);
	if (!ps)
		return;
	obs_data_set_bool(ps, kSettingKey, enabled);
	obs_data_release(ps);
}

MsrVerticalAudioMix::~MsrVerticalAudioMix()
{
	shutdown();
}

void MsrVerticalAudioMix::clear_captures_locked()
{
	for (Cap &c : caps_) {
		if (c.source) {
			obs_source_remove_audio_capture_callback(c.source, audio_capture_callback, this);
			obs_source_release(c.source);
			c.source = nullptr;
		}
	}
	caps_.clear();
}

void MsrVerticalAudioMix::shutdown()
{
	closing_.store(true);
	std::lock_guard<std::mutex> lock(mutex_);
	clear_captures_locked();
	if (audio_output_) {
		audio_output_close(audio_output_);
		audio_output_ = nullptr;
	}
	closing_.store(false);
}

audio_t *MsrVerticalAudioMix::audio()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (audio_output_)
		return audio_output_;

	closing_.store(false);
	struct audio_output_info oi = {};
	oi.name = "msr-vertical-audio";
	oi.speakers = SPEAKERS_STEREO;
	oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
	if (!oi.samples_per_sec)
		oi.samples_per_sec = 48000;
	oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
	oi.input_param = this;
	oi.input_callback = audio_input_callback;

	if (audio_output_open(&audio_output_, &oi) != 0) {
		obs_log(LOG_ERROR, "failed to open vertical private audio mix");
		audio_output_ = nullptr;
		return nullptr;
	}
	obs_log(LOG_INFO, "vertical private audio mix open @ %u Hz", oi.samples_per_sec);
	return audio_output_;
}

void MsrVerticalAudioMix::rebuild(obs_scene_t *vert_scene)
{
	/* Caller must open via audio() first — do not call audio() here (non-recursive mutex). */
	std::lock_guard<std::mutex> lock(mutex_);
	clear_captures_locked();
	if (!vert_scene || !audio_output_)
		return;

	struct Ctx {
		MsrVerticalAudioMix *self;
		std::unordered_set<obs_source_t *> seen;
	} ctx{this, {}};

	obs_scene_enum_items(
		vert_scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
			auto *c = static_cast<Ctx *>(p);
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (!src)
				return true;
			if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO))
				return true;
			if (!source_audio_enabled(src))
				return true;
			if (c->seen.count(src))
				return true;
			c->seen.insert(src);
			obs_source_get_ref(src);
			obs_source_add_audio_capture_callback(src, audio_capture_callback, c->self);
			Cap cap;
			cap.source = src;
			c->self->caps_.push_back(std::move(cap));
			return true;
		},
		&ctx);

	obs_log(LOG_INFO, "vertical audio mix: capturing %zu source(s)", caps_.size());
}

void MsrVerticalAudioMix::audio_capture_callback(void *param, obs_source_t *source,
						 const struct audio_data *audio_data, bool muted)
{
	auto *self = static_cast<MsrVerticalAudioMix *>(param);
	if (!self || self->closing_.load() || !audio_data || muted || !audio_data->frames)
		return;
	if (!source_audio_enabled(source))
		return;
	std::lock_guard<std::mutex> lock(self->mutex_);
	for (Cap &c : self->caps_) {
		if (c.source == source) {
			c.fifo.push_planar(audio_data);
			return;
		}
	}
}

bool MsrVerticalAudioMix::audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in,
					       uint64_t *out_ts, uint32_t mixers, struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(end_ts_in);
	auto *self = static_cast<MsrVerticalAudioMix *>(param);
	/* Always return true and lock to the private audio_output clock.
	 * Returning false when the FIFO is empty (silent scene / startup)
	 * starves the AAC encoder; rtmp_output interleave then never starts
	 * (needs audio+video), so Total frames output stays 0 while X shows
	 * resolution metadata with bitrate/fps/audio dashes. Matches MsrSession. */
	*out_ts = start_ts_in;
	if (!self || self->closing_.load())
		return true;

	std::lock_guard<std::mutex> lock(self->mutex_);
	for (Cap &c : self->caps_) {
		if (c.fifo.has_frames())
			c.fifo.mix_pop_into(mixes, mixers, 2);
	}
	return true;
}
