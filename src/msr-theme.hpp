/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QString>

/* Promo-inspired dock palette: charcoal cards, purple + teal accents. */
inline QString msr_dock_stylesheet()
{
	return QStringLiteral(
		"MsrDock, MsrDock QWidget#msrRoot {"
		"  background: #141418;"
		"  color: #f3f4f6;"
		"  font-size: 12px;"
		"}"
		"MsrDock QScrollArea { background: transparent; border: none; }"
		"MsrDock QWidget#msrCard {"
		"  background: #1c1c22;"
		"  border: 1px solid #2a2a32;"
		"  border-radius: 12px;"
		"}"
		"MsrDock QLabel#msrHeaderTitle {"
		"  color: #ffffff;"
		"  font-size: 14px;"
		"  font-weight: 700;"
		"}"
		"MsrDock QLabel#msrCardCaption {"
		"  color: #9ca3af;"
		"  font-size: 10px;"
		"  font-weight: 700;"
		"  letter-spacing: 1px;"
		"}"
		"MsrDock QWidget#msrSourceRow {"
		"  background: transparent;"
		"  border-radius: 8px;"
		"  padding: 4px;"
		"}"
		"MsrDock QWidget#msrSourceRow:hover { background: rgba(139,92,246,30); }"
		"MsrDock QCheckBox#msrSrCheck {"
		"  spacing: 10px;"
		"  color: #f3f4f6;"
		"  font-weight: 600;"
		"}"
		"MsrDock QCheckBox#msrSrCheck::indicator {"
		"  width: 18px; height: 18px;"
		"  border-radius: 5px;"
		"  border: 2px solid #6b7280;"
		"  background: #111114;"
		"}"
		"MsrDock QComboBox#msrSrAudio {"
		"  background: #111114;"
		"  color: #e5e7eb;"
		"  border: 1px solid #3f3f4a;"
		"  border-radius: 6px;"
		"  padding: 2px 6px;"
		"  min-height: 22px;"
		"}"
		"MsrDock QCheckBox#msrSrCheck::indicator:checked {"
		"  background: #8b5cf6;"
		"  border: 2px solid #8b5cf6;"
		"  image: none;"
		"}"
		"MsrDock QLabel#msrTypeIcon { color: #e5e7eb; font-size: 14px; }"
		"MsrDock QLabel#msrRecDot { color: #ef4444; font-size: 18px; font-weight: 800; }"
		"MsrDock QLabel#msrRecText { color: #ef4444; font-size: 16px; font-weight: 800; }"
		"MsrDock QLabel#msrRecIdle { color: #9ca3af; font-size: 14px; font-weight: 600; }"
		"MsrDock QLabel#msrTimer {"
		"  color: #ffffff;"
		"  font-size: 18px;"
		"  font-weight: 700;"
		"  font-family: 'Segoe UI', 'Inter', sans-serif;"
		"}"
		"MsrDock QFrame#msrVSep {"
		"  background: #4b5563;"
		"  max-width: 1px;"
		"  min-width: 1px;"
		"}"
		"MsrDock QWidget#msrFileCard {"
		"  background: #16161c;"
		"  border: 1px solid #2f2f38;"
		"  border-radius: 10px;"
		"}"
		"MsrDock QLabel#msrFileName { color: #ffffff; font-weight: 600; }"
		"MsrDock QLabel#msrFileMeta { color: #9ca3af; font-size: 11px; }"
		"MsrDock QToolButton {"
		"  border: none;"
		"  color: #e5e7eb;"
		"  font-weight: 600;"
		"  text-align: left;"
		"  padding: 6px 4px;"
		"  border-radius: 6px;"
		"}"
		"MsrDock QToolButton:hover { background: rgba(139,92,246,40); }"
		"MsrDock QPushButton {"
		"  background: #2a2a33;"
		"  color: #f3f4f6;"
		"  border: 1px solid #3f3f4a;"
		"  border-radius: 8px;"
		"  padding: 7px 12px;"
		"  font-weight: 600;"
		"}"
		"MsrDock QPushButton:hover { background: #353542; border-color: #8b5cf6; }"
		"MsrDock QPushButton:disabled { color: #6b7280; background: #1a1a20; }"
		"MsrDock QPushButton#msrPrimaryBtn {"
		"  background: #8b5cf6;"
		"  border: 1px solid #7c3aed;"
		"  color: #ffffff;"
		"}"
		"MsrDock QPushButton#msrPrimaryBtn:hover { background: #7c3aed; }"
		"MsrDock QPushButton#msrDangerBtn {"
		"  background: #7f1d1d;"
		"  border: 1px solid #ef4444;"
		"  color: #fecaca;"
		"}"
		"MsrDock QLineEdit, MsrDock QSpinBox, MsrDock QComboBox {"
		"  background: #111114;"
		"  color: #f3f4f6;"
		"  border: 1px solid #3f3f4a;"
		"  border-radius: 6px;"
		"  padding: 4px 6px;"
		"  selection-background-color: #8b5cf6;"
		"}"
		"MsrDock QComboBox QAbstractItemView {"
		"  background: #1c1c22;"
		"  color: #f3f4f6;"
		"  selection-background-color: #8b5cf6;"
		"}"
		"MsrDock QCheckBox { color: #e5e7eb; }"
		"MsrDock QLabel { color: #e5e7eb; }"
		"MsrDock QScrollBar:vertical {"
		"  background: transparent; width: 8px; margin: 2px;"
		"}"
		"MsrDock QScrollBar::handle:vertical {"
		"  background: #3f3f4a; border-radius: 4px; min-height: 24px;"
		"}"
		"MsrDock QScrollBar::add-line:vertical, MsrDock QScrollBar::sub-line:vertical { height: 0; }");
}

inline QString msr_source_type_icon(const QString &source_name, const QString &obs_id)
{
	const QString id = obs_id.toLower();
	const QString n = source_name.toLower();
	if (id.contains(QStringLiteral("game")) || n.contains(QStringLiteral("game")))
		return QStringLiteral("🎮");
	if (id.contains(QStringLiteral("monitor")) || id.contains(QStringLiteral("display")) ||
	    n.contains(QStringLiteral("display")) || n.contains(QStringLiteral("monitor")))
		return QStringLiteral("🖥️");
	if (id.contains(QStringLiteral("dshow")) || id.contains(QStringLiteral("v4l")) ||
	    id.contains(QStringLiteral("av_capture")) || id.contains(QStringLiteral("camera")) ||
	    n.contains(QStringLiteral("cam")) || n.contains(QStringLiteral("phone")))
		return QStringLiteral("📷");
	return QStringLiteral("●");
}

inline QString msr_source_accent(const QString &icon)
{
	if (icon.contains(QStringLiteral("🎮")))
		return QStringLiteral("#8b5cf6");
	if (icon.contains(QStringLiteral("🖥️")) || icon.contains(QStringLiteral("📷")))
		return QStringLiteral("#2dd4bf");
	return QStringLiteral("#a78bfa");
}
