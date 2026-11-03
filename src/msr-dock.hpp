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
#include <memory>

class QVBoxLayout;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class QSpinBox;
class QTimer;
class QToolButton;
class QScrollArea;
class MsrXLiveClient;

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
	void on_x_live_sign_in();
	void on_x_live_sign_out();
	void on_x_live_sync_source();
	void on_x_live_go_live();
	void on_x_live_end();
	void on_x_live_send_chat();
	void on_x_live_load_chat_history();
	void on_x_live_status_changed();

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
	QStringList enumerate_sources() const;
	QStringList enumerate_audio_sources() const;

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
	QLabel *status_label_ = nullptr;
	QLabel *source_record_status_label_ = nullptr;
	QLabel *stream_status_label_ = nullptr;
	QLabel *replay_status_label_ = nullptr;
	QTimer *status_timer_ = nullptr;
	qint64 record_started_ms_ = 0;
	qint64 stream_started_ms_ = 0;
	qint64 replay_started_ms_ = 0;
	bool was_recording_ = false;
	bool was_streaming_ = false;
	bool was_replaying_ = false;
	QWidget *advanced_box_ = nullptr;
	QWidget *multistream_box_ = nullptr;
	QWidget *x_live_box_ = nullptr;
	QLineEdit *x_live_client_id_ = nullptr;
	QLineEdit *x_live_title_ = nullptr;
	QLabel *x_live_status_ = nullptr;
	QPushButton *x_live_signin_btn_ = nullptr;
	QPushButton *x_live_signout_btn_ = nullptr;
	QPushButton *x_live_sync_btn_ = nullptr;
	QPushButton *x_live_golive_btn_ = nullptr;
	QPushButton *x_live_end_btn_ = nullptr;
	QLineEdit *x_live_chat_edit_ = nullptr;
	QPushButton *x_live_send_chat_btn_ = nullptr;
	QPushButton *x_live_chat_hist_btn_ = nullptr;
	QPlainTextEdit *x_live_chat_view_ = nullptr;
	MsrXLiveClient *x_live_ = nullptr;

	void ensure_x_live();
	void apply_x_live_to_destination(const std::string &rtmps_url, const std::string &stream_key);
	void persist_x_live_settings();

	static QString format_elapsed(qint64 elapsed_ms);
	void sync_elapsed_anchors();
};
