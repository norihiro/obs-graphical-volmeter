#pragma once

#ifdef __cplusplus
extern "C" {
#endif

gs_effect_t *create_effect_from_module_file(const char *basename);

#ifdef WITH_ASSERT_THREAD
#define ASSERT_THREAD(type)                                                                     \
	do {                                                                                    \
		if (!obs_in_task_thread(type))                                                  \
			blog(LOG_ERROR, "%s: ASSERT_THREAD failed: Expected " #type, __func__); \
	} while (false)
#define ASSERT_THREAD2(type1, type2)                                                                            \
	do {                                                                                                    \
		if (!obs_in_task_thread(type1) && !obs_in_task_thread(type2))                                   \
			blog(LOG_ERROR, "%s: ASSERT_THREAD2 failed: Expected " #type1 " or " #type2, __func__); \
	} while (false)
#define ASSERT_GRAPHICS_CONTEXT()                                                                                   \
	do {                                                                                                        \
		if (!gs_get_context())                                                                              \
			blog(LOG_ERROR, "%s: ASSERT_GRAPHICS_CONTEXT failed: Expected graphics context", __func__); \
	} while (false)
#else
#define ASSERT_THREAD(type)
#define ASSERT_THREAD2(type1, type2)
#define ASSERT_GRAPHICS_CONTEXT()
#endif

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
