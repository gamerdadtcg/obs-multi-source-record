/*
Multi Source Record for OBS Studio — X Livestream API client (BYOK)
Copyright (C) 2026 Travvvman
GPL-2.0-or-later
*/
#include "msr-x-live.hpp"
#include "plugin-support.h"

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QHostAddress>
#include <QUrl>
#include <QUrlQuery>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QFile>
#include <QDir>

#include <fstream>
#include <sstream>

/* Avoid obs-module.h here: on MSVC it redefines blogva vs plugin-support.h. */
extern "C" {
char *obs_module_config_path(const char *file);
void bfree(void *ptr);
}

#ifndef LOG_ERROR
#define LOG_ERROR 100
#endif
#ifndef LOG_WARNING
#define LOG_WARNING 200
#endif
#ifndef LOG_INFO
#define LOG_INFO 300
#endif

static const char *kWhitelistUrl = "https://docs.x.com/forms/livestream-api-access.md";

MsrXLiveClient::MsrXLiveClient(QObject *parent) : QObject(parent)
{
	status_ = "X Live: not configured — paste your X OAuth Client ID";
	nam_ = new QNetworkAccessManager(this);
	status_timer_ = new QTimer(this);
	status_timer_->setInterval(60000);
	connect(status_timer_, &QTimer::timeout, this, &MsrXLiveClient::on_status_timer);
	load_tokens();
}

MsrXLiveClient::~MsrXLiveClient()
{
	if (loopback_) {
		loopback_->close();
		loopback_->deleteLater();
		loopback_ = nullptr;
	}
	status_timer_->stop();
}

void MsrXLiveClient::set_client_id(std::string client_id)
{
	bool emit_signed = false;
	bool was_signed = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		was_signed = !access_token_.empty() && !client_id_.empty();
		client_id_ = std::move(client_id);
		if (client_id_.empty()) {
			state_ = State::Disabled;
			status_ = "X Live: not configured — paste your X OAuth Client ID";
		} else if (!access_token_.empty() || !refresh_token_.empty()) {
			state_ = State::SignedIn;
			if (!username_.empty())
				status_ = "X Live: signed in as @" + username_;
			else
				status_ = "X Live: signed in";
			emit_signed = !was_signed;
		} else if (state_ == State::Disabled || state_ == State::Error || state_ == State::SignedOut) {
			state_ = State::SignedOut;
			status_ = "X Live: signed out — click Sign in (PKCE)";
		}
	}
	emit statusChanged();
	if (emit_signed)
		emit signedInChanged(true);
}

std::string MsrXLiveClient::client_id() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return client_id_;
}

void MsrXLiveClient::set_cached_source_id(std::string source_id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	source_id_ = std::move(source_id);
}

std::string MsrXLiveClient::cached_source_id() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return source_id_;
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

bool MsrXLiveClient::is_live() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return live_;
}

std::string MsrXLiveClient::broadcast_id() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return broadcast_id_;
}

std::string MsrXLiveClient::share_url() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return share_url_;
}

void MsrXLiveClient::set_error(const std::string &msg, bool whitelist)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		last_error_ = msg;
		if (whitelist) {
			status_ = std::string("X Live: app not whitelisted for Livestream API. Request access: ") +
				  kWhitelistUrl;
			last_error_ = status_;
		} else {
			status_ = msg;
		}
		state_ = State::Error;
	}
	obs_log(LOG_WARNING, "X Live: %s", whitelist ? "not whitelisted for Livestream API" : msg.c_str());
	emit statusChanged();
}

void MsrXLiveClient::set_status(const std::string &msg)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		status_ = msg;
		last_error_.clear();
	}
	emit statusChanged();
}

void MsrXLiveClient::clear_tokens_locked()
{
	access_token_.clear();
	refresh_token_.clear();
	user_id_.clear();
	username_.clear();
	broadcast_id_.clear();
	share_url_.clear();
	live_ = false;
}

std::string MsrXLiveClient::tokens_path() const
{
	char *path = obs_module_config_path("x-live-tokens.json");
	std::string out = path ? path : "";
	if (path)
		bfree(path);
	return out;
}

