#include <inttypes.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <graphics/matrix4.h>
#include "plugin-macros.generated.h"
#include "volmeter.h"
#include "util.h"

#define AGE_THRESHOLD 0.05f // [s]

#define DISPLAY_WIDTH_PER_CHANNEL 2
#define DISPLAY_HEIGHT_PER_DB 2

static inline float clamp_flt(float x, float min, float max)
{
	return fminf(fmaxf(x, min), max);
}

struct channel_volume_s
{
	float display_magnitude;
	float display_peak;
	float peak_hold;
	float peak_hold_age;
};

struct source_s
{
	obs_source_t *context;
	gs_effect_t *effect;

	// properties
	int track;
	float magnitude_attack_rate;
	float magnitude_min;
	float peak_decay_rate;
	float peak_hold_duration;

	// internal data
	// thread: audio
	volmeter_t *volmeter;
	DARRAY(uint8_t) buffer;

	// magnitude and peak values written by audio thread
	pthread_mutex_t mutex;
	float current_magnitude[MAX_AUDIO_CHANNELS];
	float current_peak[MAX_AUDIO_CHANNELS];
	bool current_volume_updated;
	float current_volume_age;

	// internal data
	// thread: graphics
	struct channel_volume_s volumes[MAX_AUDIO_CHANNELS];
};

static void audio_cb(void *param, size_t mix_idx, struct audio_data *data);
static void volume_cb(void *param, const float magnitude[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
		      const float input_peak[MAX_AUDIO_CHANNELS]);

static const char *get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("GraphicalVolMeter.Source.Name");
}

static obs_properties_t *get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, "track", obs_module_text("Prop.Track"), 0, MAX_AUDIO_MIXES - 1, 1);

	return props;
}

static void get_defaults(obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
}

static void update(void *data, obs_data_t *settings)
{
	struct source_s *s = data;

	int track = obs_data_get_int(settings, "track");
	if (track != s->track && 0 <= track && track < MAX_AUDIO_MIXES) {
		obs_remove_raw_audio_callback(s->track, audio_cb, s);
		obs_add_raw_audio_callback(track, NULL, audio_cb, s);
		s->track = track;
	}
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct source_s *s = bzalloc(sizeof(struct source_s));
	s->context = source;
	s->track = -1;

	s->current_volume_age = M_INFINITE;

	for (uint32_t ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
		s->current_magnitude[ch] = -M_INFINITE;
		s->current_peak[ch] = -M_INFINITE;
		s->volumes[ch].display_magnitude = -M_INFINITE;
		s->volumes[ch].display_peak = -M_INFINITE;
		s->volumes[ch].peak_hold = -M_INFINITE;
	}

	obs_enter_graphics();
	s->effect = create_effect_from_module_file("volmeter.effect");
	obs_leave_graphics();

	pthread_mutex_init(&s->mutex, NULL);

	// TODO: Set by properties.
	s->magnitude_attack_rate = 0.99f / 0.3f;
	s->magnitude_min = -60.0f;
	s->peak_decay_rate = 20.0f / 0.85f; // [dB/s]
	s->peak_hold_duration = 20.0f;      // [s]

	s->volmeter = volmeter_create();
	if (!s->volmeter)
		goto fail;

	update(s, settings);

	volmeter_add_callback(s->volmeter, volume_cb, s);

	return s;

fail:
	bfree(s);
	return NULL;
}

static void destroy(void *data)
{
	struct source_s *s = data;

	if (s->track >= 0)
		obs_remove_raw_audio_callback(s->track, audio_cb, s);

	volmeter_remove_callback(s->volmeter, volume_cb, s);
	volmeter_destroy(s->volmeter);

	pthread_mutex_destroy(&s->mutex);

	da_free(s->buffer);

	bfree(s);
}

static inline void tick_magnitude(const struct source_s *s, struct channel_volume_s *c, float mag, float duration)
{
	if (!isfinite(c->display_magnitude)) {
		c->display_magnitude = mag;
	}
	else {
		float attack = (mag - c->display_magnitude) * duration * s->magnitude_attack_rate;
		c->display_magnitude = clamp_flt(c->display_magnitude + attack, s->magnitude_min, 0.0f);
	}
}

static inline void tick_peak(const struct source_s *s, struct channel_volume_s *c, float peak, float duration)
{
	if (peak >= c->display_peak || isnan(c->display_peak)) {
		c->display_peak = peak;
	}
	else {
		float decay = duration * s->peak_decay_rate;
		c->display_peak = clamp_flt(c->display_peak - decay, peak, 0.0f);
	}

	if (peak >= c->peak_hold || !isfinite(c->peak_hold) || c->peak_hold_age > s->peak_hold_duration) {
		c->peak_hold = peak;
		c->peak_hold_age = 0.0f;
	}
	else {
		c->peak_hold_age += duration;
	}
}

