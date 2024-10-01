#include <inttypes.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <graphics/matrix4.h>
#include "plugin-macros.generated.h"
#include "volmeter.h"
#include "global-config.h"
#include "util.h"

#define AGE_THRESHOLD 0.05f      // [s]
#define CLIP_FLASH_DURATION 1.0f // [s]

#define DISPLAY_WIDTH_PER_CHANNEL 16
#define DISPLAY_HEIGHT_PER_DB 8
#define LABEL_IMAGE_WIDTH 48
#define N_LABELS 13

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
	bool clip_flash;
	float clip_flash_age;
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
	bool peak_decay_rate_default;
	enum obs_peak_meter_type peak_meter_type;
	bool peak_meter_type_default;

	// internal data
	// thread: audio
	volmeter_t *volmeter;
	DARRAY(uint8_t) buffer;

	// magnitude and peak values written by audio thread
	pthread_mutex_t mutex;
	float current_magnitude[MAX_AUDIO_CHANNELS];
	float current_peak[MAX_AUDIO_CHANNELS];

	// last updated time information
	bool current_volume_updated;
	float current_volume_age;

	// internal data
	// thread: graphics
	struct channel_volume_s volumes[MAX_AUDIO_CHANNELS];
	gs_vertbuffer_t *label_vbuf;
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
	obs_property_t *prop;

	obs_properties_add_int(props, "track", obs_module_text("Prop.Track"), 0, MAX_AUDIO_MIXES - 1, 1);

	prop = obs_properties_add_list(props, "peak_decay_rate", obs_module_text("Prop.PeakDecayRate"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
	obs_property_list_add_float(prop, obs_module_text("Prop.PeakDecayRate.Default"), 0.0);
	obs_property_list_add_float(prop, obs_module_text("Prop.PeakDecayRate.Fast"), 20.0 / 0.85); // [dB/s]
	obs_property_list_add_float(prop, obs_module_text("Prop.PeakDecayRate.Medium"), 20.0 / 1.7);
	obs_property_list_add_float(prop, obs_module_text("Prop.PeakDecayRate.Slow"), 20.0 / 2.333);

	prop = obs_properties_add_list(props, "peak_meter_type", obs_module_text("Prop.PeakMeterType"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Prop.PeakMeterType.Default"), -1);
	obs_property_list_add_int(prop, obs_module_text("Prop.PeakMeterType.SamplePeak"), 0);
	obs_property_list_add_int(prop, obs_module_text("Prop.PeakMeterType.TruePeak"), 1);

	return props;
}

static void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "peak_meter_type", -1);
}

static void update_internal(struct source_s *s, obs_data_t *settings)
{
	int track = (int)obs_data_get_int(settings, "track");
	if (track != s->track && 0 <= track && track < MAX_AUDIO_MIXES) {
		obs_remove_raw_audio_callback(s->track, audio_cb, s);
		obs_add_raw_audio_callback(track, NULL, audio_cb, s);
		s->track = track;
	}

	double peak_decay_rate = obs_data_get_double(settings, "peak_decay_rate");
	if (peak_decay_rate <= 0.0) {
		s->peak_decay_rate_default = true;
	}
	else {
		s->peak_decay_rate_default = false;
		s->peak_decay_rate = (float)peak_decay_rate;
	}

	int peak_meter_type = (int)obs_data_get_int(settings, "peak_meter_type");
	if (peak_meter_type == -1) {
		s->peak_meter_type_default = true;
		s->peak_meter_type = gcfg.peak_meter_type;
	}
	else {
		s->peak_meter_type_default = false;
		s->peak_meter_type = peak_meter_type_from_int(peak_meter_type);
	}
	volmeter_set_peak_meter_type(s->volmeter, s->peak_meter_type);
}

static void update(void *data, obs_data_t *settings)
{
	ASSERT_THREAD(OBS_TASK_GRAPHICS);
	update_internal(data, settings);
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	gcfg_inc();

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

	s->magnitude_attack_rate = 0.99f / 0.3f;
	s->magnitude_min = -60.0f;
	s->peak_decay_rate = 20.0f / 0.85f; // [dB/s]
	s->peak_hold_duration = 20.0f;      // [s]

	s->volmeter = volmeter_create();
	if (!s->volmeter)
		goto fail;

	update_internal(s, settings);

	volmeter_add_callback(s->volmeter, volume_cb, s);

	return s;

fail:
	bfree(s);
	return NULL;
}

static void destroy(void *data)
{
	struct source_s *s = data;

	gcfg_dec();

	if (s->label_vbuf) {
		obs_enter_graphics();
		gs_vertexbuffer_destroy(s->label_vbuf);
		obs_leave_graphics();
	}

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
		float peak_decay_rate = s->peak_decay_rate_default ? gcfg.peak_decay_rate : s->peak_decay_rate;
		float decay = duration * peak_decay_rate;
		c->display_peak = clamp_flt(c->display_peak - decay, peak, 0.0f);
	}

	if (peak >= c->peak_hold || !isfinite(c->peak_hold) || c->peak_hold_age > s->peak_hold_duration) {
		c->peak_hold = peak;
		c->peak_hold_age = 0.0f;
	}
	else {
		c->peak_hold_age += duration;
	}

	if (c->clip_flash) {
		if (c->clip_flash_age >= CLIP_FLASH_DURATION)
			c->clip_flash = false;
		else
			c->clip_flash_age += duration;
	}
	if (peak >= 0.0f && !c->clip_flash) {
		c->clip_flash = true;
		c->clip_flash_age = 0.0f;
	}
}

