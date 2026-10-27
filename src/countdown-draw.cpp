#include "countdown-draw.h"

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <obs.h>
#include <util/bmem.h>

#include <cmath>
#include <vector>

namespace cddraw {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Vertex {
	float x, y;
	uint32_t color;
};

/* begin() 拿到的着色器 pass；nullptr 表示当前不能画 */
gs_technique_t *g_tech = nullptr;

/*
 * 进来时的面剔除模式。OBS 画 2D 精灵默认是不剔除（GS_NEITHER），
 * 退出时必须还原成它——改成 GS_BACK 会把后面子源画的文字整片剔掉。
 */
gs_cull_mode g_saved_cull = GS_NEITHER;

void submit(const std::vector<Vertex> &verts, gs_draw_mode mode)
{
	if (!g_tech || verts.size() < 3)
		return;

	gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = verts.size();
	vbd->points = (vec3 *)bzalloc(sizeof(vec3) * verts.size());
	vbd->colors = (uint32_t *)bzalloc(sizeof(uint32_t) * verts.size());
	if (!vbd->points || !vbd->colors) {
		gs_vbdata_destroy(vbd);
		return;
	}

	for (size_t i = 0; i < verts.size(); i++) {
		vec3_set(&vbd->points[i], verts[i].x, verts[i].y, 0.0f);
		vbd->colors[i] = verts[i].color;
	}

	gs_vertbuffer_t *vb = gs_vertexbuffer_create(vbd, GS_DYNAMIC);
	if (!vb) {
		gs_vbdata_destroy(vbd);
		return;
	}

	gs_load_vertexbuffer(vb);
	gs_draw(mode, 0, (uint32_t)verts.size());
	gs_vertexbuffer_destroy(vb);
	gs_load_vertexbuffer(nullptr);
}

void quad(std::vector<Vertex> &v, float x0, float y0, float x1, float y1, uint32_t color)
{
	v.push_back({x0, y0, color});
	v.push_back({x1, y0, color});
	v.push_back({x1, y1, color});

	v.push_back({x0, y0, color});
	v.push_back({x1, y1, color});
	v.push_back({x0, y1, color});
}

void arc_band(std::vector<Vertex> &v, float cx, float cy, float r_in, float r_out, float a0,
	      float a1, uint32_t color)
{
	const float span = std::fabs(a1 - a0);
	if (span <= 0.0001f)
		return;

	/* 每段弧长约 4px（400px 的圆约 300 段，肉眼看不出折线），上限 240 段：
	   顶点缓冲每帧都要重建，段数太多会白白吃带宽 */
	int segs = (int)(span * (r_out + 2.0f) / 4.0f);
	if (segs < 2)
		segs = 2;
	if (segs > 240)
		segs = 240;

	for (int i = 0; i < segs; i++) {
		const float t0 = a0 + (a1 - a0) * (float)i / (float)segs;
		const float t1 = a0 + (a1 - a0) * (float)(i + 1) / (float)segs;

		const float c0 = cosf(t0), s0 = sinf(t0);
		const float c1 = cosf(t1), s1 = sinf(t1);

		const float in0x = cx + r_in * c0, in0y = cy + r_in * s0;
		const float out0x = cx + r_out * c0, out0y = cy + r_out * s0;
		const float in1x = cx + r_in * c1, in1y = cy + r_in * s1;
		const float out1x = cx + r_out * c1, out1y = cy + r_out * s1;

		v.push_back({in0x, in0y, color});
		v.push_back({out0x, out0y, color});
		v.push_back({out1x, out1y, color});

		v.push_back({in0x, in0y, color});
		v.push_back({out1x, out1y, color});
		v.push_back({in1x, in1y, color});
	}
}

} /* namespace */

uint32_t pack(int64_t rgb, int opacity)
{
	int a = opacity < 0 ? 0 : (opacity > 100 ? 100 : opacity);

	/*
	 * 注意：OBS 的颜色整数是 0xAABBGGRR —— 低位是 R、高位是 B。
	 * （见 libobs 的 gs_effect_set_color()，它内部走 vec4_from_bgra）
	 * 属性面板的取色器、obs_data 里存的都是这个顺序，所以这里不能按 0xRRGGBB 拆。
	 */
	const uint32_t r = (uint32_t)(rgb & 0xFF);
	const uint32_t g = (uint32_t)((rgb >> 8) & 0xFF);
	const uint32_t b = (uint32_t)((rgb >> 16) & 0xFF);
	const uint32_t av = (uint32_t)((a * 255 + 50) / 100);

	/* libobs 顶点色的字节序是 R,G,B,A，小端读成整数就是 0xAABBGGRR */
	return (av << 24) | (b << 16) | (g << 8) | r;
}

void begin()
{
	g_tech = nullptr;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	if (!effect)
		return;

	gs_technique_t *tech = gs_effect_get_technique(effect, "SolidColored");
	if (!tech)
		return;

	/* SolidColored = 顶点色 * color，把 color 固定成白色即可直接用顶点色 */
	struct vec4 white;
	vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &white);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	g_saved_cull = gs_get_cull_mode();
	gs_set_cull_mode(GS_NEITHER);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	g_tech = tech;
}

void end()
{
	if (!g_tech)
		return;

	gs_technique_end_pass(g_tech);
	gs_technique_end(g_tech);
	g_tech = nullptr;

	gs_set_cull_mode(g_saved_cull);
	gs_blend_state_pop();
}

