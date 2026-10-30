/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-dock.hpp"
#include "msr-controller.hpp"
#include "plugin-support.h"

#include <obs.h>
#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSpinBox>
#include <QGroupBox>
#include <QScrollArea>
#include <QFileDialog>
#include <QFrame>

#include <string>

namespace {

struct SourceListCtx {
	QStringList *out;
	bool audio_only;
};

bool enum_sources_cb(void *data, obs_source_t *source)
{
	auto *ctx = static_cast<SourceListCtx *>(data);
	const uint32_t flags = obs_source_get_output_flags(source);
	if (ctx->audio_only) {
		if ((flags & OBS_SOURCE_AUDIO) == 0 && (flags & OBS_SOURCE_COMPOSITE) == 0)
			return true;
	} else {
		if ((flags & OBS_SOURCE_VIDEO) == 0)
			return true;
	}
	ctx->out->push_back(QString::fromUtf8(obs_source_get_name(source)));
	return true;
}

} // namespace

MsrDock::MsrDock(QWidget *parent) : QWidget(parent)
{
	build_ui();
	load_from_controller();
	if (auto *c = MsrController::instance()) {
		c->set_state_callback([this](bool recording) {
			QMetaObject::invokeMethod(this, "update_recording_state", Qt::QueuedConnection,
						  Q_ARG(bool, recording));
		});
		update_recording_state(c->recording());
	}
}

MsrDock::~MsrDock()
{
	if (auto *c = MsrController::instance())
		c->set_state_callback(nullptr);
}

