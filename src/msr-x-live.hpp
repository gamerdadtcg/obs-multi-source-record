/*
Multi Source Record for OBS Studio — X Livestream API client (BYOK)
Copyright (C) 2026 Travvvman
GPL-2.0-or-later
*/
#pragma once

#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTimer>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * Bring-your-own X OAuth Client ID + PKCE (Native App / public client).
 * Tokens never logged. Status poll runs only while a broadcast is live (60s).
 * Chat v1: send while live + history after END (no live chat polling).
 */
class MsrXLiveClient : public QObject {
	Q_OBJECT
public:
	enum class State {
		Disabled,
		SignedOut,
		SigningIn,
		SignedIn,
		Error,
	};

	explicit MsrXLiveClient(QObject *parent = nullptr);
	~MsrXLiveClient() override;

	void set_client_id(std::string client_id);
	std::string client_id() const;

	/** Cached source id from settings (not a secret by itself, but equals stream key — never log). */
	void set_cached_source_id(std::string source_id);
	std::string cached_source_id() const;

	/** Absolute path for token JSON (set by dock via obs_module_config_path). */
	void set_tokens_path(std::string path);

	State state() const;
	std::string status_text() const;
	std::string last_error() const;
	bool signed_in() const { return state() == State::SignedIn; }
	bool is_live() const;
	std::string broadcast_id() const;
	std::string share_url() const;

	/** Opens system browser + loopback http://127.0.0.1:8765/callback PKCE. */
	bool begin_sign_in();
	void sign_out();

	/**
	 * GET existing sources or POST create; caches source id; returns RTMPS URL + stream key.
	 * Caller fills MSR destination; must never display/log the key.
	 */
	bool sync_source(std::string preferred_name, std::string *rtmps_url_out, std::string *stream_key_out);

	/** Create broadcast (if needed) then PUBLISH with title. Explicit user click only. */
	bool go_live(std::string title);

	/** PUT state END. Explicit user click only. Stops 60s status poll. */
	bool end_broadcast();

	/** POST chat message while live. */
	bool send_chat(std::string text);

	/** GET chat history — only valid after END. */
	bool load_chat_history(std::string *history_out);

	/** One status fetch (also used by the 60s timer while live). */
	bool poll_status();

signals:
	void statusChanged();
	void signedInChanged(bool signed_in);
	void liveChanged(bool live);
	void chatHistoryReady(QString text);

private slots:
	void on_loopback_connection();
	void on_status_timer();

private:
	bool ensure_access_token();
	bool refresh_access_token();
	bool http_json(const QString &method, const QString &url, const QByteArray &body, int *http_status,
		       QByteArray *response_out, bool auth);
	bool http_form(const QString &url, const QByteArray &form, int *http_status, QByteArray *response_out);
	void set_error(const std::string &msg, bool whitelist = false);
	void set_status(const std::string &msg);
	void clear_tokens_locked();
	void load_tokens();
	void save_tokens() const;
	void erase_token_file() const;
	std::string tokens_path_;
	static std::string base64url(const QByteArray &in);
	static QByteArray random_bytes(int n);
	static bool looks_like_whitelist_error(int status, const QByteArray &body);

	mutable std::mutex mutex_;
	std::string client_id_;
	State state_ = State::Disabled;
	std::string status_;
	std::string last_error_;
	std::string access_token_;  /* never log */
	std::string refresh_token_; /* never log */
	std::string user_id_;
	std::string username_;
	std::string source_id_; /* == stream key; never log */
	std::string broadcast_id_;
	std::string share_url_;
	bool live_ = false;

	QString pkce_verifier_;
	QString pkce_state_;
	QTcpServer *loopback_ = nullptr;
	QNetworkAccessManager *nam_ = nullptr;
	QTimer *status_timer_ = nullptr;

	static constexpr const char *kRedirectUri = "http://127.0.0.1:8765/callback";
	static constexpr const char *kScopes = "broadcast.read broadcast.write offline.access users.read";
	static constexpr int kLoopbackPort = 8765;
};
