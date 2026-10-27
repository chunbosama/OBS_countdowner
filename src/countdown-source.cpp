/*
 * obs-countdown —— 在 OBS 画面中显示倒计时
 *
 * 设计要点：
 *  1. 计时基于单调时钟记录"目标结束时刻"，不用每帧累减，掉帧/源隐藏都不会漂移。
 *  2. 文字渲染复用 OBS 内置文本源（text_gdiplus / text_ft2_source）作为私有子源，
 *     字体、描边、对齐全部交给官方实现，插件只负责格式化字符串。
 *  3. 状态只有四个：IDLE / RUNNING / PAUSED / FINISHED，动作全部幂等。
 */

#include "countdown-source.h"
#include "countdown-core.h"
#include "countdown-draw.h"
#include "countdown-registry.h"

#include <obs-module.h>
#include <util/platform.h>
#include <graphics/graphics.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#define T_(key) obs_module_text(key)

namespace {

/* -------------------------------------------------------------------------- */
/* 常量                                                                        */
/* -------------------------------------------------------------------------- */

enum cd_action {
	ACT_START = 0,
	ACT_TOGGLE,
	ACT_STOP,
	ACT_RESET,
	ACT_COUNT
};

/* 内置文本源候选 id，按优先级排列 */
const char *const kTextSourceIds[] = {
	"text_gdiplus_v3",
	"text_gdiplus_v2",
	"text_gdiplus",
	"text_ft2_source_v2",
	"text_ft2_source",
};

const int64_t kBlinkWindowNs = 3000000000LL; /* 结束闪烁持续 3 秒 */
const int64_t kBlinkStepNs = 500000000LL;    /* 每 500ms 切换一次 */

/* -------------------------------------------------------------------------- */
/* 数据结构                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * 从互斥锁内拷出的一帧快照。
 *
 * 视频线程拿到它就可以放下锁去做 obs_source_update / 渲染这类慢操作；
 * 而 UI 线程（属性面板）随时可能在锁内改写配置。
 * 所以凡是「出了锁还要用」的字段都必须在这里有副本——包括 std::string：
 * 一边赋新值一边读旧值是真实的数据竞争，不是理论问题。
 */
struct cd_snapshot {
	int64_t remaining_ms = 0;
	int64_t duration_ms = 0;
	int state = cd::kIdle;
	bool blink_on = true;
	bool blink = false;
	int format = cd::kFmtAuto;

	int64_t color = 0;
	int64_t end_color = 0;
	int64_t outline_color = 0;
	int64_t outline_size = 0;
	bool outline = true;

	std::string align;
	std::string valign;
	std::string font_face;
	int64_t font_size = 0;
	int64_t font_flags = 0;

	int style = cd::kStyleDigits;
	int64_t padding = 0;
	int64_t card_radius = 0;
	int64_t card_color = 0;
	int64_t card_opacity = 0;
	int64_t thickness = 0;
	int64_t progress_color = 0;
	int64_t track_color = 0;
	int64_t track_opacity = 0;
	bool show_ticks = false;

	float progress = 1.0f; /* 剩余占比，环/条按它画长度 */
	bool finished = false;

	uint32_t width = 0;
	uint32_t height = 0;
};

struct countdown {
	obs_source_t *source = nullptr;
	obs_source_t *text = nullptr;        /* 私有子源，负责真正绘制文字 */
	obs_data_t *text_settings = nullptr; /* 子源的完整配置，只在 tick 里改 */

	std::mutex lock;

	/* --- 用户配置 ---
	   颜色都是 OBS 的 0xBBGGRR 格式（低字节是 R），和属性面板取色器保持一致 */
	int64_t color = 0xFFFFFF;
	int64_t end_color = 0x303BFF; /* #FF3B30 红 */
	int64_t outline_color = 0x000000;
	int64_t outline_size = 6;
	bool outline = true;
	bool blink = false;
	int format = cd::kFmtAuto;
	std::string align = "center";
	std::string valign = "center";
	std::string font_face = "Arial";
	int64_t font_size = 96;
	int64_t font_flags = 0;
	std::string label; /* 实例标签，留空则用源名；只影响控制栏与提示 */

	/* --- 外观样式（见 countdown-core.h 的 compute_layout） --- */
	int style = cd::kStyleDigits;
	int64_t padding = 24;
	int64_t card_radius = 24;
	int64_t card_color = 0x2A170F;   /* #0F172A */
	int64_t card_opacity = 60;
	int64_t thickness = 12;
	int64_t progress_color = 0xF8BD38; /* #38BDF8 */
	int64_t track_color = 0xFFFFFF;
	int64_t track_opacity = 22;
	bool show_ticks = false;

