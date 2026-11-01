/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman
*/
#include "msr-sources-inject.hpp"
#include "msr-vertical-canvas.hpp"
#include "plugin-support.h"

#include <obs-module.h>

#include <QMainWindow>
#include <QDockWidget>
#include <QListView>
#include <QAbstractItemModel>
#include <QHBoxLayout>
#include <QToolButton>
#include <QLabel>
#include <QTimer>
#include <QAbstractButton>
#include <QCheckBox>
#include <QSignalBlocker>

#include <callback/signal.h>

MsrSourcesInject *MsrSourcesInject::instance_ = nullptr;

MsrSourcesInject *MsrSourcesInject::instance()
{
	return instance_;
}

void MsrSourcesInject::create()
{
	if (!instance_)
		instance_ = new MsrSourcesInject();
}

void MsrSourcesInject::destroy()
{
	delete instance_;
	instance_ = nullptr;
}

MsrSourcesInject::MsrSourcesInject(QObject *parent) : QObject(parent)
{
	timer_ = new QTimer(this);
	timer_->setInterval(500);
	connect(timer_, &QTimer::timeout, this, &MsrSourcesInject::inject_now);
	timer_->start();

	obs_frontend_add_event_callback(on_frontend_event, this);
	attach_main_select_signals();
	QTimer::singleShot(800, this, &MsrSourcesInject::inject_now);
}

MsrSourcesInject::~MsrSourcesInject()
{
	obs_frontend_remove_event_callback(on_frontend_event, this);
	detach_main_select_signals();
}

void MsrSourcesInject::refresh_soon()
{
	QTimer::singleShot(50, this, &MsrSourcesInject::inject_now);
}

void MsrSourcesInject::on_frontend_event(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<MsrSourcesInject *>(data);
	if (!self)
		return;
	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED || event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
	    event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED) {
		self->attach_main_select_signals();
		self->refresh_soon();
	}
}

void MsrSourcesInject::detach_main_select_signals()
{
	if (!select_signal_source_)
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(select_signal_source_);
	if (sh)
		signal_handler_disconnect(sh, "item_select", on_main_item_select, this);
	obs_source_release(select_signal_source_);
	select_signal_source_ = nullptr;
}

void MsrSourcesInject::attach_main_select_signals()
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (!scene_src)
		return;
	if (select_signal_source_ == scene_src) {
		obs_source_release(scene_src);
		return;
	}
	detach_main_select_signals();
	select_signal_source_ = scene_src; /* take ownership of get ref */
	signal_handler_t *sh = obs_source_get_signal_handler(select_signal_source_);
	if (sh)
		signal_handler_connect(sh, "item_select", on_main_item_select, this);
}

void MsrSourcesInject::on_main_item_select(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(cd, "item");
	if (!item)
		return;
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *name = obs_source_get_name(src);
	if (!name || !*name)
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (vc)
		vc->select_source(name, true);
}

static QString label_text_from_row(QWidget *row)
{
	const auto labels = row->findChildren<QLabel *>();
	for (QLabel *lb : labels) {
		const QString t = lb->text().trimmed();
		if (!t.isEmpty() && t.size() < 256 && !t.startsWith("background"))
			return t;
	}
	return {};
}