void MsrXLiveClient::load_tokens()
{
	const std::string path = tokens_path();
	if (path.empty())
		return;
	std::ifstream in(path);
	if (!in)
		return;
	std::stringstream buf;
	buf << in.rdbuf();
	const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(buf.str()));
	if (!doc.isObject())
		return;
	const QJsonObject o = doc.object();
	std::lock_guard<std::mutex> lock(mutex_);
	access_token_ = o.value(QStringLiteral("access_token")).toString().toStdString();
	refresh_token_ = o.value(QStringLiteral("refresh_token")).toString().toStdString();
	user_id_ = o.value(QStringLiteral("user_id")).toString().toStdString();
	username_ = o.value(QStringLiteral("username")).toString().toStdString();
	/* source_id may also live in MSR settings; tokens file is a fallback cache */
	const QString sid = o.value(QStringLiteral("source_id")).toString();
	if (!sid.isEmpty() && source_id_.empty())
		source_id_ = sid.toStdString();
	if (!access_token_.empty() || !refresh_token_.empty()) {
		state_ = client_id_.empty() ? State::Disabled : State::SignedIn;
		if (!username_.empty())
			status_ = "X Live: signed in as @" + username_ + " (restored)";
		else
			status_ = "X Live: tokens restored — set Client ID if needed";
	}
}

void MsrXLiveClient::save_tokens() const
{
	const std::string path = tokens_path();
	if (path.empty())
		return;
	QJsonObject o;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		o.insert(QStringLiteral("access_token"), QString::fromStdString(access_token_));
		o.insert(QStringLiteral("refresh_token"), QString::fromStdString(refresh_token_));
		o.insert(QStringLiteral("user_id"), QString::fromStdString(user_id_));
		o.insert(QStringLiteral("username"), QString::fromStdString(username_));
		/* Do not write source_id here as plaintext companion if empty; optional cache */
		if (!source_id_.empty())
			o.insert(QStringLiteral("source_id"), QString::fromStdString(source_id_));
	}
	QFile f(QString::fromStdString(path));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		obs_log(LOG_WARNING, "X Live: failed to write token file");
		return;
	}
	f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
	f.close();
	obs_log(LOG_INFO, "X Live: tokens saved (contents never logged)");
}

void MsrXLiveClient::erase_token_file() const
{
	const std::string path = tokens_path();
	if (path.empty())
		return;
	QFile::remove(QString::fromStdString(path));
}

QByteArray MsrXLiveClient::random_bytes(int n)
{
	QByteArray out;
	out.resize(n);
	auto *rng = QRandomGenerator::global();
	for (int i = 0; i < n; i++)
		out[i] = static_cast<char>(rng->bounded(256));
	return out;
}

std::string MsrXLiveClient::base64url(const QByteArray &in)
{
	QByteArray b64 = in.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	return b64.toStdString();
}

bool MsrXLiveClient::looks_like_whitelist_error(int status, const QByteArray &body)
{
	if (status != 403)
		return false;
	const QByteArray lower = body.toLower();
	return lower.contains("whitelist") || lower.contains("not authorized") || lower.contains("not allowed") ||
	       lower.contains("livestream") || lower.contains("access denied") || lower.contains("forbidden");
}

bool MsrXLiveClient::http_json(const QString &method, const QString &url, const QByteArray &body, int *http_status,
			       QByteArray *response_out, bool auth)
{
	QNetworkRequest req{QUrl(url)};
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	req.setRawHeader("User-Agent", "obs-multi-source-record-x-live/1.0");
	if (auth) {
		std::string token;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			token = access_token_;
		}
		if (token.empty())
			return false;
		req.setRawHeader("Authorization", QByteArray("Bearer ") + QByteArray::fromStdString(token));
	}

	QNetworkReply *reply = nullptr;
	if (method == QLatin1String("GET"))
		reply = nam_->get(req);
	else if (method == QLatin1String("POST"))
		reply = nam_->post(req, body);
	else if (method == QLatin1String("PUT"))
		reply = nam_->put(req, body);
	else if (method == QLatin1String("DELETE"))
		reply = nam_->deleteResource(req);
	else
		return false;

	QEventLoop loop;
	connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	loop.exec();

	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QByteArray resp = reply->readAll();
	if (http_status)
		*http_status = status;
	if (response_out)
		*response_out = resp;
	const bool ok = reply->error() == QNetworkReply::NoError || (status >= 200 && status < 300);
	reply->deleteLater();
	return ok && status >= 200 && status < 300;
}

