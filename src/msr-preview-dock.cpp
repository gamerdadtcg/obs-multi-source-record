/*
Multi Source Record for OBS Studio
Copyright (C) 2026 Travvvman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "msr-preview-dock.hpp"
#include "msr-vertical-canvas.hpp"
#include "msr-controller.hpp"
#include "plugin-support.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QLabel>
#include <QInputDialog>
#include <QTimer>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QWindow>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QSizePolicy>

#include <obs-module.h>
#include <obs-frontend-api.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <objc/objc.h>
#else
#include <obs-nix-platform.h>
#endif

namespace {

inline QSize pixel_size(QWidget *w)
{
	return w->size() * w->devicePixelRatioF();
}

inline void get_scale_and_center(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	const double windowAspect = double(windowCX) / double(windowCY);
	const double baseAspect = double(baseCX) / double(baseCY);
	int newCX, newCY;
	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}
	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

struct EnumSourcesCtx {
	QStringList *out;
};

bool enum_video_sources_cb(void *data, obs_source_t *source)
{
	auto *ctx = static_cast<EnumSourcesCtx *>(data);
	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_VIDEO) == 0)
		return true;
	/* Skip scenes — add concrete sources only for vertical. */
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE)
		return true;
	const char *name = obs_source_get_name(source);
	if (name && *name)
		ctx->out->push_back(QString::fromUtf8(name));
	return true;
}

struct EnumItemsCtx {
	QListWidget *list;
};

bool enum_scene_items_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<EnumItemsCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *name = obs_source_get_name(src);
	if (!name || !*name)
		return true;
	auto *row = new QListWidgetItem(QString::fromUtf8(name));
	row->setFlags(row->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
	row->setCheckState(obs_sceneitem_visible(item) ? Qt::Checked : Qt::Unchecked);
	/* Insert at top so list order matches OBS top-to-bottom (last drawn first in enum?).
	 * obs_scene_enum_items walks bottom→top; prepend so top of list = top of stack. */
	ctx->list->insertItem(0, row);
	return true;
}

} // namespace

/* ---- MsrPreviewDisplay -------------------------------------------------- */

MsrPreviewDisplay::MsrPreviewDisplay(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumSize(90, 160);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

MsrPreviewDisplay::~MsrPreviewDisplay()
{
	destroy_display();
}

void MsrPreviewDisplay::ensure_canvas()
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	int w = 1080, h = 1920;
	if (auto *c = MsrController::instance()) {
		auto s = c->settings();
		w = s.vertical_width;
		h = s.vertical_height;
	}
	vc->ensure((uint32_t)w, (uint32_t)h);
}

void MsrPreviewDisplay::create_display()
{
	if (display_)
		return;
	if (!windowHandle())
		winId(); /* force native window */
	if (!windowHandle() || !windowHandle()->isExposed())
		return;

	ensure_canvas();

	QSize size = pixel_size(this);
	if (size.width() < 2 || size.height() < 2)
		return;

	gs_init_data info = {};
	info.cx = (uint32_t)size.width();
	info.cy = (uint32_t)size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

#ifdef _WIN32
	info.window.hwnd = (HWND)winId();
#elif defined(__APPLE__)
	info.window.view = (id)winId();
#else
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL) {
		obs_log(LOG_WARNING, "vertical preview: unsupported nix platform");
		return;
	}
	info.window.id = (uint32_t)winId();
	info.window.display = obs_get_nix_platform_display();
#endif

	/* Dark clear so an empty scene is distinct from a failed display. */
	display_ = obs_display_create(&info, 0xFF1A1A1A);
	if (!display_) {
		obs_log(LOG_WARNING, "vertical preview display create failed");
		return;
	}
	obs_display_add_draw_callback(display_, draw_preview, this);
	obs_log(LOG_INFO, "vertical preview display created %dx%d", size.width(), size.height());
}

void MsrPreviewDisplay::destroy_display()
{
	if (!display_)
		return;
	obs_display_remove_draw_callback(display_, draw_preview, this);
	obs_display_destroy(display_);
	display_ = nullptr;
}

void MsrPreviewDisplay::refresh_display()
{
	if (!display_)
		create_display();
	else {
		QSize size = pixel_size(this);
		if (size.width() >= 2 && size.height() >= 2)
			obs_display_resize(display_, (uint32_t)size.width(), (uint32_t)size.height());
	}
}

void MsrPreviewDisplay::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	ensure_canvas();
	/* Defer until the native window is exposed (dock may show before expose). */
	QTimer::singleShot(0, this, [this]() { create_display(); });
	QTimer::singleShot(150, this, [this]() {
		if (!display_)
			create_display();
	});
}

void MsrPreviewDisplay::hideEvent(QHideEvent *event)
{
	destroy_display();
	QWidget::hideEvent(event);
}

void MsrPreviewDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!display_) {
		create_display();
		return;
	}
	QSize size = pixel_size(this);
	obs_display_resize(display_, (uint32_t)size.width(), (uint32_t)size.height());
}

void MsrPreviewDisplay::paintEvent(QPaintEvent *)
{
	create_display();
}

QPaintEngine *MsrPreviewDisplay::paintEngine() const
{
	return nullptr;
}

void MsrPreviewDisplay::draw_preview(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(param);
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc || !vc->ready())
		return;

	obs_source_t *program = vc->program_source();
	if (!program)
		return;

	uint32_t sourceCX = vc->width();
	uint32_t sourceCY = vc->height();
	if (sourceCX < 1)
		sourceCX = obs_source_get_width(program);
	if (sourceCY < 1)
		sourceCY = obs_source_get_height(program);
	if (sourceCX < 1)
		sourceCX = 1;
	if (sourceCY < 1)
		sourceCY = 1;

	int x, y;
	float scale;
	get_scale_and_center((int)sourceCX, (int)sourceCY, (int)cx, (int)cy, x, y, scale);
	int newCX = (int)(scale * float(sourceCX));
	int newCY = (int)(scale * float(sourceCY));
	if (newCX < 1)
		newCX = 1;
	if (newCY < 1)
		newCY = 1;

	gs_viewport_push();
	gs_projection_push();

	const bool previous = gs_set_linear_srgb(true);
	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCX, newCY);

	/* Render the program scene source — reliable for secondary-canvas docks. */
	obs_source_video_render(program);

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();

	obs_source_release(program);
}

/* ---- MsrPreviewDock ----------------------------------------------------- */

MsrPreviewDock::MsrPreviewDock(QWidget *parent) : QWidget(parent)
{
	build_ui();
	ensure_canvas();
	refresh_all();
}

void MsrPreviewDock::build_ui()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	auto *scene_row = new QHBoxLayout();
	scene_row->addWidget(new QLabel(QString::fromUtf8(obs_module_text("VerticalScene")), this));
	scene_combo_ = new QComboBox(this);
	scene_combo_->setMinimumWidth(100);
	scene_row->addWidget(scene_combo_, 1);
	auto *new_scene = new QPushButton(QString::fromUtf8(obs_module_text("VerticalNewScene")), this);
	auto *refresh = new QPushButton(QString::fromUtf8(obs_module_text("VerticalRefresh")), this);
	scene_row->addWidget(new_scene);
	scene_row->addWidget(refresh);
	root->addLayout(scene_row);

	connect(scene_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&MsrPreviewDock::on_scene_changed);
	connect(new_scene, &QPushButton::clicked, this, &MsrPreviewDock::on_new_scene);
	connect(refresh, &QPushButton::clicked, this, &MsrPreviewDock::refresh_all);

	display_ = new MsrPreviewDisplay(this);
	root->addWidget(display_, 1);

	hint_label_ = new QLabel(QString::fromUtf8(obs_module_text("VerticalPreviewHint")), this);
	hint_label_->setWordWrap(true);
	root->addWidget(hint_label_);

	root->addWidget(new QLabel(QString::fromUtf8(obs_module_text("VerticalSources")), this));
	source_list_ = new QListWidget(this);
	source_list_->setMinimumHeight(80);
	source_list_->setMaximumHeight(160);
	root->addWidget(source_list_);
	connect(source_list_, &QListWidget::itemChanged, this, &MsrPreviewDock::on_visibility_toggled);

	auto *btn_row = new QHBoxLayout();
	auto *add_btn = new QPushButton(QString::fromUtf8(obs_module_text("VerticalAddSource")), this);
	auto *up_btn = new QPushButton(QString::fromUtf8(obs_module_text("VerticalMoveUp")), this);
	auto *down_btn = new QPushButton(QString::fromUtf8(obs_module_text("VerticalMoveDown")), this);
	auto *fit_btn = new QPushButton(QString::fromUtf8(obs_module_text("VerticalFit")), this);
	btn_row->addWidget(add_btn);
	btn_row->addWidget(up_btn);
	btn_row->addWidget(down_btn);
	btn_row->addWidget(fit_btn);
	root->addLayout(btn_row);

	connect(add_btn, &QPushButton::clicked, this, &MsrPreviewDock::on_add_source);
	connect(up_btn, &QPushButton::clicked, this, &MsrPreviewDock::on_move_up);
	connect(down_btn, &QPushButton::clicked, this, &MsrPreviewDock::on_move_down);
	connect(fit_btn, &QPushButton::clicked, this, &MsrPreviewDock::on_fit_selected);
}

void MsrPreviewDock::ensure_canvas()
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	int w = 1080, h = 1920;
	if (auto *c = MsrController::instance()) {
		auto s = c->settings();
		w = s.vertical_width;
		h = s.vertical_height;
	}
	vc->ensure((uint32_t)w, (uint32_t)h);
}