	/* --- 运行时状态（时间与状态机都在 cd::Timer 里，单位换算不外泄） --- */
	cd::Timer timer;
	int64_t finished_ns = 0; /* 进入 FINISHED 的时刻，用于闪烁节奏 */
	bool blink_on = true;
	bool dirty = true;

	/* 在全局登记表里的编号，0 表示没登记上 */
	uint64_t reg_id = 0;

	/* 可见性：只有 source 处于「激活且在显示」时才推进计时 */
	bool active = false;
	bool shown = true;

	/* --- 渲染缓存 --- */
	uint32_t width = 320;
	uint32_t height = 120;
	cd::Layout layout; /* 版面：文字位置与装饰图元的位置 */

	/* video_tick 写、video_render 读（两者都在图形线程），
	   这样渲染侧不必再去读那些会被 UI 线程改写的配置字段 */
	cd_snapshot view;

	obs_hotkey_id hotkeys[ACT_COUNT] = {(obs_hotkey_id)OBS_INVALID_HOTKEY_ID,
					    (obs_hotkey_id)OBS_INVALID_HOTKEY_ID,
					    (obs_hotkey_id)OBS_INVALID_HOTKEY_ID,
					    (obs_hotkey_id)OBS_INVALID_HOTKEY_ID};
};

/* -------------------------------------------------------------------------- */
/* 工具函数                                                                    */
/* -------------------------------------------------------------------------- */

static void cd_update(void *data, obs_data_t *settings);
static void report_status(countdown *cd);
static void do_action(countdown *cd, int action);

static const char *find_text_source_id()
{
	for (const char *id : kTextSourceIds) {
		if (obs_source_get_display_name(id) != nullptr)
			return id;
	}

	/* 兜底：遍历已注册类型，找任意 text_ 开头的源 */
	const char *id = nullptr;
	size_t idx = 0;
	while (obs_enum_source_types(idx++, &id)) {
		if (id && strncmp(id, "text_", 5) == 0)
			return id;
	}
	return nullptr;
}

static const char *state_text(cd::State state)
{
	switch (state) {
	case cd::kRunning:
		return T_("State.Running");
	case cd::kPaused:
		return T_("State.Paused");
	case cd::kFinished:
		return T_("State.Finished");
	default:
		return T_("State.Idle");
	}
}

/* -------------------------------------------------------------------------- */
/* 应用到子源                                                                  */
/* -------------------------------------------------------------------------- */

static void take_snapshot(countdown *cd, cd_snapshot *out)
{
	out->remaining_ms = cd->timer.remaining_ms;
	out->duration_ms = cd->timer.duration_ms;
	out->state = cd->timer.state;
	out->blink_on = cd->blink_on;
	out->blink = cd->blink;
	out->format = cd->format;
	out->color = cd->color;
	out->end_color = cd->end_color;
	out->outline_color = cd->outline_color;
	out->outline_size = cd->outline_size;
	out->outline = cd->outline;
	out->align = cd->align;
	out->valign = cd->valign;
	out->font_face = cd->font_face;
	out->font_size = cd->font_size;
	out->font_flags = cd->font_flags;
	out->style = cd->style;
	out->padding = cd->padding;
	out->card_radius = cd->card_radius;
	out->card_color = cd->card_color;
	out->card_opacity = cd->card_opacity;
	out->thickness = cd->thickness;
	out->progress_color = cd->progress_color;
	out->track_color = cd->track_color;
	out->track_opacity = cd->track_opacity;
	out->show_ticks = cd->show_ticks;
	out->width = cd->width;
	out->height = cd->height;

	/* 环/条的长度每帧都要新，所以每帧都算一次 */
	out->progress = cd::remaining_fraction(cd->timer.remaining_ms, cd->timer.duration_ms);
	out->finished = (cd->timer.state == cd::kFinished);
}

static void apply_text(countdown *cd, const cd_snapshot &snap)
{
	char buf[64];
	cd::format_display(snap.remaining_ms, (cd::Format)snap.format, buf, sizeof(buf));

	bool use_end_color = (snap.state == cd::kFinished);
	if (use_end_color && snap.blink)
		use_end_color = snap.blink_on;

	obs_data_set_string(cd->text_settings, "text", buf);
	obs_data_set_int(cd->text_settings, "color", use_end_color ? snap.end_color : snap.color);
	obs_data_set_bool(cd->text_settings, "outline", snap.outline);
	obs_data_set_int(cd->text_settings, "outline_color", snap.outline_color);
	obs_data_set_int(cd->text_settings, "outline_size", snap.outline_size);
	obs_data_set_int(cd->text_settings, "outline_opacity", 100);
	obs_data_set_int(cd->text_settings, "opacity", 100);
	obs_data_set_string(cd->text_settings, "align", snap.align.c_str());
	obs_data_set_string(cd->text_settings, "valign", snap.valign.c_str());

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", snap.font_face.c_str());
	obs_data_set_int(font, "size", snap.font_size);
	obs_data_set_int(font, "flags", snap.font_flags);
	obs_data_set_string(font, "style", "Regular");
	obs_data_set_obj(cd->text_settings, "font", font);
	obs_data_release(font);

	obs_source_update(cd->text, cd->text_settings);

	/* 文本源在渲染前宽高可能还是 0，给个兜底值免得画面里选不中 */
	const uint32_t raw_w = obs_source_get_width(cd->text);
	const uint32_t raw_h = obs_source_get_height(cd->text);

	/* 装饰图元都摆在文字外面，所以整源尺寸要由版面算，而不是直接用文字尺寸 */
	cd::StyleParams params;
	params.style = snap.style;
	params.padding = (int)snap.padding;
	params.radius = (int)snap.card_radius;
	params.thickness = (int)snap.thickness;
	params.ticks = snap.show_ticks ? 12 : 0;

	const cd::Layout lay = cd::compute_layout(params, raw_w ? (int)raw_w : 320,
						  raw_h ? (int)raw_h : 120);
	cd->layout = lay;
	cd->width = (uint32_t)(lay.width > 0 ? lay.width : 320);
	cd->height = (uint32_t)(lay.height > 0 ? lay.height : 120);
}

/* -------------------------------------------------------------------------- */
/* 动作                                                                        */
/* -------------------------------------------------------------------------- */

/* 只改状态，不碰登记表。凡是从登记表回调进来的路径都必须走这个版本，
   否则会在 registry 锁上自我死锁。 */
static void do_action_internal(countdown *cd, int action)
{
	const int64_t now = (int64_t)os_gettime_ns();

	std::lock_guard<std::mutex> guard(cd->lock);
	const bool visible = cd->active && cd->shown;

	switch (action) {
	case ACT_START:
		if (cd->timer.state != cd::kRunning) {
			cd->timer.start(now);
			/* 源本来就不在画面上时，别让它在后台偷偷走 */
			if (!visible)
				cd->timer.suspend(now);
			cd->blink_on = true;
			cd->dirty = true;
		}
		break;

	case ACT_TOGGLE:
		if (cd->timer.state == cd::kRunning || cd->timer.state == cd::kPaused) {
			cd->timer.toggle(now);
			if (cd->timer.state == cd::kRunning && !visible)
				cd->timer.suspend(now);
			cd->dirty = true;
		}
		break;

	case ACT_STOP:
		cd->timer.stop(now);
		cd->blink_on = true;
		cd->dirty = true;
		break;

	case ACT_RESET:
		cd->timer.reset();
		cd->blink_on = true;
		cd->dirty = true;
		break;

	default:
		break;
	}
}

static void do_action(countdown *cd, int action)
{
	do_action_internal(cd, action);
	report_status(cd);
}

/* -------------------------------------------------------------------------- */
/* 状态上报给快速控制栏                                                        */
/* -------------------------------------------------------------------------- */

/* 调用前必须确保没有持有 cd->lock：登记表内部有自己的锁，
   锁顺序只能是 registry -> countdown，反过来会死锁。 */
static void report_status(countdown *cd)
{
	const char *source_name = obs_source_get_name(cd->source);

	cdreg::Status status;
	std::string label;
	{
		std::lock_guard<std::mutex> guard(cd->lock);
		status.state = cd->timer.state;
		status.suspended = cd->timer.suspended;
		status.format = cd->format;
		status.remaining_ms = cd->timer.remaining_ms;
		status.duration_ms = cd->timer.duration_ms;
		label = cd->label;
	}

	cdreg::refresh(cd, label.empty() ? source_name : label.c_str(), status);
}

/* 登记表回调：控制栏点按钮时走这里。
   此刻 registry 锁是持着的，所以只能调 internal 版本。 */
static void registry_action(void *handle, int action)
{
	do_action_internal(static_cast<countdown *>(handle), action);
}

/* -------------------------------------------------------------------------- */
/* 源回调                                                                      */
/* -------------------------------------------------------------------------- */

static const char *cd_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return T_("Source.Name");
}

