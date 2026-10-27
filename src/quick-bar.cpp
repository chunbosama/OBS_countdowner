/*
 * 快速控制栏：把场景里所有倒计时实例收成一行一行的按钮，点一下就触发动作。
 *
 * 几个刻意的选择：
 *  1. 不声明 Q_OBJECT。这里不用自定义信号槽，全部用 Qt 的函数式 connect，
 *     所以不需要 moc，编译链里少一个步骤。
 *  2. 界面不直接持有 countdown 指针，只拿 cdreg 的编号，避免源被删除后访问野指针。
 *  3. 定时轮询登记表（200ms），而不是让计时器反过来推 UI——视频线程和 UI 线程
 *     之间只通过一张加锁的表交换数据，链路最短。
 */

#include "quick-bar.h"

#ifndef CD_NO_QUICKBAR

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QDockWidget>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <vector>

#include "countdown-core.h"
#include "countdown-registry.h"

namespace {

const char *const kDockId = "countdown_quick_bar";
const int kRefreshMs = 200;

QString tr_utf8(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* 把文字调淡一档：不写死颜色，而是拿当前主题的文字色和背景色混合，
   这样明暗两种 OBS 主题都合适 */
void set_dim_text(QLabel *label)
{
	QPalette pal = label->palette();
	const QColor fg = pal.color(QPalette::WindowText);
	const QColor bg = pal.color(QPalette::Window);
	QColor dim((fg.red() * 62 + bg.red() * 38) / 100, (fg.green() * 62 + bg.green() * 38) / 100,
		   (fg.blue() * 62 + bg.blue() * 38) / 100);
	pal.setColor(QPalette::WindowText, dim);
	label->setPalette(pal);
}

class QuickBar : public QWidget {
public:
	explicit QuickBar(QWidget *parent = nullptr);
	~QuickBar() override;

protected:
	/* 主题切换时重新算一次淡色，避免颜色被固定在旧主题上 */
	void changeEvent(QEvent *event) override;

private:
	struct Row {
		uint64_t id = 0;
		QFrame *frame = nullptr;
		QLabel *title = nullptr;
		QLabel *state = nullptr;
		QLabel *time = nullptr;
		QPushButton *start = nullptr;
		QPushButton *pause = nullptr;
		QPushButton *stop = nullptr;
		QPushButton *reset = nullptr;
	};

	void refresh();
	void rebuild(const std::vector<cdreg::Entry> &entries);
	void applyRow(Row &row, const cdreg::Entry &entry);
	QPushButton *addButton(QHBoxLayout *layout, uint64_t id, int action, const char *label_key);

	std::vector<Row> rows_;
	std::vector<uint64_t> order_;
	QVBoxLayout *stack_ = nullptr;
	QLabel *empty_ = nullptr;
	QTimer *timer_ = nullptr;
	bool alive_ = true;
};

/* -------------------------------------------------------------------------- */

QuickBar::QuickBar(QWidget *parent) : QWidget(parent)
{
	setObjectName("countdownQuickBar");
	setMinimumWidth(230);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(6, 6, 6, 6);
	outer->setSpacing(6);

	stack_ = new QVBoxLayout();
	stack_->setContentsMargins(0, 0, 0, 0);
	stack_->setSpacing(6);
	outer->addLayout(stack_);

	empty_ = new QLabel(tr_utf8("QuickBar.Empty"), this);
	empty_->setWordWrap(true);
	empty_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
	set_dim_text(empty_);
	stack_->addWidget(empty_);

	outer->addStretch(1);

	timer_ = new QTimer(this);
	timer_->setInterval(kRefreshMs);
	connect(timer_, &QTimer::timeout, this, [this] {
		if (alive_)
			refresh();
	});
	timer_->start();

	refresh();
}

QuickBar::~QuickBar()
{
	alive_ = false;
	if (timer_)
		timer_->stop();
}

void QuickBar::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
		set_dim_text(empty_);
		for (Row &row : rows_) {
			if (row.state)
				set_dim_text(row.state);
		}
	}
}

QPushButton *QuickBar::addButton(QHBoxLayout *layout, uint64_t id, int action, const char *label_key)
{
	auto *button = new QPushButton(tr_utf8(label_key));
	button->setFocusPolicy(Qt::NoFocus);
	button->setMinimumWidth(48);
	button->setToolTip(tr_utf8(label_key));

	/* 点击后不在这里刷新界面：真正的状态变化发生在视频线程，
	   200ms 内的下一次轮询会把新状态带过来，避免在信号里删掉自己 */
	connect(button, &QPushButton::clicked, this, [id, action] { cdreg::perform(id, action); });

	layout->addWidget(button);
	return button;
}

