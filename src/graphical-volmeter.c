#include <inttypes.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include "plugin-macros.generated.h"
#include "volmeter.h"

struct source_s
{
	obs_source_t *context;

	// properties
	int track;

	// internal data
	// thread: audio
	volmeter_t *volmeter;
	DARRAY(uint8_t) buffer;
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
	if (track != s->track) {
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

	volmeter_remove_callback(s->volmeter, volume_cb, s);
	volmeter_destroy(s->volmeter);

	if (s->track >= 0)
		obs_remove_raw_audio_callback(s->track, audio_cb, s);

	da_free(s->buffer);

	bfree(s);
}

void tick(void *data, float unused)
{
	UNUSED_PARAMETER(unused);
	struct source_s *s = data;
	(void)s; // TODO: Implement
}

static void video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct source_s *s = data;
	(void)s; // TODO: Implement
}

static uint32_t get_width(void *data)
{
	struct source_s *s = data;
	(void)s; // TODO: Implement
	return 1;
}

static uint32_t get_height(void *data)
{
	struct source_s *s = data;
	(void)s; // TODO: Implement
	return 1;
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
	(void)param, (void)peak; // TODO: Implement
	blog(LOG_INFO, "magnitude: %f %f", magnitude[0], magnitude[1]);
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