static void *cd_create(obs_data_t *settings, obs_source_t *source)
{
	auto *cd = new countdown();
	cd->source = source;

	const char *text_id = find_text_source_id();
	if (!text_id) {
		blog(LOG_ERROR, "[obs-countdown] 找不到可用的内置文本源，插件无法工作");
		delete cd;
		return nullptr;
	}

	cd->text_settings = obs_data_create();
	/* 先铺一套完整默认值，之后每次整体覆盖，避免 update 时丢键 */
	obs_data_set_string(cd->text_settings, "text", "05:00");
	obs_data_set_int(cd->text_settings, "color", 0xFFFFFF);
	obs_data_set_int(cd->text_settings, "opacity", 100);
	obs_data_set_string(cd->text_settings, "align", "center");
	obs_data_set_string(cd->text_settings, "valign", "center");
	obs_data_set_bool(cd->text_settings, "outline", true);
	obs_data_set_int(cd->text_settings, "outline_color", 0x000000);
	obs_data_set_int(cd->text_settings, "outline_size", 6);
	obs_data_set_int(cd->text_settings, "outline_opacity", 100);
	obs_data_set_bool(cd->text_settings, "antialiasing", true);
	obs_data_set_bool(cd->text_settings, "gradient", false);
	obs_data_set_int(cd->text_settings, "gradient_color", 0xFFFFFF);
	obs_data_set_int(cd->text_settings, "gradient_opacity", 100);
	obs_data_set_double(cd->text_settings, "gradient_dir", 90.0);
	obs_data_set_bool(cd->text_settings, "extents", false);
	obs_data_set_bool(cd->text_settings, "extents_wrap", false);
	obs_data_set_int(cd->text_settings, "extents_cx", 0);
	obs_data_set_int(cd->text_settings, "extents_cy", 0);
	obs_data_set_bool(cd->text_settings, "read_from_file", false);
	obs_data_set_string(cd->text_settings, "file", "");
	obs_data_set_bool(cd->text_settings, "chatlog", false);
	obs_data_set_int(cd->text_settings, "chatlog_lines", 6);
	obs_data_set_int(cd->text_settings, "transform", 0);
	obs_data_set_bool(cd->text_settings, "vertical", false);
	obs_data_set_int(cd->text_settings, "bk_color", 0x000000);
	obs_data_set_int(cd->text_settings, "bk_opacity", 0);

	cd->text = obs_source_create_private(text_id, "countdown_text", cd->text_settings);
	if (!cd->text) {
		blog(LOG_ERROR, "[obs-countdown] 创建文本子源失败");
		obs_data_release(cd->text_settings);
		delete cd;
		return nullptr;
	}

	/* 应用用户配置（此时 text 子源已就绪） */
	cd_update(cd, settings);

	cd->hotkeys[ACT_START] =
		obs_hotkey_register_source(source, "Countdown.Start", T_("Hotkey.Start"),
					   [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
						   if (pressed)
							   do_action(static_cast<countdown *>(data), ACT_START);
					   },
					   cd);

	cd->hotkeys[ACT_TOGGLE] =
		obs_hotkey_register_source(source, "Countdown.Toggle", T_("Hotkey.Toggle"),
					   [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
						   if (pressed)
							   do_action(static_cast<countdown *>(data), ACT_TOGGLE);
					   },
					   cd);

	cd->hotkeys[ACT_STOP] =
		obs_hotkey_register_source(source, "Countdown.Stop", T_("Hotkey.Stop"),
					   [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
						   if (pressed)
							   do_action(static_cast<countdown *>(data), ACT_STOP);
					   },
					   cd);

	cd->hotkeys[ACT_RESET] =
		obs_hotkey_register_source(source, "Countdown.Reset", T_("Hotkey.Reset"),
					   [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
						   if (pressed)
							   do_action(static_cast<countdown *>(data), ACT_RESET);
					   },
					   cd);

	/* 挂到全局登记表，快速控制栏才看得见这个实例 */
	cd->reg_id = cdreg::add(cd, &registry_action);
	report_status(cd);

	return cd;
}