void QuickBar::rebuild(const std::vector<cdreg::Entry> &entries)
{
	for (Row &row : rows_) {
		if (!row.frame)
			continue;
		stack_->removeWidget(row.frame);
		row.frame->setParent(nullptr);
		row.frame->deleteLater();
	}
	rows_.clear();
	order_.clear();

	for (const cdreg::Entry &entry : entries) {
		Row row;
		row.id = entry.id;

		row.frame = new QFrame(this);
		row.frame->setFrameShape(QFrame::StyledPanel);

		auto *box = new QVBoxLayout(row.frame);
		box->setContentsMargins(6, 4, 6, 4);
		box->setSpacing(3);

		auto *top = new QHBoxLayout();
		top->setContentsMargins(0, 0, 0, 0);
		top->setSpacing(6);

		row.title = new QLabel(row.frame);
		QFont title_font = row.title->font();
		title_font.setBold(true);
		row.title->setFont(title_font);
		row.title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		row.title->setToolTip(row.title->text());

		row.state = new QLabel(row.frame);
		QFont state_font = row.state->font();
		if (state_font.pointSizeF() > 0)
			state_font.setPointSizeF(state_font.pointSizeF() * 0.9);
		row.state->setFont(state_font);
		set_dim_text(row.state);

		row.time = new QLabel(row.frame);
		QFont time_font = row.time->font();
		time_font.setBold(true);
		time_font.setFamily(QStringLiteral("Consolas"));
		row.time->setFont(time_font);
		row.time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

		top->addWidget(row.title, 1);
		top->addWidget(row.state, 0);
		top->addWidget(row.time, 0);
		box->addLayout(top);

		auto *bottom = new QHBoxLayout();
		bottom->setContentsMargins(0, 0, 0, 0);
		bottom->setSpacing(4);
		row.start = addButton(bottom, row.id, cdreg::kStart, "Action.Start");
		row.pause = addButton(bottom, row.id, cdreg::kToggle, "Action.Pause");
		row.stop = addButton(bottom, row.id, cdreg::kStop, "Action.Stop");
		row.reset = addButton(bottom, row.id, cdreg::kReset, "Action.Reset");
		bottom->addStretch(1);
		box->addLayout(bottom);

		stack_->addWidget(row.frame);
		rows_.push_back(row);
		order_.push_back(entry.id);
	}

	empty_->setVisible(entries.empty());

	for (size_t i = 0; i < rows_.size(); ++i)
		applyRow(rows_[i], entries[i]);
}

void QuickBar::applyRow(Row &row, const cdreg::Entry &entry)
{
	char buf[64];
	cd::format_display(entry.status.remaining_ms, (cd::Format)entry.status.format, buf,
			   sizeof(buf));

	const QString name = QString::fromUtf8(entry.name.c_str());
	row.title->setText(name);
	row.title->setToolTip(name);

	QString state_text;
	switch (entry.status.state) {
	case cd::kRunning:
		state_text = tr_utf8("State.Running");
		if (entry.status.suspended)
			state_text += QStringLiteral(" · ") + tr_utf8("QuickBar.Frozen");
		break;
	case cd::kPaused:
		state_text = tr_utf8("State.Paused");
		break;
	case cd::kFinished:
		state_text = tr_utf8("State.Finished");
		break;
	default:
		state_text = tr_utf8("State.Idle");
		break;
	}
	row.state->setText(state_text);
	row.time->setText(QString::fromLatin1(buf));

	const bool running = entry.status.state == cd::kRunning;
	const bool paused = entry.status.state == cd::kPaused;
	const bool rest = !running && !paused; /* 空闲或已结束 */

	row.start->setEnabled(rest || paused);
	row.start->setText(paused ? tr_utf8("Action.Resume") : tr_utf8("Action.Start"));
	row.pause->setEnabled(running);
	row.stop->setEnabled(running || paused);
	row.reset->setEnabled(true);
}

void QuickBar::refresh()
{
	const std::vector<cdreg::Entry> entries = cdreg::snapshot();

	bool same = entries.size() == order_.size();
	if (same) {
		for (size_t i = 0; i < entries.size(); ++i) {
			if (entries[i].id != order_[i]) {
				same = false;
				break;
			}
		}
	}

	if (!same) {
		rebuild(entries);
		return;
	}

	for (size_t i = 0; i < rows_.size(); ++i)
		applyRow(rows_[i], entries[i]);
}

/* -------------------------------------------------------------------------- */

QPointer<QuickBar> g_bar;

QDockWidget *find_dock()
{
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window)
		return nullptr;
	return main_window->findChild<QDockWidget *>(QString::fromLatin1(kDockId));
}

