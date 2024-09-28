#pragma once

#ifdef __cplusplus
extern "C" {
#endif

gs_effect_t *create_effect_from_module_file(const char *basename);

static inline enum obs_peak_meter_type peak_meter_type_from_int(int value)
{
	switch (value) {
	case 0:
		return SAMPLE_PEAK_METER;
	case 1:
		return TRUE_PEAK_METER;
	default:
		return SAMPLE_PEAK_METER;
	}
}

#ifdef __cplusplus
}
#endif
