#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QPushButton>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QApplication>
#include <QItemSelectionModel>
#include <QDir>
#include <QDockWidget>
#include <QDropEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QSvgRenderer>
#include <QFont>
#include <QGuiApplication>
#include <QHash>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <functional>

#include "scene-tree-dock.hpp"

#define T(key) obs_module_text(key)

static QString L(const char *key)
{
	const char *text = obs_frontend_get_locale_string(key);
	QString value = QString::fromUtf8(text && *text ? text : key);
	return value.remove(QChar('&'));
}

static const int ROLE_KIND = Qt::UserRole;
static const int ROLE_UUID = Qt::UserRole + 1;
static const int ROLE_STATE = Qt::UserRole + 2;
static const int ROLE_COLOR = Qt::UserRole + 3;
static const int ROLE_HIDDEN = Qt::UserRole + 4;
static const int ROLE_ICON = Qt::UserRole + 5;
static const int STATE_NONE = 0;
static const int STATE_PREVIEW = 1;
static const int STATE_LIVE = 2;
static const char *KIND_FOLDER = "folder";
static const char *KIND_SCENE = "scene";
static const char *SAVE_KEY = "gd_scene_tree";
static const char *DOCK_ID = "gd-scene-tree";

struct ColorPreset {
	const char *label;
	const char *hex;
};

static const ColorPreset COLOR_PRESETS[] = {
	{"Color.Red", "#d64545"},  {"Color.Orange", "#e08a2e"}, {"Color.Yellow", "#d8b628"}, {"Color.Green", "#3f9a4a"},
	{"Color.Teal", "#2a9d8f"}, {"Color.Blue", "#3b7dd8"},   {"Color.Purple", "#8a5cd6"}, {"Color.Grey", "#7a7f87"},
};

static bool isFolder(const QTreeWidgetItem *item)
{
	return item && item->data(0, ROLE_KIND).toString() == KIND_FOLDER;
}

static bool isScene(const QTreeWidgetItem *item)
{
	return item && item->data(0, ROLE_KIND).toString() == KIND_SCENE;
}

static QIcon lucideIcon(const char *name, const QColor &color, bool filled = false)
{
	char *path = obs_module_file((QString("icons/%1.svg").arg(name)).toUtf8().constData());
	QString file = path ? QString::fromUtf8(path) : QString();
	bfree(path);
	QFile svgFile(file);
	if (file.isEmpty() || !svgFile.open(QIODevice::ReadOnly))
		return QIcon();
	QString svg = QString::fromUtf8(svgFile.readAll());
	svg.replace("currentColor", color.name(QColor::HexRgb));
	if (filled)
		svg.replace("fill=\"none\"", QString("fill=\"%1\"").arg(color.name(QColor::HexRgb)));

	QSvgRenderer renderer(svg.toUtf8());
	const int size = 16;
	const qreal dpr = 2.0;
	QPixmap pixmap(int(size * dpr), int(size * dpr));
	pixmap.setDevicePixelRatio(dpr);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	renderer.render(&painter, QRectF(1.0, 1.0, size - 2.0, size - 2.0));
	painter.end();
	return QIcon(pixmap);
}

struct TreeSettings {
	bool showSearch = false;
	bool showIcons = true;
	bool showHidden = false;
	bool rememberExpansion = true;
	int itemHeight = 24;
	bool doubleClickSwitch = false;
	bool noPreviewSwitch = false;
	bool noTransition = false;
	bool switchOnDuplicate = true;
	bool autoSort = false;
	int autoSortMode = 0;
	bool foldersOnTop = true;
};

class RowDelegate : public QStyledItemDelegate {
public:
	int rowHeight = 24;
	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		QSize size = QStyledItemDelegate::sizeHint(option, index);
		size.setHeight(rowHeight);
		return size;
	}
};

class SceneTreeWidget : public QTreeWidget {
public:
	std::function<void()> onDropped;

	SceneTreeWidget()
	{
		setMouseTracking(true);
		viewport()->setMouseTracking(true);
		viewport()->setAttribute(Qt::WA_Hover, true);
		setAllColumnsShowFocus(true);
		setRootIsDecorated(true);
		setItemsExpandable(true);
	}

protected:
	void dropEvent(QDropEvent *event) override
	{
		QTreeWidget::dropEvent(event);
		if (onDropped)
			onDropped();
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		setHover(indexAt(event->pos()));
		QTreeWidget::mouseMoveEvent(event);
	}

	void leaveEvent(QEvent *event) override
	{
		setHover(QModelIndex());
		QTreeWidget::leaveEvent(event);
	}

	void setHover(const QModelIndex &index)
	{
		if (hoverIndex == index)
			return;
		hoverIndex = index;
		viewport()->update();
	}

	void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		int state = index.data(ROLE_STATE).toInt();
		bool hover = hoverIndex.isValid() && hoverIndex == index;
		bool selected = selectionModel() && selectionModel()->isSelected(index);
		bool hidden = index.data(ROLE_HIDDEN).toBool();
		QColor custom(index.data(ROLE_COLOR).toString());

		QRect itemRect = visualRect(index);
		QRect pill(itemRect.left() - 6, option.rect.top() + 1, option.rect.right() - itemRect.left() + 2,
			   option.rect.height() - 2);
		auto fillPill = [painter, pill](const QColor &color) {
			painter->save();
			painter->setRenderHint(QPainter::Antialiasing);
			painter->setPen(Qt::NoPen);
			painter->setBrush(color);
			painter->drawRoundedRect(pill, 4.0, 4.0);
			painter->restore();
		};

		if (state == STATE_LIVE)
			fillPill(QColor(222, 92, 84, hover ? 255 : 230));
		else if (state == STATE_PREVIEW)
			fillPill(QColor(125, 196, 122, hover ? 255 : 230));
		else if (custom.isValid())
			fillPill(QColor(custom.red(), custom.green(), custom.blue(), hover ? 150 : 100));
		if (state == STATE_NONE) {
			if (selected)
				painter->fillRect(option.rect, QColor(255, 255, 255, 40));
			else if (hover && !custom.isValid())
				painter->fillRect(option.rect, QColor(255, 255, 255, 22));
		}

		QStyleOptionViewItem opt(option);
		opt.rect = itemRect;
		opt.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver | QStyle::State_HasFocus);
		opt.showDecorationSelected = false;
		QColor text = opt.palette.color(QPalette::Text);
		if (state != STATE_NONE)
			text = Qt::white;
		if (hidden)
			text.setAlpha(110);
		opt.palette.setColor(QPalette::Text, text);
		opt.palette.setColor(QPalette::HighlightedText, text);
		itemDelegateForIndex(index)->paint(painter, opt, index);

		if (model()->hasChildren(index)) {
			QRect branch(opt.rect.left() - indentation(), opt.rect.top(), indentation(), opt.rect.height());
			QPointF c = branch.center();
			painter->save();
			painter->setRenderHint(QPainter::Antialiasing);
			QColor chevron = text;
			chevron.setAlpha(state != STATE_NONE ? 255 : 170);
			QPen pen(chevron, 1.6);
			pen.setCapStyle(Qt::RoundCap);
			pen.setJoinStyle(Qt::RoundJoin);
			painter->setPen(pen);
			painter->setBrush(Qt::NoBrush);
			QPainterPath path;
			if (isExpanded(index)) {
				path.moveTo(c.x() - 3.5, c.y() - 1.5);
				path.lineTo(c.x(), c.y() + 2.0);
				path.lineTo(c.x() + 3.5, c.y() - 1.5);
			} else {
				path.moveTo(c.x() - 1.5, c.y() - 3.5);
				path.lineTo(c.x() + 2.0, c.y());
				path.lineTo(c.x() - 1.5, c.y() + 3.5);
			}
			painter->drawPath(path);
			painter->restore();
		}
	}

	void drawBranches(QPainter *, const QRect &, const QModelIndex &) const override {}

private:
	QPersistentModelIndex hoverIndex;
};

class SceneTreeDock;
static QPointer<SceneTreeDock> dockWidget;

static void layout_undo_redo(const char *data);
static void scene_add_undo(const char *data);
static void scene_add_redo(const char *data);
static void scene_remove_undo(const char *data);
static void scene_remove_redo(const char *data);
static void scene_rename_undo(const char *data);
static void scene_rename_redo(const char *data);
static void scene_duplicate_undo(const char *data);
static void scene_duplicate_redo(const char *data);

static bool save_undo_source_enum(obs_scene_t *, obs_sceneitem_t *item, void *p)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (obs_obj_is_private(source) && !obs_source_removed(source))
		return true;
	obs_data_array_t *array = static_cast<obs_data_array_t *>(p);
	const char *name = obs_source_get_name(source);
	size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *stored = obs_data_array_item(array, i);
		bool same = strcmp(name, obs_data_get_string(stored, "name")) == 0;
		obs_data_release(stored);
		if (same)
			return true;
	}
	if (obs_source_is_group(source))
		obs_scene_enum_items(obs_group_from_source(source), save_undo_source_enum, p);
	obs_data_t *data = obs_save_source(source);
	obs_data_array_push_back(array, data);
	obs_data_release(data);
	return true;
}

static void remove_scene_and_prune(obs_source_t *source)
{
	obs_source_remove(source);
	auto prune = [](void *, obs_source_t *scene) {
		if (strcmp(obs_source_get_id(scene), "scene") == 0)
			obs_scene_prune_sources(obs_scene_from_source(scene));
		return true;
	};
	obs_enum_scenes(prune, nullptr);
}

class SceneTreeDock : public QWidget {
public:
	SceneTreeDock()
	{
		QColor iconColor = palette().color(QPalette::Text);
		folderClosedIcon = lucideIcon("folder", iconColor);
		folderOpenIcon = lucideIcon("folder-open", iconColor);
		sceneIcon = lucideIcon("monitor", iconColor);
		playIcon = lucideIcon("play", Qt::white, true);
		lockIcon = lucideIcon("lock", iconColor);
		unlockIcon = lucideIcon("lock-open", iconColor);
		collapseIcon = lucideIcon("chevrons-down-up", iconColor);
		expandIcon = lucideIcon("chevrons-up-down", iconColor);
		gearIcon = lucideIcon("settings", iconColor);

		loadSettings();

		QVBoxLayout *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);
		layout->setSpacing(4);