bool MsrXLiveClient::http_form(const QString &url, const QByteArray &form, int *http_status, QByteArray *response_out)
{
	QNetworkRequest req{QUrl(url)};
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
	req.setRawHeader("User-Agent", "obs-multi-source-record-x-live/1.0");
	QNetworkReply *reply = nam_->post(req, form);
	QEventLoop loop;
	connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	loop.exec();
	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QByteArray resp = reply->readAll();
	if (http_status)
		*http_status = status;
	if (response_out)
		*response_out = resp;
	const bool ok = status >= 200 && status < 300;
	reply->deleteLater();
	return ok;
}

bool MsrXLiveClient::refresh_access_token()
{
	std::string client_id;
	std::string refresh;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		client_id = client_id_;
		refresh = refresh_token_;
	}
	if (client_id.empty() || refresh.empty())
		return false;

	QUrlQuery q;
	q.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
	q.addQueryItem(QStringLiteral("refresh_token"), QString::fromStdString(refresh));
	q.addQueryItem(QStringLiteral("client_id"), QString::fromStdString(client_id));

	int status = 0;
	QByteArray resp;
	if (!http_form(QStringLiteral("https://api.x.com/2/oauth2/token"), q.query(QUrl::FullyEncoded).toUtf8(),
		       &status, &resp)) {
		obs_log(LOG_WARNING, "X Live: token refresh failed (HTTP %d)", status);
		return false;
	}
	const QJsonObject o = QJsonDocument::fromJson(resp).object();
	const QString at = o.value(QStringLiteral("access_token")).toString();
	const QString rt = o.value(QStringLiteral("refresh_token")).toString();
	if (at.isEmpty())
		return false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		access_token_ = at.toStdString();
		if (!rt.isEmpty())
			refresh_token_ = rt.toStdString();
	}
	save_tokens();
	obs_log(LOG_INFO, "X Live: access token refreshed");
	return true;
}

bool MsrXLiveClient::ensure_access_token()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!access_token_.empty())
			return true;
	}
	return refresh_access_token();
}

bool MsrXLiveClient::begin_sign_in()
{
	std::string client_id;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		client_id = client_id_;
		if (client_id.empty()) {
			last_error_ = "Set your X OAuth Client ID first (bring-your-own; never shared)";
			status_ = last_error_;
			state_ = State::Error;
			emit statusChanged();
			obs_log(LOG_WARNING, "X Live sign-in blocked: no client id");
			return false;
		}
		state_ = State::SigningIn;
		status_ = "X Live: waiting for browser sign-in…";
		last_error_.clear();
	}
	emit statusChanged();

	const QByteArray verifier_raw = random_bytes(32);
	pkce_verifier_ = QString::fromStdString(base64url(verifier_raw));
	const QByteArray challenge = QCryptographicHash::hash(pkce_verifier_.toUtf8(), QCryptographicHash::Sha256);
	const QString challenge_b64 = QString::fromStdString(base64url(challenge));
	pkce_state_ = QString::fromStdString(base64url(random_bytes(16)));

	if (loopback_) {
		loopback_->close();
		loopback_->deleteLater();
		loopback_ = nullptr;
	}
	loopback_ = new QTcpServer(this);
	connect(loopback_, &QTcpServer::newConnection, this, &MsrXLiveClient::on_loopback_connection);
	if (!loopback_->listen(QHostAddress::LocalHost, kLoopbackPort)) {
		set_error("X Live: cannot bind loopback 127.0.0.1:8765 — is another app using it?");
		return false;
	}

	QUrl auth(QStringLiteral("https://x.com/i/oauth2/authorize"));
	QUrlQuery q;
	q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
	q.addQueryItem(QStringLiteral("client_id"), QString::fromStdString(client_id));
	q.addQueryItem(QStringLiteral("redirect_uri"), QString::fromUtf8(kRedirectUri));
	q.addQueryItem(QStringLiteral("scope"), QString::fromUtf8(kScopes));
	q.addQueryItem(QStringLiteral("state"), pkce_state_);
	q.addQueryItem(QStringLiteral("code_challenge"), challenge_b64);
	q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
	auth.setQuery(q);

	if (!QDesktopServices::openUrl(auth)) {
		set_error("X Live: failed to open system browser");
		loopback_->close();
		return false;
	}
	obs_log(LOG_INFO, "X Live: browser opened for PKCE sign-in (loopback :8765)");
	return true;
}