void card(int x, int y, int w, int h, int radius, uint32_t color)
{
	if (w <= 0 || h <= 0)
		return;

	const float fx = (float)x, fy = (float)y;
	const float fw = (float)w, fh = (float)h;

	std::vector<Vertex> v;

	const int limit = (w < h ? w : h) / 2;
	int r = radius;
	if (r > limit)
		r = limit;

	if (r <= 1) {
		quad(v, fx, fy, fx + fw, fy + fh, color);
		submit(v, GS_TRIS);
		return;
	}

	const float fr = (float)r;
	v.reserve(4 * 9 + 2);

	/* 四段圆角弧首尾相接，直线边由扇形三角自然连起来 */
	struct Corner {
		float ox, oy, a0;
	};
	const Corner corners[4] = {
		{fx + fr, fy + fr, 180.0f},
		{fx + fw - fr, fy + fr, 270.0f},
		{fx + fw - fr, fy + fh - fr, 0.0f},
		{fx + fr, fy + fh - fr, 90.0f},
	};

	std::vector<Vertex> outline;
	const int steps = 8;
	for (const Corner &c : corners) {
		for (int i = 0; i <= steps; i++) {
			const float a = (c.a0 + 90.0f * (float)i / (float)steps) * kPi / 180.0f;
			outline.push_back({c.ox + fr * cosf(a), c.oy + fr * sinf(a), color});
		}
	}

	const float cx = fx + fw / 2.0f;
	const float cy = fy + fh / 2.0f;
	for (size_t i = 0; i < outline.size(); i++) {
		const Vertex &a = outline[i];
		const Vertex &b = outline[(i + 1) % outline.size()];
		v.push_back({cx, cy, color});
		v.push_back(a);
		v.push_back(b);
	}

	submit(v, GS_TRIS);
}

void ring(int cx, int cy, int radius, int thickness, float fraction, uint32_t fill,
	  uint32_t track, int ticks)
{
	const float fc_x = (float)cx;
	const float fc_y = (float)cy;
	const float half = thickness > 0 ? thickness / 2.0f : 0.5f;
	const float r_mid = radius > 0 ? (float)radius : 1.0f;
	float r_out = r_mid + half;
	float r_in = r_mid - half;
	if (r_in < 1.0f)
		r_in = 1.0f;

	std::vector<Vertex> v;

	/* 轨道：整圈 */
	if (track != 0)
		arc_band(v, fc_x, fc_y, r_in, r_out, 0.0f, 2.0f * kPi, track);

	/* 剩余弧：从 12 点方向顺时针 */
	float f = fraction;
	if (f < 0.0f)
		f = 0.0f;
	if (f > 1.0f)
		f = 1.0f;
	if (f > 0.0f) {
		const float a0 = -kPi / 2.0f;
		arc_band(v, fc_x, fc_y, r_in, r_out, a0, a0 + 2.0f * kPi * f, fill);
	}

	/* 钟面刻度：贴在环内侧，短一小截 */
	if (ticks > 0) {
		const float tick_out = r_in - 3.0f;
		float tick_len = thickness * 0.9f;
		if (tick_len < 4.0f)
			tick_len = 4.0f;
		float tick_in = tick_out - tick_len;
		if (tick_in < 0.0f)
			tick_in = 0.0f;

		const float half_px = 1.5f;
		for (int i = 0; i < ticks; i++) {
			const float a = -kPi / 2.0f + 2.0f * kPi * (float)i / (float)ticks;
			float da = half_px / (tick_out > 1.0f ? tick_out : 1.0f);
			if (da > 0.35f)
				da = 0.35f;

			const float a0 = a - da, a1 = a + da;
			const float c0 = cosf(a0), s0 = sinf(a0);
			const float c1 = cosf(a1), s1 = sinf(a1);

			const float p0x = fc_x + tick_in * c0, p0y = fc_y + tick_in * s0;
			const float p1x = fc_x + tick_out * c0, p1y = fc_y + tick_out * s0;
			const float p2x = fc_x + tick_out * c1, p2y = fc_y + tick_out * s1;
			const float p3x = fc_x + tick_in * c1, p3y = fc_y + tick_in * s1;

			v.push_back({p0x, p0y, fill});
			v.push_back({p1x, p1y, fill});
			v.push_back({p2x, p2y, fill});

			v.push_back({p0x, p0y, fill});
			v.push_back({p2x, p2y, fill});
			v.push_back({p3x, p3y, fill});
		}
	}

	submit(v, GS_TRIS);
}

void bar(int x, int y, int w, int h, float fraction, uint32_t fill, uint32_t track)
{
	if (w <= 0 || h <= 0)
		return;

	const float fx = (float)x, fy = (float)y;
	const float fw = (float)w, fh = (float)h;

	std::vector<Vertex> v;

	if (track != 0)
		quad(v, fx, fy, fx + fw, fy + fh, track);

	float f = fraction;
	if (f < 0.0f)
		f = 0.0f;
	if (f > 1.0f)
		f = 1.0f;
	if (f > 0.0f)
		quad(v, fx, fy, fx + fw * f, fy + fh, fill);

	submit(v, GS_TRIS);
}

} /* namespace cddraw */