void tick(void *data, float duration)
{
	ASSERT_THREAD(OBS_TASK_GRAPHICS);
	struct source_s *s = data;

	float current_magnitude[MAX_AUDIO_CHANNELS];
	float current_peak[MAX_AUDIO_CHANNELS];

	pthread_mutex_lock(&s->mutex);
	memcpy(current_magnitude, s->current_magnitude, sizeof(float) * MAX_AUDIO_CHANNELS);
	memcpy(current_peak, s->current_peak, sizeof(float) * MAX_AUDIO_CHANNELS);
	bool updated = s->current_volume_updated;
	if (updated) {
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

	if (s->peak_meter_type_default && s->peak_meter_type != gcfg.peak_meter_type) {
		s->peak_meter_type = gcfg.peak_meter_type;
		volmeter_set_peak_meter_type(s->volmeter, s->peak_meter_type);
	}
}

static uint32_t get_width(void *data)
{
	struct source_s *s = data;
	return DISPLAY_WIDTH_PER_CHANNEL * volmeter_get_nr_channels(s->volmeter) + LABEL_IMAGE_WIDTH;
}

static uint32_t get_height(void *data)
{
	struct source_s *s = data;
	return DISPLAY_HEIGHT_PER_DB * (uint32_t)-s->magnitude_min;
}

static gs_vertbuffer_t *create_vbuf(uint32_t n)
{
	struct gs_vb_data *vrect = gs_vbdata_create();
	vrect->num = n;
	vrect->points = bzalloc(sizeof(struct vec3) * n);
	vrect->num_tex = 1;
	vrect->tvarray = bzalloc(sizeof(struct gs_tvertarray));
	vrect->tvarray[0].width = 2;
	vrect->tvarray[0].array = bzalloc(sizeof(struct vec2) * n);

	return gs_vertexbuffer_create(vrect, GS_DYNAMIC);
}

static inline void set_v3_rect(struct vec3 *a, float x, float y, float w, float h)
{
	vec3_set(a + 0, x, y, 0.0f);
	vec3_set(a + 1, x + w, y, 0.0f);
	vec3_set(a + 2, x, y + h, 0.0f);
	vec3_set(a + 3, x, y + h, 0.0f);
	vec3_set(a + 4, x + w, y, 0.0f);
	vec3_set(a + 5, x + w, y + h, 0.0f);
}

static inline void set_v2_uv(struct vec2 *a, float u, float v, float u2, float v2)
{
	vec2_set(a + 0, u, v);
	vec2_set(a + 1, u2, v);
	vec2_set(a + 2, u, v2);
	vec2_set(a + 3, u, v2);
	vec2_set(a + 4, u2, v);
	vec2_set(a + 5, u2, v2);
}

static void draw_vbuf(gs_texture_t *tex, gs_vertbuffer_t *vbuf, uint32_t vbuf_size)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

	gs_vertexbuffer_flush(vbuf);
	gs_load_vertexbuffer(vbuf);
	gs_load_indexbuffer(NULL);

	size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		if (gs_technique_begin_pass(tech, i)) {
			gs_effect_set_texture(image, tex);
			gs_draw(GS_TRIS, 0, vbuf_size);
			gs_technique_end_pass(tech);
		}
	}
	gs_technique_end(tech);
}