		search = new QLineEdit;
		search->setPlaceholderText(T("Search"));
		search->setClearButtonEnabled(true);
		search->setVisible(settings.showSearch);
		connect(search, &QLineEdit::textChanged, this, [this](const QString &) { applyVisibility(); });
		layout->addWidget(search);

		tree = new SceneTreeWidget;
		delegate = new RowDelegate;
		delegate->rowHeight = settings.itemHeight;
		tree->setItemDelegate(delegate);
		tree->setHeaderHidden(true);
		tree->setDropIndicatorShown(true);
		tree->setDragDropMode(QAbstractItemView::InternalMove);
		tree->setDefaultDropAction(Qt::MoveAction);
		tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
		tree->setContextMenuPolicy(Qt::CustomContextMenu);
		tree->setIndentation(16);
		tree->onDropped = [this]() {
			persist();
		};
		connect(tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
			Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
			if (mods & (Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier))
				return;
			activate(item, false);
		});
		connect(tree, &QTreeWidget::itemDoubleClicked, this,
			[this](QTreeWidgetItem *item, int) { activate(item, true); });
		connect(tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int) { renamed(item); });
		connect(tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
			applyItemIcon(item);
			updateToolbar();
			persistLater();
		});
		connect(tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
			applyItemIcon(item);
			updateToolbar();
			persistLater();
		});
		connect(tree, &QTreeWidget::currentItemChanged, this,
			[this](QTreeWidgetItem *, QTreeWidgetItem *) { updateToolbar(); });
		connect(tree, &QWidget::customContextMenuRequested, this,
			[this](const QPoint &pos) { contextMenu(pos); });
		layout->addWidget(tree, 1);

		QToolBar *toolbar = new QToolBar;
		toolbar->setObjectName("sceneTreeToolbar");
		toolbar->setIconSize(QSize(16, 16));
		toolbar->setFloatable(false);
		toolbar->setMovable(false);
		addButton = toolButton("icon-plus", L("AddScene"), [this]() { addScene(); });
		removeButton = toolButton("icon-trash", L("Remove"), [this]() { removeScenes(selectedScenes()); });
		filtersButton = toolButton("icon-filter", L("Filters"), [this]() { openSelectedFilters(); });
		upButton = toolButton("icon-up", L("Basic.MainMenu.Edit.Order.MoveUp"), [this]() { moveSelected(-1); });
		downButton =
			toolButton("icon-down", L("Basic.MainMenu.Edit.Order.MoveDown"), [this]() { moveSelected(1); });
		toolbar->addWidget(addButton);
		toolbar->addWidget(removeButton);
		toolbar->addSeparator();
		toolbar->addWidget(filtersButton);
		toolbar->addSeparator();
		toolbar->addWidget(upButton);
		toolbar->addWidget(downButton);

		QWidget *spacer = new QWidget;
		spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		toolbar->addWidget(spacer);

		lockButton = toolButton(nullptr, T("Lock"), [this]() { setLocked(!locked); });
		collapseButton = toolButton(nullptr, T("CollapseAll"), [this]() { toggleCollapse(); });
		toolbar->addWidget(lockButton);
		toolbar->addWidget(collapseButton);
		toolbar->addSeparator();

		QToolButton *gear = toolButton(nullptr, T("Settings"), [this]() { openSettings(); });
		gear->setIcon(gearIcon);
		toolbar->addWidget(gear);
		layout->addWidget(toolbar);

		setLocked(false);

		obs_frontend_add_event_callback(frontendEvent, this);
		obs_frontend_add_save_callback(saveCallback, this);
		signal_handler_connect(obs_get_signal_handler(), "source_rename", sourceRenamed, this);
	}

	~SceneTreeDock() override { detach(); }

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		hideCloseButton();
	}

public:
	void hideCloseButton()
	{
		QWidget *p = parentWidget();
		while (p) {
			QDockWidget *dock = qobject_cast<QDockWidget *>(p);
			if (dock) {
				dock->setFeatures(dock->features() & ~QDockWidget::DockWidgetClosable);
				return;
			}
			p = p->parentWidget();
		}
	}

	void detach()
	{
		if (detached)
			return;
		detached = true;
		signal_handler_disconnect(obs_get_signal_handler(), "source_rename", sourceRenamed, this);
		obs_frontend_remove_save_callback(saveCallback, this);
		obs_frontend_remove_event_callback(frontendEvent, this);
	}

	void syncScenes()
	{
		if (detached)
			return;
		applying = true;

		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);

		QHash<QString, QTreeWidgetItem *> existing;
		collectScenes(tree->invisibleRootItem(), existing);

		QSet<QString> live;
		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *scene = scenes.sources.array[i];
			QString uuid = obs_source_get_uuid(scene);
			QString name = obs_source_get_name(scene);
			live.insert(uuid);
			QTreeWidgetItem *item = existing.value(uuid);
			if (item) {
				if (item->text(0) != name)
					item->setText(0, name);
			} else {
				tree->invisibleRootItem()->addChild(makeScene(uuid, name));
			}
		}
		obs_frontend_source_list_free(&scenes);

		for (auto it = existing.begin(); it != existing.end(); ++it) {
			if (!live.contains(it.key()))
				delete it.value();
		}

		applying = false;
		autoSortAll();
		highlightCurrent();
		applyVisibility();
		updateToolbar();
		lastLayoutJson = layoutJson();
	}

	void applyLayout(obs_data_t *layout)
	{
		applying = true;
		tree->clear();
		bool wasLocked = false;
		if (layout) {
			obs_data_array_t *items = obs_data_get_array(layout, "items");
			buildItems(tree->invisibleRootItem(), items);
			obs_data_array_release(items);
			wasLocked = obs_data_get_bool(layout, "locked");
		}
		applying = false;
		if (!settings.rememberExpansion)
			tree->collapseAll();
		setLocked(wasLocked, false);
		syncScenes();
	}

	QString layoutJson()
	{
		obs_data_t *layout = saveLayout();
		QString json = QString::fromUtf8(obs_data_get_json(layout));
		obs_data_release(layout);
		return json;
	}

	void applyLayoutJson(const QString &json)
	{
		obs_data_t *layout = obs_data_create_from_json(json.toUtf8().constData());
		if (!layout)
			return;
		applyLayout(layout);
		obs_data_release(layout);
		lastLayoutJson = layoutJson();
		obs_frontend_save();
	}

	void recordSceneAction(const QString &name, undo_redo_cb undo, undo_redo_cb redo, const QString &undoData,
			       const QString &redoData)
	{
		obs_frontend_add_undo_redo_action(name.toUtf8().constData(), undo, redo, undoData.toUtf8().constData(),
						  redoData.toUtf8().constData(), false);
	}

	obs_data_t *saveLayout()
	{
		obs_data_t *layout = obs_data_create();
		obs_data_array_t *items = obs_data_array_create();
		collectItems(tree->invisibleRootItem(), items);
		obs_data_set_array(layout, "items", items);
		obs_data_set_bool(layout, "locked", locked);
		obs_data_array_release(items);
		return layout;
	}

	void highlightCurrent()
	{
		if (detached)
			return;
		bool studio = obs_frontend_preview_program_mode_active();
		obs_source_t *program = obs_frontend_get_current_scene();
		obs_source_t *preview = studio ? obs_frontend_get_current_preview_scene() : nullptr;
		QString programUuid = program ? obs_source_get_uuid(program) : QString();
		QString previewUuid = preview ? obs_source_get_uuid(preview) : QString();
		obs_source_release(program);
		obs_source_release(preview);

		applying = true;
		QTreeWidgetItem *focus = nullptr;
		QTreeWidgetItemIterator it(tree);
		while (*it) {
			QTreeWidgetItem *item = *it;
			if (isScene(item)) {
				QString uuid = item->data(0, ROLE_UUID).toString();
				int state = STATE_NONE;
				if (uuid == programUuid)
					state = STATE_LIVE;
				else if (studio && uuid == previewUuid)
					state = STATE_PREVIEW;
				item->setData(0, ROLE_STATE, state);
				applyItemIcon(item);
				if (uuid == (studio ? previewUuid : programUuid))
					focus = item;
			}
			++it;
		}
		if (focus && tree->currentItem() != focus) {
			bool autoScroll = tree->hasAutoScroll();
			tree->setAutoScroll(false);
			tree->setCurrentItem(focus, 0, QItemSelectionModel::NoUpdate);
			tree->setAutoScroll(autoScroll);
		}
		tree->viewport()->update();
		applying = false;
	}

