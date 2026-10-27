#pragma once

/*
 * 全局倒计时实例登记表。
 *
 * 为什么需要它：
 *   倒计时源本身跑在 libobs 的线程里（video_tick / 热键回调），
 *   而快速控制栏跑在 Qt 主线程，两边无法直接互相调用。
 *   于是让每个实例在这里挂个号，控制栏只认「编号」，不碰指针。
 *
 * 锁顺序（很重要）：
 *   registry  ->  countdown
 * 也就是「持有 registry 锁时可以去拿实例锁」，但反过来绝对不行。
 * 所以任何已经持有 countdown::lock 的代码都不许再调用本文件的接口。
 */

#include <cstdint>
#include <string>
#include <vector>

namespace cdreg {

enum Action : int {
	kStart = 0,
	kToggle = 1,
	kStop = 2,
	kReset = 3,
};

/* 一次状态上报的纯数据快照，不含任何 libobs 类型 */
struct Status {
	int state = 0; /* cd::State */
	bool suspended = false;
	int format = 0; /* cd::Format，控制栏按同一格式显示时间 */
	int64_t remaining_ms = 0;
	int64_t duration_ms = 0;
};

struct Entry {
	uint64_t id = 0;
	std::string name; /* 展示用名字：没填标签就退回源名 */
	Status status;
};

using ActionFn = void (*)(void *handle, int action);

/* 登记一个实例，返回它的编号（从 1 开始，0 表示失败） */
uint64_t add(void *handle, ActionFn action);

/* 注销。重复调用无害。 */
void remove(void *handle);

/*
 * 刷新状态。由 video_tick 每秒几十次地调用，所以这里只做纯数据拷贝，
 * 不调用任何 libobs 接口。
 * name 传 nullptr 表示名字没变。
 */
void refresh(void *handle, const char *name, const Status &status);

/* 当前所有实例的快照，按登记顺序排列 */
std::vector<Entry> snapshot();

/* 对指定实例执行动作；编号不存在时返回 false */
bool perform(uint64_t id, int action);

} /* namespace cdreg */
