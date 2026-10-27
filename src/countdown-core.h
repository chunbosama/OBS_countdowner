#pragma once

/*
 * 倒计时的纯逻辑核心：状态机 + 时间换算 + 文本格式化。
 *
 * 这里不引用任何 libobs 头文件，目的是能被独立编译和测试（tests/core_test.cpp）。
 * 所有「纳秒 <-> 毫秒」的换算都必须走 ns_to_ms / ms_to_ns，
 * 不要再把 (end_ns - now) 的结果直接塞进 *_ms 变量里——那正是 0.1.0 的 bug。
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace cd {

constexpr int64_t kNsPerMs = 1000000LL;
constexpr int64_t kNsPerSec = 1000LL * kNsPerMs;
constexpr int64_t kMaxDisplaySec = 359999; /* 99:59:59 */

enum State : int {
	kIdle = 0,
	kRunning,
	kPaused,
	kFinished,
};

/* 纳秒差 -> 毫秒，负数一律归零 */
inline int64_t ns_to_ms(int64_t ns)
{
	return ns > 0 ? ns / kNsPerMs : 0;
}

/* 毫秒 -> 纳秒，负数一律归零 */
inline int64_t ms_to_ns(int64_t ms)
{
	return ms > 0 ? ms * kNsPerMs : 0;
}

/* 显示格式。值会被存进场景集合，增删项只能往后追加。 */
enum Format : int {
	kFmtAuto = 0,   /* 不足 1 小时 MM:SS，达到 1 小时自动带小时 */
	kFmtHours,      /* 始终 HH:MM:SS */
	kFmtChinese,    /* 5分00秒 / 1时30分00秒 */
	kFmtSeconds,    /* 纯秒数：300 */
};

inline const char *format_id(Format f)
{
	switch (f) {
	case kFmtHours:
		return "hms";
	case kFmtChinese:
		return "chinese";
	case kFmtSeconds:
		return "seconds";
	default:
		return "auto";
	}
}

inline Format format_from_id(const char *id)
{
	if (!id)
		return kFmtAuto;
	if (strcmp(id, "hms") == 0)
		return kFmtHours;
	if (strcmp(id, "chinese") == 0)
		return kFmtChinese;
	if (strcmp(id, "seconds") == 0)
		return kFmtSeconds;
	return kFmtAuto;
}

/* 剩余毫秒 -> 显示文本，向上取整到秒（5:00 会一直显示到剩 4:59.001） */
inline void format_display(int64_t ms, Format fmt, char *buf, size_t size)
{
	int64_t total_sec = (ms + 999) / 1000;
	if (total_sec < 0)
		total_sec = 0;
	if (total_sec > kMaxDisplaySec)
		total_sec = kMaxDisplaySec;

	const int h = (int)(total_sec / 3600);
	const int m = (int)((total_sec % 3600) / 60);
	const int s = (int)(total_sec % 60);

	switch (fmt) {
	case kFmtHours:
		snprintf(buf, size, "%02d:%02d:%02d", h, m, s);
		break;
	case kFmtChinese:
		if (h > 0)
			snprintf(buf, size, "%d时%02d分%02d秒", h, m, s);
		else if (m > 0)
			snprintf(buf, size, "%d分%02d秒", m, s);
		else
			snprintf(buf, size, "%d秒", s);
		break;
	case kFmtSeconds:
		snprintf(buf, size, "%lld", (long long)total_sec);
		break;
	default:
		if (h > 0)
			snprintf(buf, size, "%02d:%02d:%02d", h, m, s);
		else
			snprintf(buf, size, "%02d:%02d", m, s);
		break;
	}
}

/* 兼容老调用：force_hours 就是 kFmtHours */
inline void format_time(int64_t ms, bool force_hours, char *buf, size_t size)
{
	format_display(ms, force_hours ? kFmtHours : kFmtAuto, buf, size);
}

/* -------------------------------------------------------------------------- */
/* 外观样式与版面                                                              */
/* -------------------------------------------------------------------------- */

