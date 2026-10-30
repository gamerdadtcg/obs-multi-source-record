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
#include <QString>
#include <obs.h>

class QComboBox;
class QListWidget;
class QPushButton;
class QLabel;
class QListWidgetItem;
class QShowEvent;
class QHideEvent;
class QResizeEvent;
class QPaintEvent;

/** Native OBS display surface for the MSR Vertical canvas (child of the dock). */
class MsrPreviewDisplay : public QWidget {
	Q_OBJECT
public:
	explicit MsrPreviewDisplay(QWidget *parent = nullptr);
	~MsrPreviewDisplay() override;

	void refresh_display();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	QPaintEngine *paintEngine() const override;

private:
	void create_display();
	void destroy_display();
	void ensure_canvas();
	static void draw_preview(void *param, uint32_t cx, uint32_t cy);

	obs_display_t *display_ = nullptr;
};

/**
 * MSR Vertical Preview dock: live canvas preview plus simple scene/source management
 * (scene picker, visibility, add-existing, reorder, fit).
 */
class MsrPreviewDock : public QWidget {
	Q_OBJECT
public:
	explicit MsrPreviewDock(QWidget *parent = nullptr);
	~MsrPreviewDock() override = default;

public slots:
	void refresh_all();

private slots:
	void on_scene_changed(int index);
	void on_new_scene();
	void on_add_source();
	void on_visibility_toggled(QListWidgetItem *item);
	void on_move_up();
	void on_move_down();
	void on_fit_selected();
	void rebuild_scene_combo();
	void rebuild_source_list();

private:
	void build_ui();
	void ensure_canvas();
	QString selected_source_name() const;

	MsrPreviewDisplay *display_ = nullptr;
	QComboBox *scene_combo_ = nullptr;
	QListWidget *source_list_ = nullptr;
	QLabel *hint_label_ = nullptr;
	bool updating_ui_ = false;
};