void MsrXLiveClient::on_loopback_connection()
{
	if (!loopback_)
		return;
	QTcpSocket *sock = loopback_->nextPendingConnection();
	if (!sock)
		return;
	connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
		const QByteArray req = sock->readAll();
		const QList<QByteArray> lines = req.split('\n');
		if (lines.isEmpty()) {
			sock->close();
			sock->deleteLater();
			return;
		}
		/* GET /callback?code=...&state=... HTTP/1.1 */
		const QByteArray first = lines.first();
		const int sp1 = first.indexOf(' ');
		const int sp2 = first.indexOf(' ', sp1 + 1);
		QString path = QString::fromUtf8(first.mid(sp1 + 1, sp2 > sp1 ? sp2 - sp1 - 1 : -1));
		QUrl cb(QStringLiteral("http://127.0.0.1") + path);
		QUrlQuery q(cb);
		const QString code = q.queryItemValue(QStringLiteral("code"));
		const QString state = q.queryItemValue(QStringLiteral("state"));
		const QString error = q.queryItemValue(QStringLiteral("error"));

		const char *html_ok =
			"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
			"<html><body><h2>Signed in to X for OBS Multi Source Record</h2>"
			"<p>You can close this window and return to OBS.</p></body></html>";
		const char *html_err =
			"HTTP/1.1 400 Bad Request\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
			"<html><body><h2>Sign-in failed</h2><p>Return to OBS and try again.</p></body></html>";

		if (!error.isEmpty() || code.isEmpty() || state != pkce_state_) {
			sock->write(html_err);
			sock->disconnectFromHost();
			sock->deleteLater();
			set_error(error.isEmpty() ? "X Live: OAuth callback missing code/state"
						  : ("X Live: OAuth error: " + error.toStdString()));
			if (loopback_)
				loopback_->close();
			return;
		}

		sock->write(html_ok);
		sock->disconnectFromHost();
		sock->deleteLater();
		if (loopback_) {
			loopback_->close();
			loopback_->deleteLater();
			loopback_ = nullptr;
		}

		std::string client_id;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			client_id = client_id_;
			status_ = "X Live: exchanging authorization code…";
		}
		emit statusChanged();

		QUrlQuery form;
		form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
		form.addQueryItem(QStringLiteral("code"), code);
		form.addQueryItem(QStringLiteral("redirect_uri"), QString::fromUtf8(kRedirectUri));
		form.addQueryItem(QStringLiteral("client_id"), QString::fromStdString(client_id));
		form.addQueryItem(QStringLiteral("code_verifier"), pkce_verifier_);

		int status = 0;
		QByteArray resp;
		if (!http_form(QStringLiteral("https://api.x.com/2/oauth2/token"),
			       form.query(QUrl::FullyEncoded).toUtf8(), &status, &resp)) {
			set_error("X Live: token exchange failed (HTTP " + std::to_string(status) + ")");
			return;
		}
		const QJsonObject o = QJsonDocument::fromJson(resp).object();
		const QString at = o.value(QStringLiteral("access_token")).toString();
		const QString rt = o.value(QStringLiteral("refresh_token")).toString();
		if (at.isEmpty()) {
			set_error("X Live: token response missing access_token");
			return;
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			access_token_ = at.toStdString();
			refresh_token_ = rt.toStdString();
		}

		/* users/me */
		int me_status = 0;
		QByteArray me_resp;
		if (!http_json(QStringLiteral("GET"), QStringLiteral("https://api.x.com/2/users/me"), {}, &me_status,
			       &me_resp, true)) {
			obs_log(LOG_WARNING, "X Live: users/me failed (HTTP %d)", me_status);
		} else {
			const QJsonObject data =
				QJsonDocument::fromJson(me_resp).object().value(QStringLiteral("data")).toObject();
			std::lock_guard<std::mutex> lock(mutex_);
			user_id_ = data.value(QStringLiteral("id")).toString().toStdString();
			username_ = data.value(QStringLiteral("username")).toString().toStdString();
		}

		save_tokens();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			state_ = State::SignedIn;
			if (!username_.empty())
				status_ = "X Live: signed in as @" + username_;
			else
				status_ = "X Live: signed in";
			last_error_.clear();
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			obs_log(LOG_INFO, "X Live: signed in (user id length=%zu)", user_id_.size());
		}
		emit statusChanged();
		emit signedInChanged(true);
	});
}

