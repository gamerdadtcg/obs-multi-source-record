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
	void update_recording_state(bool recording);
	void on_split_mode_changed(int index);

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	void build_ui();
	void save_window_geometry();
	void restore_window_geometry();
	void save_section_states();
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
	QPushButton *stream_btn_ = nullptr;
	QLabel *status_label_ = nullptr;
	QLabel *stream_status_label_ = nullptr;
	QWidget *advanced_box_ = nullptr;
	QWidget *multistream_box_ = nullptr;
};