void MsrSourcesInject::inject_into_view(QListView *view)
{
	if (!view || !view->model())
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;

	const int rows = view->model()->rowCount();
	for (int r = 0; r < rows; r++) {
		QModelIndex idx = view->model()->index(r, 0);
		QWidget *row = view->indexWidget(idx);
		if (!row)
			continue;
		if (row->property("msrVerticalInjected").toBool()) {
			/* Refresh checked state on existing toggles. */
			auto *hid = row->findChild<QToolButton *>("msrHideHorizontal");
			auto *vid = row->findChild<QToolButton *>("msrVertVideo");
			auto *aud = row->findChild<QToolButton *>("msrVertAudio");
			const QString name = row->property("msrSourceName").toString();
			if (!name.isEmpty()) {
				if (hid) {
					const bool hide = vc->item_hide_horizontal(name.toStdString());
					if (hid->isChecked() != hide) {
						QSignalBlocker b(hid);
						hid->setChecked(hide);
					}
				}
				if (vid) {
					const bool vis = vc->item_visible(name.toStdString());
					if (vid->isChecked() != vis) {
						QSignalBlocker b(vid);
						vid->setChecked(vis);
					}
				}
				if (aud) {
					const bool en = vc->item_audio_enabled(name.toStdString());
					if (aud->isChecked() != en) {
						QSignalBlocker b(aud);
						aud->setChecked(en);
					}
				}
			}
			continue;
		}

		auto *lay = qobject_cast<QHBoxLayout *>(row->layout());
		if (!lay)
			continue;

		QString name = label_text_from_row(row);
		if (name.isEmpty())
			continue;

		auto *hid = new QToolButton(row);
		hid->setObjectName("msrHideHorizontal");
		hid->setCheckable(true);
		hid->setAutoRaise(true);
		hid->setToolTip(QString::fromUtf8(obs_module_text("HideHorizontalToggleTip")));
		hid->setText(QString::fromUtf8("H"));
		hid->setFixedSize(22, 22);
		hid->setChecked(vc->item_hide_horizontal(name.toStdString()));
		hid->setStyleSheet(QStringLiteral(
			"QToolButton { color: #bbb; font-weight: bold; border: none; padding: 0; }"
			"QToolButton:checked { color: #fc6; }"
			"QToolButton:!checked { color: #666; }"));

		auto *vid = new QToolButton(row);
		vid->setObjectName("msrVertVideo");
		vid->setCheckable(true);
		vid->setAutoRaise(true);
		vid->setToolTip(QString::fromUtf8(obs_module_text("VerticalVideoToggleTip")));
		vid->setText(QString::fromUtf8("V"));
		vid->setFixedSize(22, 22);
		vid->setChecked(vc->item_visible(name.toStdString()));
		vid->setStyleSheet(QStringLiteral(
			"QToolButton { color: #bbb; font-weight: bold; border: none; padding: 0; }"
			"QToolButton:checked { color: #6cf; }"
			"QToolButton:!checked { color: #666; }"));

		auto *aud = new QToolButton(row);
		aud->setObjectName("msrVertAudio");
		aud->setCheckable(true);
		aud->setAutoRaise(true);
		aud->setToolTip(QString::fromUtf8(obs_module_text("VerticalAudioToggleTip")));
		aud->setText(QString::fromUtf8("A"));
		aud->setFixedSize(22, 22);
		aud->setChecked(vc->item_audio_enabled(name.toStdString()));
		aud->setStyleSheet(QStringLiteral(
			"QToolButton { color: #bbb; font-weight: bold; border: none; padding: 0; }"
			"QToolButton:checked { color: #6c6; }"
			"QToolButton:!checked { color: #666; }"));

		/* After eye/lock — append at end of the row layout: H, V, A. */
		lay->addWidget(hid);
		lay->addWidget(vid);
		lay->addWidget(aud);

		row->setProperty("msrVerticalInjected", true);
		row->setProperty("msrSourceName", name);

		QObject::connect(hid, &QToolButton::toggled, row, [name](bool on) {
			auto *v = MsrVerticalCanvas::instance();
			if (v)
				v->set_item_hide_horizontal(name.toStdString(), on);
		});
		QObject::connect(vid, &QToolButton::toggled, row, [name](bool on) {
			auto *v = MsrVerticalCanvas::instance();
			if (v)
				v->set_item_visible(name.toStdString(), on);
		});
		QObject::connect(aud, &QToolButton::toggled, row, [name](bool on) {
			auto *v = MsrVerticalCanvas::instance();
			if (v)
				v->set_item_audio_enabled(name.toStdString(), on);
		});
	}
}

void MsrSourcesInject::inject_now()
{
	QMainWindow *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main)
		return;

	QListView *view = main->findChild<QListView *>("sources");
	if (!view) {
		QDockWidget *dock = main->findChild<QDockWidget *>("sourcesDock");
		if (dock)
			view = dock->findChild<QListView *>();
	}
	if (!view) {
		/* Last-resort: any QListView under a dock titled like Sources. */
		const auto docks = main->findChildren<QDockWidget *>();
		for (QDockWidget *d : docks) {
			const QString t = d->windowTitle();
			if (t.contains(QStringLiteral("Sources"), Qt::CaseInsensitive) ||
			    t.contains(QStringLiteral("Source"), Qt::CaseInsensitive)) {
				view = d->findChild<QListView *>();
				if (view)
					break;
			}
		}
	}
	if (!view)
		return;
	inject_into_view(view);
}