void MsrDock::build_ui()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(8);

	auto *title = new QLabel(QString::fromUtf8(obs_module_text("DockTitle")), this);
	QFont f = title->font();
	f.setBold(true);
	f.setPointSize(f.pointSize() + 1);
	title->setFont(f);
	root->addWidget(title);

	auto *hint = new QLabel(QString::fromUtf8(obs_module_text("DockHint")), this);
	hint->setWordWrap(true);
	root->addWidget(hint);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *source_host = new QWidget(scroll);
	source_list_layout_ = new QVBoxLayout(source_host);
	source_list_layout_->setContentsMargins(0, 0, 0, 0);
	source_list_layout_->setSpacing(4);
	source_list_layout_->addStretch(1);
	scroll->setWidget(source_host);
	scroll->setMinimumHeight(140);
	root->addWidget(scroll, 1);

	auto *refresh = new QPushButton(QString::fromUtf8(obs_module_text("RefreshSources")), this);
	connect(refresh, &QPushButton::clicked, this, &MsrDock::refresh_sources);
	root->addWidget(refresh);

	auto *folder_row = new QHBoxLayout();
	folder_edit_ = new QLineEdit(this);
	folder_edit_->setPlaceholderText(QString::fromUtf8(obs_module_text("OutputFolder")));
	auto *browse = new QPushButton(QString::fromUtf8(obs_module_text("Browse")), this);
	connect(browse, &QPushButton::clicked, this, &MsrDock::on_browse_folder);
	folder_row->addWidget(folder_edit_, 1);
	folder_row->addWidget(browse);
	root->addLayout(folder_row);

	pattern_edit_ = new QLineEdit(this);
	pattern_edit_->setPlaceholderText(QStringLiteral("{source}_{orientation}_{date}_{time}"));
	root->addWidget(new QLabel(QString::fromUtf8(obs_module_text("FilenamePattern")), this));
	root->addWidget(pattern_edit_);

	auto *fmt_row = new QHBoxLayout();
	container_combo_ = new QComboBox(this);
	container_combo_->addItem(QStringLiteral("mkv"), QStringLiteral("mkv"));
	container_combo_->addItem(QStringLiteral("mp4"), QStringLiteral("mp4"));
	fmt_row->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Container")), this));
	fmt_row->addWidget(container_combo_, 1);
	root->addLayout(fmt_row);

	advanced_box_ = new QGroupBox(QString::fromUtf8(obs_module_text("Advanced")), this);
	advanced_box_->setCheckable(true);
	advanced_box_->setChecked(false);
	auto *adv = new QVBoxLayout(advanced_box_);

	encoder_combo_ = new QComboBox(advanced_box_);
	encoder_combo_->addItem(QString::fromUtf8(obs_module_text("EncoderAuto")), QStringLiteral("auto"));
	encoder_combo_->addItem(QStringLiteral("NVENC"), QStringLiteral("nvenc"));
	encoder_combo_->addItem(QStringLiteral("AMD AMF"), QStringLiteral("amf"));
	encoder_combo_->addItem(QStringLiteral("Intel QSV"), QStringLiteral("qsv"));
	encoder_combo_->addItem(QStringLiteral("x264"), QStringLiteral("x264"));
	adv->addWidget(new QLabel(QString::fromUtf8(obs_module_text("VideoEncoder")), advanced_box_));
	adv->addWidget(encoder_combo_);

	video_bitrate_ = new QSpinBox(advanced_box_);
	video_bitrate_->setRange(1000, 100000);
	video_bitrate_->setSuffix(QStringLiteral(" kbps"));
	adv->addWidget(new QLabel(QString::fromUtf8(obs_module_text("VideoBitrate")), advanced_box_));
	adv->addWidget(video_bitrate_);

	audio_bitrate_ = new QSpinBox(advanced_box_);
	audio_bitrate_->setRange(64, 512);
	audio_bitrate_->setSuffix(QStringLiteral(" kbps"));
	adv->addWidget(new QLabel(QString::fromUtf8(obs_module_text("AudioBitrate")), advanced_box_));
	adv->addWidget(audio_bitrate_);

	auto *vert_row = new QHBoxLayout();
	vert_w_ = new QSpinBox(advanced_box_);
	vert_w_->setRange(360, 2160);
	vert_h_ = new QSpinBox(advanced_box_);
	vert_h_->setRange(640, 3840);
	vert_row->addWidget(new QLabel(QString::fromUtf8(obs_module_text("VerticalRes")), advanced_box_));
	vert_row->addWidget(vert_w_);
	vert_row->addWidget(new QLabel(QStringLiteral("x"), advanced_box_));
	vert_row->addWidget(vert_h_);
	adv->addLayout(vert_row);

	root->addWidget(advanced_box_);

	multistream_box_ = new QGroupBox(QString::fromUtf8(obs_module_text("Multistream")), this);
	multistream_box_->setCheckable(true);
	multistream_box_->setChecked(false);
	auto *ms = new QVBoxLayout(multistream_box_);
	auto *ms_hint = new QLabel(QString::fromUtf8(obs_module_text("MultistreamHint")), multistream_box_);
	ms_hint->setWordWrap(true);
	ms->addWidget(ms_hint);
	dest_list_layout_ = new QVBoxLayout();
	dest_list_layout_->setContentsMargins(0, 0, 0, 0);
	dest_list_layout_->setSpacing(4);
	ms->addLayout(dest_list_layout_);
	auto *add_dest = new QPushButton(QString::fromUtf8(obs_module_text("AddDestination")), multistream_box_);
	connect(add_dest, &QPushButton::clicked, this, &MsrDock::on_add_destination);
	ms->addWidget(add_dest);
	stream_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("StartStreaming")), multistream_box_);
	connect(stream_btn_, &QPushButton::clicked, this, &MsrDock::on_start_stop_stream);
	ms->addWidget(stream_btn_);
	stream_status_label_ = new QLabel(QString::fromUtf8(obs_module_text("StatusStreamIdle")), multistream_box_);
	ms->addWidget(stream_status_label_);
	root->addWidget(multistream_box_);

	auto *btn_row = new QHBoxLayout();
	auto *save_btn = new QPushButton(QString::fromUtf8(obs_module_text("Save")), this);
	connect(save_btn, &QPushButton::clicked, this, &MsrDock::on_save);
	start_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("StartRecording")), this);
	connect(start_btn_, &QPushButton::clicked, this, &MsrDock::on_start_stop);
	pause_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("PauseRecording")), this);
	connect(pause_btn_, &QPushButton::clicked, this, &MsrDock::on_pause_resume);
	pause_btn_->setEnabled(false);
	chapter_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("AddChapter")), this);
	connect(chapter_btn_, &QPushButton::clicked, this, &MsrDock::on_add_chapter);
	chapter_btn_->setEnabled(false);
	btn_row->addWidget(save_btn);
	btn_row->addWidget(start_btn_, 1);
	btn_row->addWidget(pause_btn_);
	btn_row->addWidget(chapter_btn_);
	root->addLayout(btn_row);

	status_label_ = new QLabel(QString::fromUtf8(obs_module_text("StatusIdle")), this);
	root->addWidget(status_label_);
}

