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
#include <graphics/vec2.h>
#include <graphics/matrix4.h>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QShowEvent;
class QHideEvent;
class QResizeEvent;
class QPaintEvent;
class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class QContextMenuEvent;

enum class MsrItemHandle : uint32_t {
	None = 0,
	Top = 1 << 0,
	Bottom = 1 << 1,
	Left = 1 << 2,
	Right = 1 << 3,
	TopLeft = Top | Left,
	TopCenter = Top,
	TopRight = Top | Right,
	CenterLeft = Left,
	CenterRight = Right,
	BottomLeft = Bottom | Left,
	BottomCenter = Bottom,
	BottomRight = Bottom | Right,
};

/** Native OBS display with click-drag move / resize like the main preview. */
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
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

signals:
	void selection_changed(const QString &source_name);

private:
	void create_display();
	void destroy_display();
	void ensure_canvas();
	static void draw_preview(void *param, uint32_t cx, uint32_t cy);

	void preview_to_world(float px, float py, vec2 &out) const;
	void update_preview_layout(uint32_t cx, uint32_t cy);
	obs_sceneitem_t *item_at_pos(const vec2 &pos);
	MsrItemHandle handle_at_pos(obs_sceneitem_t *item, const vec2 &pos);
	void draw_selection_overlay();
	void move_selected(const vec2 &pos);
	void stretch_selected(const vec2 &pos);
	void crop_selected(const vec2 &pos);
	static bool draw_selected_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param);

	obs_display_t *display_ = nullptr;

	/* Letterbox mapping: world (canvas) <-> preview pixels */
	int preview_x_ = 0;
	int preview_y_ = 0;
	float preview_scale_ = 1.0f;
	uint32_t world_cx_ = 1080;
	uint32_t world_cy_ = 1920;

	bool mouse_down_ = false;
	bool mouse_moved_ = false;
	bool cropping_ = false;
	vec2 start_pos_{};
	vec2 last_pos_{};
	vec2 stretch_start_size_{};
	vec2 stretch_start_pos_{};
	obs_sceneitem_crop start_crop_{};
	MsrItemHandle stretch_handle_ = MsrItemHandle::None;
	obs_sceneitem_t *stretch_item_ = nullptr; /* non-owning while mouse down */
	matrix4 item_to_screen_{};
	matrix4 screen_to_item_{};
};

/**
 * MSR Vertical Preview dock: follows the main scene (linked layout), live preview
 * with interactive transforms, and source visibility / fit helpers.
 */
class MsrPreviewDock : public QWidget {
	Q_OBJECT
public:
	explicit MsrPreviewDock(QWidget *parent = nullptr);
	~MsrPreviewDock() override = default;

public slots:
	void refresh_all();
	void on_main_scene_changed();

private slots:
	void on_visibility_toggled(QListWidgetItem *item);
	void on_move_up();
	void on_move_down();
	void on_fit_selected();
	void on_list_selection_changed();
	void on_preview_selection_changed(const QString &name);
	void on_audio_button_clicked();
	void rebuild_source_list();

private:
	void build_ui();
	void ensure_canvas();
	QString selected_source_name() const;

	MsrPreviewDisplay *display_ = nullptr;
	QLabel *follow_label_ = nullptr;
	QListWidget *source_list_ = nullptr;
	QLabel *hint_label_ = nullptr;
	bool updating_ui_ = false;
};
