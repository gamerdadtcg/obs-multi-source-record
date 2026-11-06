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
#include "msr-theme.hpp"
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
#include <QScrollArea>
#include <QFileDialog>
#include <QFrame>
#include <QSizePolicy>
#include <util/config-file.h>
#include <QByteArray>
#include <QDockWidget>
#include <QHideEvent>
#include <QShowEvent>
#include <QSizeGrip>
#include <QToolButton>
#include <QObject>
#include <QTimer>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QStyle>
#include <QSet>

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

struct SectionWidgets {
	QWidget *section = nullptr;
	QToolButton *toggle = nullptr;
	QWidget *body = nullptr;
};

SectionWidgets make_collapsible(QWidget *parent, const QString &title, bool default_open)
{
	SectionWidgets sw;
	sw.section = new QWidget(parent);
	auto *vl = new QVBoxLayout(sw.section);
	vl->setContentsMargins(0, 0, 0, 0);
	vl->setSpacing(2);

	sw.toggle = new QToolButton(sw.section);
	sw.toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
	sw.toggle->setArrowType(Qt::NoArrow);
	sw.toggle->setCheckable(true);
	sw.toggle->setChecked(default_open);
	sw.toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	sw.toggle->setStyleSheet(QString());
	auto set_header = [toggle = sw.toggle, title](bool on) {
		toggle->setText(QStringLiteral("%1  %2").arg(on ? QStringLiteral("▾") : QStringLiteral("▸"), title));
	};
	set_header(default_open);
	vl->addWidget(sw.toggle);

	sw.body = new QWidget(sw.section);
	sw.body->setVisible(default_open);
	auto *body_l = new QVBoxLayout(sw.body);
	body_l->setContentsMargins(12, 2, 0, 4);
	body_l->setSpacing(6);
	vl->addWidget(sw.body);

	QObject::connect(sw.toggle, &QToolButton::toggled, sw.section, [set_header, body = sw.body](bool on) {
		body->setVisible(on);
		set_header(on);
	});
	return sw;
}

} // namespace

MsrDock::MsrDock(QWidget *parent) : QWidget(parent)
{
	build_ui();
	status_timer_ = new QTimer(this);
	status_timer_->setInterval(1000);
	connect(status_timer_, &QTimer::timeout, this, &MsrDock::refresh_status_labels);
	status_timer_->start();
	load_from_controller();
	if (auto *c = MsrController::instance()) {
		c->set_state_callback([this](bool recording) {
			QMetaObject::invokeMethod(this, "update_recording_state", Qt::QueuedConnection,
						  Q_ARG(bool, recording));
		});
		c->set_source_record_state_callback([this](bool active) {
			QMetaObject::invokeMethod(this, "update_source_record_state", Qt::QueuedConnection,
						  Q_ARG(bool, active));
		});
		update_recording_state(c->recording());
		update_source_record_state(c->source_recording());
	}
	refresh_status_labels();
}

MsrDock::~MsrDock()
{
	if (auto *c = MsrController::instance()) {
		c->set_state_callback(nullptr);
		c->set_source_record_state_callback(nullptr);
	}
}