QStringList MsrDock::enumerate_sources() const
{
	QStringList out;
	SourceListCtx ctx{&out, false};
	obs_enum_sources(enum_sources_cb, &ctx);
	out.sort(Qt::CaseInsensitive);
	return out;
}

QStringList MsrDock::enumerate_audio_sources() const
{
	QStringList out;
	out.push_back(QString());
	SourceListCtx ctx{&out, true};
	obs_enum_sources(enum_sources_cb, &ctx);
	return out;
}

void MsrDock::rebuild_source_list()
{
	while (QLayoutItem *item = source_list_layout_->takeAt(0)) {
		if (QWidget *w = item->widget())
			w->deleteLater();
		delete item;
	}
	rows_.clear();

	MsrSettings settings;
	if (auto *c = MsrController::instance())
		settings = c->settings();

	QStringList names = enumerate_sources();
	QStringList audio_names = enumerate_audio_sources();

	for (const QString &name : names) {
		auto *row = new QWidget(this);
		auto *hl = new QHBoxLayout(row);
		hl->setContentsMargins(0, 0, 0, 0);

		MsrSourceRow sr;
		sr.source_name = name;
		sr.enabled = new QCheckBox(row);
		sr.name = new QLabel(name, row);
		sr.orientation = new QComboBox(row);
		sr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationHorizontal")),
					(int)MsrOrientation::Horizontal);
		sr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationVertical")),
					(int)MsrOrientation::Vertical);
		sr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationBoth")),
					(int)MsrOrientation::Both);
		sr.extra_audio = new QComboBox(row);
		sr.extra_audio->addItem(QString::fromUtf8(obs_module_text("ExtraAudioNone")), QString());
		for (const QString &a : audio_names) {
			if (a.isEmpty() || a == name)
				continue; /* never offer the row's own source as extra */
			sr.extra_audio->addItem(a, a);
		}

		for (const auto &cfg : settings.sources) {
			if (QString::fromStdString(cfg.source_name) == name) {
				sr.enabled->setChecked(cfg.enabled);
				int oi = sr.orientation->findData((int)cfg.orientation);
				if (oi >= 0)
					sr.orientation->setCurrentIndex(oi);
				int ai = sr.extra_audio->findData(QString::fromStdString(cfg.extra_audio_source));
				if (ai >= 0)
					sr.extra_audio->setCurrentIndex(ai);
				break;
			}
		}

		hl->addWidget(sr.enabled);
		hl->addWidget(sr.name, 1);
		hl->addWidget(sr.orientation);
		hl->addWidget(sr.extra_audio);
		source_list_layout_->addWidget(row);
		rows_.push_back(sr);
	}
	source_list_layout_->addStretch(1);
}