static void cd_destroy(void *data)
{
	auto *cd = static_cast<countdown *>(data);
	if (!cd)
		return;

	cdreg::remove(cd);

	for (obs_hotkey_id id : cd->hotkeys) {
		if (id != (obs_hotkey_id)OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(id);
	}

	if (cd->text)
		obs_source_release(cd->text);
	if (cd->text_settings)
		obs_data_release(cd->text_settings);

	delete cd;
}

static uint32_t cd_get_width(void *data)
{
	auto *cd = static_cast<countdown *>(data);
	return cd ? cd->width : 0;
}

static uint32_t cd_get_height(void *data)
{
	auto *cd = static_cast<countdown *>(data);
	return cd ? cd->height : 0;
}

static void cd_update(void *data, obs_data_t *settings)
{
	auto *cd = static_cast<countdown *>(data);
	if (!cd)
		return;

	const int64_t hours = obs_data_get_int(settings, "hours");
	const int64_t minutes = obs_data_get_int(settings, "minutes");
	const int64_t seconds = obs_data_get_int(settings, "seconds");
	int64_t duration = (hours * 3600 + minutes * 60 + seconds) * 1000;
	if (duration <= 0)
		duration = 1000; /* 拒绝 0，最小 1 秒 */

	std::string face = "Arial";
	int64_t font_size = 96;
	int64_t font_flags = 0;
	obs_data_t *font = obs_data_get_obj(settings, "font");
	if (font) {
		const char *f = obs_data_get_string(font, "face");
		if (f && *f)
			face = f;
		const int64_t size = obs_data_get_int(font, "size");
		if (size > 0)
			font_size = size;
		font_flags = obs_data_get_int(font, "flags");
		obs_data_release(font);
	}

	/* 显示格式：老场景集合里没有 format 键，按 0.2.x 的「始终显示小时」迁移一次。
	   写回 settings 是为了让下拉框显示的值和实际生效的一致。 */
	int format = (int)cd::kFmtAuto;
	if (obs_data_has_user_value(settings, "format")) {
		format = (int)cd::format_from_id(obs_data_get_string(settings, "format"));
	} else if (obs_data_get_bool(settings, "force_hours")) {
		format = (int)cd::kFmtHours;
		obs_data_set_string(settings, "format", cd::format_id(cd::kFmtHours));
		obs_data_set_bool(settings, "force_hours", false);
	}

	const int style = (int)cd::style_from_id(obs_data_get_string(settings, "style"));

	/* 0.2.x 的「结束红」写成了 0xFF3B30，但 OBS 颜色是 0xBBGGRR 字节序，
	   那个值实际渲染出来是蓝紫色。老场景集合里还是这个旧默认值的话顺手迁成真红。 */
	if (obs_data_get_int(settings, "end_color") == 0xFF3B30)
		obs_data_set_int(settings, "end_color", 0x303BFF);

	{
		std::lock_guard<std::mutex> guard(cd->lock);
		/* 只有「设定时长本身变了」才同步到剩余时间。
		   不能只看当前状态：改颜色、改字体都会走 update，
		   按状态判断会把「停止」保留下来的剩余时间冲成满值。 */
		cd->timer.apply_duration(duration);
		cd->color = obs_data_get_int(settings, "color");
		cd->end_color = obs_data_get_int(settings, "end_color");
		cd->outline = obs_data_get_bool(settings, "outline");
		cd->outline_color = obs_data_get_int(settings, "outline_color");
		cd->outline_size = obs_data_get_int(settings, "outline_size");
		cd->blink = obs_data_get_bool(settings, "blink");
		cd->format = format;
		cd->align = obs_data_get_string(settings, "align");
		cd->valign = obs_data_get_string(settings, "valign");
		cd->font_face = face;
		cd->font_size = font_size;
		cd->font_flags = font_flags;

		cd->style = style;
		cd->padding = obs_data_get_int(settings, "padding");
		cd->card_radius = obs_data_get_int(settings, "card_radius");
		cd->card_color = obs_data_get_int(settings, "card_color");
		cd->card_opacity = obs_data_get_int(settings, "card_opacity");
		cd->thickness = obs_data_get_int(settings, "thickness");
		cd->progress_color = obs_data_get_int(settings, "progress_color");
		cd->track_color = obs_data_get_int(settings, "track_color");
		cd->track_opacity = obs_data_get_int(settings, "track_opacity");
		cd->show_ticks = obs_data_get_bool(settings, "show_ticks");

		const char *label = obs_data_get_string(settings, "label");
		cd->label = label ? label : "";

		cd->dirty = true;
	}

	report_status(cd);
}

static void cd_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	auto *cd = static_cast<countdown *>(data);
	if (!cd)
		return;

	const int64_t now = (int64_t)os_gettime_ns();
	bool need_apply = false;

	std::unique_lock<std::mutex> guard(cd->lock);

	const bool was_running = cd->timer.state == cd::kRunning;
	if (cd->timer.advance(now)) {
		cd->dirty = true;
		if (was_running && cd->timer.state == cd::kFinished) {
			cd->finished_ns = now;
			cd->blink_on = true;
		}
	}

	if (cd->timer.state == cd::kFinished && cd->blink) {
		const int64_t elapsed = now - cd->finished_ns;
		if (elapsed < kBlinkWindowNs) {
			const bool on = ((elapsed / kBlinkStepNs) & 1) == 0;
			if (on != cd->blink_on) {
				cd->blink_on = on;
				cd->dirty = true;
			}
		} else if (!cd->blink_on) {
			cd->blink_on = true;
			cd->dirty = true;
		}
	}

	if (cd->dirty) {
		cd->dirty = false;
		need_apply = true;
	}

	cd_snapshot &snap = cd->view;
	take_snapshot(cd, &snap);
	guard.unlock();

	if (need_apply)
		apply_text(cd, snap);

	report_status(cd);
}

