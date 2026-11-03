/*
Multi Source Record for OBS Studio — X Livestream API client (scaffold)
Copyright (C) 2026 Travvvman
GPL-2.0-or-later
*/
#pragma once

#include <atomic>
#include <mutex>
#include <string>

/** Scaffold for OAuth2 PKCE + Livestream REST. No network until configured. */
class MsrXLiveClient {
public:
	enum class State {
		Disabled,
		SignedOut,
		SigningIn,
		SignedIn,
		Error,
	};

	MsrXLiveClient();
	~MsrXLiveClient();

	void set_client_id(std::string client_id);
	std::string client_id() const;

	State state() const;
	std::string status_text() const;
	std::string last_error() const;

	/** Opens system browser + loopback PKCE (Phase 1). Currently no-op stub. */
	bool begin_sign_in();
	void sign_out();

	bool signed_in() const { return state() == State::SignedIn; }

	/** Phase 1: list/create source and return RTMPS URL (key kept internal). */
	bool sync_source(std::string preferred_name, std::string *rtmps_url_out, size_t *key_len_out);

private:
	mutable std::mutex mutex_;
	std::string client_id_;
	State state_ = State::Disabled;
	std::string status_;
	std::string last_error_;
	std::string access_token_;  /* never log */
	std::string refresh_token_; /* never log */
	std::string user_id_;
	std::string source_id_; /* == stream key; never log */
};