/* helper to read user_id under lock from lambda above without deadlock — use method */
/* actually used user_id() which locks — but we're not holding lock when logging. OK. */

void MsrXLiveClient::sign_out()
{
	const bool was_live = is_live();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		clear_tokens_locked();
		/* keep source_id_ cache so re-login can reuse; clear on explicit sync recreate if needed */
		last_error_.clear();
		if (client_id_.empty()) {
			state_ = State::Disabled;
			status_ = "X Live: not configured";
		} else {
			state_ = State::SignedOut;
			status_ = "X Live: signed out";
		}
	}
	erase_token_file();
	status_timer_->stop();
	if (loopback_) {
		loopback_->close();
		loopback_->deleteLater();
		loopback_ = nullptr;
	}
	obs_log(LOG_INFO, "X Live: signed out (tokens erased)");
	emit statusChanged();
	emit signedInChanged(false);
	if (was_live)
		emit liveChanged(false);
}

bool MsrXLiveClient::sync_source(std::string preferred_name, std::string *rtmps_url_out, std::string *stream_key_out)
{
	if (rtmps_url_out)
		rtmps_url_out->clear();
	if (stream_key_out)
		stream_key_out->clear();

	if (!ensure_access_token()) {
		set_error("X Live: not signed in — Sign in first");
		return false;
	}

	std::string user_id;
	std::string cached;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		user_id = user_id_;
		cached = source_id_;
	}
	if (user_id.empty()) {
		int st = 0;
		QByteArray resp;
		if (!http_json(QStringLiteral("GET"), QStringLiteral("https://api.x.com/2/users/me"), {}, &st, &resp,
			       true)) {
			if (looks_like_whitelist_error(st, resp))
				set_error("", true);
			else
				set_error("X Live: users/me failed (HTTP " + std::to_string(st) + ")");
			return false;
		}
		const QJsonObject data =
			QJsonDocument::fromJson(resp).object().value(QStringLiteral("data")).toObject();
		user_id = data.value(QStringLiteral("id")).toString().toStdString();
		const std::string uname = data.value(QStringLiteral("username")).toString().toStdString();
		std::lock_guard<std::mutex> lock(mutex_);
		user_id_ = user_id;
		if (!uname.empty())
			username_ = uname;
	}

	auto apply_source = [&](const QJsonObject &src) -> bool {
		const QString sid = src.value(QStringLiteral("id")).toString();
		QString key = src.value(QStringLiteral("rtmp_stream_key")).toString();
		if (key.isEmpty())
			key = sid;
		QString rtmps = src.value(QStringLiteral("rtmps_url")).toString();
		if (rtmps.isEmpty())
			rtmps = src.value(QStringLiteral("rtmp_url")).toString();
		if (sid.isEmpty() || key.isEmpty() || rtmps.isEmpty())
			return false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			source_id_ = sid.toStdString();
			status_ = "X Live: source synced — paste RTMPS into destination (key hidden)";
			last_error_.clear();
			state_ = State::SignedIn;
		}
		if (rtmps_url_out)
			*rtmps_url_out = rtmps.toStdString();
		if (stream_key_out)
			*stream_key_out = key.toStdString();
		save_tokens();
		obs_log(LOG_INFO, "X Live: source synced (id_len=%d, key never logged)", sid.size());
		emit statusChanged();
		return true;
	};

	/* Prefer cached source via GET list */
	{
		int st = 0;
		QByteArray resp;
		const QString list_url =
			QStringLiteral("https://api.x.com/2/users/%1/sources").arg(QString::fromStdString(user_id));
		if (!http_json(QStringLiteral("GET"), list_url, {}, &st, &resp, true)) {
			if (looks_like_whitelist_error(st, resp)) {
				set_error("", true);
				return false;
			}
			obs_log(LOG_WARNING, "X Live: list sources failed (HTTP %d) — will try create", st);
		} else {
			const QJsonObject root = QJsonDocument::fromJson(resp).object();
			const QJsonObject data = root.value(QStringLiteral("data")).toObject();
			QJsonArray arr = data.value(QStringLiteral("sources")).toArray();
			if (arr.isEmpty() && root.value(QStringLiteral("data")).isArray())
				arr = root.value(QStringLiteral("data")).toArray();
			/* Prefer cached id match */
			for (const QJsonValue &v : arr) {
				const QJsonObject src = v.toObject();
				if (!cached.empty() &&
				    src.value(QStringLiteral("id")).toString().toStdString() == cached) {
					if (apply_source(src))
						return true;
				}
			}
			/* Else first source named like preferred or first available */
			for (const QJsonValue &v : arr) {
				const QJsonObject src = v.toObject();
				const QString name = src.value(QStringLiteral("name")).toString();
				if (name.contains(QString::fromStdString(preferred_name), Qt::CaseInsensitive) ||
				    name.contains(QStringLiteral("MSR"), Qt::CaseInsensitive)) {
					if (apply_source(src))
						return true;
				}
			}
			if (!arr.isEmpty() && apply_source(arr.first().toObject()))
				return true;
		}
	}

	/* POST create */
	QJsonObject body;
	body.insert(QStringLiteral("name"),
		    preferred_name.empty() ? QStringLiteral("MSR OBS") : QString::fromStdString(preferred_name));
	int st = 0;
	QByteArray resp;
	const QString create_url =
		QStringLiteral("https://api.x.com/2/users/%1/sources").arg(QString::fromStdString(user_id));
	if (!http_json(QStringLiteral("POST"), create_url, QJsonDocument(body).toJson(QJsonDocument::Compact), &st,
		       &resp, true)) {
		if (looks_like_whitelist_error(st, resp))
			set_error("", true);
		else
			set_error("X Live: create source failed (HTTP " + std::to_string(st) + ")");
		return false;
	}
	const QJsonObject data = QJsonDocument::fromJson(resp).object().value(QStringLiteral("data")).toObject();
	QJsonObject src = data.value(QStringLiteral("source")).toObject();
	if (src.isEmpty())
		src = data;
	if (!apply_source(src)) {
		set_error("X Live: create source response missing rtmps fields");
		return false;
	}
	return true;
}

