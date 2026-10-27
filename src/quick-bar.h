#pragma once

/*
 * 快速控制栏（OBS 停靠窗口）。
 *
 * 依赖 obs-frontend-api + Qt6，因此在 module_load 里只能「登记回调」，
 * 真正创建窗口要等前端发出 FINISHED_LOADING——那时主窗口和停靠系统才就绪。
 */

namespace quickbar {

void init();
void shutdown();

} /* namespace quickbar */