void MsrDock::build_ui()
{
	setObjectName(QStringLiteral("MsrDock"));
	setMinimumSize(320, 480);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	apply_theme();

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	/* ---- Scrollable sections ---- */
	outer_scroll_ = new QScrollArea(this);
	outer_scroll_->setWidgetResizable(true);
	outer_scroll_->setFrameShape(QFrame::NoFrame);
	outer_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	outer_scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	outer_scroll_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	auto *content = new QWidget(outer_scroll_);
	content->setObjectName(QStringLiteral("msrRoot"));
	content->setMinimumWidth(280);
	auto *root = new QVBoxLayout(content);
	root->setContentsMargins(10, 10, 10, 10);
	root->setSpacing(10);

	auto *header = new QLabel(QString::fromUtf8(obs_module_text("DockHeaderTitle")), content);
	header->setObjectName(QStringLiteral("msrHeaderTitle"));
	root->addWidget(header);

	/* Source Record picker card */
	auto *sr_card = new QWidget(content);
	sr_card->setObjectName(QStringLiteral("msrCard"));
	auto *sr_card_l = new QVBoxLayout(sr_card);
	sr_card_l->setContentsMargins(10, 10, 10, 10);
	sr_card_l->setSpacing(6);
	auto *sr_cap = new QLabel(QString::fromUtf8(obs_module_text("SourceRecordPickCaption")), sr_card);
	sr_cap->setObjectName(QStringLiteral("msrCardCaption"));
	sr_card_l->addWidget(sr_cap);
	auto *sr_header = new QHBoxLayout();
	sr_header->addStretch(1);
	auto *refresh = new QPushButton(QString::fromUtf8(obs_module_text("RefreshSources")), sr_card);
	connect(refresh, &QPushButton::clicked, this, &MsrDock::refresh_sources);
	sr_header->addWidget(refresh);
	sr_card_l->addLayout(sr_header);
	sr_pick_layout_ = new QVBoxLayout();
	sr_pick_layout_->setContentsMargins(0, 0, 0, 0);
	sr_pick_layout_->setSpacing(4);
	sr_card_l->addLayout(sr_pick_layout_);
	root->addWidget(sr_card);

	/* REC + timer hero */
	rec_hero_ = new QWidget(content);
	rec_hero_->setObjectName(QStringLiteral("msrCard"));
	auto *hero_l = new QHBoxLayout(rec_hero_);
	hero_l->setContentsMargins(14, 12, 14, 12);
	hero_l->setSpacing(12);
	rec_dot_label_ = new QLabel(QStringLiteral("●"), rec_hero_);
	rec_dot_label_->setObjectName(QStringLiteral("msrRecDot"));
	rec_text_label_ = new QLabel(QStringLiteral("REC"), rec_hero_);
	rec_text_label_->setObjectName(QStringLiteral("msrRecText"));
	auto *sep = new QFrame(rec_hero_);
	sep->setObjectName(QStringLiteral("msrVSep"));
	sep->setFrameShape(QFrame::VLine);
	rec_timer_label_ = new QLabel(QStringLiteral("00:00:00"), rec_hero_);
	rec_timer_label_->setObjectName(QStringLiteral("msrTimer"));
	hero_l->addWidget(rec_dot_label_);
	hero_l->addWidget(rec_text_label_);
	hero_l->addWidget(sep);
	hero_l->addWidget(rec_timer_label_, 1);
	root->addWidget(rec_hero_);

	/* Output files card */
	auto *files_card = new QWidget(content);
	files_card->setObjectName(QStringLiteral("msrCard"));
	auto *files_l = new QVBoxLayout(files_card);
	files_l->setContentsMargins(10, 10, 10, 10);
	files_l->setSpacing(6);
	auto *files_cap = new QLabel(QString::fromUtf8(obs_module_text("OutputFilesCaption")), files_card);
	files_cap->setObjectName(QStringLiteral("msrCardCaption"));
	files_l->addWidget(files_cap);
	output_files_layout_ = new QVBoxLayout();
	output_files_layout_->setContentsMargins(0, 0, 0, 0);
	output_files_layout_->setSpacing(6);
	files_l->addLayout(output_files_layout_);

	auto add_section = [this, content, root](const QString &title, const char *key,
						 bool default_open) -> QWidget * {
		SectionWidgets sw = make_collapsible(content, title, default_open);
		sections_.push_back(SectionRef{sw.toggle, sw.body, key, default_open});
		auto *wrap = new QWidget(content);
		wrap->setObjectName(QStringLiteral("msrCard"));
		auto *wl = new QVBoxLayout(wrap);
		wl->setContentsMargins(8, 6, 8, 8);
		wl->setSpacing(4);
		wl->addWidget(sw.section);
		root->addWidget(wrap);
		return sw.body;
	};

	/* Output — default open */
	auto *output_body = add_section(QString::fromUtf8(obs_module_text("Output")), "SectionOutput", true);
	auto *output_l = qobject_cast<QVBoxLayout *>(output_body->layout());
	auto *folder_row = new QHBoxLayout();
	folder_edit_ = new QLineEdit(output_body);
	folder_edit_->setPlaceholderText(QString::fromUtf8(obs_module_text("OutputFolder")));
	auto *browse = new QPushButton(QString::fromUtf8(obs_module_text("Browse")), output_body);
	connect(browse, &QPushButton::clicked, this, &MsrDock::on_browse_folder);
	connect(folder_edit_, &QLineEdit::editingFinished, this, &MsrDock::refresh_output_files);
	folder_row->addWidget(folder_edit_, 1);
	folder_row->addWidget(browse);
	output_l->addLayout(folder_row);
	output_l->addWidget(new QLabel(QString::fromUtf8(obs_module_text("FilenamePattern")), output_body));
	pattern_edit_ = new QLineEdit(output_body);
	pattern_edit_->setPlaceholderText(QStringLiteral("{source} {date} {time}"));
	output_l->addWidget(pattern_edit_);
	auto *fmt_row = new QHBoxLayout();
	container_combo_ = new QComboBox(output_body);
	container_combo_->addItem(QStringLiteral("mkv"), QStringLiteral("mkv"));
	container_combo_->addItem(QStringLiteral("mp4"), QStringLiteral("mp4"));
	fmt_row->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Container")), output_body));
	connect(container_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&MsrDock::refresh_output_files);
	fmt_row->addWidget(container_combo_, 1);
	output_l->addLayout(fmt_row);
	/* Last-recorded ISOs stay with the shared Output settings at the bottom. */
	root->addWidget(files_card);

	/* Recording options — default closed */
	auto *opts_body =
		add_section(QString::fromUtf8(obs_module_text("RecordingOptions")), "SectionRecording", false);
	auto *opts_l = qobject_cast<QVBoxLayout *>(opts_body->layout());
	auto_start_combo_ = new QComboBox(opts_body);
	auto_start_combo_->addItem(QString::fromUtf8(obs_module_text("AutoStartOff")), (int)MsrAutoStartTrigger::Off);
	auto_start_combo_->addItem(QString::fromUtf8(obs_module_text("AutoStartRecord")),
				   (int)MsrAutoStartTrigger::Record);
	auto_start_combo_->addItem(QString::fromUtf8(obs_module_text("AutoStartStream")),
				   (int)MsrAutoStartTrigger::Stream);
	auto_start_combo_->addItem(QString::fromUtf8(obs_module_text("AutoStartBoth")), (int)MsrAutoStartTrigger::Both);
	opts_l->addWidget(new QLabel(QString::fromUtf8(obs_module_text("AutoStart")), opts_body));
	opts_l->addWidget(auto_start_combo_);
	auto *as_hint = new QLabel(QString::fromUtf8(obs_module_text("AutoStartHint")), opts_body);
	as_hint->setWordWrap(true);
	opts_l->addWidget(as_hint);
	split_mode_combo_ = new QComboBox(opts_body);
	split_mode_combo_->addItem(QString::fromUtf8(obs_module_text("SplitOff")), (int)MsrSplitMode::Off);
	split_mode_combo_->addItem(QString::fromUtf8(obs_module_text("SplitByTime")), (int)MsrSplitMode::ByTime);
	split_mode_combo_->addItem(QString::fromUtf8(obs_module_text("SplitBySize")), (int)MsrSplitMode::BySize);
	opts_l->addWidget(new QLabel(QString::fromUtf8(obs_module_text("FileSplit")), opts_body));
	opts_l->addWidget(split_mode_combo_);
	connect(split_mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&MsrDock::on_split_mode_changed);
	auto *split_row = new QHBoxLayout();
	split_time_label_ = new QLabel(QString::fromUtf8(obs_module_text("SplitTimeMin")), opts_body);
	split_time_min_ = new QSpinBox(opts_body);
	split_time_min_->setRange(1, 24 * 60);
	split_time_min_->setSuffix(QStringLiteral(" min"));
	split_size_label_ = new QLabel(QString::fromUtf8(obs_module_text("SplitSizeMb")), opts_body);
	split_size_mb_ = new QSpinBox(opts_body);
	split_size_mb_->setRange(50, 100000);
	split_size_mb_->setSuffix(QStringLiteral(" MB"));
	split_row->addWidget(split_time_label_);
	split_row->addWidget(split_time_min_);
	split_row->addWidget(split_size_label_);
	split_row->addWidget(split_size_mb_);
	split_row->addStretch(1);
	opts_l->addLayout(split_row);

	auto *sr_sep = new QLabel(QString::fromUtf8(obs_module_text("SourceRecordSection")), opts_body);
	sr_sep->setStyleSheet(QStringLiteral("font-weight: 600; margin-top: 6px;"));
	opts_l->addWidget(sr_sep);
	auto *sr_hint = new QLabel(QString::fromUtf8(obs_module_text("SourceRecordHint")), opts_body);
	sr_hint->setWordWrap(true);
	opts_l->addWidget(sr_hint);
	opts_l->addWidget(new QLabel(QString::fromUtf8(obs_module_text("SourceRecordFilter")), opts_body));
	source_record_filter_edit_ = new QLineEdit(opts_body);
	source_record_filter_edit_->setPlaceholderText(QStringLiteral("Source Record"));
	opts_l->addWidget(source_record_filter_edit_);
	opts_l->addWidget(new QLabel(QString::fromUtf8(obs_module_text("SourceRecordSources")), opts_body));
	source_record_sources_edit_ = new QLineEdit(opts_body);
	source_record_sources_edit_->setPlaceholderText(
		QString::fromUtf8(obs_module_text("SourceRecordSourcesPlaceholder")));
	source_record_sources_edit_->setVisible(false);
	opts_l->addWidget(source_record_sources_edit_);

	/* Clipping (replay buffer) — default closed */
	auto *replay_body = add_section(QString::fromUtf8(obs_module_text("ReplayBuffer")), "SectionReplay", false);
	auto *replay_l = qobject_cast<QVBoxLayout *>(replay_body->layout());
	auto *replay_row = new QHBoxLayout();
	replay_seconds_ = new QSpinBox(replay_body);
	replay_seconds_->setRange(5, 300);
	replay_seconds_->setSuffix(QStringLiteral(" s"));
	replay_row->addWidget(new QLabel(QString::fromUtf8(obs_module_text("ReplaySeconds")), replay_body));
	replay_row->addWidget(replay_seconds_);
	replay_row->addStretch(1);
	replay_l->addLayout(replay_row);
	auto *rb_hint = new QLabel(QString::fromUtf8(obs_module_text("ReplayHint")), replay_body);
	rb_hint->setWordWrap(true);
	replay_l->addWidget(rb_hint);
	clipping_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("StartClipping")), replay_body);
	connect(clipping_btn_, &QPushButton::clicked, this, &MsrDock::on_start_stop_clipping);
	replay_l->addWidget(clipping_btn_);

	/* Advanced — default closed */
	advanced_box_ = add_section(QString::fromUtf8(obs_module_text("Advanced")), "SectionAdvanced", false);
	auto *adv = qobject_cast<QVBoxLayout *>(advanced_box_->layout());
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

	/* Multistream — default closed */
	multistream_box_ = add_section(QString::fromUtf8(obs_module_text("Multistream")), "SectionMultistream", false);
	auto *ms = qobject_cast<QVBoxLayout *>(multistream_box_->layout());
	auto *ms_hint = new QLabel(QString::fromUtf8(obs_module_text("MultistreamHint")), multistream_box_);
	ms_hint->setWordWrap(true);
	ms->addWidget(ms_hint);
	dest_list_layout_ = new QVBoxLayout();
	dest_list_layout_->setContentsMargins(0, 0, 0, 0);
	dest_list_layout_->setSpacing(8);
	ms->addLayout(dest_list_layout_);
	auto *add_dest = new QPushButton(QString::fromUtf8(obs_module_text("AddDestination")), multistream_box_);
	connect(add_dest, &QPushButton::clicked, this, &MsrDock::on_add_destination);
	ms->addWidget(add_dest);
	stream_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("StartStreaming")), multistream_box_);
	connect(stream_btn_, &QPushButton::clicked, this, &MsrDock::on_start_stop_stream);
	ms->addWidget(stream_btn_);
	stream_status_label_ = new QLabel(QString::fromUtf8(obs_module_text("StatusStreamIdle")), multistream_box_);
	stream_status_label_->setWordWrap(true);
	ms->addWidget(stream_status_label_);

	root->addStretch(1);
	outer_scroll_->setWidget(content);
	outer->addWidget(outer_scroll_, 1);

	/* ---- Always-visible action bar ---- */
	auto *footer = new QWidget(this);
	auto *footer_l = new QVBoxLayout(footer);
	footer_l->setContentsMargins(8, 6, 8, 6);
	footer_l->setSpacing(4);

	auto *sr_row = new QHBoxLayout();
	source_record_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("StartSourceRecord")), footer);
	source_record_btn_->setObjectName(QStringLiteral("msrPrimaryBtn"));
	connect(source_record_btn_, &QPushButton::clicked, this, &MsrDock::on_toggle_source_record);
	sr_row->addWidget(source_record_btn_, 1);
	footer_l->addLayout(sr_row);

	auto *btn_row = new QHBoxLayout();
	auto *save_btn = new QPushButton(QString::fromUtf8(obs_module_text("Save")), footer);
	connect(save_btn, &QPushButton::clicked, this, &MsrDock::on_save);
	start_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("StartRecording")), footer);
	connect(start_btn_, &QPushButton::clicked, this, &MsrDock::on_start_stop);
	pause_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("PauseRecording")), footer);
	connect(pause_btn_, &QPushButton::clicked, this, &MsrDock::on_pause_resume);
	pause_btn_->setEnabled(false);
	chapter_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("AddChapter")), footer);
	connect(chapter_btn_, &QPushButton::clicked, this, &MsrDock::on_add_chapter);
	chapter_btn_->setEnabled(false);
	replay_btn_ = new QPushButton(QString::fromUtf8(obs_module_text("SaveReplay")), footer);
	connect(replay_btn_, &QPushButton::clicked, this, &MsrDock::on_save_replay);
	btn_row->addWidget(save_btn);
	btn_row->addWidget(start_btn_, 1);
	btn_row->addWidget(pause_btn_);
	btn_row->addWidget(chapter_btn_);
	btn_row->addWidget(replay_btn_);
	footer_l->addLayout(btn_row);

	status_label_ = new QLabel(QString::fromUtf8(obs_module_text("StatusIdle")), footer);
	status_label_->setWordWrap(true);
	footer_l->addWidget(status_label_);
	source_record_status_label_ = new QLabel(QString::fromUtf8(obs_module_text("StatusSourceRecordIdle")), footer);
	source_record_status_label_->setWordWrap(true);
	footer_l->addWidget(source_record_status_label_);
	replay_status_label_ = new QLabel(QString::fromUtf8(obs_module_text("StatusReplayIdle")), footer);
	replay_status_label_->setWordWrap(true);
	footer_l->addWidget(replay_status_label_);

	auto *grip_row = new QHBoxLayout();
	grip_row->addStretch(1);
	grip_row->addWidget(new QSizeGrip(footer), 0, Qt::AlignBottom | Qt::AlignRight);
	footer_l->addLayout(grip_row);

	outer->addWidget(footer, 0);
}