static void cd_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	auto *cd = static_cast<countdown *>(data);
	if (!cd || !cd->text)
		return;

	/* 只读 video_tick 留下的快照，不碰那些会被 UI 线程改写的配置字段 */
	const cd_snapshot &v = cd->view;
	const cd::Layout &lay = cd->layout;
	if (v.style == cd::kStyleDigits || lay.width <= 0) {
		obs_source_video_render(cd->text);
		return;
	}

	/* 结束时进度也跟着换成结束色，跟数字保持一致 */
	const int64_t fill_rgb = v.finished ? v.end_color : v.progress_color;
	const uint32_t fill = cddraw::pack(fill_rgb, 100);
	const uint32_t track = cddraw::pack(v.track_color, (int)v.track_opacity);

	cddraw::begin();
	switch (v.style) {
	case cd::kStyleCard:
		cddraw::card(0, 0, lay.width, lay.height, lay.card_radius,
			     cddraw::pack(v.card_color, (int)v.card_opacity));
		break;
	case cd::kStyleRing:
		cddraw::ring(lay.ring_cx, lay.ring_cy, lay.ring_radius, lay.ring_thickness,
			     v.progress, fill, track, v.show_ticks ? 12 : 0);
		break;
	case cd::kStyleBar:
		cddraw::bar(lay.bar_x, lay.bar_y, lay.bar_w, lay.bar_h, v.progress, fill, track);
		break;
	default:
		break;
	}
	cddraw::end();

	/* 装饰画完再把文字叠上去：文字由内置文本子源绘制，只挪一下位置 */
	gs_matrix_push();
	gs_matrix_translate3f((float)lay.text_x, (float)lay.text_y, 0.0f);
	obs_source_video_render(cd->text);
	gs_matrix_pop();
}