/* --- 位置记忆 -------------------------------------------------------------
 * OBS 是在恢复主窗口布局之后才发出 FINISHED_LOADING 的，而我们的停靠窗口要到那
 * 时候才建得出来，所以 user.ini 里那份 DockState 永远落不到它头上——不管用户把
 * 它拖到哪儿，下次启动都会回到默认的浮动位置。
 * 于是自己记一份：位置、是否浮动、停靠区域、是否可见。
 */
config_t *open_dock_config()
{
	char *path = obs_module_config_path("dock.ini");
	if (!path)
		return nullptr;

	/* OBS 30.2 起 config_open 改成「出参 + 打开方式」的形式了 */
	config_t *cfg = nullptr;
	if (config_open(&cfg, path, CONFIG_OPEN_ALWAYS) != CONFIG_SUCCESS)
		cfg = nullptr;

	bfree(path);
	return cfg;
}

void save_dock_state()
{
	if (!g_bar)
		return;

	QDockWidget *dock = find_dock();
	if (!dock)
		return;

	config_t *cfg = open_dock_config();
	if (!cfg)
		return;

	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	const bool floating = dock->isFloating();
	const int area = (floating || !main_window) ? 0 : (int)main_window->dockWidgetArea(dock);

	const QByteArray geometry = dock->saveGeometry().toBase64();
	config_set_string(cfg, "Dock", "geometry", geometry.constData());
	config_set_bool(cfg, "Dock", "floating", floating);
	config_set_int(cfg, "Dock", "area", area);
	config_set_bool(cfg, "Dock", "visible", dock->isVisible());
	config_save(cfg);
	config_close(cfg);
}

/* 返回 true 表示恢复过上一次的位置 */
bool restore_dock_state(QDockWidget *dock)
{
	config_t *cfg = open_dock_config();
	if (!cfg)
		return false;

	const char *geo = config_get_string(cfg, "Dock", "geometry");
	const bool has_state = geo && *geo;
	if (has_state) {
		const bool floating = config_get_bool(cfg, "Dock", "floating");
		const int area = (int)config_get_int(cfg, "Dock", "area");
		auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

		if (floating) {
			dock->setFloating(true);
			dock->restoreGeometry(QByteArray::fromBase64(QByteArray(geo)));
		} else if (main_window && area > 0) {
			/* 浮动中的停靠窗口必须先取消浮动，addDockWidget 才会真的把它收进去 */
			dock->setFloating(false);
			main_window->addDockWidget((Qt::DockWidgetArea)area, dock);
			dock->restoreGeometry(QByteArray::fromBase64(QByteArray(geo)));
		}

		dock->setVisible(config_get_bool(cfg, "Dock", "visible"));
	}

	config_close(cfg);
	return has_state;
}

/* 工具菜单里的开关：关掉停靠窗口之后还能从这里找回来 */
void on_tools_menu(void *data)
{
	UNUSED_PARAMETER(data);
	if (QDockWidget *dock = find_dock()) {
		dock->setVisible(!dock->isVisible());
		save_dock_state();
	}
}

void create_dock()
{
	if (g_bar)
		return;

	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	auto *bar = new QuickBar(main_window);
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("QuickBar.Title"), bar)) {
		blog(LOG_WARNING, "[obs-countdown] 快速控制栏添加失败（停靠 id 冲突？）");
		delete bar;
		return;
	}

	g_bar = bar;

	QDockWidget *dock = find_dock();
	if (dock) {
		if (!restore_dock_state(dock)) {
			/* 第一次加载：收进右侧停靠区，别浮在预览画面上挡画面。
			   用户之后拖到哪儿，下次启动就还在哪儿。 */
			dock->setFloating(false);
			if (main_window)
				main_window->addDockWidget(Qt::RightDockWidgetArea, dock);
			dock->setVisible(true);
		}
	}

	obs_frontend_add_tools_menu_item(obs_module_text("QuickBar.Title"), on_tools_menu, nullptr);

	blog(LOG_INFO, "[obs-countdown] 快速控制栏已创建");
}

void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		create_dock();
	else if (event == OBS_FRONTEND_EVENT_EXIT)
		save_dock_state(); /* 关窗口之前记下位置，此时主窗口还在 */
}

} /* namespace */

namespace quickbar {

void init()
{
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

void shutdown()
{
	/* 这里不存位置：模块卸载时主窗口可能已经析构，再去摸它会是野指针。
	   位置在 OBS_FRONTEND_EVENT_EXIT 里存过了。 */
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	obs_frontend_remove_dock(kDockId);
	g_bar = nullptr;
}

} /* namespace quickbar */

#else /* CD_NO_QUICKBAR：没有 Qt 头文件时的降级版本，插件其余功能照常 */

namespace quickbar {

void init() {}
void shutdown() {}

} /* namespace quickbar */

#endif