void MsrDock::rebuild_dest_list()
{
	if (!dest_list_layout_)
		return;
	while (QLayoutItem *item = dest_list_layout_->takeAt(0)) {
		if (QWidget *w = item->widget())
			w->deleteLater();
		delete item;
	}
	dest_rows_.clear();

	MsrSettings settings;
	if (auto *c = MsrController::instance())
		settings = c->settings();

	for (const auto &d : settings.destinations) {
		auto *row = new QWidget(multistream_box_);
		auto *vl = new QVBoxLayout(row);
		vl->setContentsMargins(0, 0, 0, 0);
		vl->setSpacing(2);
		auto *top = new QWidget(row);
		auto *hl = new QHBoxLayout(top);
		hl->setContentsMargins(0, 0, 0, 0);

		MsrDestRow dr;
		dr.enabled = new QCheckBox(top);
		dr.enabled->setChecked(d.enabled);
		dr.name = new QLineEdit(QString::fromStdString(d.name), top);
		dr.name->setPlaceholderText(QString::fromUtf8(obs_module_text("DestinationName")));
		dr.orientation = new QComboBox(top);
		dr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationHorizontal")),
					(int)MsrOrientation::Horizontal);
		dr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationVertical")),
					(int)MsrOrientation::Vertical);
		int oi = dr.orientation->findData((int)d.orientation);
		if (oi >= 0)
			dr.orientation->setCurrentIndex(oi);
		dr.server = new QLineEdit(QString::fromStdString(d.server_url), top);
		dr.server->setPlaceholderText(QString::fromUtf8(obs_module_text("DestinationServer")));
		dr.key = new QLineEdit(top);
		dr.key->setEchoMode(QLineEdit::Password);
		dr.key->setPlaceholderText(QString::fromUtf8(obs_module_text("DestinationKey")));
		/* Keep existing key unless the user types a replacement. */
		dr.key->setText(QString::fromStdString(d.stream_key));

		hl->addWidget(dr.enabled);
		hl->addWidget(dr.name, 1);
		hl->addWidget(dr.orientation);
		hl->addWidget(dr.server, 2);
		hl->addWidget(dr.key, 1);

		auto *tracks = new QWidget(row);
		auto *tl = new QHBoxLayout(tracks);
		tl->setContentsMargins(0, 0, 0, 0);
		tl->setSpacing(4);
		tl->addWidget(new QLabel(QString::fromUtf8(obs_module_text("AudioTracks")), tracks));
		const uint32_t mask = d.audio_track_mask ? d.audio_track_mask : 1u;
		for (int i = 0; i < 6; i++) {
			dr.track_checks[i] = new QCheckBox(QString::number(i + 1), tracks);
			dr.track_checks[i]->setChecked((mask & (1u << i)) != 0);
			dr.track_checks[i]->setToolTip(QString::fromUtf8(obs_module_text("AudioTrackTooltip")));
			tl->addWidget(dr.track_checks[i]);
		}
		tl->addStretch(1);

		vl->addWidget(top);
		vl->addWidget(tracks);
		dest_list_layout_->addWidget(row);
		dest_rows_.push_back(dr);
	}
}

void MsrDock::load_from_controller()
{
	auto *c = MsrController::instance();
	if (!c)
		return;
	MsrSettings s = c->settings();
	folder_edit_->setText(QString::fromStdString(s.output_folder));
	pattern_edit_->setText(QString::fromStdString(s.filename_pattern));
	int ci = container_combo_->findData(QString::fromStdString(s.container));
	if (ci >= 0)
		container_combo_->setCurrentIndex(ci);
	int ei = encoder_combo_->findData(QString::fromStdString(s.video_encoder));
	if (ei >= 0)
		encoder_combo_->setCurrentIndex(ei);
	video_bitrate_->setValue(s.video_bitrate_kbps);
	audio_bitrate_->setValue(s.audio_bitrate_kbps);
	vert_w_->setValue(s.vertical_width);
	vert_h_->setValue(s.vertical_height);
	rebuild_source_list();
	rebuild_dest_list();
	if (stream_btn_) {
		const bool streaming = c->streaming();
		stream_btn_->setText(
			QString::fromUtf8(obs_module_text(streaming ? "StopStreaming" : "StartStreaming")));
		if (stream_status_label_) {
			stream_status_label_->setText(
				QString::fromUtf8(obs_module_text(streaming ? "StatusStreaming" : "StatusStreamIdle")));
		}
	}
}