/* 可见性变化时冻结 / 恢复计时。调用方必须已持有互斥锁。 */
static void apply_visibility_locked(countdown *cd, int64_t now)
{
	const bool visible = cd->active && cd->shown;

	/* 离开画面就把剩余时间冻在当前值，回来再从冻结处接着走 */
	if (!visible) {
		if (!cd->timer.suspended) {
			cd->timer.suspend(now);
			cd->dirty = true;
		}
	} else if (cd->timer.suspended) {
		cd->timer.resume(now);
		cd->dirty = true;
	}
}

static void cd_activate(void *data)
{
	auto *cd = static_cast<countdown *>(data);
	if (!cd)
		return;
	{
		std::lock_guard<std::mutex> guard(cd->lock);
		cd->active = true;
		apply_visibility_locked(cd, (int64_t)os_gettime_ns());
	}
	report_status(cd);
}

static void cd_deactivate(void *data)
{
	auto *cd = static_cast<countdown *>(data);
	if (!cd)
		return;
	{
		std::lock_guard<std::mutex> guard(cd->lock);
		cd->active = false;
		apply_visibility_locked(cd, (int64_t)os_gettime_ns());
	}
	report_status(cd);
}

static void cd_show(void *data)
{
	auto *cd = static_cast<countdown *>(data);
	if (!cd)
		return;
	{
		std::lock_guard<std::mutex> guard(cd->lock);
		cd->shown = true;
		apply_visibility_locked(cd, (int64_t)os_gettime_ns());
	}
	report_status(cd);
}

static void cd_hide(void *data)
{
	auto *cd = static_cast<countdown *>(data);
	if (!cd)
		return;
	{
		std::lock_guard<std::mutex> guard(cd->lock);
		cd->shown = false;
		apply_visibility_locked(cd, (int64_t)os_gettime_ns());
	}
	report_status(cd);
}

/* -------------------------------------------------------------------------- */
/* 属性面板                                                                    */
/* -------------------------------------------------------------------------- */

static bool on_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	auto *cd = static_cast<countdown *>(data);
	if (cd) {
		const char *name = obs_property_name(property);
		if (name) {
			if (strcmp(name, "action_start") == 0)
				do_action(cd, ACT_START);
			else if (strcmp(name, "action_toggle") == 0)
				do_action(cd, ACT_TOGGLE);
			else if (strcmp(name, "action_stop") == 0)
				do_action(cd, ACT_STOP);
			else if (strcmp(name, "action_reset") == 0)
				do_action(cd, ACT_RESET);
		}
	}
	/* 返回 true 让属性面板刷新状态文字 */
	return true;
}

