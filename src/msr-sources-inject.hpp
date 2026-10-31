/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

Injects per-source vertical video + audio toggles into the main OBS Sources
list (next to eye/lock). Fail-soft if the SourceTree layout is unexpected.
*/
#pragma once

#include <obs.h>
#include <obs-frontend-api.h>
#include <QObject>
#include <QPointer>

class QListView;
class QTimer;

class MsrSourcesInject : public QObject {
	Q_OBJECT
public:
	explicit MsrSourcesInject(QObject *parent = nullptr);
	~MsrSourcesInject() override;

	static MsrSourcesInject *instance();
	static void create();
	static void destroy();

	void refresh_soon();

public slots:
	void inject_now();

private:
	void inject_into_view(QListView *view);
	void attach_main_select_signals();
	void detach_main_select_signals();

	static void on_main_item_select(void *data, calldata_t *cd);
	static void on_frontend_event(enum obs_frontend_event event, void *data);

	QTimer *timer_ = nullptr;
	obs_source_t *select_signal_source_ = nullptr;
	static MsrSourcesInject *instance_;
};