void MsrDock::apply_to_controller()
{
	auto *c = MsrController::instance();
	if (!c)
		return;
	MsrSettings s = c->settings();
	s.output_folder = folder_edit_->text().trimmed().toStdString();
	s.filename_pattern = pattern_edit_->text().trimmed().toStdString();
	if (s.filename_pattern.empty())
		s.filename_pattern = "{source}_{orientation}_{date}_{time}";
	s.container = container_combo_->currentData().toString().toStdString();
	s.video_encoder = encoder_combo_->currentData().toString().toStdString();
	s.video_bitrate_kbps = video_bitrate_->value();
	s.audio_bitrate_kbps = audio_bitrate_->value();
	s.vertical_width = vert_w_->value();
	s.vertical_height = vert_h_->value();
	s.sources.clear();
	for (const auto &row : rows_) {
		MsrSourceConfig cfg;
		cfg.source_name = row.source_name.toStdString();
		cfg.enabled = row.enabled->isChecked();
		cfg.orientation = static_cast<MsrOrientation>(row.orientation->currentData().toInt());
		cfg.extra_audio_source = row.extra_audio->currentData().toString().toStdString();
		if (cfg.extra_audio_source == cfg.source_name)
			cfg.extra_audio_source.clear();
		s.sources.push_back(cfg);
	}
	s.destinations.clear();
	for (const auto &row : dest_rows_) {
		MsrStreamDestination d;
		d.name = row.name->text().trimmed().toStdString();
		d.enabled = row.enabled->isChecked();
		d.orientation = static_cast<MsrOrientation>(row.orientation->currentData().toInt());
		d.server_url = row.server->text().trimmed().toStdString();
		d.stream_key = row.key->text().toStdString();
		uint32_t mask = 0;
		for (int i = 0; i < 6; i++) {
			if (row.track_checks[i] && row.track_checks[i]->isChecked())
				mask |= (1u << i);
		}
		d.audio_track_mask = mask ? mask : 1u;
		if (!d.name.empty())
			s.destinations.push_back(d);
	}
	c->set_settings(s);
}

void MsrDock::refresh_sources()
{
	/* Reload from controller (e.g. after profile change) without overwriting. */
	load_from_controller();
}

void MsrDock::on_browse_folder()
{
	QString dir = QFileDialog::getExistingDirectory(this, QString::fromUtf8(obs_module_text("OutputFolder")),
							folder_edit_->text());
	if (!dir.isEmpty())
		folder_edit_->setText(dir);
}

void MsrDock::on_save()
{
	apply_to_controller();
	status_label_->setText(QString::fromUtf8(obs_module_text("StatusSaved")));
}

void MsrDock::on_start_stop()
{
	auto *c = MsrController::instance();
	if (!c)
		return;
	apply_to_controller();
	if (c->recording())
		c->stop_all();
	else
		c->start_all();
}

void MsrDock::on_pause_resume()
{
	auto *c = MsrController::instance();
	if (!c || !c->recording())
		return;
	c->toggle_pause();
	update_recording_state(true);
}

void MsrDock::on_add_chapter()
{
	auto *c = MsrController::instance();
	if (!c || !c->recording() || c->paused())
		return;
	if (c->add_chapter({}))
		status_label_->setText(QString::fromUtf8(obs_module_text("StatusChapterAdded")));
}

void MsrDock::update_recording_state(bool recording)
{
	auto *c = MsrController::instance();
	const bool paused = recording && c && c->paused();
	if (pause_btn_) {
		pause_btn_->setEnabled(recording);
		pause_btn_->setText(QString::fromUtf8(obs_module_text(paused ? "ResumeRecording" : "PauseRecording")));
	}
	if (recording) {
		start_btn_->setText(QString::fromUtf8(obs_module_text("StopRecording")));
		status_label_->setText(QString::fromUtf8(obs_module_text(paused ? "StatusPaused" : "StatusRecording")));
	} else {
		start_btn_->setText(QString::fromUtf8(obs_module_text("StartRecording")));
		status_label_->setText(QString::fromUtf8(obs_module_text("StatusIdle")));
	}
}

void MsrDock::on_add_destination()
{
	apply_to_controller();
	auto *c = MsrController::instance();
	if (!c)
		return;
	MsrSettings s = c->settings();
	MsrStreamDestination d;
	d.name = "dest" + std::to_string(s.destinations.size() + 1);
	d.enabled = false;
	d.orientation = MsrOrientation::Horizontal;
	d.server_url = "rtmp://127.0.0.1:1935/live";
	s.destinations.push_back(d);
	c->set_settings(s);
	rebuild_dest_list();
}

void MsrDock::on_start_stop_stream()
{
	auto *c = MsrController::instance();
	if (!c)
		return;
	apply_to_controller();
	if (c->streaming())
		c->stop_streams();
	else
		c->start_streams();
	const bool streaming = c->streaming();
	stream_btn_->setText(QString::fromUtf8(obs_module_text(streaming ? "StopStreaming" : "StartStreaming")));
	if (stream_status_label_) {
		stream_status_label_->setText(
			QString::fromUtf8(obs_module_text(streaming ? "StatusStreaming" : "StatusStreamIdle")));
	}
}
