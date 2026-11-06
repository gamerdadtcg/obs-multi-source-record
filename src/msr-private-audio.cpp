/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-private-audio.hpp"
#include "plugin-support.h"

#include <obs-module.h>

namespace {
constexpr const char *kRelayPrefix = "msr_iso_audio_";
constexpr const char *kRelayType = "msr_iso_audio_relay";
} // namespace

MsrSourceRecordAudio &MsrSourceRecordAudio::instance()
{
	static MsrSourceRecordAudio audio;
	return audio;
}

void *MsrSourceRecordAudio::relay_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	return new RelayContext{source};
}

void MsrSourceRecordAudio::relay_destroy(void *data)
{
	delete static_cast<RelayContext *>(data);
}

uint32_t MsrSourceRecordAudio::relay_flags(void *data)
{
	UNUSED_PARAMETER(data);
	return OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CAP_DISABLED;
}

void MsrSourceRecordAudio::capture_callback(void *param, obs_source_t *source, const struct audio_data *audio_data,
					    bool muted)
{
	UNUSED_PARAMETER(source);
	auto *ctx = static_cast<RelayContext *>(param);
	if (!ctx || !ctx->source || !audio_data || muted || !audio_data->frames)
		return;

	/* Capture callbacks use the global mix's planar float convention, exactly
 * Source Record's separate input does. No copy, conversion or mixer routing. */
	struct obs_source_audio out = {};
	out.frames = audio_data->frames;
	out.speakers = audio_output_get_channels(obs_get_audio());
	out.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.timestamp = audio_data->timestamp;
	for (size_t ch = 0; ch < MAX_AV_PLANES; ch++)
		out.data[ch] = audio_data->data[ch];
	obs_source_output_audio(ctx->source, &out);
}

std::string MsrSourceRecordAudio::prepare(const std::string &iso_source, const std::string &audio_source)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto found = relays_.find(iso_source);
	if (found != relays_.end()) {
		const char *current = obs_source_get_name(found->second.captured);
		if (current && audio_source == current)
			return obs_source_get_name(found->second.relay);
		obs_source_remove_audio_capture_callback(
			found->second.captured, capture_callback,
			static_cast<RelayContext *>(obs_obj_get_data(found->second.relay)));
		obs_source_dec_active(found->second.captured);
		obs_source_dec_showing(found->second.captured);
		obs_source_release(found->second.captured);
		obs_source_release(found->second.relay);
		relays_.erase(found);
	}

	obs_source_t *captured = obs_get_source_by_name(audio_source.c_str());
	if (!captured) {
		obs_log(LOG_WARNING, "ISO audio: source '%s' not found", audio_source.c_str());
		return {};
	}
	const std::string relay_name = std::string(kRelayPrefix) + iso_source;
	/* Lookup must be a public source for Source Record. Keep it hidden from
	 * the UI, with no scene reference. It never changes any mixer state. */
	obs_source_t *relay = obs_source_create_set_last_ver(kRelayType, relay_name.c_str(), nullptr, nullptr, nullptr,
							     false, LIBOBS_API_VER, nullptr);
	if (!relay) {
		obs_source_release(captured);
		return {};
	}
	obs_source_add_audio_capture_callback(captured, capture_callback,
					      static_cast<RelayContext *>(obs_obj_get_data(relay)));
	relays_[iso_source] = Relay{captured, relay};
	obs_log(LOG_INFO, "ISO audio: '%s' <- '%s'; no mixer changes", iso_source.c_str(), audio_source.c_str());
	return relay_name;
}

void MsrSourceRecordAudio::stop()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &entry : relays_) {
		Relay &r = entry.second;
		obs_source_remove_audio_capture_callback(r.captured, capture_callback,
							 static_cast<RelayContext *>(obs_obj_get_data(r.relay)));
		obs_source_release(r.captured);
		obs_source_release(r.relay);
	}
	relays_.clear();
}

static bool cleanup_stale_relay(void *, obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	if (id && std::string(id) == kRelayType)
		obs_source_remove(source);
	return true;
}

const struct obs_source_info MsrSourceRecordAudio::relay_info = {
	.id = kRelayType,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CAP_DISABLED,
	.get_name = [](void *) { return "MSR Private ISO Audio"; },
	.create = relay_create,
	.destroy = relay_destroy,
	.get_output_flags = relay_flags,
};

void msr_register_private_audio_source()
{
	obs_register_source(&MsrSourceRecordAudio::relay_info);
	obs_enum_sources(cleanup_stale_relay, nullptr);
}
