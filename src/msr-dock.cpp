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
	sw.toggle->setStyleSheet(QStringLiteral(
		"QToolButton { border: none; font-weight: 600; text-align: left; padding: 4px 2px; }"
		"QToolButton:hover { background: rgba(127,127,127,40); }"));
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

	QObject::connect(sw.toggle, &QToolButton::toggled, sw.section,
			 [set_header, body = sw.body](bool on) {
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
		update_recording_state(c->recording());
	}
	refresh_status_labels();
}

MsrDock::~MsrDock()
{
	if (auto *c = MsrController::instance())
		c->set_state_callback(nullptr);
}

void MsrDock::build_ui()
{
	setMinimumSize(380, 480);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

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
	content->setMinimumWidth(340);
	auto *root = new QVBoxLayout(content);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(6);

	auto *title = new QLabel(QString::fromUtf8(obs_module_text("DockTitle")), content);
	QFont f = title->font();
	f.setBold(true);
	f.setPointSize(f.pointSize() + 1);
	title->setFont(f);
	root->addWidget(title);

	auto *hint = new QLabel(QString::fromUtf8(obs_module_text("DockHint")), content);
	hint->setWordWrap(true);
	root->addWidget(hint);

	auto add_section = [this, content, root](const QString &title, const char *key,
						bool default_open) -> QWidget * {
		SectionWidgets sw = make_collapsible(content, title, default_open);
		sections_.push_back(SectionRef{sw.toggle, sw.body, key, default_open});
		connect(sw.toggle, &QToolButton::toggled, this, [this](bool) { save_section_states(); });
		root->addWidget(sw.section);
		return sw.body;
	};

	/* Sources — default open */
	auto *sources_body = add_section(QString::fromUtf8(obs_module_text("VerticalSources")), "SectionSources",
					 true);
	auto *sources_l = qobject_cast<QVBoxLayout *>(sources_body->layout());
	source_scroll_ = new QScrollArea(sources_body);
	source_scroll_->setWidgetResizable(true);
	source_scroll_->setFrameShape(QFrame::StyledPanel);
	source_scroll_->setMinimumHeight(8 * 28);
	source_scroll_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
	auto *source_host = new QWidget(source_scroll_);
	source_list_layout_ = new QVBoxLayout(source_host);
	source_list_layout_->setContentsMargins(4, 4, 4, 4);
	source_list_layout_->setSpacing(4);
	source_list_layout_->addStretch(1);
	source_scroll_->setWidget(source_host);
	sources_l->addWidget(source_scroll_);
	auto *refresh = new QPushButton(QString::fromUtf8(obs_module_text("RefreshSources")), sources_body);
	connect(refresh, &QPushButton::clicked, this, &MsrDock::refresh_sources);
	sources_l->addWidget(refresh);

	/* Output — default closed */
	auto *output_body = add_section(QString::fromUtf8(obs_module_text("Output")), "SectionOutput", false);
	auto *output_l = qobject_cast<QVBoxLayout *>(output_body->layout());
	auto *folder_row = new QHBoxLayout();
	folder_edit_ = new QLineEdit(output_body);
	folder_edit_->setPlaceholderText(QString::fromUtf8(obs_module_text("OutputFolder")));
	auto *browse = new QPushButton(QString::fromUtf8(obs_module_text("Browse")), output_body);
	connect(browse, &QPushButton::clicked, this, &MsrDock::on_browse_folder);
	folder_row->addWidget(folder_edit_, 1);
	folder_row->addWidget(browse);
	output_l->addLayout(folder_row);
	output_l->addWidget(new QLabel(QString::fromUtf8(obs_module_text("FilenamePattern")), output_body));
	pattern_edit_ = new QLineEdit(output_body);
	pattern_edit_->setPlaceholderText(QStringLiteral("{source}_{orientation}_{date}_{time}"));
	output_l->addWidget(pattern_edit_);
	auto *fmt_row = new QHBoxLayout();
	container_combo_ = new QComboBox(output_body);
	container_combo_->addItem(QStringLiteral("mkv"), QStringLiteral("mkv"));
	container_combo_->addItem(QStringLiteral("mp4"), QStringLiteral("mp4"));
	fmt_row->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Container")), output_body));
	fmt_row->addWidget(container_combo_, 1);
	output_l->addLayout(fmt_row);

	/* Recording options — default open */
	auto *opts_body = add_section(QString::fromUtf8(obs_module_text("RecordingOptions")), "SectionRecording",
				      true);
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
		sr.enabled->setToolTip(QString::fromUtf8(obs_module_text("RecordSourceTooltip")));
		sr.replay = new QCheckBox(QString::fromUtf8(obs_module_text("SourceReplay")), row);
		sr.replay->setToolTip(QString::fromUtf8(obs_module_text("SourceReplayTooltip")));
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
				if (sr.replay)
					sr.replay->setChecked(cfg.replay_enabled);
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
		hl->addWidget(sr.replay);
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
	on_split_mode_changed(split_mode_combo_ ? split_mode_combo_->currentIndex() : 0);
	rebuild_source_list();
	rebuild_dest_list();
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
		s.filename_pattern = "{source}_{orientation}_{date}_{time}";
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
	s.sources.clear();
	for (const auto &row : rows_) {
		MsrSourceConfig cfg;
		cfg.source_name = row.source_name.toStdString();
		cfg.enabled = row.enabled->isChecked();
		cfg.replay_enabled = row.replay && row.replay->isChecked();
		cfg.orientation = static_cast<MsrOrientation>(row.orientation->currentData().toInt());
		cfg.extra_audio_source = row.extra_audio->currentData().toString().toStdString();
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
	return QStringLiteral("%1:%2:%3")
		.arg(h, 2, 10, QChar('0'))
		.arg(m, 2, 10, QChar('0'))
		.arg(s, 2, 10, QChar('0'));
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
			const QString elapsed = format_elapsed(now - stream_started_ms_);
			stream_status_label_->setText(
				QString::fromUtf8(obs_module_text("StatusStreaming")).arg(elapsed));
		} else {
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

void MsrDock::save_section_states()
{
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		cfg = obs_frontend_get_app_config();
	if (!cfg)
		return;
	for (const SectionRef &sec : sections_) {
		if (!sec.toggle || !sec.key)
			continue;
		config_set_bool(cfg, "obs-multi-source-record", sec.key, sec.toggle->isChecked());
	}
	config_save_safe(cfg, "tmp", nullptr);
}

void MsrDock::restore_section_states()
{
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		cfg = obs_frontend_get_app_config();
	for (const SectionRef &sec : sections_) {
		if (!sec.toggle || !sec.key)
			continue;
		bool open = sec.default_open;
		if (cfg && config_has_user_value(cfg, "obs-multi-source-record", sec.key))
			open = config_get_bool(cfg, "obs-multi-source-record", sec.key);
		sec.toggle->setChecked(open);
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
	save_section_states();
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
