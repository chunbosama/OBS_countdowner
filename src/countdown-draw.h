#pragma once

/*
 * 倒计时的装饰图元：圆角卡片、进度环（含钟面刻度）、进度条。
 *
 * 只做「几何 + 纯色」，不碰字体——文字仍然由内置文本子源画，
 * 这里只负责给它铺个底/套个环。
 *
 * 调用约定：
 *   begin() / end() 必须成对，且只能出现在 video_render 里（要有 graphics 上下文）。
 *   中间可以连续画多个图元，彼此共享同一套混合状态与着色器 pass。
 *
 * 颜色统一用 OBS 的属性面板格式（0xAABBGGRR，低位是 R）加 0..100 的不透明度传入，
 * 内部自己转成顶点色。
 */

#include <cstdint>

namespace cddraw {

/* OBS 颜色整数 + 不透明度(0..100) -> 顶点色（libobs 顶点色是 RGBA 字节序） */
uint32_t pack(int64_t rgb, int opacity);

/* 开始 / 结束一批纯色绘制 */
void begin();
void end();

/* 圆角矩形，(x, y) 是左上角；radius 会被夹到不超过短边的一半 */
void card(int x, int y, int w, int h, int radius, uint32_t color);

/*
 * 进度环：
 *   radius   中心线半径
 *   thickness 环宽
 *   fraction 剩余占比 0..1，从 12 点方向顺时针铺开
 *   ticks    刻度数量（12 就是钟面），0 表示不画
 */
void ring(int cx, int cy, int radius, int thickness, float fraction, uint32_t fill,
	  uint32_t track, int ticks);

/* 进度条：从左往右按 fraction 填充 */
void bar(int x, int y, int w, int h, float fraction, uint32_t fill, uint32_t track);

} /* namespace cddraw */