static inline void render_labels(struct source_s *s, uint32_t height)
{
	if (!label_image.texture)
		return;

	const uint32_t vbuf_size = N_LABELS * 6;
	if (!s->label_vbuf) {
		s->label_vbuf = create_vbuf(vbuf_size);
		if (!s->label_vbuf) {
			blog(LOG_ERROR, "Failed to create vbuf");
			return;
		}
	}

	struct gs_vb_data *vdata = gs_vertexbuffer_get_data(s->label_vbuf);
	struct vec2 *tvarray = vdata->tvarray[0].array;

	int label_cx = label_image.cx;
	int label_cy = label_image.cy / N_LABELS;
	if (label_cx != LABEL_IMAGE_WIDTH)
		blog(LOG_WARNING, "Expected label image width %d, got %d", LABEL_IMAGE_WIDTH, label_cx);

	for (int i = 0; i < N_LABELS; i++) {
		float x = 0.0f;
		float y = height * i / (float)(N_LABELS - 1) - label_cy * 0.5f;

		set_v3_rect(vdata->points + i * 6, x, y, label_cx, label_cy);
		set_v2_uv(tvarray + i * 6, 0.0f, i / (float)N_LABELS, 1.f, (i + 1) / (float)N_LABELS);
	}

	draw_vbuf(label_image.texture, s->label_vbuf, vbuf_size);
}

static void video_render(void *data, gs_effect_t *effect)
{
	ASSERT_GRAPHICS_CONTEXT();
	UNUSED_PARAMETER(effect);
	struct source_s *s = data;

	if (!s->effect)
		return;

	const uint32_t width = DISPLAY_WIDTH_PER_CHANNEL;
	const uint32_t height = get_height(s);

	const bool srgb_prev = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_reset_blend_state();

	const uint32_t channels = volmeter_get_nr_channels(s->volmeter);
	for (uint32_t ch = 0; ch < channels; ch++) {
		struct channel_volume_s *v = s->volumes + ch;

		gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "mag_min"), s->magnitude_min);

		switch (s->peak_meter_type) {
		case TRUE_PEAK_METER:
			gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "warning"), -13.0f);
			gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "error"), -2.0f);
			break;
		case SAMPLE_PEAK_METER:
		default:
			gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "warning"), -20.0f);
			gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "error"), -9.0f);
			break;
		}

		if (gcfg.override_colors) {
			gs_effect_set_color(gs_effect_get_param_by_name(s->effect, "color_bg_nominal"),
					    gcfg.color_bg_nominal);
			gs_effect_set_color(gs_effect_get_param_by_name(s->effect, "color_bg_warning"),
					    gcfg.color_bg_warning);
			gs_effect_set_color(gs_effect_get_param_by_name(s->effect, "color_bg_error"),
					    gcfg.color_bg_error);
			gs_effect_set_color(gs_effect_get_param_by_name(s->effect, "color_fg_nominal"),
					    gcfg.color_fg_nominal);
			gs_effect_set_color(gs_effect_get_param_by_name(s->effect, "color_fg_warning"),
					    gcfg.color_fg_warning);
			gs_effect_set_color(gs_effect_get_param_by_name(s->effect, "color_fg_error"),
					    gcfg.color_fg_error);
		}

		gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "mag"), v->display_magnitude);
		gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "peak"),
				    v->clip_flash ? 0.0f : v->display_peak);
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

	{
		gs_matrix_push();

		struct matrix4 tr = {
			{.ptr = {1.0f, 0.0f, 0.0f, 0.0f}},
			{.ptr = {0.0f, 1.0f, 0.0f, 0.0f}},
			{.ptr = {0.0f, 0.0f, 1.0f, 0.0f}},
			{.ptr = {(float)(width * channels), 0.0f, 0.0f, 1.0f}},
		};
		gs_matrix_mul(&tr);

		render_labels(s, height);

		gs_matrix_pop();
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(srgb_prev);
}

static void audio_cb(void *param, size_t mix_idx, struct audio_data *data)
{
	ASSERT_THREAD(OBS_TASK_AUDIO);
	UNUSED_PARAMETER(mix_idx);
	struct source_s *s = param;

	audio_t *audio = obs_get_audio();
	if (!audio)
		return;

	/* Need to align the audio data */
	struct audio_data ad = *data;

	uint32_t planes = (uint32_t)audio_output_get_planes(audio);
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
	ASSERT_THREAD(OBS_TASK_AUDIO);
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