void tick(void *data, float duration)
{
	struct source_s *s = data;

	float current_magnitude[MAX_AUDIO_CHANNELS];
	float current_peak[MAX_AUDIO_CHANNELS];

	pthread_mutex_lock(&s->mutex);
	bool updated = s->current_volume_updated;
	if (updated) {
		memcpy(current_magnitude, s->current_magnitude, sizeof(float) * MAX_AUDIO_CHANNELS);
		memcpy(current_peak, s->current_peak, sizeof(float) * MAX_AUDIO_CHANNELS);
		s->current_volume_updated = false;
		s->current_volume_age = 0;
	}
	pthread_mutex_unlock(&s->mutex);

	if (!updated) {
		if (s->current_volume_age >= AGE_THRESHOLD) {
			for (uint32_t ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
				current_magnitude[ch] = -M_INFINITE;
				current_peak[ch] = -M_INFINITE;
			}
		}
		else {
			s->current_volume_age += duration;
		}
	}

	for (uint32_t ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
		tick_magnitude(s, &s->volumes[ch], current_magnitude[ch], duration);
		tick_peak(s, &s->volumes[ch], current_peak[ch], duration);
	}
}

static uint32_t get_width(void *data)
{
	struct source_s *s = data;
	return DISPLAY_WIDTH_PER_CHANNEL * volmeter_get_nr_channels(s->volmeter);
}

static uint32_t get_height(void *data)
{
	struct source_s *s = data;
	return DISPLAY_HEIGHT_PER_DB * (uint32_t)-s->magnitude_min;
}

static void video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct source_s *s = data;

	const uint32_t width = DISPLAY_WIDTH_PER_CHANNEL;
	const uint32_t height = get_height(s);

	gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "mag_min"), s->magnitude_min);

	const bool srgb_prev = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_reset_blend_state();

	const uint32_t channels = volmeter_get_nr_channels(s->volmeter);
	for (uint32_t ch = 0; ch < channels; ch++) {
		struct channel_volume_s *v = s->volumes + ch;

		gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "mag"), v->display_magnitude);
		gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "peak"), v->display_peak);
		gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "peak_hold"), v->peak_hold);

		gs_matrix_push();

		struct matrix4 tr = {
			{.ptr = {1.0f, 0.0f, 0.0f, 0.0f}},
			{.ptr = {0.0f, 1.0f, 0.0f, 0.0f}},
			{.ptr = {0.0f, 0.0f, 1.0f, 0.0f}},
			{.ptr = {(float)(width * ch), 0.0f, 0.0f, 1.0f}},
		};
		gs_matrix_mul(&tr);

		while (gs_effect_loop(s->effect, "DrawVolMeter"))
			gs_draw_sprite(0, 0, width, height);

		gs_matrix_pop();
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(srgb_prev);
}

static void audio_cb(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct source_s *s = param;

	audio_t *audio = obs_get_audio();
	if (!audio)
		return;

	/* Need to align the audio data */
	struct audio_data ad = *data;

	uint32_t planes = audio_output_get_planes(audio);
	da_resize(s->buffer, sizeof(float) * AUDIO_OUTPUT_FRAMES * planes);

	for (uint32_t i = 0; i < planes; i++) {
		ad.data[i] = s->buffer.array + sizeof(float) * AUDIO_OUTPUT_FRAMES * i;
		memcpy(ad.data[i], data->data[i], sizeof(float) * AUDIO_OUTPUT_FRAMES);
	}
	for (uint32_t i = planes; i < MAX_AV_PLANES; i++)
		ad.data[i] = NULL;

	volmeter_push_audio_data(s->volmeter, &ad);
}

static void volume_cb(void *param, const float magnitude[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
		      const float input_peak[MAX_AUDIO_CHANNELS])
{
	UNUSED_PARAMETER(input_peak);
	struct source_s *s = param;

	pthread_mutex_lock(&s->mutex);

	memcpy(s->current_magnitude, magnitude, sizeof(float) * MAX_AUDIO_CHANNELS);
	memcpy(s->current_peak, peak, sizeof(float) * MAX_AUDIO_CHANNELS);
	s->current_volume_updated = true;

	pthread_mutex_unlock(&s->mutex);
}

const struct obs_source_info volmeter_source_info = {
	.id = ID_PREFIX "graphical-volmeter-source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
	.update = update,
	.get_properties = get_properties,
	.get_defaults = get_defaults,
	.video_tick = tick,
	.video_render = video_render,
	.get_width = get_width,
	.get_height = get_height,
};
