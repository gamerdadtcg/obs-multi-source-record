/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QWidget>
#include <QPointer>
#include <vector>

class QVBoxLayout;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QSpinBox;
class QTimer;
class QToolButton;
class QScrollArea;

struct MsrSourceRow {
	QCheckBox *enabled = nullptr;
	QCheckBox *replay = nullptr;
	QLabel *name = nullptr;
	QComboBox *orientation = nullptr;
	QComboBox *extra_audio = nullptr;
	QString source_name;
};

struct MsrDestRow {
	QCheckBox *enabled = nullptr;
	QLineEdit *name = nullptr;
	QComboBox *orientation = nullptr;
	QLineEdit *server = nullptr;
	QLineEdit *key = nullptr;
	QPushButton *remove = nullptr;
	QCheckBox *track_checks[6] = {};
};

struct MsrSrPickRow {
	QCheckBox *check = nullptr;
	QComboBox *audio = nullptr;
	QLabel *icon = nullptr;
	QString source_name;
};

class MsrDock : public QWidget {
	Q_OBJECT
public:
	explicit MsrDock(QWidget *parent = nullptr);
	~MsrDock() override;

public slots:
	void refresh_sources();
	void on_start_stop();
	void on_pause_resume();
	void on_add_chapter();
	void on_browse_folder();
	void on_save();
	void on_start_stop_stream();
	void on_add_destination();
	void on_remove_destination();
	void on_save_replay();
	void on_start_stop_clipping();
	void on_toggle_source_record();
	void update_recording_state(bool recording);
	void update_source_record_state(bool active);
	void on_split_mode_changed(int index);
	void refresh_status_labels();
	void on_sr_pick_changed();
	void on_sr_audio_changed();
	void on_open_output_file();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	void build_ui();
	void save_window_geometry();
	void restore_window_geometry();
	void restore_section_states();
	QWidget *floating_host() const;
	void load_from_controller();
	void apply_to_controller();
	void rebuild_source_list();
	void rebuild_dest_list();
	void rebuild_sr_picker();
	void refresh_output_files();
	void apply_theme();
	QStringList enumerate_sources() const;
	QStringList enumerate_audio_sources() const;
	QString obs_source_id_for(const QString &name) const;

	QScrollArea *outer_scroll_ = nullptr;
	QScrollArea *source_scroll_ = nullptr;
	QVBoxLayout *source_list_layout_ = nullptr;
	struct SectionRef {
		QToolButton *toggle = nullptr;
		QWidget *body = nullptr;
		const char *key = nullptr;
		bool default_open = false;
	};
	std::vector<SectionRef> sections_;
	std::vector<MsrSourceRow> rows_;
	QVBoxLayout *dest_list_layout_ = nullptr;
	std::vector<MsrDestRow> dest_rows_;
	QLineEdit *folder_edit_ = nullptr;
	QLineEdit *pattern_edit_ = nullptr;
	QComboBox *container_combo_ = nullptr;
	QComboBox *encoder_combo_ = nullptr;
	QSpinBox *video_bitrate_ = nullptr;
	QSpinBox *audio_bitrate_ = nullptr;
	QSpinBox *vert_w_ = nullptr;
	QSpinBox *vert_h_ = nullptr;
	QComboBox *auto_start_combo_ = nullptr;
	QSpinBox *replay_seconds_ = nullptr;
	QComboBox *split_mode_combo_ = nullptr;
	QSpinBox *split_time_min_ = nullptr;
	QSpinBox *split_size_mb_ = nullptr;
	QLabel *split_time_label_ = nullptr;
	QLabel *split_size_label_ = nullptr;
	QPushButton *start_btn_ = nullptr;
	QPushButton *pause_btn_ = nullptr;
	QPushButton *chapter_btn_ = nullptr;
	QPushButton *replay_btn_ = nullptr;
	QPushButton *clipping_btn_ = nullptr;
	QPushButton *stream_btn_ = nullptr;
	QPushButton *source_record_btn_ = nullptr;
	QLineEdit *source_record_filter_edit_ = nullptr;
	QLineEdit *source_record_sources_edit_ = nullptr;
	QVBoxLayout *sr_pick_layout_ = nullptr;
	std::vector<MsrSrPickRow> sr_pick_rows_;
	QWidget *rec_hero_ = nullptr;
	QLabel *rec_dot_label_ = nullptr;
	QLabel *rec_text_label_ = nullptr;
	QLabel *rec_timer_label_ = nullptr;
	QVBoxLayout *output_files_layout_ = nullptr;
	QLabel *status_label_ = nullptr;
	QLabel *source_record_status_label_ = nullptr;
	QLabel *stream_status_label_ = nullptr;
	QLabel *replay_status_label_ = nullptr;
	QTimer *status_timer_ = nullptr;
	qint64 record_started_ms_ = 0;
	qint64 stream_started_ms_ = 0;
	qint64 replay_started_ms_ = 0;
	qint64 source_record_started_ms_ = 0;
	bool was_recording_ = false;
	bool was_streaming_ = false;
	bool was_replaying_ = false;
	bool was_source_recording_ = false;
	QWidget *advanced_box_ = nullptr;
	QWidget *multistream_box_ = nullptr;

	static QString format_elapsed(qint64 elapsed_ms);
	void sync_elapsed_anchors();
};