private:
	static void frontendEvent(enum obs_frontend_event event, void *data)
	{
		SceneTreeDock *dock = static_cast<SceneTreeDock *>(data);
		switch (event) {
		case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
			dock->syncScenes();
			break;
		case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
			dock->highlightCurrent();
			break;
		case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		case OBS_FRONTEND_EVENT_EXIT:
			dock->detach();
			break;
		default:
			break;
		}
	}

	static void saveCallback(obs_data_t *data, bool saving, void *priv)
	{
		SceneTreeDock *dock = static_cast<SceneTreeDock *>(priv);
		if (saving) {
			obs_data_t *layout = dock->saveLayout();
			obs_data_set_obj(data, SAVE_KEY, layout);
			obs_data_release(layout);
		} else {
			obs_data_t *layout = obs_data_get_obj(data, SAVE_KEY);
			if (layout) {
				dock->applyLayout(layout);
				obs_data_release(layout);
			} else {
				dock->applyLayout(nullptr);
				dock->importFromStreamUp(false);
			}
		}
	}

	static void sourceRenamed(void *data, calldata_t *)
	{
		SceneTreeDock *dock = static_cast<SceneTreeDock *>(data);
		QMetaObject::invokeMethod(dock, [dock]() { dock->syncScenes(); }, Qt::QueuedConnection);
	}

	QString settingsFile()
	{
		char *path = obs_module_config_path("settings.json");
		QString file = path ? QString::fromUtf8(path) : QString();
		bfree(path);
		return file;
	}

	void loadSettings()
	{
		QString file = settingsFile();
		if (file.isEmpty())
			return;
		obs_data_t *data = obs_data_create_from_json_file(file.toUtf8().constData());
		if (!data)
			return;
		TreeSettings d;
		obs_data_set_default_bool(data, "show_search", d.showSearch);
		obs_data_set_default_bool(data, "show_icons", d.showIcons);
		obs_data_set_default_bool(data, "show_hidden", d.showHidden);
		obs_data_set_default_bool(data, "remember_expansion", d.rememberExpansion);
		obs_data_set_default_int(data, "item_height", d.itemHeight);
		obs_data_set_default_bool(data, "double_click_switch", d.doubleClickSwitch);
		obs_data_set_default_bool(data, "no_preview_switch", d.noPreviewSwitch);
		obs_data_set_default_bool(data, "no_transition", d.noTransition);
		obs_data_set_default_bool(data, "switch_on_duplicate", d.switchOnDuplicate);
		obs_data_set_default_bool(data, "auto_sort", d.autoSort);
		obs_data_set_default_int(data, "auto_sort_mode", d.autoSortMode);
		obs_data_set_default_bool(data, "folders_on_top", d.foldersOnTop);
		settings.showSearch = obs_data_get_bool(data, "show_search");
		settings.showIcons = obs_data_get_bool(data, "show_icons");
		settings.showHidden = obs_data_get_bool(data, "show_hidden");
		settings.rememberExpansion = obs_data_get_bool(data, "remember_expansion");
		settings.itemHeight = std::max(18, std::min(48, int(obs_data_get_int(data, "item_height"))));
		settings.doubleClickSwitch = obs_data_get_bool(data, "double_click_switch");
		settings.noPreviewSwitch = obs_data_get_bool(data, "no_preview_switch");
		settings.noTransition = obs_data_get_bool(data, "no_transition");
		settings.switchOnDuplicate = obs_data_get_bool(data, "switch_on_duplicate");
		settings.autoSort = obs_data_get_bool(data, "auto_sort");
		settings.autoSortMode = std::max(0, std::min(3, int(obs_data_get_int(data, "auto_sort_mode"))));
		settings.foldersOnTop = obs_data_get_bool(data, "folders_on_top");
		obs_data_release(data);
	}

	void saveSettings()
	{
		QString file = settingsFile();
		if (file.isEmpty())
			return;
		char *dir = obs_module_config_path("");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
		obs_data_t *data = obs_data_create();
		obs_data_set_bool(data, "show_search", settings.showSearch);
		obs_data_set_bool(data, "show_icons", settings.showIcons);
		obs_data_set_bool(data, "show_hidden", settings.showHidden);
		obs_data_set_bool(data, "remember_expansion", settings.rememberExpansion);
		obs_data_set_int(data, "item_height", settings.itemHeight);
		obs_data_set_bool(data, "double_click_switch", settings.doubleClickSwitch);
		obs_data_set_bool(data, "no_preview_switch", settings.noPreviewSwitch);
		obs_data_set_bool(data, "no_transition", settings.noTransition);
		obs_data_set_bool(data, "switch_on_duplicate", settings.switchOnDuplicate);
		obs_data_set_bool(data, "auto_sort", settings.autoSort);
		obs_data_set_int(data, "auto_sort_mode", settings.autoSortMode);
		obs_data_set_bool(data, "folders_on_top", settings.foldersOnTop);
		obs_data_save_json_safe(data, file.toUtf8().constData(), "tmp", "bak");
		obs_data_release(data);
	}

	void applySettings()
	{
		search->setVisible(settings.showSearch);
		if (!settings.showSearch)
			search->clear();
		delegate->rowHeight = settings.itemHeight;
		tree->doItemsLayout();
		applyIcons();
		applyVisibility();
		if (settings.autoSort)
			autoSortAll();
		setLocked(locked, false);
		saveSettings();
	}

	bool streamUpAvailable()
	{
		obs_data_array_t *items = streamUpTreeForCurrentCollection();
		bool available = items != nullptr;
		obs_data_array_release(items);
		return available;
	}

	void openSettings()
	{
		QDialog dialog(this);
		dialog.setWindowTitle(T("Settings.Title"));
		dialog.resize(640, 440);
		QVBoxLayout *root = new QVBoxLayout(&dialog);

		QHBoxLayout *body = new QHBoxLayout;
		QListWidget *nav = new QListWidget;
		nav->setFixedWidth(170);
		nav->setSpacing(2);
		QStackedWidget *pages = new QStackedWidget;
		body->addWidget(nav);
		body->addWidget(pages, 1);
		root->addLayout(body, 1);

		auto addPage = [nav, pages](const char *title) {
			QWidget *page = new QWidget;
			QFormLayout *form = new QFormLayout(page);
			form->setContentsMargins(12, 12, 12, 12);
			form->setHorizontalSpacing(24);
			form->setVerticalSpacing(12);
			form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
			QLabel *heading = new QLabel(T(title));
			QFont font = heading->font();
			font.setBold(true);
			font.setPointSizeF(font.pointSizeF() + 2.0);
			heading->setFont(font);
			form->addRow(heading);
			nav->addItem(T(title));
			pages->addWidget(page);
			return form;
		};

		auto check = [this](QFormLayout *form, const char *label, bool &field) {
			QCheckBox *box = new QCheckBox;
			box->setChecked(field);
			connect(box, &QCheckBox::toggled, this, [this, &field](bool on) {
				field = on;
				applySettings();
			});
			form->addRow(T(label), box);
			return box;
		};

		QFormLayout *display = addPage("Settings.Display");
		check(display, "ShowSearch", settings.showSearch);
		check(display, "ShowIcons", settings.showIcons);
		check(display, "ShowHidden", settings.showHidden);
		check(display, "RememberExpansion", settings.rememberExpansion);
		QWidget *heightRow = new QWidget;
		QHBoxLayout *heightLayout = new QHBoxLayout(heightRow);
		heightLayout->setContentsMargins(0, 0, 0, 0);
		QSlider *height = new QSlider(Qt::Horizontal);
		height->setRange(18, 48);
		height->setValue(settings.itemHeight);
		QLabel *heightValue = new QLabel(QString("%1 px").arg(settings.itemHeight));
		heightValue->setMinimumWidth(44);
		connect(height, &QSlider::valueChanged, this, [this, heightValue](int value) {
			settings.itemHeight = value;
			heightValue->setText(QString("%1 px").arg(value));
			applySettings();
		});
		heightLayout->addWidget(height, 1);
		heightLayout->addWidget(heightValue);
		display->addRow(T("ItemHeight"), heightRow);

		QFormLayout *behaviour = addPage("Settings.Behaviour");
		QComboBox *mode = new QComboBox;
		mode->addItem(T("SwitchMode.Single"));
		mode->addItem(T("SwitchMode.Double"));
		mode->setCurrentIndex(settings.doubleClickSwitch ? 1 : 0);
		connect(mode, &QComboBox::currentIndexChanged, this, [this](int index) {
			settings.doubleClickSwitch = index == 1;
			applySettings();
		});
		behaviour->addRow(T("SwitchMode"), mode);
		check(behaviour, "NoPreviewSwitch", settings.noPreviewSwitch);
		check(behaviour, "NoTransition", settings.noTransition);
		check(behaviour, "SwitchOnDuplicate", settings.switchOnDuplicate);

		QFormLayout *sorting = addPage("Settings.Sorting");
		QCheckBox *autoSort = new QCheckBox;
		autoSort->setChecked(settings.autoSort);
		QComboBox *sortMode = new QComboBox;
		sortMode->addItem(T("Sort.AZ"));
		sortMode->addItem(T("Sort.ZA"));
		sortMode->addItem(T("Sort.Newest"));
		sortMode->addItem(T("Sort.Oldest"));
		sortMode->setCurrentIndex(settings.autoSortMode);
		QCheckBox *foldersTop = new QCheckBox;
		foldersTop->setChecked(settings.foldersOnTop);
		QLabel *autoSortHint = new QLabel(T("AutoSort.Hint"));
		autoSortHint->setWordWrap(true);
		autoSortHint->setEnabled(false);
		connect(autoSort, &QCheckBox::toggled, this, [this](bool on) {
			settings.autoSort = on;
			applySettings();
		});
		connect(sortMode, &QComboBox::currentIndexChanged, this, [this](int index) {
			settings.autoSortMode = index;
			applySettings();
		});
		connect(foldersTop, &QCheckBox::toggled, this, [this](bool on) {
			settings.foldersOnTop = on;
			applySettings();
		});
		sorting->addRow(T("AutoSort"), autoSort);
		sorting->addRow(T("SortMethod"), sortMode);
		sorting->addRow(T("FoldersOnTop"), foldersTop);
		sorting->addRow(autoSortHint);

		QFormLayout *transfer = addPage("Settings.Transfer");
		QPushButton *exportButton = new QPushButton(T("ExportJson"));
		QPushButton *importButton = new QPushButton(T("ImportJson"));
		connect(exportButton, &QPushButton::clicked, this, [this]() { exportJson(); });
		connect(importButton, &QPushButton::clicked, this, [this]() { importJson(); });
		transfer->addRow(exportButton);
		transfer->addRow(importButton);
		QLabel *streamUpHint = new QLabel(T("ImportStreamUp.Hint"));
		streamUpHint->setWordWrap(true);
		streamUpHint->setEnabled(false);
		transfer->addRow(streamUpHint);
		QPushButton *streamUpButton = new QPushButton(T("ImportStreamUp"));
		streamUpButton->setEnabled(streamUpAvailable());
		connect(streamUpButton, &QPushButton::clicked, this, [this]() {
			QMessageBox::StandardButton answer =
				QMessageBox::warning(this, T("DockTitle"), T("ImportStreamUp.Warning"),
						     QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (answer == QMessageBox::Yes && !importFromStreamUp(true))
				QMessageBox::information(this, T("DockTitle"), T("ImportStreamUp.Missing"));
		});
		transfer->addRow(streamUpButton);

		connect(nav, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
		nav->setCurrentRow(0);

		QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
		connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		root->addWidget(buttons);
		dialog.exec();
	}

	QToolButton *toolButton(const char *iconClass, const QString &tooltip, std::function<void()> handler)
	{
		QToolButton *button = new QToolButton;
		if (iconClass)
			button->setProperty("class", iconClass);
		button->setToolTip(tooltip);
		button->setAutoRaise(true);
		connect(button, &QToolButton::clicked, this, [handler]() { handler(); });
		return button;
	}

	obs_source_t *selectedScene()
	{
		QTreeWidgetItem *item = tree->currentItem();
		if (!isScene(item))
			return nullptr;
		return obs_get_source_by_uuid(item->data(0, ROLE_UUID).toString().toUtf8().constData());
	}

	QList<QTreeWidgetItem *> selectedScenes()
	{
		QList<QTreeWidgetItem *> scenes;
		for (QTreeWidgetItem *item : tree->selectedItems()) {
			if (isScene(item))
				scenes.append(item);
		}
		if (scenes.isEmpty() && isScene(tree->currentItem()))
			scenes.append(tree->currentItem());
		return scenes;
	}

	QList<QTreeWidgetItem *> targetsFor(QTreeWidgetItem *item)
	{
		QList<QTreeWidgetItem *> selected = tree->selectedItems();
		if (selected.size() > 1 && selected.contains(item))
			return selected;
		return {item};
	}

	QString uniqueSceneName(const QString &base)
	{
		for (int n = 1;; n++) {
			QString candidate = n == 1 ? base : QString("%1 %2").arg(base).arg(n);
			obs_source_t *clash = obs_get_source_by_name(candidate.toUtf8().constData());
			if (!clash)
				return candidate;
			obs_source_release(clash);
		}
	}

	bool askSceneName(QString &name)
	{
		for (;;) {
			bool ok = false;
			name = QInputDialog::getText(this, L("Basic.Main.AddSceneDlg.Title"),
						     L("Basic.Main.AddSceneDlg.Text"), QLineEdit::Normal, name, &ok)
				       .trimmed();
			if (!ok)
				return false;
			if (name.isEmpty()) {
				QMessageBox::warning(this, L("NoNameEntered.Title"), L("NoNameEntered.Text"));
				continue;
			}
			obs_source_t *clash = obs_get_source_by_name(name.toUtf8().constData());
			if (clash) {
				obs_source_release(clash);
				QMessageBox::warning(this, L("NameExists.Title"), L("NameExists.Text"));
				continue;
			}
			return true;
		}
	}

	void addScene()
	{
		QString name = uniqueSceneName(L("Basic.Scene"));
		if (!askSceneName(name))
			return;
		obs_scene_t *scene = obs_scene_create(name.toUtf8().constData());
		if (!scene)
			return;
		obs_frontend_set_current_scene(obs_scene_get_source(scene));
		obs_scene_release(scene);
		recordSceneAction(L("Undo.Add").arg(name), scene_add_undo, scene_add_redo, name, name);
	}

	void removeScenes(const QList<QTreeWidgetItem *> &items)
	{
		if (items.isEmpty() || locked)
			return;
		QString question = items.size() == 1 ? L("ConfirmRemove.Text").arg(items.first()->text(0))
						     : L("ConfirmRemove.TextMultiple").arg(items.size());
		QMessageBox::StandardButton answer = QMessageBox::question(
			this, L("ConfirmRemove.Title"), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
		QStringList uuids;
		for (QTreeWidgetItem *item : items)
			uuids.append(item->data(0, ROLE_UUID).toString());
		for (const QString &uuid : uuids) {
			obs_source_t *scene = obs_get_source_by_uuid(uuid.toUtf8().constData());
			if (scene)
				removeSceneWithUndo(scene);
			obs_source_release(scene);
		}
	}

	void removeSceneWithUndo(obs_source_t *source)
	{
		obs_scene_t *scene = obs_scene_from_source(source);
		QString name = obs_source_get_name(source);

		obs_data_array_t *inScene = obs_data_array_create();
		obs_scene_enum_items(scene, save_undo_source_enum, inScene);
		obs_data_t *sceneData = obs_save_source(source);
		obs_data_array_push_back(inScene, sceneData);
		obs_data_release(sceneData);

		obs_data_array_t *usedIn = obs_data_array_create();
		struct Ctx {
			obs_source_t *removed;
			obs_data_array_t *usedIn;
		} ctx = {source, usedIn};
		auto collect = [](void *ptr, obs_source_t *other) {
			Ctx *c = static_cast<Ctx *>(ptr);
			if (strcmp(obs_source_get_name(other), obs_source_get_name(c->removed)) == 0)
				return true;
			obs_sceneitem_t *item = obs_scene_find_source(obs_group_or_scene_from_source(other),
								      obs_source_get_name(c->removed));
			if (item) {
				obs_data_t *data = obs_save_source(obs_scene_get_source(obs_sceneitem_get_scene(item)));
				obs_data_array_push_back(c->usedIn, data);
				obs_data_release(data);
			}
			return true;
		};
		obs_enum_scenes(collect, &ctx);

		obs_data_t *undoData = obs_data_create();
		obs_data_set_array(undoData, "sources_in_deleted_scene", inScene);
		obs_data_set_array(undoData, "scene_used_in_other_scenes", usedIn);
		obs_data_set_string(undoData, "layout", layoutJson().toUtf8().constData());
		obs_data_array_release(inScene);
		obs_data_array_release(usedIn);

		remove_scene_and_prune(source);
		syncScenes();

		obs_data_t *redoData = obs_data_create();
		obs_data_set_string(redoData, "name", name.toUtf8().constData());
		obs_data_set_string(redoData, "layout", layoutJson().toUtf8().constData());

		recordSceneAction(L("Undo.Delete").arg(name), scene_remove_undo, scene_remove_redo,
				  obs_data_get_json(undoData), obs_data_get_json(redoData));
		obs_data_release(undoData);
		obs_data_release(redoData);
	}

public:
	void restoreRemovedScene(const char *json)
	{
		obs_data_t *base = obs_data_create_from_json(json);
		if (!base)
			return;
		obs_data_array_t *inScene = obs_data_get_array(base, "sources_in_deleted_scene");
		obs_data_array_t *usedIn = obs_data_get_array(base, "scene_used_in_other_scenes");

		QList<obs_source_t *> created;
		size_t count = obs_data_array_count(inScene);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *data = obs_data_array_item(inScene, i);
			obs_source_t *existing = obs_get_source_by_name(obs_data_get_string(data, "name"));
			if (existing)
				obs_source_release(existing);
			else
				created.append(obs_load_source(data));
			obs_data_release(data);
		}
		for (obs_source_t *source : created)
			obs_source_load2(source);

		size_t usedCount = obs_data_array_count(usedIn);
		for (size_t i = 0; i < usedCount; i++) {
			obs_data_t *data = obs_data_array_item(usedIn, i);
			obs_source_t *other = obs_get_source_by_name(obs_data_get_string(data, "name"));
			if (other) {
				obs_data_t *settings = obs_data_get_obj(data, "settings");
				obs_data_array_t *items = obs_data_get_array(settings, "items");
				QList<obs_source_t *> keep;
				auto clear = [](obs_scene_t *, obs_sceneitem_t *item, void *ptr) {
					QList<obs_source_t *> *list = static_cast<QList<obs_source_t *> *>(ptr);
					obs_source_t *src = obs_sceneitem_get_source(item);
					list->append(obs_source_get_ref(src));
					obs_sceneitem_remove(item);
					return true;
				};
				obs_scene_enum_items(obs_group_or_scene_from_source(other), clear, &keep);
				obs_sceneitems_add(obs_group_or_scene_from_source(other), items);
				for (obs_source_t *src : keep)
					obs_source_release(src);
				obs_data_array_release(items);
				obs_data_release(settings);
				obs_source_release(other);
			}
			obs_data_release(data);
		}

		if (!created.isEmpty())
			obs_frontend_set_current_scene(created.last());
		for (obs_source_t *source : created)
			obs_source_release(source);

		obs_data_array_release(inScene);
		obs_data_array_release(usedIn);
		applyLayoutJson(QString::fromUtf8(obs_data_get_string(base, "layout")));
		obs_data_release(base);
	}

private:
	void openSelectedFilters()
	{
		obs_source_t *scene = selectedScene();
		if (!scene)
			return;
		obs_frontend_open_source_filters(scene);
		obs_source_release(scene);
	}

	void setLocked(bool value, bool save = true)
	{
		locked = value;
		bool frozen = locked || settings.autoSort;
		tree->setDragEnabled(!frozen);
		tree->setAcceptDrops(!frozen);
		tree->setEditTriggers(
			locked ? QAbstractItemView::NoEditTriggers
			       : (QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked));
		lockButton->setIcon(locked ? lockIcon : unlockIcon);
		lockButton->setToolTip(locked ? T("Unlock") : T("Lock"));
		updateToolbar();
		if (save)
			persist();
	}

	bool anyFolderExpanded()
	{
		QTreeWidgetItemIterator it(tree);
		while (*it) {
			if (isFolder(*it) && (*it)->isExpanded())
				return true;
			++it;
		}
		return false;
	}

	void toggleCollapse()
	{
		if (anyFolderExpanded())
			tree->collapseAll();
		else
			tree->expandAll();
		updateToolbar();
	}

	void updateToolbar()
	{
		QTreeWidgetItem *item = tree->currentItem();
		bool scene = isScene(item);
		QTreeWidgetItem *parent = item ? (item->parent() ? item->parent() : tree->invisibleRootItem())
					       : nullptr;
		int index = parent ? parent->indexOfChild(item) : -1;
		removeButton->setEnabled(scene && !locked);
		filtersButton->setEnabled(scene);
		bool frozen = locked || settings.autoSort;
		upButton->setEnabled(item && !frozen && index > 0);
		downButton->setEnabled(item && !frozen && parent && index < parent->childCount() - 1);
		bool expanded = anyFolderExpanded();
		collapseButton->setIcon(expanded ? collapseIcon : expandIcon);
		collapseButton->setToolTip(expanded ? T("CollapseAll") : T("ExpandAll"));
	}

	void moveItemTo(QTreeWidgetItem *item, int target)
	{
		if (!item || locked)
			return;
		QTreeWidgetItem *parent = item->parent() ? item->parent() : tree->invisibleRootItem();
		int index = parent->indexOfChild(item);
		target = std::max(0, std::min(target, parent->childCount() - 1));
		if (target == index)
			return;
		bool expanded = item->isExpanded();
		applying = true;
		parent->takeChild(index);
		parent->insertChild(target, item);
		item->setExpanded(expanded);
		tree->setCurrentItem(item);
		applying = false;
		applyItemIcon(item);
		updateToolbar();
		persist();
	}

	void moveItem(QTreeWidgetItem *item, int direction)
	{
		if (!item)
			return;
		QTreeWidgetItem *parent = item->parent() ? item->parent() : tree->invisibleRootItem();
		moveItemTo(item, parent->indexOfChild(item) + direction);
	}

	void moveSelected(int direction)
	{
		QList<QTreeWidgetItem *> selected = tree->selectedItems();
		if (selected.isEmpty()) {
			moveItem(tree->currentItem(), direction);
			return;
		}
		std::sort(selected.begin(), selected.end(), [](QTreeWidgetItem *a, QTreeWidgetItem *b) {
			QTreeWidgetItem *pa = a->parent();
			QTreeWidgetItem *pb = b->parent();
			int ia = pa ? pa->indexOfChild(a) : a->treeWidget()->indexOfTopLevelItem(a);
			int ib = pb ? pb->indexOfChild(b) : b->treeWidget()->indexOfTopLevelItem(b);
			return ia < ib;
		});
		if (direction > 0)
			std::reverse(selected.begin(), selected.end());
		for (QTreeWidgetItem *item : selected)
			moveItem(item, direction);
		for (QTreeWidgetItem *item : selected)
			item->setSelected(true);
	}

	void placeAfter(QTreeWidgetItem *item, QTreeWidgetItem *anchor)
	{
		if (!item || !anchor || item == anchor)
			return;
		QTreeWidgetItem *from = item->parent() ? item->parent() : tree->invisibleRootItem();
		QTreeWidgetItem *to = anchor->parent() ? anchor->parent() : tree->invisibleRootItem();
		applying = true;
		from->takeChild(from->indexOfChild(item));
		to->insertChild(to->indexOfChild(anchor) + 1, item);
		tree->setCurrentItem(item);
		applying = false;
	}

	QTreeWidgetItem *findSceneItem(const QString &uuid)
	{
		QTreeWidgetItemIterator it(tree);
		while (*it) {
			if (isScene(*it) && (*it)->data(0, ROLE_UUID).toString() == uuid)
				return *it;
			++it;
		}
		return nullptr;
	}

	void duplicateScene(QTreeWidgetItem *item)
	{
		obs_source_t *scene = obs_get_source_by_uuid(item->data(0, ROLE_UUID).toString().toUtf8().constData());
		if (!scene)
			return;
		QString base = obs_source_get_name(scene);
		QString candidate;
		for (int n = 2;; n++) {
			candidate = QString("%1 %2").arg(base).arg(n);
			obs_source_t *clash = obs_get_source_by_name(candidate.toUtf8().constData());
			if (!clash)
				break;
			obs_source_release(clash);
		}
		bool ok = false;
		QString name = QInputDialog::getText(this, L("Basic.Main.AddSceneDlg.Title"),
						     L("Basic.Main.AddSceneDlg.Text"), QLineEdit::Normal, candidate,
						     &ok)
				       .trimmed();
		if (ok && !name.isEmpty()) {
			obs_source_t *clash = obs_get_source_by_name(name.toUtf8().constData());
			if (clash) {
				obs_source_release(clash);
			} else {
				QString before = layoutJson();
				obs_scene_t *copy = obs_scene_duplicate(obs_scene_from_source(scene),
									name.toUtf8().constData(), OBS_SCENE_DUP_REFS);
				if (copy) {
					obs_source_t *copySource = obs_scene_get_source(copy);
					QString uuid = obs_source_get_uuid(copySource);
					syncScenes();
					placeAfter(findSceneItem(uuid), item);
					if (settings.switchOnDuplicate)
						obs_frontend_set_current_scene(copySource);
					obs_scene_release(copy);
					persist(false);
					obs_data_t *undoData = obs_data_create();
					obs_data_set_string(undoData, "copy", name.toUtf8().constData());
					obs_data_set_string(undoData, "layout", before.toUtf8().constData());
					obs_data_t *redoData = obs_data_create();
					obs_data_set_string(redoData, "original", base.toUtf8().constData());
					obs_data_set_string(redoData, "copy", name.toUtf8().constData());
					obs_data_set_string(redoData, "layout", layoutJson().toUtf8().constData());
					recordSceneAction(L("Undo.Scene.Duplicate").arg(name), scene_duplicate_undo,
							  scene_duplicate_redo, obs_data_get_json(undoData),
							  obs_data_get_json(redoData));
					obs_data_release(undoData);
					obs_data_release(redoData);
				}
			}
		}
		obs_source_release(scene);
	}

	void autoSortAll()
	{
		if (!settings.autoSort)
			return;
		QList<QTreeWidgetItem *> parents;
		parents.append(tree->invisibleRootItem());
		QTreeWidgetItemIterator it(tree);
		while (*it) {
			if (isFolder(*it))
				parents.append(*it);
			++it;
		}
		bool wasLocked = locked;
		locked = false;
		for (QTreeWidgetItem *parent : parents)
			sortChildren(parent, settings.autoSortMode, false);
		locked = wasLocked;
	}

	void sortChildren(QTreeWidgetItem *parent, int mode, bool save = true)
	{
		if (locked || !parent)
			return;
		QHash<QString, int> order;
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++)
			order.insert(obs_source_get_uuid(scenes.sources.array[i]), int(i));
		obs_frontend_source_list_free(&scenes);

		QList<QTreeWidgetItem *> children;
		QHash<QTreeWidgetItem *, bool> expanded;
		while (parent->childCount() > 0) {
			QTreeWidgetItem *child = parent->child(0);
			expanded.insert(child, child->isExpanded());
			parent->takeChild(0);
			children.append(child);
		}
		auto sceneIndex = [&order](QTreeWidgetItem *item) {
			return isScene(item) ? order.value(item->data(0, ROLE_UUID).toString(), -1) : -1;
		};
		bool foldersTop = settings.foldersOnTop;
		std::stable_sort(children.begin(), children.end(),
				 [mode, foldersTop, sceneIndex](QTreeWidgetItem *a, QTreeWidgetItem *b) {
					 if (foldersTop && isFolder(a) != isFolder(b))
						 return isFolder(a);
					 if (mode == 0 || mode == 1) {
						 int cmp = QString::localeAwareCompare(a->text(0), b->text(0));
						 return mode == 0 ? cmp < 0 : cmp > 0;
					 }
					 if (isFolder(a) || isFolder(b))
						 return false;
					 return mode == 2 ? sceneIndex(a) > sceneIndex(b)
							  : sceneIndex(a) < sceneIndex(b);
				 });
		applying = true;
		for (QTreeWidgetItem *child : children) {
			parent->addChild(child);
			child->setExpanded(expanded.value(child));
			applyItemIcon(child);
		}
		applying = false;
		updateToolbar();
		if (save)
			persist();
	}

	void sortMenu(QMenu *parent, QTreeWidgetItem *target)
	{
		QMenu *menu = parent->addMenu(T("Sort"));
		menu->setEnabled(!locked);
		const char *labels[] = {"Sort.AZ", "Sort.ZA", "Sort.Newest", "Sort.Oldest"};
		for (int mode = 0; mode < 4; mode++)
			menu->addAction(T(labels[mode]), this, [this, target, mode]() { sortChildren(target, mode); });
	}

	void colorMenu(QMenu *parent, QTreeWidgetItem *item)
	{
		QMenu *menu = parent->addMenu(T("SetColor"));
		menu->setEnabled(!locked);
		QString current = item->data(0, ROLE_COLOR).toString();
		QAction *none = menu->addAction(T("NoColor"), this, [this, item]() { setItemColor(item, QString()); });
		none->setCheckable(true);
		none->setChecked(current.isEmpty());
		menu->addSeparator();
		for (const ColorPreset &preset : COLOR_PRESETS) {
			QPixmap swatch(14, 14);
			swatch.fill(QColor(preset.hex));
			QString hex = preset.hex;
			QAction *action = menu->addAction(QIcon(swatch), T(preset.label), this,
							  [this, item, hex]() { setItemColor(item, hex); });
			action->setCheckable(true);
			action->setChecked(current.compare(hex, Qt::CaseInsensitive) == 0);
		}
	}

	void setItemColor(QTreeWidgetItem *item, const QString &hex)
	{
		applying = true;
		for (QTreeWidgetItem *target : targetsFor(item))
			target->setData(0, ROLE_COLOR, hex);
		applying = false;
		tree->viewport()->update();
		persist();
	}

	void setSceneHidden(QTreeWidgetItem *item, bool hidden)
	{
		applying = true;
		item->setData(0, ROLE_HIDDEN, hidden);
		applying = false;
		applyVisibility();
		persist();
	}

	QMenu *transitionOverrideMenu(QMenu *parent, obs_source_t *scene)
	{
		QMenu *menu = parent->addMenu(L("TransitionOverride"));
		obs_data_t *data = obs_source_get_private_settings(scene);
		obs_data_set_default_int(data, "transition_duration", 300);
		QString current = obs_data_get_string(data, "transition");
		int duration = int(obs_data_get_int(data, "transition_duration"));
		obs_data_release(data);
		QString sceneUuid = obs_source_get_uuid(scene);

		QSpinBox *spin = new QSpinBox(menu);
		spin->setRange(50, 20000);
		spin->setSingleStep(50);
		spin->setSuffix(" ms");
		spin->setValue(duration);
		connect(spin, &QSpinBox::valueChanged, this, [sceneUuid](int value) {
			obs_source_t *target = obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
			if (!target)
				return;
			obs_data_t *settings = obs_source_get_private_settings(target);
			obs_data_set_int(settings, "transition_duration", value);
			obs_data_release(settings);
			obs_source_release(target);
		});
		QWidgetAction *spinAction = new QWidgetAction(menu);
		spinAction->setDefaultWidget(spin);
		menu->addAction(spinAction);
		menu->addSeparator();

		auto addChoice = [this, menu, current, sceneUuid](const QString &label, const QString &name) {
			QAction *action = menu->addAction(label);
			action->setCheckable(true);
			action->setChecked(name == current);
			connect(action, &QAction::triggered, this, [sceneUuid, name]() {
				obs_source_t *target = obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
				if (!target)
					return;
				obs_data_t *settings = obs_source_get_private_settings(target);
				obs_data_set_string(settings, "transition", name.toUtf8().constData());
				obs_data_release(settings);
				obs_source_release(target);
			});
		};
		addChoice(L("None"), QString());
		struct obs_frontend_source_list transitions = {};
		obs_frontend_get_transitions(&transitions);
		for (size_t i = 0; i < transitions.sources.num; i++) {
			const char *name = obs_source_get_name(transitions.sources.array[i]);
			addChoice(QString::fromUtf8(name), QString::fromUtf8(name));
		}
		obs_frontend_source_list_free(&transitions);
		return menu;
	}

	void projectorMenu(QMenu *parent, obs_source_t *scene)
	{
		QMenu *menu = parent->addMenu(L("Projector.Open.Scene"));
		QString sceneName = obs_source_get_name(scene);
		QList<QScreen *> screens = QGuiApplication::screens();
		for (int i = 0; i < screens.size(); i++) {
			QRect geo = screens[i]->geometry();
			QString label = QString("%1 %2: %3x%4 @ %5,%6")
						.arg(L("Display"))
						.arg(i + 1)
						.arg(geo.width())
						.arg(geo.height())
						.arg(geo.x())
						.arg(geo.y());
			menu->addAction(label, this, [sceneName, i]() {
				obs_frontend_open_projector("Scene", i, nullptr, sceneName.toUtf8().constData());
			});
		}
		menu->addSeparator();
		menu->addAction(L("Projector.Window"), this, [sceneName]() {
			obs_frontend_open_projector("Scene", -1, nullptr, sceneName.toUtf8().constData());
		});
	}

	void orderMenu(QMenu *parent, QTreeWidgetItem *item)
	{
		QMenu *menu = parent->addMenu(L("Basic.MainMenu.Edit.Order"));
		menu->setEnabled(!locked && !settings.autoSort);
		QTreeWidgetItem *container = item->parent() ? item->parent() : tree->invisibleRootItem();
		int index = container->indexOfChild(item);
		int last = container->childCount() - 1;
		QAction *up = menu->addAction(L("Basic.MainMenu.Edit.Order.MoveUp"), this,
					      [this, item]() { moveItem(item, -1); });
		QAction *down = menu->addAction(L("Basic.MainMenu.Edit.Order.MoveDown"), this,
						[this, item]() { moveItem(item, 1); });
		QAction *top = menu->addAction(L("Basic.MainMenu.Edit.Order.MoveToTop"), this,
					       [this, item]() { moveItemTo(item, 0); });
		QAction *bottom = menu->addAction(L("Basic.MainMenu.Edit.Order.MoveToBottom"), this,
						  [this, item]() { moveItemTo(item, 1 << 20); });
		up->setEnabled(index > 0);
		top->setEnabled(index > 0);
		down->setEnabled(index < last);
		bottom->setEnabled(index < last);
	}

	QIcon customIcon(const QString &name)
	{
		if (name.isEmpty())
			return QIcon();
		auto it = iconCache.find(name);
		if (it != iconCache.end())
			return it.value();
		QIcon icon = lucideIcon(name.toUtf8().constData(), palette().color(QPalette::Text));
		iconCache.insert(name, icon);
		return icon;
	}

	QStringList availableIcons()
	{
		char *dir = obs_module_file("icons");
		QString path = dir ? QString::fromUtf8(dir) : QString();
		bfree(dir);
		if (path.isEmpty())
			return {};
		QStringList names;
		for (const QString &file : QDir(path).entryList(QStringList() << "*.svg", QDir::Files, QDir::Name))
			names.append(file.chopped(4));
		return names;
	}

	void applyItemIcon(QTreeWidgetItem *item)
	{
		if (!item)
			return;
		if (!settings.showIcons) {
			item->setIcon(0, QIcon());
			return;
		}
		if (isScene(item) && item->data(0, ROLE_STATE).toInt() == STATE_LIVE) {
			item->setIcon(0, playIcon);
			return;
		}
		QString custom = item->data(0, ROLE_ICON).toString();
		if (!custom.isEmpty()) {
			QIcon icon = customIcon(custom);
			if (!icon.isNull()) {
				item->setIcon(0, icon);
				return;
			}
		}
		if (isFolder(item))
			item->setIcon(0, item->isExpanded() ? folderOpenIcon : folderClosedIcon);
		else
			item->setIcon(0, sceneIcon);
	}

	void chooseIcon(QTreeWidgetItem *item)
	{
		QDialog dialog(this);
		dialog.setWindowTitle(T("SetIcon"));
		dialog.resize(380, 520);
		QVBoxLayout *root = new QVBoxLayout(&dialog);

		QLineEdit *filter = new QLineEdit;
		filter->setPlaceholderText(T("SetIcon.Filter"));
		filter->setClearButtonEnabled(true);
		root->addWidget(filter);

		QListWidget *grid = new QListWidget;
		grid->setIconSize(QSize(20, 20));
		grid->setUniformItemSizes(true);
		QString current = item->data(0, ROLE_ICON).toString();
		for (const QString &name : availableIcons()) {
			QString label = name;
			label.replace('-', ' ');
			label[0] = label[0].toUpper();
			QListWidgetItem *entry = new QListWidgetItem(customIcon(name), label);
			entry->setData(Qt::UserRole, name);
			grid->addItem(entry);
			if (name == current)
				grid->setCurrentItem(entry);
		}
		if (grid->currentItem())
			grid->scrollToItem(grid->currentItem(), QAbstractItemView::PositionAtCenter);
		root->addWidget(grid, 1);

		connect(filter, &QLineEdit::textChanged, grid, [grid](const QString &text) {
			QString needle = text.trimmed();
			for (int i = 0; i < grid->count(); i++) {
				QListWidgetItem *entry = grid->item(i);
				entry->setHidden(!needle.isEmpty() &&
						 !entry->text().contains(needle, Qt::CaseInsensitive));
			}
		});

		QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		QPushButton *clear = buttons->addButton(T("SetIcon.Default"), QDialogButtonBox::ResetRole);
		bool cleared = false;
		connect(clear, &QPushButton::clicked, &dialog, [&dialog, &cleared]() {
			cleared = true;
			dialog.accept();
		});
		connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		connect(grid, &QListWidget::itemDoubleClicked, &dialog,
			[&dialog](QListWidgetItem *) { dialog.accept(); });
		root->addWidget(buttons);

		if (dialog.exec() != QDialog::Accepted)
			return;
		QString chosen;
		if (!cleared && grid->currentItem())
			chosen = grid->currentItem()->data(Qt::UserRole).toString();
		applying = true;
		for (QTreeWidgetItem *target : targetsFor(item)) {
			target->setData(0, ROLE_ICON, chosen);
			applyItemIcon(target);
		}
		applying = false;
		persist();
	}

	void applyIcons()
	{
		applying = true;
		QTreeWidgetItemIterator it(tree);
		while (*it) {
			applyItemIcon(*it);
			++it;
		}
		applying = false;
	}

	QTreeWidgetItem *makeFolder(const QString &name)
	{
		QTreeWidgetItem *item = new QTreeWidgetItem;
		item->setText(0, name);
		item->setData(0, ROLE_KIND, KIND_FOLDER);
		item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled |
			       Qt::ItemIsDropEnabled);
		applyItemIcon(item);
		return item;
	}

	QTreeWidgetItem *makeScene(const QString &uuid, const QString &name)
	{
		QTreeWidgetItem *item = new QTreeWidgetItem;
		item->setText(0, name);
		item->setData(0, ROLE_KIND, KIND_SCENE);
		item->setData(0, ROLE_UUID, uuid);
		item->setData(0, ROLE_STATE, STATE_NONE);
		item->setData(0, ROLE_HIDDEN, false);
		item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
		applyItemIcon(item);
		return item;
	}

	void collectScenes(QTreeWidgetItem *parent, QHash<QString, QTreeWidgetItem *> &out)
	{
		for (int i = 0; i < parent->childCount(); i++) {
			QTreeWidgetItem *child = parent->child(i);
			if (isScene(child))
				out.insert(child->data(0, ROLE_UUID).toString(), child);
			else
				collectScenes(child, out);
		}
	}

	void collectItems(QTreeWidgetItem *parent, obs_data_array_t *out)
	{
		for (int i = 0; i < parent->childCount(); i++) {
			QTreeWidgetItem *child = parent->child(i);
			obs_data_t *entry = obs_data_create();
			QString color = child->data(0, ROLE_COLOR).toString();
			if (!color.isEmpty())
				obs_data_set_string(entry, "color", color.toUtf8().constData());
			QString icon = child->data(0, ROLE_ICON).toString();
			if (!icon.isEmpty())
				obs_data_set_string(entry, "icon", icon.toUtf8().constData());
			if (isFolder(child)) {
				obs_data_set_string(entry, "type", KIND_FOLDER);
				obs_data_set_string(entry, "name", child->text(0).toUtf8().constData());
				obs_data_set_bool(entry, "expanded", child->isExpanded());
				obs_data_array_t *children = obs_data_array_create();
				collectItems(child, children);
				obs_data_set_array(entry, "items", children);
				obs_data_array_release(children);
			} else {
				obs_data_set_string(entry, "type", KIND_SCENE);
				obs_data_set_string(entry, "uuid",
						    child->data(0, ROLE_UUID).toString().toUtf8().constData());
				obs_data_set_string(entry, "name", child->text(0).toUtf8().constData());
				if (child->data(0, ROLE_HIDDEN).toBool())
					obs_data_set_bool(entry, "hidden", true);
			}
			obs_data_array_push_back(out, entry);
			obs_data_release(entry);
		}
	}

	void buildItems(QTreeWidgetItem *parent, obs_data_array_t *items)
	{
		if (!items)
			return;
		size_t count = obs_data_array_count(items);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *entry = obs_data_array_item(items, i);
			const char *type = obs_data_get_string(entry, "type");
			QString color = QString::fromUtf8(obs_data_get_string(entry, "color"));
			QString iconName = QString::fromUtf8(obs_data_get_string(entry, "icon"));
			if (strcmp(type, KIND_FOLDER) == 0) {
				QTreeWidgetItem *folder = makeFolder(obs_data_get_string(entry, "name"));
				folder->setData(0, ROLE_COLOR, color);
				folder->setData(0, ROLE_ICON, iconName);
				parent->addChild(folder);
				obs_data_array_t *children = obs_data_get_array(entry, "items");
				buildItems(folder, children);
				obs_data_array_release(children);
				folder->setExpanded(obs_data_get_bool(entry, "expanded"));
				applyItemIcon(folder);
			} else {
				const char *uuid = obs_data_get_string(entry, "uuid");
				const char *name = obs_data_get_string(entry, "name");
				obs_source_t *scene = uuid && *uuid ? obs_get_source_by_uuid(uuid) : nullptr;
				if (!scene && name && *name)
					scene = obs_get_source_by_name(name);
				if (scene && obs_source_get_type(scene) == OBS_SOURCE_TYPE_SCENE) {
					QTreeWidgetItem *item =
						makeScene(obs_source_get_uuid(scene), obs_source_get_name(scene));
					item->setData(0, ROLE_COLOR, color);
					item->setData(0, ROLE_ICON, iconName);
					item->setData(0, ROLE_HIDDEN, obs_data_get_bool(entry, "hidden"));
					parent->addChild(item);
					applyItemIcon(item);
				}
				obs_source_release(scene);
			}
			obs_data_release(entry);
		}
	}

	static QString streamUpTreeFile()
	{
		char *own = obs_module_config_path("");
		QString path = own ? QString::fromUtf8(own) : QString();
		bfree(own);
		if (path.isEmpty())
			return QString();
		QDir dir(path);
		dir.cdUp();
		return dir.filePath("streamup/scene_organiser_configs/scene_tree_normal.json");
	}

	static obs_data_array_t *streamUpTreeForCurrentCollection()
	{
		QString file = streamUpTreeFile();
		if (file.isEmpty() || !QFileInfo::exists(file))
			return nullptr;
		obs_data_t *root = obs_data_create_from_json_file(file.toUtf8().constData());
		if (!root)
			return nullptr;
		char *collection = obs_frontend_get_current_scene_collection();
		obs_data_array_t *items = collection ? obs_data_get_array(root, collection) : nullptr;
		bfree(collection);
		obs_data_release(root);
		if (items && obs_data_array_count(items) == 0) {
			obs_data_array_release(items);
			return nullptr;
		}
		return items;
	}

	void buildStreamUpItems(QTreeWidgetItem *parent, obs_data_array_t *items)
	{
		size_t count = obs_data_array_count(items);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *entry = obs_data_array_item(items, i);
			const char *type = obs_data_get_string(entry, "type");
			const char *name = obs_data_get_string(entry, "name");
			if (strcmp(type, "folder") == 0) {
				QTreeWidgetItem *folder = makeFolder(QString::fromUtf8(name));
				parent->addChild(folder);
				obs_data_array_t *children = obs_data_get_array(entry, "children");
				if (children) {
					buildStreamUpItems(folder, children);
					obs_data_array_release(children);
				}
				folder->setExpanded(obs_data_get_bool(entry, "expanded"));
				applyItemIcon(folder);
			} else {
				obs_source_t *scene = obs_get_source_by_name(name);
				if (scene) {
					parent->addChild(
						makeScene(obs_source_get_uuid(scene), obs_source_get_name(scene)));
					obs_source_release(scene);
				}
			}
			obs_data_release(entry);
		}
	}

	bool importFromStreamUp(bool save)
	{
		obs_data_array_t *items = streamUpTreeForCurrentCollection();
		if (!items)
			return false;
		applying = true;
		tree->clear();
		buildStreamUpItems(tree->invisibleRootItem(), items);
		obs_data_array_release(items);
		applying = false;
		syncScenes();
		blog(LOG_INFO, "[gd-scene-tree] imported folder layout from StreamUP");
		if (save)
			persist();
		return true;
	}

	void exportJson()
	{
		char *collection = obs_frontend_get_current_scene_collection();
		QString suggested =
			QString("%1-scene-tree.json").arg(collection ? QString::fromUtf8(collection) : "scenes");
		bfree(collection);
		QString file = QFileDialog::getSaveFileName(this, T("ExportJson"), suggested, "JSON (*.json)");
		if (file.isEmpty())
			return;
		obs_data_t *layout = saveLayout();
		bool ok = obs_data_save_json(layout, file.toUtf8().constData());
		obs_data_release(layout);
		if (!ok)
			QMessageBox::warning(this, T("DockTitle"), T("ExportJson.Failed"));
	}

	void importJson()
	{
		QString file = QFileDialog::getOpenFileName(this, T("ImportJson"), QString(), "JSON (*.json)");
		if (file.isEmpty())
			return;
		obs_data_t *layout = obs_data_create_from_json_file(file.toUtf8().constData());
		obs_data_array_t *items = layout ? obs_data_get_array(layout, "items") : nullptr;
		if (!items) {
			obs_data_release(layout);
			QMessageBox::warning(this, T("DockTitle"), T("ImportJson.Failed"));
			return;
		}
		obs_data_array_release(items);
		applyLayout(layout);
		obs_data_release(layout);
		persist();
	}

	void activate(QTreeWidgetItem *item, bool doubleClick)
	{
		if (!isScene(item) || detached)
			return;
		obs_source_t *scene = obs_get_source_by_uuid(item->data(0, ROLE_UUID).toString().toUtf8().constData());
		if (!scene)
			return;
		bool studio = obs_frontend_preview_program_mode_active();
		if (studio) {
			if (doubleClick) {
				if (!settings.noTransition)
					obs_frontend_set_current_scene(scene);
			} else if (!settings.noPreviewSwitch) {
				obs_frontend_set_current_preview_scene(scene);
			}
		} else if (doubleClick == settings.doubleClickSwitch) {
			obs_frontend_set_current_scene(scene);
		}
		obs_source_release(scene);
	}

	void renamed(QTreeWidgetItem *item)
	{
		if (applying || detached)
			return;
		QString name = item->text(0).trimmed();
		if (isFolder(item)) {
			if (name.isEmpty()) {
				applying = true;
				item->setText(0, T("NewFolderName"));
				applying = false;
			}
			persist();
			return;
		}

		obs_source_t *scene = obs_get_source_by_uuid(item->data(0, ROLE_UUID).toString().toUtf8().constData());
		if (!scene)
			return;
		QString current = obs_source_get_name(scene);
		if (name.isEmpty() || name == current) {
			applying = true;
			item->setText(0, current);
			applying = false;
		} else {
			obs_source_t *clash = obs_get_source_by_name(name.toUtf8().constData());
			if (clash) {
				obs_source_release(clash);
				applying = true;
				item->setText(0, current);
				applying = false;
			} else {
				obs_source_set_name(scene, name.toUtf8().constData());
				obs_data_t *undoData = obs_data_create();
				obs_data_set_string(undoData, "from", name.toUtf8().constData());
				obs_data_set_string(undoData, "to", current.toUtf8().constData());
				obs_data_t *redoData = obs_data_create();
				obs_data_set_string(redoData, "from", current.toUtf8().constData());
				obs_data_set_string(redoData, "to", name.toUtf8().constData());
				recordSceneAction(L("Undo.Rename").arg(name), scene_rename_undo, scene_rename_redo,
						  obs_data_get_json(undoData), obs_data_get_json(redoData));
				obs_data_release(undoData);
				obs_data_release(redoData);
			}
		}
		obs_source_release(scene);
	}

	void addFolderAction(QMenu &menu, QTreeWidgetItem *item)
	{
		QAction *action = menu.addAction(T("NewFolder"), this, [this, item]() {
			QTreeWidgetItem *parent = tree->invisibleRootItem();
			if (isFolder(item))
				parent = item;
			else if (item && item->parent())
				parent = item->parent();
			QTreeWidgetItem *folder = makeFolder(T("NewFolderName"));
			parent->addChild(folder);
			parent->setExpanded(true);
			applyItemIcon(parent);
			tree->setCurrentItem(folder);
			tree->editItem(folder, 0);
			persist();
		});
		action->setEnabled(!locked);
	}

	void contextMenu(const QPoint &pos)
	{
		QTreeWidgetItem *item = tree->itemAt(pos);
		if (item)
			tree->setCurrentItem(item);
		QMenu menu(this);

		menu.addAction(L("AddScene"), this, [this]() { addScene(); });
		addFolderAction(menu, item);

		if (isScene(item)) {
			obs_source_t *scene =
				obs_get_source_by_uuid(item->data(0, ROLE_UUID).toString().toUtf8().constData());
			if (!scene)
				return;
			QString uuid = obs_source_get_uuid(scene);
			bool hidden = item->data(0, ROLE_HIDDEN).toBool();

			menu.addSeparator();
			menu.addAction(L("Duplicate"), this, [this, item]() { duplicateScene(item); });
			menu.addAction(L("Copy.Filters"), this, [this, uuid]() { filterClipboard = uuid; });
			QAction *paste = menu.addAction(L("Paste.Filters"), this, [this, uuid]() {
				obs_source_t *from = obs_get_source_by_uuid(filterClipboard.toUtf8().constData());
				obs_source_t *to = obs_get_source_by_uuid(uuid.toUtf8().constData());
				if (from && to)
					obs_source_copy_filters(to, from);
				obs_source_release(from);
				obs_source_release(to);
			});
			bool canPaste = !filterClipboard.isEmpty() && filterClipboard != uuid;
			if (canPaste) {
				obs_source_t *from = obs_get_source_by_uuid(filterClipboard.toUtf8().constData());
				canPaste = from != nullptr;
				obs_source_release(from);
			}
			paste->setEnabled(canPaste);

			menu.addSeparator();
			QAction *rename =
				menu.addAction(L("Rename"), this, [this, item]() { tree->editItem(item, 0); });
			rename->setEnabled(!locked);
			QAction *remove = menu.addAction(L("Remove"), this, [this, item]() {
				QList<QTreeWidgetItem *> targets;
				for (QTreeWidgetItem *t : targetsFor(item)) {
					if (isScene(t))
						targets.append(t);
				}
				removeScenes(targets);
			});
			remove->setEnabled(!locked);
			QAction *hide = menu.addAction(hidden ? T("UnhideScene") : T("HideScene"), this,
						       [this, item, hidden]() {
							       for (QTreeWidgetItem *t : targetsFor(item)) {
								       if (isScene(t))
									       setSceneHidden(t, !hidden);
							       }
						       });
			hide->setEnabled(!locked);

			menu.addSeparator();
			orderMenu(&menu, item);

			menu.addSeparator();
			projectorMenu(&menu, scene);

			menu.addSeparator();
			menu.addAction(L("Screenshot.Scene"), this, [uuid]() {
				obs_source_t *target = obs_get_source_by_uuid(uuid.toUtf8().constData());
				if (target)
					obs_frontend_take_source_screenshot(target);
				obs_source_release(target);
			});

			menu.addSeparator();
			menu.addAction(L("Filters"), this, [this]() { openSelectedFilters(); });

			menu.addSeparator();
			transitionOverrideMenu(&menu, scene);

			obs_data_t *priv = obs_source_get_private_settings(scene);
			obs_data_set_default_bool(priv, "show_in_multiview", true);
			bool inMultiview = obs_data_get_bool(priv, "show_in_multiview");
			obs_data_release(priv);
			QAction *multiview = menu.addAction(L("ShowInMultiview"), this, [uuid, inMultiview]() {
				obs_source_t *target = obs_get_source_by_uuid(uuid.toUtf8().constData());
				if (!target)
					return;
				obs_data_t *settings = obs_source_get_private_settings(target);
				obs_data_set_bool(settings, "show_in_multiview", !inMultiview);
				obs_data_release(settings);
				obs_source_release(target);
			});
			multiview->setCheckable(true);
			multiview->setChecked(inMultiview);

			menu.addSeparator();
			colorMenu(&menu, item);
			QAction *icon = menu.addAction(QString(T("SetIcon")) + "...", this,
						       [this, item]() { chooseIcon(item); });
			icon->setEnabled(!locked);
			sortMenu(&menu, item->parent() ? item->parent() : tree->invisibleRootItem());

			if (obs_frontend_preview_program_mode_active()) {
				menu.addSeparator();
				menu.addAction(T("TransitionToProgram"), this,
					       [this, item]() { activate(item, true); });
			}
			obs_source_release(scene);
		} else if (isFolder(item)) {
			menu.addSeparator();
			QAction *rename =
				menu.addAction(L("Rename"), this, [this, item]() { tree->editItem(item, 0); });
			rename->setEnabled(!locked);
			QAction *remove = menu.addAction(T("DeleteFolder"), this, [this, item]() {
				QTreeWidgetItem *parent = item->parent() ? item->parent() : tree->invisibleRootItem();
				int index = parent->indexOfChild(item);
				while (item->childCount() > 0) {
					QTreeWidgetItem *child = item->takeChild(0);
					parent->insertChild(index++, child);
					applyItemIcon(child);
				}
				delete item;
				updateToolbar();
				persist();
			});
			remove->setEnabled(!locked);
			menu.addSeparator();
			orderMenu(&menu, item);
			menu.addSeparator();
			colorMenu(&menu, item);
			QAction *icon = menu.addAction(QString(T("SetIcon")) + "...", this,
						       [this, item]() { chooseIcon(item); });
			icon->setEnabled(!locked);
			sortMenu(&menu, item);
		} else {
			menu.addSeparator();
			sortMenu(&menu, tree->invisibleRootItem());
		}

		menu.addSeparator();
		QAction *lock = menu.addAction(T("LockTree"), this, [this]() { setLocked(!locked); });
		lock->setCheckable(true);
		lock->setChecked(locked);
		menu.addSeparator();
		menu.addAction(T("ExpandAll"), this, [this]() {
			tree->expandAll();
			updateToolbar();
		});
		menu.addAction(T("CollapseAll"), this, [this]() {
			tree->collapseAll();
			updateToolbar();
		});
		menu.exec(tree->viewport()->mapToGlobal(pos));
	}

	bool applyVisibilityTo(QTreeWidgetItem *item, const QString &needle)
	{
		bool own = true;
		if (isScene(item) && item->data(0, ROLE_HIDDEN).toBool() && !settings.showHidden)
			own = false;
		if (own && !needle.isEmpty() && !item->text(0).contains(needle, Qt::CaseInsensitive))
			own = false;
		bool childVisible = false;
		for (int i = 0; i < item->childCount(); i++)
			childVisible = applyVisibilityTo(item->child(i), needle) || childVisible;
		bool show = own || childVisible;
		if (isFolder(item) && needle.isEmpty())
			show = true;
		item->setHidden(!show);
		if (!needle.isEmpty() && childVisible)
			item->setExpanded(true);
		return show;
	}

	void applyVisibility()
	{
		applying = true;
		QString needle = search->text().trimmed();
		for (int i = 0; i < tree->topLevelItemCount(); i++)
			applyVisibilityTo(tree->topLevelItem(i), needle);
		applying = false;
		tree->viewport()->update();
	}

	void persist(bool record = true)
	{
		if (applying || detached)
			return;
		QString json = layoutJson();
		if (record && !lastLayoutJson.isEmpty() && json != lastLayoutJson)
			recordSceneAction(T("Undo.Tree"), layout_undo_redo, layout_undo_redo, lastLayoutJson, json);
		lastLayoutJson = json;
		obs_frontend_save();
	}

	void persistLater()
	{
		if (applying || detached)
			return;
		QMetaObject::invokeMethod(this, [this]() { persist(false); }, Qt::QueuedConnection);
	}

	QLineEdit *search = nullptr;
	SceneTreeWidget *tree = nullptr;
	QToolButton *addButton = nullptr;
	QToolButton *removeButton = nullptr;
	QToolButton *filtersButton = nullptr;
	QToolButton *upButton = nullptr;
	QToolButton *downButton = nullptr;
	QToolButton *lockButton = nullptr;
	QToolButton *collapseButton = nullptr;
	QIcon folderClosedIcon;
	QIcon folderOpenIcon;
	QIcon sceneIcon;
	QIcon playIcon;
	QIcon lockIcon;
	QIcon unlockIcon;
	QIcon collapseIcon;
	QIcon expandIcon;
	QIcon gearIcon;
	QString filterClipboard;
	QString lastLayoutJson;
	QHash<QString, QIcon> iconCache;
	TreeSettings settings;
	RowDelegate *delegate = nullptr;
	bool locked = false;
	bool applying = false;
	bool detached = false;
};

