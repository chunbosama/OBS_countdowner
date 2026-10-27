/*
 * 倒计时核心逻辑测试（不依赖 libobs，可直接编译运行）
 *
 * 覆盖：状态机四个状态的动作语义、时间单位换算、格式化边界、隐藏冻结。
 * 这里专门盯住 0.1.0 的那个 bug：把纳秒当成毫秒写回 remaining_ms。
 */

#include "../src/countdown-core.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                       \
	do {                                                              \
		if (!(cond)) {                                            \
			printf("  FAIL  line %d: %s\n", __LINE__, #cond); \
			++g_failures;                                     \
		}                                                         \
	} while (0)

#define CHECK_TEXT(ms, force_hours, expect)                                              \
	do {                                                                             \
		char buf_[64];                                                           \
		cd::format_time((ms), (force_hours), buf_, sizeof(buf_));                \
		if (strcmp(buf_, (expect)) != 0) {                                       \
			printf("  FAIL  line %d: format(%lld, %d) = \"%s\", want \"%s\"\n", \
			       __LINE__, (long long)(ms), (int)(force_hours), buf_, (expect)); \
			++g_failures;                                                    \
		}                                                                        \
	} while (0)

static void test_format()
{
	printf("[format]\n");
	CHECK_TEXT(0, false, "00:00");
	CHECK_TEXT(500, false, "00:01");          /* 向上取整到秒 */
	CHECK_TEXT(300000, false, "05:00");
	CHECK_TEXT(299000, false, "04:59");
	CHECK_TEXT(3599000, false, "59:59");      /* 59:59 */
	CHECK_TEXT(3600000, false, "01:00:00");   /* 满 1 小时自动带小时 */
	CHECK_TEXT(60000, true, "00:01:00");      /* 强制显示小时 */
	CHECK_TEXT(-5, false, "00:00");           /* 负数归零 */
	CHECK_TEXT(1000000000LL, false, "99:59:59"); /* 超大值截到上限 */
}

static void test_basic_flow()
{
	printf("[idle -> start -> tick]\n");
	const int64_t MS = 1000000LL;
	const int64_t t0 = 1000000000LL;

	cd::Timer t;
	CHECK(t.state == cd::kIdle);
	CHECK(t.remaining_ms == 300000);

	t.start(t0);
	CHECK(t.state == cd::kRunning);
	CHECK(t.end_ns == t0 + 300000 * MS);

	t.advance(t0);
	CHECK(t.remaining_ms == 300000);

	/* 关键回归：1 秒后剩余必须是毫秒量级，不是纳秒量级 */
	t.advance(t0 + 1000 * MS);
	CHECK(t.remaining_ms == 299000);
	CHECK(t.remaining_ms < 1000 * 1000);

	t.advance(t0 + 60000 * MS);
	CHECK(t.remaining_ms == 240000);
}

static void test_pause_resume()
{
	printf("[pause / resume]\n");
	const int64_t MS = 1000000LL;
	const int64_t t0 = 1000000000LL;

	cd::Timer t;
	t.start(t0);
	t.advance(t0 + 65000 * MS); /* 剩 235 秒 */

	t.toggle(t0 + 65000 * MS);
	CHECK(t.state == cd::kPaused);
	const int64_t frozen = t.remaining_ms;
	CHECK(frozen == 235000);

	/* 暂停期间无论过多久都不变 */
	t.advance(t0 + 600000 * MS);
	CHECK(t.remaining_ms == frozen);
	CHECK(t.state == cd::kPaused);

	t.toggle(t0 + 600000 * MS);
	CHECK(t.state == cd::kRunning);
	t.advance(t0 + 601000 * MS);
	CHECK(t.remaining_ms == frozen - 1000);
}

static void test_finish_and_restart()
{
	printf("[finish / restart]\n");
	const int64_t MS = 1000000LL;
	const int64_t t0 = 1000000000LL;

	cd::Timer t;
	t.start(t0);
	t.advance(t0 + 300000 * MS + 1); /* 越过终点 */
	CHECK(t.state == cd::kFinished);
	CHECK(t.remaining_ms == 0);

	/* 结束后再开始：先回满值 */
	t.start(t0 + 400000 * MS);
	CHECK(t.state == cd::kRunning);
	CHECK(t.remaining_ms == 300000);
}

static void test_stop_keeps_remaining()
{
	printf("[stop keeps remaining]\n");
	const int64_t MS = 1000000LL;
	const int64_t t0 = 1000000000LL;

	cd::Timer t;
	t.start(t0);
	t.advance(t0 + 5000 * MS);
	t.stop(t0 + 5000 * MS);
	CHECK(t.state == cd::kIdle);
	CHECK(t.remaining_ms == 295000);

	/* 停止后重新开始，从保留值接着走 */
	t.start(t0 + 10000 * MS);
	t.advance(t0 + 11000 * MS);
	CHECK(t.remaining_ms == 294000);

	/* 重置回设定时长 */
	t.reset();
	CHECK(t.state == cd::kIdle);
	CHECK(t.remaining_ms == 300000);
}

static void test_suspend_resume()
{
	printf("[suspend / resume]\n");
	const int64_t MS = 1000000LL;
	const int64_t t0 = 1000000000LL;

	cd::Timer t;
	t.start(t0);
	t.advance(t0 + 10000 * MS); /* 剩 290 秒 */
	const int64_t before = t.remaining_ms;
	CHECK(before == 290000);

	t.suspend(t0 + 10000 * MS);
	CHECK(t.suspended);

	/* 隐藏 100 秒：不推进 */
	t.advance(t0 + 110000 * MS);
	CHECK(t.remaining_ms == before);

	/* 恢复后继续从冻结处走 */
	t.resume(t0 + 110000 * MS);
	CHECK(!t.suspended);
	t.advance(t0 + 111000 * MS);
	CHECK(t.remaining_ms == before - 1000);
}

static void test_duration_change()
{
	printf("[duration change]\n");
	const int64_t MS = 1000000LL;
	const int64_t t0 = 1000000000LL;

	cd::Timer t;
	t.set_duration(60000, true);
	CHECK(t.remaining_ms == 60000);

	/* 0 会被拒绝，回落到 1 秒 */
	t.set_duration(0, true);
	CHECK(t.duration_ms == 1000);
	CHECK(t.remaining_ms == 1000);

	/* 运行中改时长不影响当前倒数 */
	t.set_duration(300000, false);
	t.reset(); /* 回到 5 分钟再跑 */
	t.start(t0);
	t.advance(t0 + 1000 * MS);
	CHECK(t.remaining_ms == 299000);

	/* 运行中再改时长：设定值变了，当前倒数不动 */
	t.set_duration(120000, false);
	CHECK(t.duration_ms == 120000);
	CHECK(t.remaining_ms == 299000);

	/* 结束态改时长会同步到显示 */
	cd::Timer t2;
	t2.set_duration(120000, true);
	CHECK(t2.remaining_ms == 120000);
}

static void test_display_format()
{
	printf("[display format]\n");

	char buf[64];

	cd::format_display(300000, cd::kFmtAuto, buf, sizeof(buf));
	CHECK(strcmp(buf, "05:00") == 0);

	cd::format_display(300000, cd::kFmtHours, buf, sizeof(buf));
	CHECK(strcmp(buf, "00:05:00") == 0);

	cd::format_display(300000, cd::kFmtChinese, buf, sizeof(buf));
	CHECK(strcmp(buf, "5分00秒") == 0);

	cd::format_display(5400000, cd::kFmtChinese, buf, sizeof(buf));
	CHECK(strcmp(buf, "1时30分00秒") == 0);

	cd::format_display(45000, cd::kFmtChinese, buf, sizeof(buf));
	CHECK(strcmp(buf, "45秒") == 0);

	cd::format_display(300000, cd::kFmtSeconds, buf, sizeof(buf));
	CHECK(strcmp(buf, "300") == 0);

	/* 向上取整仍然成立：剩 0.5 秒是 1 秒，走完是 0 */
	cd::format_display(500, cd::kFmtSeconds, buf, sizeof(buf));
	CHECK(strcmp(buf, "1") == 0);
	cd::format_display(0, cd::kFmtSeconds, buf, sizeof(buf));
	CHECK(strcmp(buf, "0") == 0);

	/* id <-> 枚举互转，场景集合里存的是 id */
	CHECK(cd::format_from_id(cd::format_id(cd::kFmtChinese)) == cd::kFmtChinese);
	CHECK(cd::format_from_id("nonsense") == cd::kFmtAuto);
	CHECK(cd::format_from_id(nullptr) == cd::kFmtAuto);
}

static void test_layout()
{
	printf("[style layout]\n");
	const int TW = 300;
	const int TH = 120;

	/* 纯数字：源就是文字大小，不做任何偏移 */
	{
		cd::StyleParams p;
		p.style = cd::kStyleDigits;
		const cd::Layout l = cd::compute_layout(p, TW, TH);
		CHECK(l.width == TW);
		CHECK(l.height == TH);
		CHECK(l.text_x == 0);
		CHECK(l.text_y == 0);
	}

	/* 卡片：四周各留一个 padding，圆角不超过短边一半 */
	{
		cd::StyleParams p;
		p.style = cd::kStyleCard;
		p.padding = 20;
		p.radius = 500; /* 故意给个超大值 */
		const cd::Layout l = cd::compute_layout(p, TW, TH);
		CHECK(l.width == TW + 40);
		CHECK(l.height == TH + 40);
		CHECK(l.text_x == 20);
		CHECK(l.text_y == 20);
		CHECK(l.card_radius == (TH + 40) / 2);
	}

	/* 圆环：正方形，文字居中，环整圈都在框内 */
	{
		cd::StyleParams p;
		p.style = cd::kStyleRing;
		p.padding = 20;
		p.thickness = 12;
		const cd::Layout l = cd::compute_layout(p, TW, TH);

		CHECK(l.width == l.height);
		CHECK(l.text_x == (l.width - TW) / 2);
		CHECK(l.text_y == (l.height - TH) / 2);
		CHECK(l.ring_cx == l.width / 2);
		CHECK(l.ring_radius == l.width / 2 - 1 - l.ring_thickness / 2);

		/* 环内缘必须包住文字的外接圆，四角才不会压到环 */
		const double half_diag = sqrt((double)TW * TW + (double)TH * TH) / 2.0;
		const int inner = l.ring_radius - l.ring_thickness / 2;
		CHECK(inner >= (int)half_diag);
	}

	/* 进度条：宽度跟文字走，高度多出一条 bar */
	{
		cd::StyleParams p;
		p.style = cd::kStyleBar;
		p.padding = 20;
		p.thickness = 10;
		const cd::Layout l = cd::compute_layout(p, TW, TH);
		CHECK(l.width == TW);
		CHECK(l.height == TH + 10 + 10);
		CHECK(l.bar_x == 0);
		CHECK(l.bar_w == TW);
		CHECK(l.bar_h == 10);
		CHECK(l.bar_y == TH + 10);
		/* 文字不能被条盖住 */
		CHECK(l.bar_y >= l.text_y + TH);
	}
}

static void test_remaining_fraction()
{
	printf("[remaining fraction]\n");
	CHECK(cd::remaining_fraction(300000, 300000) == 1.0f);
	CHECK(cd::remaining_fraction(150000, 300000) == 0.5f);
	CHECK(cd::remaining_fraction(0, 300000) == 0.0f);
	CHECK(cd::remaining_fraction(-100, 300000) == 0.0f);
	CHECK(cd::remaining_fraction(500, 0) == 0.0f);       /* 时长为 0 不除零 */
	CHECK(cd::remaining_fraction(400000, 300000) == 1.0f); /* 超界夹到 1 */
}

int main()
{
	test_format();
	test_display_format();
	test_layout();
	test_remaining_fraction();
	test_basic_flow();
	test_pause_resume();
	test_finish_and_restart();
	test_stop_keeps_remaining();
	test_suspend_resume();
	test_duration_change();

	if (g_failures == 0) {
		printf("\nALL PASS\n");
		return 0;
	}
	printf("\n%d check(s) failed\n", g_failures);
	return 1;
}
