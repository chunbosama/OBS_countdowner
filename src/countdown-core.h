#pragma once

/*
 * 倒计时的纯逻辑核心：状态机 + 时间换算 + 文本格式化。
 *
 * 这里不引用任何 libobs 头文件，目的是能被独立编译和测试（tests/core_test.cpp）。
 * 所有「纳秒 <-> 毫秒」的换算都必须走 ns_to_ms / ms_to_ns，
 * 不要再把 (end_ns - now) 的结果直接塞进 *_ms 变量里——那正是 0.1.0 的 bug。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

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

/* 剩余毫秒 -> "MM:SS" / "HH:MM:SS"，向上取整到秒（5:00 会一直显示到剩 4:59.001） */
inline void format_time(int64_t ms, bool force_hours, char *buf, size_t size)
{
	int64_t total_sec = (ms + 999) / 1000;
	if (total_sec < 0)
		total_sec = 0;
	if (total_sec > kMaxDisplaySec)
		total_sec = kMaxDisplaySec;

	const int h = (int)(total_sec / 3600);
	const int m = (int)((total_sec % 3600) / 60);
	const int s = (int)(total_sec % 60);

	if (h > 0 || force_hours)
		snprintf(buf, size, "%02d:%02d:%02d", h, m, s);
	else
		snprintf(buf, size, "%02d:%02d", m, s);
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

	void start(int64_t now_ns)
	{
		if (state == kRunning)
			return;
		if (state == kFinished || remaining_ms <= 0)
			remaining_ms = duration_ms;
		end_ns = now_ns + ms_to_ns(remaining_ms);
		state = kRunning;
	}

	void toggle(int64_t now_ns)
	{
		if (state == kRunning) {
			remaining_ms = ns_to_ms(end_ns - now_ns);
			state = kPaused;
		} else if (state == kPaused) {
			end_ns = now_ns + ms_to_ns(remaining_ms);
			state = kRunning;
		}
	}

	void stop(int64_t now_ns)
	{
		if (state == kRunning)
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