void MsrPreviewDock::refresh_all()
{
	ensure_canvas();
	rebuild_scene_combo();
	rebuild_source_list();
	if (display_)
		display_->refresh_display();
}

void MsrPreviewDock::rebuild_scene_combo()
{
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc || !scene_combo_)
		return;
	updating_ui_ = true;
	const QString current = scene_combo_->currentData().toString();
	scene_combo_->clear();
	for (const auto &name : vc->list_scenes()) {
		scene_combo_->addItem(QString::fromStdString(name), QString::fromStdString(name));
	}
	obs_source_t *prog = vc->program_source();
	QString prog_name;
	if (prog) {
		prog_name = QString::fromUtf8(obs_source_get_name(prog));
		obs_source_release(prog);
	}
	int idx = scene_combo_->findData(prog_name.isEmpty() ? current : prog_name);
	if (idx < 0 && scene_combo_->count() > 0)
		idx = 0;
	if (idx >= 0)
		scene_combo_->setCurrentIndex(idx);
	updating_ui_ = false;
}

void MsrPreviewDock::rebuild_source_list()
{
	if (!source_list_)
		return;
	updating_ui_ = true;
	const QString sel = selected_source_name();
	source_list_->clear();
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc) {
		updating_ui_ = false;
		return;
	}
	obs_scene_t *scene = vc->program_scene();
	if (!scene) {
		updating_ui_ = false;
		return;
	}
	EnumItemsCtx ctx{source_list_};
	obs_scene_enum_items(scene, enum_scene_items_cb, &ctx);
	obs_scene_release(scene);

	if (!sel.isEmpty()) {
		for (int i = 0; i < source_list_->count(); i++) {
			if (source_list_->item(i)->text() == sel) {
				source_list_->setCurrentRow(i);
				break;
			}
		}
	}
	updating_ui_ = false;

	if (hint_label_) {
		hint_label_->setVisible(source_list_->count() == 0);
	}
}

QString MsrPreviewDock::selected_source_name() const
{
	if (!source_list_)
		return {};
	auto *item = source_list_->currentItem();
	return item ? item->text() : QString();
}

void MsrPreviewDock::on_scene_changed(int)
{
	if (updating_ui_ || !scene_combo_)
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	const QString name = scene_combo_->currentData().toString();
	if (name.isEmpty())
		return;
	vc->set_program_scene(name.toStdString());
	rebuild_source_list();
	if (display_)
		display_->refresh_display();
}

void MsrPreviewDock::on_new_scene()
{
	bool ok = false;
	const QString name = QInputDialog::getText(this, QString::fromUtf8(obs_module_text("VerticalNewScene")),
						   QString::fromUtf8(obs_module_text("VerticalSceneName")),
						   QLineEdit::Normal, QStringLiteral("Vertical"), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	ensure_canvas();
	if (!vc->create_scene(name.trimmed().toStdString()))
		return;
	vc->set_program_scene(name.trimmed().toStdString());
	refresh_all();
}

void MsrPreviewDock::on_add_source()
{
	QStringList names;
	EnumSourcesCtx ctx{&names};
	obs_enum_sources(enum_video_sources_cb, &ctx);
	names.sort(Qt::CaseInsensitive);
	if (names.isEmpty())
		return;

	bool ok = false;
	const QString pick = QInputDialog::getItem(this, QString::fromUtf8(obs_module_text("VerticalAddSource")),
						   QString::fromUtf8(obs_module_text("VerticalPickSource")), names, 0,
						   false, &ok);
	if (!ok || pick.isEmpty())
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	ensure_canvas();
	vc->add_existing_source(pick.toStdString());
	rebuild_source_list();
	if (display_)
		display_->refresh_display();
}

void MsrPreviewDock::on_visibility_toggled(QListWidgetItem *item)
{
	if (updating_ui_ || !item)
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	vc->set_item_visible(item->text().toStdString(), item->checkState() == Qt::Checked);
}

void MsrPreviewDock::on_move_up()
{
	const QString name = selected_source_name();
	if (name.isEmpty())
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	vc->reorder_source(name.toStdString(), true);
	rebuild_source_list();
}

void MsrPreviewDock::on_move_down()
{
	const QString name = selected_source_name();
	if (name.isEmpty())
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	vc->reorder_source(name.toStdString(), false);
	rebuild_source_list();
}

void MsrPreviewDock::on_fit_selected()
{
	const QString name = selected_source_name();
	if (name.isEmpty())
		return;
	auto *vc = MsrVerticalCanvas::instance();
	if (!vc)
		return;
	vc->fit_source(name.toStdString());
	if (display_)
		display_->refresh_display();
}