/*
 * 样式只决定「除了数字还画什么」：
 *   digits  只画数字（0.2.x 的行为）
 *   card    数字 + 圆角卡片底
 *   ring    数字 + 圆环进度（钟面感，可选 12 个刻度）
 *   bar     数字 + 底部横向进度条
 */
enum Style : int {
	kStyleDigits = 0,
	kStyleCard,
	kStyleRing,
	kStyleBar,
};

inline const char *style_id(Style s)
{
	switch (s) {
	case kStyleCard:
		return "card";
	case kStyleRing:
		return "ring";
	case kStyleBar:
		return "bar";
	default:
		return "digits";
	}
}

inline Style style_from_id(const char *id)
{
	if (!id)
		return kStyleDigits;
	if (strcmp(id, "card") == 0)
		return kStyleCard;
	if (strcmp(id, "ring") == 0)
		return kStyleRing;
	if (strcmp(id, "bar") == 0)
		return kStyleBar;
	return kStyleDigits;
}

/* 用户可调的几何/样式参数（颜色不参与版面计算） */
struct StyleParams {
	int style = kStyleDigits;
	int padding = 24;   /* 数字到卡片边缘 / 圆环内缘的距离 */
	int radius = 24;    /* 卡片圆角，仅在 card 下生效 */
	int thickness = 12; /* 环宽 / 条高 */
	int ticks = 0;      /* 钟面刻度数量，0 表示不画 */
};

/* 版面结果，坐标都是源内的像素，原点在左上角、y 向下（libobs 源坐标系） */
struct Layout {
	int width = 0;
	int height = 0;

	int text_x = 0;
	int text_y = 0;

	int card_radius = 0;

	int ring_cx = 0;
	int ring_cy = 0;
	int ring_radius = 0; /* 环中心线半径 */
	int ring_thickness = 0;

	int bar_x = 0;
	int bar_y = 0;
	int bar_w = 0;
	int bar_h = 0;
};

/*
 * 给定文字子源的实际尺寸，算出整个源的尺寸与各元素位置。
 *
 * ring 的直径按文字外接圆求（半对角线 + padding），这样四个角不会压到环上；
 * 因此长文本（05:00）得到的圆环比短文本（9）大，但不同格式之间不会跳动。
 */
inline Layout compute_layout(const StyleParams &p, int text_w, int text_h)
{
	Layout l;
	if (text_w < 1)
		text_w = 1;
	if (text_h < 1)
		text_h = 1;

	const int pad = p.padding > 0 ? p.padding : 0;
	const int thick = p.thickness > 0 ? p.thickness : 1;

	l.width = text_w;
	l.height = text_h;

	switch (p.style) {
	case kStyleCard: {
		l.width = text_w + pad * 2;
		l.height = text_h + pad * 2;
		l.text_x = pad;
		l.text_y = pad;
		const int limit = (l.width < l.height ? l.width : l.height) / 2;
		int r = p.radius;
		if (r < 0)
			r = 0;
		if (r > limit)
			r = limit;
		l.card_radius = r;
		break;
	}
	case kStyleRing: {
		const double hw = text_w / 2.0;
		const double hh = text_h / 2.0;
		const int inner = (int)(std::sqrt(hw * hw + hh * hh) + 0.5) + pad;
		const int outer = inner + thick;
		const int side = outer * 2 + 2; /* 留 1px 余量，免得抗锯齿被边缘切掉 */
		l.width = side;
		l.height = side;
		l.text_x = (side - text_w) / 2;
		l.text_y = (side - text_h) / 2;
		l.ring_cx = side / 2;
		l.ring_cy = side / 2;
		l.ring_thickness = thick;
		l.ring_radius = inner + thick / 2;
		break;
	}
	case kStyleBar: {
		int gap = pad / 2;
		if (gap < 4)
			gap = 4;
		l.width = text_w;
		l.height = text_h + gap + thick;
		l.text_x = 0;
		l.text_y = 0;
		l.bar_x = 0;
		l.bar_y = text_h + gap;
		l.bar_w = l.width;
		l.bar_h = thick;
		break;
	}
	default:
		break;
	}

	return l;
}

