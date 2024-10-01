#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/threading.h>
#include "plugin-macros.generated.h"
#include "global-config.h"
#include "util.h"

#define LABEL_IMAGE_FILE_NAME "labels.png"

static volatile long refcnt = 0;
struct global_config_s gcfg = {0};
gs_image_file_t label_image = {0};

static inline uint32_t color_from_cfg(long long value)
{
	return (value & 0xFF) << 16 | (value & 0xFF00) | (value & 0xFF0000) >> 16 | 0xFF000000;
}

static void gcfg_update()
{
	ASSERT_THREAD(OBS_TASK_UI);
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile) {
		blog(LOG_ERROR, "obs_frontend_get_profile_config returns NULL.");
		return;
	}

	struct global_config_s c = gcfg;

	c.peak_decay_rate = (float)config_get_double(profile, "Audio", "MeterDecayRate");
	c.peak_meter_type = peak_meter_type_from_int(config_get_int(profile, "Audio", "PeakMeterType"));

#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(31, 0, 0) || defined(WITH_FRONTEND_USER_CONFIG)
	config_t *user = obs_frontend_get_user_config();
	if (!user) {
		blog(LOG_ERROR, "obs_frontend_get_user_config returns NULL.");
		return;
	}
#else
	config_t *user = obs_frontend_get_global_config();
	if (!user) {
		blog(LOG_ERROR, "obs_frontend_get_global_config returns NULL.");
		return;
	}
#endif

	c.override_colors = config_get_bool(user, "Accessibility", "OverrideColors");
	c.color_bg_nominal = color_from_cfg(config_get_int(user, "Accessibility", "MixerGreen"));
	c.color_bg_warning = color_from_cfg(config_get_int(user, "Accessibility", "MixerYellow"));
	c.color_bg_error = color_from_cfg(config_get_int(user, "Accessibility", "MixerRed"));
	c.color_fg_nominal = color_from_cfg(config_get_int(user, "Accessibility", "MixerGreenActive"));
	c.color_fg_warning = color_from_cfg(config_get_int(user, "Accessibility", "MixerYellowActive"));
	c.color_fg_error = color_from_cfg(config_get_int(user, "Accessibility", "MixerRedActive"));

	obs_enter_graphics();
	gcfg = c;
	obs_leave_graphics();
}

static void frontend_save_cb(obs_data_t *save_data, bool saving, void *private_data)
{
	/* When the settings has been changed, this callback will be called. */
	UNUSED_PARAMETER(save_data);
	UNUSED_PARAMETER(private_data);
	if (!saving)
		return;

	gcfg_update();
}

static void run_in_ui(obs_task_t task, void *param)
{
	if (obs_in_task_thread(OBS_TASK_UI))
		task(param);
	else
		obs_queue_task(OBS_TASK_UI, task, param, false);
}

static void gcfg_inc_defer_ui(void *data)
{
	ASSERT_THREAD(OBS_TASK_UI);
	UNUSED_PARAMETER(data);
	gcfg_update();
	obs_frontend_add_save_callback(frontend_save_cb, NULL);

	if (!label_image.loaded) {
		char *f = obs_module_file(LABEL_IMAGE_FILE_NAME);
		blog(LOG_DEBUG, "Opening image file '" LABEL_IMAGE_FILE_NAME "' (%s)...", f);
		gs_image_file_init(&label_image, f);
		if (!label_image.loaded)
			blog(LOG_ERROR, "Cannot load '" LABEL_IMAGE_FILE_NAME "' (%s)", f);
		bfree(f);

		obs_enter_graphics();
		gs_image_file_init_texture(&label_image);
		obs_leave_graphics();
	}
}

void gcfg_inc()
{
	if (os_atomic_inc_long(&refcnt) == 1)
		run_in_ui(gcfg_inc_defer_ui, NULL);
}

static void gcfg_dec_defer_ui(void *data)
{
	ASSERT_THREAD(OBS_TASK_UI);
	UNUSED_PARAMETER(data);
	obs_frontend_remove_save_callback(frontend_save_cb, NULL);

	blog(LOG_DEBUG, "Releasing image file '" LABEL_IMAGE_FILE_NAME "'...");
	obs_enter_graphics();
	gs_image_file_free(&label_image);
	obs_leave_graphics();
}

void gcfg_dec()
{
	if (os_atomic_dec_long(&refcnt) == 0)
		run_in_ui(gcfg_dec_defer_ui, NULL);
}
