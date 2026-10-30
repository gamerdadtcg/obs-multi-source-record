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
class QGroupBox;

struct MsrSourceRow {
	QCheckBox *enabled = nullptr;
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
	void update_recording_state(bool recording);

private:
	void build_ui();
	void load_from_controller();
	void apply_to_controller();
	void rebuild_source_list();
	void rebuild_dest_list();
	QStringList enumerate_sources() const;
	QStringList enumerate_audio_sources() const;

	QVBoxLayout *source_list_layout_ = nullptr;
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
	QPushButton *start_btn_ = nullptr;
	QPushButton *pause_btn_ = nullptr;
	QPushButton *chapter_btn_ = nullptr;
	QPushButton *stream_btn_ = nullptr;
	QLabel *status_label_ = nullptr;
	QLabel *stream_status_label_ = nullptr;
	QGroupBox *advanced_box_ = nullptr;
	QGroupBox *multistream_box_ = nullptr;
};