bool MsrXLiveClient::go_live(std::string title)
{
	if (!ensure_access_token()) {
		set_error("X Live: not signed in");
		return false;
	}
	std::string user_id;
	std::string source_id;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		user_id = user_id_;
		source_id = source_id_;
	}
	if (user_id.empty() || source_id.empty()) {
		set_error("X Live: sync source first (need user + source)");
		return false;
	}

	/* Create broadcast */
	QJsonObject create_body;
	create_body.insert(QStringLiteral("source_id"), QString::fromStdString(source_id));
	int st = 0;
	QByteArray resp;
	const QString create_url =
		QStringLiteral("https://api.x.com/2/users/%1/broadcasts").arg(QString::fromStdString(user_id));
	if (!http_json(QStringLiteral("POST"), create_url, QJsonDocument(create_body).toJson(QJsonDocument::Compact),
		       &st, &resp, true)) {
		if (looks_like_whitelist_error(st, resp))
			set_error("", true);
		else
			set_error("X Live: create broadcast failed (HTTP " + std::to_string(st) + ")");
		return false;
	}
	const QJsonObject data = QJsonDocument::fromJson(resp).object().value(QStringLiteral("data")).toObject();
	QJsonObject bcast = data.value(QStringLiteral("broadcast")).toObject();
	if (bcast.isEmpty())
		bcast = data;
	QString bid = bcast.value(QStringLiteral("id")).toString();
	if (bid.isEmpty())
		bid = bcast.value(QStringLiteral("broadcast_id")).toString();
	const QString share = bcast.value(QStringLiteral("share_url")).toString();
	if (bid.isEmpty()) {
		set_error("X Live: create broadcast missing id");
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		broadcast_id_ = bid.toStdString();
		share_url_ = share.toStdString();
	}

	/* PUBLISH */
	QJsonObject pub;
	pub.insert(QStringLiteral("state"), QStringLiteral("PUBLISH"));
	if (!title.empty())
		pub.insert(QStringLiteral("title"), QString::fromStdString(title));
	const QString state_url = QStringLiteral("https://api.x.com/2/users/%1/broadcasts/%2/state")
					  .arg(QString::fromStdString(user_id), bid);
	st = 0;
	resp.clear();
	if (!http_json(QStringLiteral("PUT"), state_url, QJsonDocument(pub).toJson(QJsonDocument::Compact), &st, &resp,
		       true)) {
		if (looks_like_whitelist_error(st, resp))
			set_error("", true);
		else
			set_error("X Live: PUBLISH failed (HTTP " + std::to_string(st) + ")");
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		live_ = true;
		status_ = "X Live: LIVE";
		if (!share_url_.empty())
			status_ += " — " + share_url_;
		last_error_.clear();
		state_ = State::SignedIn;
	}
	status_timer_->start();
	obs_log(LOG_INFO, "X Live: broadcast PUBLISH (status poll 60s started)");
	emit statusChanged();
	emit liveChanged(true);
	return true;
}

