/*
Multi Source Record for OBS Studio — X Livestream API client (scaffold)
Copyright (C) 2026 Travvvman
GPL-2.0-or-later
*/
#include "msr-x-live.hpp"
#include "plugin-support.h"

#include <obs-module.h>

MsrXLiveClient::MsrXLiveClient()
{
	status_ = "X Live: not configured (needs OAuth Client ID + Livestream API access)";
}

MsrXLiveClient::~MsrXLiveClient() = default;

void MsrXLiveClient::set_client_id(std::string client_id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	client_id_ = std::move(client_id);
	if (client_id_.empty()) {
		state_ = State::Disabled;
		status_ = "X Live: not configured (needs OAuth Client ID + Livestream API access)";
	} else if (state_ == State::Disabled || state_ == State::Error) {
		state_ = State::SignedOut;
		status_ = "X Live: signed out — click Sign in (PKCE)";
	}
}

std::string MsrXLiveClient::client_id() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return client_id_;
}

MsrXLiveClient::State MsrXLiveClient::state() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return state_;
}

std::string MsrXLiveClient::status_text() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return status_;
}

std::string MsrXLiveClient::last_error() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return last_error_;
}

bool MsrXLiveClient::begin_sign_in()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (client_id_.empty()) {
		last_error_ = "Set X OAuth Client ID first";
		status_ = last_error_;
		state_ = State::Error;
		obs_log(LOG_WARNING, "X Live sign-in blocked: no client id");
		return false;
	}
	/* Phase 1 will: PKCE challenge, open browser, loopback, token exchange. */
	last_error_ = "OAuth PKCE not implemented yet (scaffold)";
	status_ = last_error_;
	state_ = State::Error;
	obs_log(LOG_INFO, "X Live sign-in stub invoked (no browser opened)");
	return false;
}

void MsrXLiveClient::sign_out()
{
	std::lock_guard<std::mutex> lock(mutex_);
	access_token_.clear();
	refresh_token_.clear();
	user_id_.clear();
	source_id_.clear();
	last_error_.clear();
	if (client_id_.empty()) {
		state_ = State::Disabled;
		status_ = "X Live: not configured";
	} else {
		state_ = State::SignedOut;
		status_ = "X Live: signed out";
	}
}

bool MsrXLiveClient::sync_source(std::string /*preferred_name*/, std::string *rtmps_url_out,
				 size_t *key_len_out)
{
	if (rtmps_url_out)
		rtmps_url_out->clear();
	if (key_len_out)
		*key_len_out = 0;
	std::lock_guard<std::mutex> lock(mutex_);
	last_error_ = "sync_source not implemented yet (scaffold)";
	status_ = last_error_;
	return false;
}
