#pragma once
#include <obs.h>
#include <graphics/image-file.h>

#ifdef __cplusplus
extern "C" {
#endif

struct global_config_s
{
	float peak_decay_rate;
	enum obs_peak_meter_type peak_meter_type;

	bool override_colors;
	uint32_t color_bg_nominal;
	uint32_t color_bg_warning;
	uint32_t color_bg_error;
	uint32_t color_fg_nominal;
	uint32_t color_fg_warning;
	uint32_t color_fg_error;
};

extern struct global_config_s gcfg;
void gcfg_inc();
void gcfg_dec();

extern gs_image_file_t label_image;

#ifdef __cplusplus
}
#endif