bool MsrXLiveClient::end_broadcast()
{
	if (!ensure_access_token()) {
		set_error("X Live: not signed in");
		return false;
	}
	std::string user_id;
	std::string bid;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		user_id = user_id_;
		bid = broadcast_id_;
	}
	if (user_id.empty() || bid.empty()) {
		set_error("X Live: no active broadcast to end");
		return false;
	}

	QJsonObject body;
	body.insert(QStringLiteral("state"), QStringLiteral("END"));
	const QString state_url = QStringLiteral("https://api.x.com/2/users/%1/broadcasts/%2/state")
					  .arg(QString::fromStdString(user_id), QString::fromStdString(bid));
	int st = 0;
	QByteArray resp;
	if (!http_json(QStringLiteral("PUT"), state_url, QJsonDocument(body).toJson(QJsonDocument::Compact), &st, &resp,
		       true)) {
		if (looks_like_whitelist_error(st, resp))
			set_error("", true);
		else
			set_error("X Live: END failed (HTTP " + std::to_string(st) + ")");
		return false;
	}

	status_timer_->stop();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		live_ = false;
		status_ = "X Live: ended — you can Load chat history";
		last_error_.clear();
	}
	obs_log(LOG_INFO, "X Live: broadcast END (status poll stopped)");
	emit statusChanged();
	emit liveChanged(false);
	return true;
}

bool MsrXLiveClient::send_chat(std::string text)
{
	if (text.empty()) {
		set_error("X Live: empty chat message");
		return false;
	}
	if (!ensure_access_token()) {
		set_error("X Live: not signed in");
		return false;
	}
	std::string bid;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		bid = broadcast_id_;
		if (!live_) {
			last_error_ = "X Live: chat send only while live";
			status_ = last_error_;
			emit statusChanged();
			return false;
		}
	}
	if (bid.empty()) {
		set_error("X Live: no broadcast id");
		return false;
	}
	QJsonObject body;
	body.insert(QStringLiteral("text"), QString::fromStdString(text).left(140));
	const QString url = QStringLiteral("https://api.x.com/2/broadcasts/%1/chat").arg(QString::fromStdString(bid));
	int st = 0;
	QByteArray resp;
	if (!http_json(QStringLiteral("POST"), url, QJsonDocument(body).toJson(QJsonDocument::Compact), &st, &resp,
		       true)) {
		if (looks_like_whitelist_error(st, resp))
			set_error("", true);
		else
			set_error("X Live: send chat failed (HTTP " + std::to_string(st) + ")");
		return false;
	}
	set_status("X Live: chat sent");
	obs_log(LOG_INFO, "X Live: chat message sent");
	return true;
}