static void layout_undo_redo(const char *data)
{
	if (dockWidget)
		dockWidget->applyLayoutJson(QString::fromUtf8(data));
}

static void scene_add_undo(const char *name)
{
	obs_source_t *source = obs_get_source_by_name(name);
	if (source)
		obs_source_remove(source);
	obs_source_release(source);
}

static void scene_add_redo(const char *name)
{
	obs_scene_t *scene = obs_scene_create(name);
	if (!scene)
		return;
	obs_frontend_set_current_scene(obs_scene_get_source(scene));
	obs_scene_release(scene);
}

static void scene_remove_undo(const char *data)
{
	if (dockWidget)
		dockWidget->restoreRemovedScene(data);
}

static void scene_remove_redo(const char *data)
{
	obs_data_t *redo = obs_data_create_from_json(data);
	if (!redo)
		return;
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(redo, "name"));
	if (source)
		remove_scene_and_prune(source);
	obs_source_release(source);
	if (dockWidget)
		dockWidget->applyLayoutJson(QString::fromUtf8(obs_data_get_string(redo, "layout")));
	obs_data_release(redo);
}

static void rename_from_json(const char *data)
{
	obs_data_t *names = obs_data_create_from_json(data);
	if (!names)
		return;
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(names, "from"));
	if (source)
		obs_source_set_name(source, obs_data_get_string(names, "to"));
	obs_source_release(source);
	obs_data_release(names);
}

