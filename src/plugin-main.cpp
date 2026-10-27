/*
 * obs-countdown —— 模块入口
 */

#include <obs-module.h>

#include "countdown-source.h"
#include "quick-bar.h"
#include "version.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-countdown", "en-US")

bool obs_module_load(void)
{
	obs_register_source(&countdown_source_info);

	/* 只是登记前端事件回调，真正的窗口要等 OBS 前端加载完才建 */
	quickbar::init();

	blog(LOG_INFO, "[obs-countdown] v%s loaded (libobs api %d.%d.%d)", PLUGIN_VERSION,
	     LIBOBS_API_MAJOR_VER, LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER);
	return true;
}

void obs_module_unload(void)
{
	quickbar::shutdown();
	blog(LOG_INFO, "[obs-countdown] unloaded");
}