bool MsrXLiveClient::load_chat_history(std::string *history_out)
{
	if (history_out)
		history_out->clear();
	if (!ensure_access_token()) {
		set_error("X Live: not signed in");
		return false;
	}
	std::string bid;
	bool live = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		bid = broadcast_id_;
		live = live_;
	}
	if (bid.empty()) {
		set_error("X Live: no broadcast id — End a broadcast first");
		return false;
	}
	if (live) {
		set_error("X Live: chat history only after End (no live polling in v1)");
		return false;
	}
	const QString url = QStringLiteral("https://api.x.com/2/broadcasts/%1/chat").arg(QString::fromStdString(bid));
	int st = 0;
	QByteArray resp;
	if (!http_json(QStringLiteral("GET"), url, {}, &st, &resp, true)) {
		if (looks_like_whitelist_error(st, resp))
			set_error("", true);
		else
			set_error("X Live: chat history failed (HTTP " + std::to_string(st) + ")");
		return false;
	}

	QStringList lines;
	const QJsonDocument doc = QJsonDocument::fromJson(resp);
	QJsonArray arr;
	if (doc.isObject()) {
		const QJsonObject root = doc.object();
		const QJsonValue data = root.value(QStringLiteral("data"));
		if (data.isArray())
			arr = data.toArray();
		else if (data.isObject())
			arr = data.toObject().value(QStringLiteral("messages")).toArray();
	} else if (doc.isArray()) {
		arr = doc.array();
	}
	for (const QJsonValue &v : arr) {
		const QJsonObject m = v.toObject();
		const QString text = m.value(QStringLiteral("text")).toString();
		QString user = m.value(QStringLiteral("username")).toString();
		if (user.isEmpty())
			user = m.value(QStringLiteral("author_id")).toString();
		if (text.isEmpty())
			continue;
		lines << (user.isEmpty() ? text : (QStringLiteral("@") + user + QStringLiteral(": ") + text));
	}
	const QString joined = lines.join(QChar('\n'));
	if (history_out)
		*history_out = joined.toStdString();
	set_status("X Live: loaded " + std::to_string(lines.size()) + " chat messages");
	emit chatHistoryReady(joined);
	return true;
}

bool MsrXLiveClient::poll_status()
{
	if (!is_live())
		return false;
	if (!ensure_access_token())
		return false;
	std::string bid;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		bid = broadcast_id_;
	}
	if (bid.empty())
		return false;
	const QString url = QStringLiteral("https://api.x.com/2/broadcasts/%1").arg(QString::fromStdString(bid));
	int st = 0;
	QByteArray resp;
	if (!http_json(QStringLiteral("GET"), url, {}, &st, &resp, true)) {
		obs_log(LOG_WARNING, "X Live: status poll failed (HTTP %d)", st);
		return false;
	}
	const QJsonObject data = QJsonDocument::fromJson(resp).object().value(QStringLiteral("data")).toObject();
	QJsonObject b = data;
	if (data.contains(QStringLiteral("broadcast")))
		b = data.value(QStringLiteral("broadcast")).toObject();
	const QString state = b.value(QStringLiteral("state")).toString();
	const QString share = b.value(QStringLiteral("share_url")).toString();
	QString viewers;
	if (b.contains(QStringLiteral("viewer_count")))
		viewers = QString::number(b.value(QStringLiteral("viewer_count")).toInt());
	else if (b.contains(QStringLiteral("total_watching")))
		viewers = QString::number(b.value(QStringLiteral("total_watching")).toInt());

	std::string msg = "X Live: LIVE";
	if (!state.isEmpty())
		msg += " (" + state.toStdString() + ")";
	if (!viewers.isEmpty())
		msg += " · viewers " + viewers.toStdString();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!share.isEmpty())
			share_url_ = share.toStdString();
		if (!share_url_.empty())
			msg += " — " + share_url_;
		status_ = msg;
		if (state.compare(QStringLiteral("ENDED"), Qt::CaseInsensitive) == 0 ||
		    state.compare(QStringLiteral("END"), Qt::CaseInsensitive) == 0) {
			live_ = false;
		}
	}
	emit statusChanged();
	if (!is_live()) {
		status_timer_->stop();
		emit liveChanged(false);
	}
	return true;
}

void MsrXLiveClient::on_status_timer()
{
	if (!is_live()) {
		status_timer_->stop();
		return;
	}
	poll_status();
}