QStringList MsrDock::enumerate_sources() const
{
	QStringList out;
	SourceListCtx ctx{&out, false};
	obs_enum_sources(enum_sources_cb, &ctx);
	out.removeIf([](const QString &name) { return name.startsWith("msr_iso_audio_"); });
	out.sort(Qt::CaseInsensitive);
	return out;
}

QStringList MsrDock::enumerate_audio_sources() const
{
	QStringList out;
	out.push_back(QString());
	SourceListCtx ctx{&out, true};
	obs_enum_sources(enum_sources_cb, &ctx);
	out.removeAll("Source audio");
	out.removeIf([](const QString &name) { return name.startsWith("msr_iso_audio_"); });
	return out;
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

	for (int di = 0; di < (int)settings.destinations.size(); di++) {
		const auto &d = settings.destinations[(size_t)di];

		auto *card = new QFrame(multistream_box_);
		card->setFrameShape(QFrame::StyledPanel);
		card->setFrameShadow(QFrame::Raised);
		card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
		auto *vl = new QVBoxLayout(card);
		vl->setContentsMargins(8, 8, 8, 8);
		vl->setSpacing(6);

		MsrDestRow dr;
		/* Row: enable + name + remove (wraps cleanly at narrow widths). */
		auto *top = new QWidget(card);
		auto *hl = new QHBoxLayout(top);
		hl->setContentsMargins(0, 0, 0, 0);
		hl->setSpacing(6);
		dr.enabled = new QCheckBox(top);
		dr.enabled->setChecked(d.enabled);
		dr.enabled->setToolTip(QString::fromUtf8(obs_module_text("DestinationEnabled")));
		dr.name = new QLineEdit(QString::fromStdString(d.name), top);
		dr.name->setPlaceholderText(QString::fromUtf8(obs_module_text("DestinationName")));
		dr.name->setMinimumWidth(80);
		dr.remove = new QPushButton(QString::fromUtf8(obs_module_text("RemoveDestination")), top);
		dr.remove->setProperty("destIndex", di);
		connect(dr.remove, &QPushButton::clicked, this, &MsrDock::on_remove_destination);
		hl->addWidget(dr.enabled);
		hl->addWidget(dr.name, 1);
		hl->addWidget(dr.remove);
		vl->addWidget(top);

		/* Canvas on its own row so it never clips against name/remove. */
		dr.orientation = new QComboBox(card);
		dr.orientation->setMinimumWidth(120);
		dr.orientation->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		dr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationHorizontal")),
					(int)MsrOrientation::Horizontal);
		dr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationVertical")),
					(int)MsrOrientation::Vertical);
		int oi = dr.orientation->findData((int)d.orientation);
		if (oi >= 0)
			dr.orientation->setCurrentIndex(oi);
		vl->addWidget(dr.orientation);

		dr.server = new QLineEdit(QString::fromStdString(d.server_url), card);
		dr.server->setPlaceholderText(QString::fromUtf8(obs_module_text("DestinationServer")));
		dr.server->setMinimumWidth(120);
		dr.server->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

		dr.key = new QLineEdit(card);
		dr.key->setEchoMode(QLineEdit::Password);
		dr.key->setPlaceholderText(QString::fromUtf8(obs_module_text("DestinationKey")));
		dr.key->setText(QString::fromStdString(d.stream_key));
		dr.key->setMinimumWidth(120);
		dr.key->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

		auto *tracks = new QWidget(card);
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

		vl->addWidget(new QLabel(QString::fromUtf8(obs_module_text("DestinationServer")), card));
		vl->addWidget(dr.server);
		vl->addWidget(new QLabel(QString::fromUtf8(obs_module_text("DestinationKey")), card));
		vl->addWidget(dr.key);
		vl->addWidget(tracks);
		dest_list_layout_->addWidget(card);
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
	if (auto_start_combo_) {
		int ai = auto_start_combo_->findData((int)s.auto_start);
		if (ai >= 0)
			auto_start_combo_->setCurrentIndex(ai);
	}
	if (replay_seconds_)
		replay_seconds_->setValue(s.replay_seconds);
	if (split_mode_combo_) {
		int si = split_mode_combo_->findData((int)s.split_mode);
		if (si >= 0)
			split_mode_combo_->setCurrentIndex(si);
	}
	if (split_time_min_)
		split_time_min_->setValue(s.split_time_min);
	if (split_size_mb_)
		split_size_mb_->setValue(s.split_size_mb);
	if (source_record_filter_edit_)
		source_record_filter_edit_->setText(QString::fromStdString(s.source_record_filter));
	if (source_record_sources_edit_) {
		QStringList names;
		for (const auto &n : s.source_record_sources)
			names << QString::fromStdString(n);
		source_record_sources_edit_->setText(names.join(QStringLiteral(", ")));
	}
	on_split_mode_changed(split_mode_combo_ ? split_mode_combo_->currentIndex() : 0);
	rebuild_sr_picker();
	rebuild_dest_list();
	refresh_output_files();
	refresh_status_labels();
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
		s.filename_pattern = "{source} {date} {time}";
	s.container = container_combo_->currentData().toString().toStdString();
	s.video_encoder = encoder_combo_->currentData().toString().toStdString();
	s.video_bitrate_kbps = video_bitrate_->value();
	s.audio_bitrate_kbps = audio_bitrate_->value();
	s.vertical_width = vert_w_->value();
	s.vertical_height = vert_h_->value();
	if (auto_start_combo_)
		s.auto_start = static_cast<MsrAutoStartTrigger>(auto_start_combo_->currentData().toInt());
	if (replay_seconds_)
		s.replay_seconds = replay_seconds_->value();
	if (split_mode_combo_)
		s.split_mode = static_cast<MsrSplitMode>(split_mode_combo_->currentData().toInt());
	if (split_time_min_)
		s.split_time_min = split_time_min_->value();
	if (split_size_mb_)
		s.split_size_mb = split_size_mb_->value();
	/* The purple checklist is the single source configuration UI. */
	s.sources.clear();
	for (const auto &row : sr_pick_rows_) {
		MsrSourceConfig cfg;
		cfg.source_name = row.source_name.toStdString();
		cfg.enabled = row.check && row.check->isChecked();
		cfg.replay_enabled = row.replay && row.replay->isChecked();
		cfg.orientation = static_cast<MsrOrientation>(row.orientation->currentData().toInt());
		cfg.extra_audio_source = row.audio ? row.audio->currentData().toString().toStdString() : std::string();
		if (cfg.extra_audio_source == cfg.source_name)
			cfg.extra_audio_source.clear();
		s.sources.push_back(cfg);
	}
	s.replay_enabled = false;
	for (const auto &cfg : s.sources) {
		if (cfg.replay_enabled) {
			s.replay_enabled = true;
			break;
		}
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
	if (source_record_filter_edit_) {
		const QString fn = source_record_filter_edit_->text().trimmed();
		s.source_record_filter = fn.isEmpty() ? "Source Record" : fn.toStdString();
	}
	s.source_record_sources.clear();
	s.source_record_audio.clear();
	for (const auto &row : sr_pick_rows_) {
		if (row.check && row.check->isChecked())
			s.source_record_sources.push_back(row.source_name.toStdString());
		if (row.audio) {
			MsrSettings::SourceRecordAudio ar;
			ar.source_name = row.source_name.toStdString();
			ar.audio_source = row.audio->currentData().toString().toStdString();
			/* Persist even for unchecked rows so choice survives re-check. */
			s.source_record_audio.push_back(ar);
		}
	}
	if (source_record_sources_edit_) {
		QStringList names;
		for (const auto &n : s.source_record_sources)
			names << QString::fromStdString(n);
		source_record_sources_edit_->setText(names.join(QStringLiteral(", ")));
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

QString MsrDock::format_elapsed(qint64 elapsed_ms)
{
	if (elapsed_ms < 0)
		elapsed_ms = 0;
	const qint64 total_sec = elapsed_ms / 1000;
	const int h = int(total_sec / 3600);
	const int m = int((total_sec % 3600) / 60);
	const int s = int(total_sec % 60);
	return QStringLiteral("%1:%2:%3").arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}

void MsrDock::sync_elapsed_anchors()
{
	auto *c = MsrController::instance();
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	const bool recording = c && c->recording();
	const bool streaming = c && c->streaming();
	const bool replaying = c && c->replaying();

	if (recording && !was_recording_)
		record_started_ms_ = now;
	if (!recording)
		record_started_ms_ = 0;
	was_recording_ = recording;

	if (streaming && !was_streaming_)
		stream_started_ms_ = now;
	if (!streaming)
		stream_started_ms_ = 0;
	was_streaming_ = streaming;

	if (replaying && !was_replaying_)
		replay_started_ms_ = now;
	if (!replaying)
		replay_started_ms_ = 0;
	was_replaying_ = replaying;

	const bool source_rec = c && c->source_recording();
	if (source_rec && !was_source_recording_)
		source_record_started_ms_ = now;
	if (!source_rec)
		source_record_started_ms_ = 0;
	was_source_recording_ = source_rec;
}

void MsrDock::refresh_status_labels()
{
	sync_elapsed_anchors();
	auto *c = MsrController::instance();
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	const bool recording = c && c->recording();
	const bool paused = recording && c->paused();
	const bool streaming = c && c->streaming();
	const bool replaying = c && c->replaying();

	if (status_label_) {
		if (recording && record_started_ms_ > 0) {
			const QString elapsed = format_elapsed(now - record_started_ms_);
			status_label_->setText(
				QString::fromUtf8(obs_module_text(paused ? "StatusPaused" : "StatusRecording"))
					.arg(elapsed));
		} else {
			status_label_->setText(QString::fromUtf8(obs_module_text("StatusIdle")));
		}
	}

	if (stream_status_label_) {
		if (streaming && stream_started_ms_ > 0) {
			const qint64 elapsed_ms = now - stream_started_ms_;
			const QString elapsed = format_elapsed(elapsed_ms);
			const uint64_t frames = c ? c->stream_total_frames_min() : 0;
			if (elapsed_ms >= 10000 && frames == 0) {
				stream_status_label_->setText(
					QString::fromUtf8(obs_module_text("StatusStreamNoFrames")).arg(elapsed));
				stream_status_label_->setStyleSheet(
					QStringLiteral("color: #e74c3c; font-weight: 600;"));
			} else {
				stream_status_label_->setStyleSheet(QString());
				stream_status_label_->setText(QString::fromUtf8(obs_module_text("StatusStreaming"))
								      .arg(elapsed)
								      .arg(QString::number(frames)));
			}
		} else {
			stream_status_label_->setStyleSheet(QString());
			stream_status_label_->setText(QString::fromUtf8(obs_module_text("StatusStreamIdle")));
		}
	}

	if (replay_status_label_) {
		if (replaying && replay_started_ms_ > 0) {
			const QString elapsed = format_elapsed(now - replay_started_ms_);
			replay_status_label_->setText(
				QString::fromUtf8(obs_module_text("StatusReplayActive")).arg(elapsed));
		} else {
			replay_status_label_->setText(QString::fromUtf8(obs_module_text("StatusReplayIdle")));
		}
	}

	if (clipping_btn_) {
		clipping_btn_->setText(
			QString::fromUtf8(obs_module_text(replaying ? "StopClipping" : "StartClipping")));
	}

	if (stream_btn_) {
		stream_btn_->setText(
			QString::fromUtf8(obs_module_text(streaming ? "StopStreaming" : "StartStreaming")));
	}

	const bool source_rec = c && c->source_recording();
	if (source_record_btn_) {
		source_record_btn_->setText(
			QString::fromUtf8(obs_module_text(source_rec ? "StopSourceRecord" : "StartSourceRecord")));
		source_record_btn_->setObjectName(source_rec ? QStringLiteral("msrDangerBtn")
							     : QStringLiteral("msrPrimaryBtn"));
		source_record_btn_->style()->unpolish(source_record_btn_);
		source_record_btn_->style()->polish(source_record_btn_);
	}
	if (rec_text_label_ && rec_dot_label_ && rec_timer_label_) {
		if (source_rec && source_record_started_ms_ > 0) {
			rec_dot_label_->setObjectName(QStringLiteral("msrRecDot"));
			rec_text_label_->setObjectName(QStringLiteral("msrRecText"));
			rec_text_label_->setText(QStringLiteral("REC"));
			rec_timer_label_->setText(format_elapsed(now - source_record_started_ms_));
		} else {
			rec_dot_label_->setObjectName(QStringLiteral("msrRecIdle"));
			rec_text_label_->setObjectName(QStringLiteral("msrRecIdle"));
			rec_text_label_->setText(QString::fromUtf8(obs_module_text("StatusSourceRecordIdleShort")));
			rec_timer_label_->setText(QStringLiteral("00:00:00"));
		}
		for (QLabel *lb : {rec_dot_label_, rec_text_label_, rec_timer_label_}) {
			lb->style()->unpolish(lb);
			lb->style()->polish(lb);
		}
	}
	if (source_record_status_label_) {
		if (source_rec && source_record_started_ms_ > 0) {
			const QString elapsed = format_elapsed(now - source_record_started_ms_);
			const auto targets = c->source_record_targets();
			QStringList names;
			for (const auto &n : targets)
				names << QString::fromStdString(n);
			const QString timer_line =
				QString::fromUtf8(obs_module_text("StatusSourceRecordActive")).arg(elapsed);
			const QString targets_line =
				QString::fromUtf8(obs_module_text("StatusSourceRecordTargets"))
					.arg(names.isEmpty() ? QStringLiteral("…") : names.join(QStringLiteral(", ")));
			source_record_status_label_->setStyleSheet(QStringLiteral("color: #ef4444; font-weight: 600;"));
			source_record_status_label_->setText(timer_line + QStringLiteral("\n") + targets_line);
		} else {
			source_record_status_label_->setStyleSheet(QString());
			source_record_status_label_->setText(
				QString::fromUtf8(obs_module_text("StatusSourceRecordIdle")));
		}
	}
}

void MsrDock::update_recording_state(bool recording)
{
	auto *c = MsrController::instance();
	const bool paused = recording && c && c->paused();
	if (pause_btn_) {
		pause_btn_->setEnabled(recording);
		pause_btn_->setText(QString::fromUtf8(obs_module_text(paused ? "ResumeRecording" : "PauseRecording")));
	}
	if (chapter_btn_)
		chapter_btn_->setEnabled(recording && !paused);
	if (recording)
		start_btn_->setText(QString::fromUtf8(obs_module_text("StopRecording")));
	else
		start_btn_->setText(QString::fromUtf8(obs_module_text("StartRecording")));
	refresh_status_labels();
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

void MsrDock::on_remove_destination()
{
	auto *btn = qobject_cast<QPushButton *>(sender());
	if (!btn)
		return;
	const int idx = btn->property("destIndex").toInt();
	apply_to_controller();
	auto *c = MsrController::instance();
	if (!c)
		return;
	MsrSettings s = c->settings();
	if (idx < 0 || idx >= (int)s.destinations.size())
		return;
	s.destinations.erase(s.destinations.begin() + idx);
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
	refresh_status_labels();
}

void MsrDock::on_start_stop_clipping()
{
	auto *c = MsrController::instance();
	if (!c)
		return;
	apply_to_controller();
	if (c->replaying())
		c->stop_replays();
	else
		c->start_replays();
	refresh_status_labels();
}

void MsrDock::on_toggle_source_record()
{
	apply_to_controller();
	auto *c = MsrController::instance();
	if (!c)
		return;
	c->toggle_source_recording();
	update_source_record_state(c->source_recording());
	refresh_status_labels();
}

void MsrDock::update_source_record_state(bool active)
{
	if (source_record_btn_) {
		source_record_btn_->setText(
			QString::fromUtf8(obs_module_text(active ? "StopSourceRecord" : "StartSourceRecord")));
	}
	refresh_status_labels();

	refresh_output_files();
}

void MsrDock::on_save_replay()
{
	auto *c = MsrController::instance();
	if (!c)
		return;
	apply_to_controller();
	if (c->save_replays())
		status_label_->setText(QString::fromUtf8(obs_module_text("StatusReplaySaved")));
	else
		status_label_->setText(QString::fromUtf8(obs_module_text("StatusReplayIdle")));
}

void MsrDock::on_split_mode_changed(int)
{
	if (!split_mode_combo_)
		return;
	const int mode = split_mode_combo_->currentData().toInt();
	const bool by_time = mode == (int)MsrSplitMode::ByTime;
	const bool by_size = mode == (int)MsrSplitMode::BySize;
	if (split_time_label_)
		split_time_label_->setVisible(by_time);
	if (split_time_min_)
		split_time_min_->setVisible(by_time);
	if (split_size_label_)
		split_size_label_->setVisible(by_size);
	if (split_size_mb_)
		split_size_mb_->setVisible(by_size);
}

QWidget *MsrDock::floating_host() const
{
	QWidget *w = const_cast<MsrDock *>(this);
	while (w) {
		if (qobject_cast<QDockWidget *>(w))
			return w;
		w = w->parentWidget();
	}
	return const_cast<MsrDock *>(this);
}

void MsrDock::restore_section_states()
{
	/* Always apply built-in defaults when the dock opens — do not restore
	 * previously saved expand/collapse states. */
	for (const SectionRef &sec : sections_) {
		if (!sec.toggle)
			continue;
		sec.toggle->setChecked(sec.default_open);
	}
}

void MsrDock::save_window_geometry()
{
	QWidget *host = floating_host();
	if (!host)
		return;
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		cfg = obs_frontend_get_app_config();
	if (!cfg)
		return;
	const QByteArray geo = host->saveGeometry().toBase64();
	config_set_string(cfg, "obs-multi-source-record", "DockGeometry", geo.constData());
	config_save_safe(cfg, "tmp", nullptr);
}

void MsrDock::restore_window_geometry()
{
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		cfg = obs_frontend_get_app_config();
	if (!cfg)
		return;
	QWidget *host = floating_host();
	const char *geo = config_get_string(cfg, "obs-multi-source-record", "DockGeometry");
	if (host && geo && *geo) {
		const QByteArray bytes = QByteArray::fromBase64(QByteArray(geo));
		if (!bytes.isEmpty())
			host->restoreGeometry(bytes);
	}
	restore_section_states();
}

void MsrDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	restore_window_geometry();
}

void MsrDock::hideEvent(QHideEvent *event)
{
	save_window_geometry();
	QWidget::hideEvent(event);
}

void MsrDock::apply_theme()
{
	setStyleSheet(msr_dock_stylesheet());
}

QString MsrDock::obs_source_id_for(const QString &name) const
{
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	if (!src)
		return {};
	const char *id = obs_source_get_id(src);
	QString out = id ? QString::fromUtf8(id) : QString();
	obs_source_release(src);
	return out;
}

void MsrDock::rebuild_sr_picker()
{
	if (!sr_pick_layout_)
		return;
	while (QLayoutItem *item = sr_pick_layout_->takeAt(0)) {
		if (QWidget *w = item->widget())
			w->deleteLater();
		delete item;
	}
	sr_pick_rows_.clear();

	MsrSettings settings;
	if (auto *c = MsrController::instance())
		settings = c->settings();

	QSet<QString> selected;
	for (const auto &n : settings.source_record_sources)
		selected.insert(QString::fromStdString(n));
	if (selected.isEmpty()) {
		selected.insert(QStringLiteral("Display Capture"));
		selected.insert(QStringLiteral("Game Capture"));
		selected.insert(QStringLiteral("sony"));
	}

	const QStringList names = enumerate_sources();
	const QStringList audio_names = enumerate_audio_sources();
	for (const QString &name : names) {
		auto *row = new QWidget(this);
		row->setObjectName(QStringLiteral("msrSourceRow"));
		auto *row_l = new QVBoxLayout(row);
		row_l->setContentsMargins(4, 4, 4, 4);
		row_l->setSpacing(2);
		auto *hl = new QHBoxLayout();
		hl->setContentsMargins(0, 0, 0, 0);
		hl->setSpacing(8);

		MsrSrPickRow pr;
		pr.source_name = name;
		pr.check = new QCheckBox(name, row);
		pr.check->setObjectName(QStringLiteral("msrSrCheck"));
		pr.check->blockSignals(true);
		pr.check->setChecked(selected.contains(name));
		pr.check->blockSignals(false);
		connect(pr.check, &QCheckBox::toggled, this, &MsrDock::on_sr_pick_changed);

		pr.replay = new QCheckBox(QString::fromUtf8(obs_module_text("SourceReplay")), row);
		pr.replay->setToolTip(QString::fromUtf8(obs_module_text("SourceReplayTooltip")));
		pr.orientation = new QComboBox(row);
		pr.orientation->setObjectName(QStringLiteral("msrSrOrientation"));
		pr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationHorizontal")),
					(int)MsrOrientation::Horizontal);
		pr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationVertical")),
					(int)MsrOrientation::Vertical);
		pr.orientation->addItem(QString::fromUtf8(obs_module_text("OrientationBoth")),
					(int)MsrOrientation::Both);

		for (const auto &cfg : settings.sources) {
			if (QString::fromStdString(cfg.source_name) != name)
				continue;
			pr.replay->setChecked(cfg.replay_enabled);
			const int oi = pr.orientation->findData((int)cfg.orientation);
			if (oi >= 0)
				pr.orientation->setCurrentIndex(oi);
			break;
		}
		connect(pr.replay, &QCheckBox::toggled, this, &MsrDock::on_sr_pick_changed);
		connect(pr.orientation, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			&MsrDock::on_sr_pick_changed);

		pr.audio = new QComboBox(row);
		pr.audio->setObjectName(QStringLiteral("msrSrAudio"));
		pr.audio->setMinimumWidth(110);
		pr.audio->setMaximumWidth(160);
		pr.audio->setToolTip(QString::fromUtf8(obs_module_text("SourceRecordAudioTooltip")));
		obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
		if (source) {
			if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				pr.audio->setToolTip(pr.audio->toolTip() + "\n" + name + ": no audio (silent).");
			obs_source_release(source);
		}
		pr.audio->addItem(QString::fromUtf8(obs_module_text("SourceRecordAudioSource")), QString());
		for (const QString &a : audio_names) {
			if (a.isEmpty() || a == name)
				continue;
			pr.audio->addItem(a, a);
		}
		const QString saved_audio =
			QString::fromStdString(source_record_audio_for(settings, name.toStdString()));
		int ai = pr.audio->findData(saved_audio);
		if (ai < 0)
			ai = 0;
		pr.audio->blockSignals(true);
		pr.audio->setCurrentIndex(ai);
		pr.audio->blockSignals(false);
		connect(pr.audio, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			&MsrDock::on_sr_audio_changed);

		pr.icon = new QLabel(msr_source_type_icon(name, obs_source_id_for(name)), row);
		pr.icon->setObjectName(QStringLiteral("msrTypeIcon"));
		hl->addWidget(pr.check, 1);
		hl->addWidget(pr.audio, 0);
		hl->addWidget(pr.icon, 0, Qt::AlignRight | Qt::AlignVCenter);
		/* Keep the source name prominent; compact secondary controls fit narrow docks. */
		auto *options = new QHBoxLayout();
		options->setContentsMargins(32, 0, 0, 0);
		options->setSpacing(8);
		options->addWidget(pr.replay);
		options->addStretch(1);
		options->addWidget(pr.orientation);
		row_l->addLayout(hl);
		row_l->addLayout(options);
		sr_pick_layout_->addWidget(row);
		sr_pick_rows_.push_back(pr);
	}
	if (names.isEmpty()) {
		auto *empty = new QLabel(QString::fromUtf8(obs_module_text("SourceRecordPickEmpty")), this);
		empty->setWordWrap(true);
		sr_pick_layout_->addWidget(empty);
	}
}