static void scene_rename_undo(const char *data)
{
	rename_from_json(data);
}

static void scene_rename_redo(const char *data)
{
	rename_from_json(data);
}

static void scene_duplicate_undo(const char *data)
{
	obs_data_t *undo = obs_data_create_from_json(data);
	if (!undo)
		return;
	obs_source_t *copy = obs_get_source_by_name(obs_data_get_string(undo, "copy"));
	if (copy)
		remove_scene_and_prune(copy);
	obs_source_release(copy);
	if (dockWidget)
		dockWidget->applyLayoutJson(QString::fromUtf8(obs_data_get_string(undo, "layout")));
	obs_data_release(undo);
}

static void scene_duplicate_redo(const char *data)
{
	obs_data_t *redo = obs_data_create_from_json(data);
	if (!redo)
		return;
	obs_source_t *original = obs_get_source_by_name(obs_data_get_string(redo, "original"));
	if (original) {
		obs_scene_t *copy = obs_scene_duplicate(obs_scene_from_source(original),
							obs_data_get_string(redo, "copy"), OBS_SCENE_DUP_REFS);
		if (copy) {
			obs_frontend_set_current_scene(obs_scene_get_source(copy));
			obs_scene_release(copy);
		}
	}
	obs_source_release(original);
	if (dockWidget)
		dockWidget->applyLayoutJson(QString::fromUtf8(obs_data_get_string(redo, "layout")));
	obs_data_release(redo);
}

void scene_tree_dock_register()
{
	dockWidget = new SceneTreeDock;
	obs_frontend_add_dock_by_id(DOCK_ID, T("DockTitle"), dockWidget);
	dockWidget->hideCloseButton();
	QTimer::singleShot(0, dockWidget, [w = dockWidget]() {
		if (w)
			w->hideCloseButton();
	});
}

void scene_tree_dock_unregister()
{
	if (dockWidget)
		dockWidget->detach();
	obs_frontend_remove_dock(DOCK_ID);
}