static void cd_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "label", "");
	obs_data_set_default_int(settings, "hours", 0);
	obs_data_set_default_int(settings, "minutes", 5);
	obs_data_set_default_int(settings, "seconds", 0);
	obs_data_set_default_int(settings, "color", 0xFFFFFF);
	obs_data_set_default_int(settings, "end_color", 0x303BFF); /* #FF3B30：OBS 颜色是 BGR 字节序 */
	obs_data_set_default_bool(settings, "outline", true);
	obs_data_set_default_int(settings, "outline_color", 0x000000);
	obs_data_set_default_int(settings, "outline_size", 6);
	obs_data_set_default_bool(settings, "blink", false);
	obs_data_set_default_string(settings, "format", "auto");
	obs_data_set_default_string(settings, "align", "center");
	obs_data_set_default_string(settings, "valign", "center");

	/* 外观样式：默认等于 0.2.x 的纯数字，老场景集合升上来不会变样 */
	obs_data_set_default_string(settings, "style", "digits");
	obs_data_set_default_int(settings, "padding", 24);
	obs_data_set_default_int(settings, "card_radius", 24);
	obs_data_set_default_int(settings, "card_color", 0x2A170F);         /* #0F172A */
	obs_data_set_default_int(settings, "card_opacity", 60);
	obs_data_set_default_int(settings, "thickness", 12);
	obs_data_set_default_int(settings, "progress_color", 0xF8BD38);     /* #38BDF8 */
	obs_data_set_default_int(settings, "track_color", 0xFFFFFF);
	obs_data_set_default_int(settings, "track_opacity", 22);
	obs_data_set_default_bool(settings, "show_ticks", false);

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Arial");
	obs_data_set_int(font, "size", 96);
	obs_data_set_int(font, "flags", 0);
	obs_data_set_string(font, "style", "Regular");
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);
}

static void add_align_items(obs_properties_t *props, const char *name, const char *desc, bool horizontal)
{
	obs_property_t *list = obs_properties_add_list(props, name, desc, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, T_("Align.Start"), horizontal ? "left" : "top");
	obs_property_list_add_string(list, T_("Align.Center"), "center");
	obs_property_list_add_string(list, T_("Align.End"), horizontal ? "right" : "bottom");
}

/* 只留下当前样式用得上的参数，免得属性面板里全是无效项 */
static void update_style_visibility(obs_properties_t *props, const char *style_id)
{
	const cd::Style style = cd::style_from_id(style_id);
	const bool card = style == cd::kStyleCard;
	const bool ring = style == cd::kStyleRing;
	const bool bar = style == cd::kStyleBar;
	const bool shape = ring || bar;

	auto show = [props](const char *name, bool visible) {
		obs_property_t *p = obs_properties_get(props, name);
		if (p)
			obs_property_set_visible(p, visible);
	};

	show("padding", style != cd::kStyleDigits);
	show("card_radius", card);
	show("card_color", card);
	show("card_opacity", card);
	show("thickness", shape);
	show("progress_color", shape);
	show("track_color", shape);
	show("track_opacity", shape);
	show("show_ticks", ring);
}

static bool on_style_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	update_style_visibility(props, obs_data_get_string(settings, "style"));
	return true;
}