void MsrDock::on_sr_pick_changed()
{
	apply_to_controller();
	refresh_output_files();
}

void MsrDock::on_sr_audio_changed()
{
	apply_to_controller();
	if (auto *c = MsrController::instance()) {
		if (c->source_recording())
			c->apply_source_record_audio_settings();
	}
}

void MsrDock::on_open_output_file()
{
	auto *btn = qobject_cast<QWidget *>(sender());
	if (!btn)
		return;
	const QString path = btn->property("msrPath").toString();
	if (path.isEmpty())
		return;
	const QFileInfo fi(path);
	const QString folder = fi.exists() ? fi.absolutePath() : path;
	QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void MsrDock::refresh_output_files()
{
	if (!output_files_layout_)
		return;
	while (QLayoutItem *item = output_files_layout_->takeAt(0)) {
		if (QWidget *w = item->widget())
			w->deleteLater();
		delete item;
	}

	QString folder;
	if (folder_edit_)
		folder = folder_edit_->text().trimmed();
	if (folder.isEmpty()) {
		if (auto *c = MsrController::instance())
			folder = QString::fromStdString(c->settings().output_folder);
	}

	QStringList targets;
	for (const auto &row : sr_pick_rows_) {
		if (row.check && row.check->isChecked())
			targets << row.source_name;
	}
	if (targets.isEmpty()) {
		if (auto *c = MsrController::instance()) {
			for (const auto &n : c->source_record_targets())
				targets << QString::fromStdString(n);
		}
	}

	bool any = false;
	const QString container = container_combo_ ? container_combo_->currentData().toString() : QStringLiteral("mkv");
	for (const QString &name : targets) {
		const QString dir_path = folder;
		if (dir_path.isEmpty())
			continue;
		QDir dir(dir_path);
		if (!dir.exists())
			continue;
		const QFileInfoList files =
			dir.entryInfoList(QStringList{QStringLiteral("*.%1").arg(container)}, QDir::Files, QDir::Time);
		QFileInfo best;
		for (const QFileInfo &fi : files) {
			if (fi.fileName().startsWith(name, Qt::CaseInsensitive)) {
				best = fi;
				break;
			}
		}
		if (!best.exists())
			continue;

		any = true;
		auto *card = new QPushButton(this);
		card->setObjectName(QStringLiteral("msrFileCard"));
		card->setCursor(Qt::PointingHandCursor);
		card->setProperty("msrPath", best.absoluteFilePath());
		card->setMinimumHeight(76);
		card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
		card->setStyleSheet(
			QStringLiteral("QPushButton#msrFileCard { text-align: left; padding: 8px 10px; "
				       "background: #16161c; border: 1px solid #2f2f38; border-radius: 10px; }"
				       "QPushButton#msrFileCard:hover { border-color: #8b5cf6; }"));
		const QString icon = msr_source_type_icon(name, obs_source_id_for(name));
		const QString meta =
			QString::fromUtf8(obs_module_text("OutputFileMetaFallback")).arg(best.absolutePath());
		card->setToolTip(meta);
		auto *card_l = new QVBoxLayout(card);
		card_l->setContentsMargins(10, 8, 10, 8);
		card_l->setSpacing(4);
		auto *filename = new QLabel(QStringLiteral("%1  %2").arg(icon, best.fileName()), card);
		filename->setObjectName(QStringLiteral("msrFileName"));
		filename->setWordWrap(true);
		filename->setTextInteractionFlags(Qt::TextSelectableByMouse);
		filename->setToolTip(best.fileName());
		card_l->addWidget(filename);
		auto *path = new QLabel(meta, card);
		path->setObjectName(QStringLiteral("msrFileMeta"));
		path->setWordWrap(true);
		path->setTextInteractionFlags(Qt::TextSelectableByMouse);
		path->setToolTip(meta);
		card_l->addWidget(path);
		connect(card, &QPushButton::clicked, this, &MsrDock::on_open_output_file);
		output_files_layout_->addWidget(card);
	}

	if (!any) {
		auto *empty = new QLabel(QString::fromUtf8(obs_module_text("OutputFilesEmpty")), this);
		empty->setWordWrap(true);
		empty->setObjectName(QStringLiteral("msrFileMeta"));
		output_files_layout_->addWidget(empty);
	}
}