/* 剩余占比，环/条按它画长度；空闲时是 1，走完是 0 */
inline float remaining_fraction(int64_t remaining_ms, int64_t duration_ms)
{
	if (duration_ms <= 0 || remaining_ms <= 0)
		return 0.0f;
	if (remaining_ms >= duration_ms)
		return 1.0f;
	return (float)((double)remaining_ms / (double)duration_ms);
}

struct Timer {
	State state = kIdle;
	int64_t duration_ms = 300000;
	int64_t remaining_ms = 300000;
	int64_t end_ns = 0;

	/* 源不可见时冻结：suspended 期间 advance() 不推进计时 */
	bool suspended = false;

	bool running() const { return state == kRunning; }

	void set_duration(int64_t ms, bool apply_to_remaining)
	{
		if (ms <= 0)
			ms = 1000; /* 最小 1 秒 */
		duration_ms = ms;
		if (apply_to_remaining)
			remaining_ms = ms;
	}

	/*
	 * 属性面板每改一个字段（包括颜色、字体这种跟时长无关的）都会走 update，
	 * 所以同步剩余时间的判据必须是「设定时长本身变了」，而不是「当前是不是空闲」。
	 * 否则「停止」保留的剩余时间会被下一次改样式冲成满值。
	 *
	 * 运行中/暂停中只记下新的设定值，不动当前这轮倒数。
	 */
	void apply_duration(int64_t ms)
	{
		if (ms <= 0)
			ms = 1000; /* 最小 1 秒 */
		const bool changed = (ms != duration_ms);
		duration_ms = ms;
		if (changed && (state == kIdle || state == kFinished))
			remaining_ms = ms;
	}

	void start(int64_t now_ns)
	{
		if (state == kRunning)
			return;
		if (state == kFinished || remaining_ms <= 0)
			remaining_ms = duration_ms;
		end_ns = now_ns + ms_to_ns(remaining_ms);
		state = kRunning;
	}

	/*
	 * 注意 toggle / stop 里的 `!suspended`：
	 * suspend() 之后 end_ns 就不再代表「真实的目标结束时刻」了（它在恢复时才会重建），
	 * 此时拿 end_ns - now 去算会把冻结期间流逝的时间算进去。
	 * 冻结中的剩余时间已经存在 remaining_ms 里，直接用就行。
	 */
	void toggle(int64_t now_ns)
	{
		if (state == kRunning) {
			if (!suspended)
				remaining_ms = ns_to_ms(end_ns - now_ns);
			state = kPaused;
		} else if (state == kPaused) {
			end_ns = now_ns + ms_to_ns(remaining_ms);
			state = kRunning;
		}
	}

	void stop(int64_t now_ns)
	{
		if (state == kRunning && !suspended)
			remaining_ms = ns_to_ms(end_ns - now_ns);
		state = kIdle;
	}

	void reset()
	{
		state = kIdle;
		remaining_ms = duration_ms;
	}

	void suspend(int64_t now_ns)
	{
		if (suspended)
			return;
		if (state == kRunning)
			remaining_ms = ns_to_ms(end_ns - now_ns);
		suspended = true;
	}

	void resume(int64_t now_ns)
	{
		if (!suspended)
			return;
		if (state == kRunning)
			end_ns = now_ns + ms_to_ns(remaining_ms);
		suspended = false;
	}

	/* 推进时间。返回 true 表示显示的秒数变了，需要重绘。 */
	bool advance(int64_t now_ns)
	{
		if (suspended || state != kRunning)
			return false;

		const int64_t remain_ns = end_ns - now_ns;
		if (remain_ns <= 0) {
			remaining_ms = 0;
			state = kFinished;
			return true;
		}

		const int64_t remain_ms = ns_to_ms(remain_ns);
		const int64_t old_sec = (remaining_ms + 999) / 1000;
		const int64_t new_sec = (remain_ms + 999) / 1000;
		remaining_ms = remain_ms;
		return old_sec != new_sec;
	}
};

} /* namespace cd */