static obs_properties_t *cd_get_properties(void *data)
{
	auto *cd = static_cast<countdown *>(data);
	obs_properties_t *props = obs_properties_create();

	/* --- 状态（只读）+ 控制按钮 --- */
	char status[256];
	if (cd) {
		char time_buf[64];
		cd::State state;
		bool suspended;
		int64_t duration_ms;
		int format;
		{
			std::lock_guard<std::mutex> guard(cd->lock);
			format = cd->format;
			cd::format_display(cd->timer.remaining_ms, (cd::Format)format, time_buf,
					   sizeof(time_buf));
			state = cd->timer.state;
			suspended = cd->timer.suspended;
			duration_ms = cd->timer.duration_ms;
		}

		const char *state_name = (state == cd::kRunning && suspended) ? T_("State.Suspended")
									     : state_text(state);
		char dur_buf[64];
		cd::format_time(duration_ms, true, dur_buf, sizeof(dur_buf));

		/* 不用 "…%s…%" 这种格式串：语言文件里的 % 会被 ini 解析吞掉，
		   所以分隔符和单位都写在代码里，ini 只提供单词。 */
		std::string line = state_name;
		line += " · ";
		line += T_("Info.Remaining");
		line += " ";
		line += time_buf;
		line += " · ";
		line += T_("Info.Set");
		line += " ";
		line += dur_buf;
		snprintf(status, sizeof(status), "%s", line.c_str());
	} else {
		snprintf(status, sizeof(status), "%s", T_("State.Idle"));
	}
	obs_properties_add_text(props, "status", status, OBS_TEXT_INFO);
	obs_properties_add_button(props, "action_start", T_("Action.Start"), on_button);
	obs_properties_add_button(props, "action_toggle", T_("Action.Toggle"), on_button);
	obs_properties_add_button(props, "action_stop", T_("Action.Stop"), on_button);
	obs_properties_add_button(props, "action_reset", T_("Action.Reset"), on_button);

	/* --- 时长与显示格式 --- */
	obs_properties_add_text(props, "label", T_("Prop.Label"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "hours", T_("Prop.Hours"), 0, 23, 1);
	obs_properties_add_int(props, "minutes", T_("Prop.Minutes"), 0, 59, 1);
	obs_properties_add_int(props, "seconds", T_("Prop.Seconds"), 0, 59, 1);

	obs_property_t *format = obs_properties_add_list(props, "format", T_("Prop.Format"),
							 OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(format, T_("Format.Auto"), "auto");
	obs_property_list_add_string(format, T_("Format.Hours"), "hms");
	obs_property_list_add_string(format, T_("Format.Chinese"), "chinese");
	obs_property_list_add_string(format, T_("Format.Seconds"), "seconds");

	/* --- 外观样式 --- */
	obs_property_t *style = obs_properties_add_list(props, "style", T_("Prop.Style"),
							OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(style, T_("Style.Digits"), "digits");
	obs_property_list_add_string(style, T_("Style.Card"), "card");
	obs_property_list_add_string(style, T_("Style.Ring"), "ring");
	obs_property_list_add_string(style, T_("Style.Bar"), "bar");
	obs_property_set_modified_callback(style, on_style_modified);

	obs_properties_add_int(props, "padding", T_("Prop.Padding"), 0, 200, 1);
	obs_properties_add_int(props, "card_radius", T_("Prop.Radius"), 0, 300, 1);
	obs_properties_add_color(props, "card_color", T_("Prop.CardColor"));
	obs_properties_add_int_slider(props, "card_opacity", T_("Prop.CardOpacity"), 0, 100, 1);
	obs_properties_add_int(props, "thickness", T_("Prop.Thickness"), 1, 80, 1);
	obs_properties_add_color(props, "progress_color", T_("Prop.ProgressColor"));
	obs_properties_add_color(props, "track_color", T_("Prop.TrackColor"));
	obs_properties_add_int_slider(props, "track_opacity", T_("Prop.TrackOpacity"), 0, 100, 1);
	obs_properties_add_bool(props, "show_ticks", T_("Prop.ShowTicks"));

	/* --- 文字 --- */
	obs_properties_add_font(props, "font", T_("Prop.Font"));
	obs_properties_add_color(props, "color", T_("Prop.Color"));
	obs_properties_add_color(props, "end_color", T_("Prop.EndColor"));
	obs_properties_add_bool(props, "outline", T_("Prop.Outline"));
	obs_properties_add_color(props, "outline_color", T_("Prop.OutlineColor"));
	obs_properties_add_int(props, "outline_size", T_("Prop.OutlineSize"), 0, 32, 1);
	obs_properties_add_bool(props, "blink", T_("Prop.Blink"));

	add_align_items(props, "align", T_("Prop.Align"), true);
	add_align_items(props, "valign", T_("Prop.Valign"), false);

	/* 按当前样式隐藏用不上的项，面板别一上来就一大坨 */
	update_style_visibility(props, cd ? cd::style_id((cd::Style)cd->style) : "digits");

	return props;
}

} /* namespace */

/* -------------------------------------------------------------------------- */
/* 源类型注册                                                                  */
/* -------------------------------------------------------------------------- */

struct obs_source_info countdown_source_info = {
	.id = "countdown_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = cd_get_name,
	.create = cd_create,
	.destroy = cd_destroy,
	.get_width = cd_get_width,
	.get_height = cd_get_height,
	.get_defaults = cd_get_defaults,
	.get_properties = cd_get_properties,
	.update = cd_update,
	.activate = cd_activate,
	.deactivate = cd_deactivate,
	.show = cd_show,
	.hide = cd_hide,
	.video_tick = cd_video_tick,
	.video_render = cd_video_render,
	.icon_type = OBS_ICON_TYPE_TEXT,
};
