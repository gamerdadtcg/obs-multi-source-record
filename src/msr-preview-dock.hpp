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
#include <obs.h>

/** Dock that shows a live preview of the MSR Vertical canvas. */
class MsrPreviewDock : public QWidget {
	Q_OBJECT
public:
	explicit MsrPreviewDock(QWidget *parent = nullptr);
	~MsrPreviewDock() override;

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
