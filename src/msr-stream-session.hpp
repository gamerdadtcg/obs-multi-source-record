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

/** One RTMP (or compatible) stream to a single destination. Does not touch OBS main stream. */
class MsrStreamSession {
public:
	MsrStreamSession(MsrStreamDestination dest, const MsrSettings &settings);
	~MsrStreamSession();

	MsrStreamSession(const MsrStreamSession &) = delete;
	MsrStreamSession &operator=(const MsrStreamSession &) = delete;

	bool start();
	void stop();
	bool active() const { return active_.load(); }

	const std::string &name() const { return dest_.name; }
	MsrOrientation orientation() const { return dest_.orientation; }
	const std::string &server_url() const { return dest_.server_url; }

private:
	bool setup_video();
	bool setup_encoders_and_output();
	void teardown();
	const char *resolve_video_encoder_id() const;
	static size_t first_mixer_index(uint32_t mask);

	MsrStreamDestination dest_;
	MsrSettings settings_;

	obs_source_t *activated_program_ = nullptr;
	video_t *video_output_ = nullptr;
	audio_t *audio_output_ = nullptr;
	obs_encoder_t *video_encoder_ = nullptr;
	obs_encoder_t *audio_encoder_ = nullptr;
	obs_output_t *rtmp_output_ = nullptr;
	obs_service_t *service_ = nullptr;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	std::atomic<bool> active_{false};
};
