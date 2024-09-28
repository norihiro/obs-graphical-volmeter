/*
Copyright (C) 2014 by Leonhard Oelke <leonhard@in-verted.de>
Modified by Norihiro Kamae <norihiro@nagater.net>
- Copied from obs-studio/libobs/obs-audio-controls.c
- Modified `obs_volmeter_*` to `volmeter_*`

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <math.h>

#include <util/sse-intrin.h>

#include <util/threading.h>
#include <util/bmem.h>
#include <media-io/audio-math.h>
#include <obs.h>

#include <obs-audio-controls.h>
#include "plugin-macros.generated.h"
#include "volmeter.h"

static inline bool obs_object_valid(const void *obj, const char *f, const char *t)
{
	if (!obj) {
		blog(LOG_DEBUG, "%s: Null '%s' parameter", f, t);
		return false;
	}

	return true;
}
#define obs_ptr_valid(ptr, func) obs_object_valid(ptr, func, #ptr)

/* These are pointless warnings generated not by our code, but by a standard
 * library macro, INFINITY */
#ifdef _MSC_VER
#pragma warning(disable : 4056)
#pragma warning(disable : 4756)
#endif

struct meter_cb
{
	obs_volmeter_updated_t callback;
	void *param;
};

struct volmeter_s
{
	pthread_mutex_t mutex;

	pthread_mutex_t callback_mutex;
	DARRAY(struct meter_cb) callbacks;

	enum obs_peak_meter_type peak_meter_type;
	unsigned int update_ms;
	float prev_samples[MAX_AUDIO_CHANNELS][4];

	float magnitude[MAX_AUDIO_CHANNELS];
	float peak[MAX_AUDIO_CHANNELS];
};

static void signal_levels_updated(volmeter_t *volmeter, const float magnitude[MAX_AUDIO_CHANNELS],
				  const float peak[MAX_AUDIO_CHANNELS], const float input_peak[MAX_AUDIO_CHANNELS])
{
	pthread_mutex_lock(&volmeter->callback_mutex);
	for (size_t i = volmeter->callbacks.num; i > 0; i--) {
		struct meter_cb cb = volmeter->callbacks.array[i - 1];
		cb.callback(cb.param, magnitude, peak, input_peak);
	}
	pthread_mutex_unlock(&volmeter->callback_mutex);
}

static int get_nr_channels_from_audio_data(const struct audio_data *data)
{
	int nr_channels = 0;
	for (int i = 0; i < MAX_AV_PLANES; i++) {
		if (data->data[i])
			nr_channels++;
	}
	if (nr_channels >= MAX_AUDIO_CHANNELS)
		return MAX_AUDIO_CHANNELS;
	return nr_channels;
}

/* msb(h, g, f, e) lsb(d, c, b, a)   -->  msb(h, h, g, f) lsb(e, d, c, b)
 */
#define SHIFT_RIGHT_2PS(msb, lsb)                                               \
	{                                                                       \
		__m128 tmp = _mm_shuffle_ps(lsb, msb, _MM_SHUFFLE(0, 0, 3, 3)); \
		lsb = _mm_shuffle_ps(lsb, tmp, _MM_SHUFFLE(2, 1, 2, 1));        \
		msb = _mm_shuffle_ps(msb, msb, _MM_SHUFFLE(3, 3, 2, 1));        \
	}

/* x(d, c, b, a) --> (|d|, |c|, |b|, |a|)
 */
#define abs_ps(v) _mm_andnot_ps(_mm_set1_ps(-0.f), v)

/* Take cross product of a vector with a matrix resulting in vector.
 */
#define VECTOR_MATRIX_CROSS_PS(out, v, m0, m1, m2, m3)    \
	{                                                 \
		out = _mm_mul_ps(v, m0);                  \
		__m128 mul1 = _mm_mul_ps(v, m1);          \
		__m128 mul2 = _mm_mul_ps(v, m2);          \
		__m128 mul3 = _mm_mul_ps(v, m3);          \
                                                          \
		_MM_TRANSPOSE4_PS(out, mul1, mul2, mul3); \
                                                          \
		out = _mm_add_ps(out, mul1);              \
		out = _mm_add_ps(out, mul2);              \
		out = _mm_add_ps(out, mul3);              \
	}

/* x4(d, c, b, a)  -->  max(a, b, c, d)
 */
#define hmax_ps(r, x4)                     \
	do {                               \
		float x4_mem[4];           \
		_mm_storeu_ps(x4_mem, x4); \
		r = x4_mem[0];             \
		r = fmaxf(r, x4_mem[1]);   \
		r = fmaxf(r, x4_mem[2]);   \
		r = fmaxf(r, x4_mem[3]);   \
	} while (false)

/* Calculate the true peak over a set of samples.
 * The algorithm implements 5x oversampling by using Whittaker-Shannon
 * interpolation over four samples.
 *
 * The four samples have location t=-1.5, -0.5, +0.5, +1.5
 * The oversamples are taken at locations t=-0.3, -0.1, +0.1, +0.3
 *
 * @param previous_samples  Last 4 samples from the previous iteration.
 * @param samples           The samples to find the peak in.
 * @param nr_samples        Number of sets of 4 samples.
 * @returns 5 times oversampled true-peak from the set of samples.
 */
static float get_true_peak(__m128 previous_samples, const float *samples, size_t nr_samples)
{
	/* These are normalized-sinc parameters for interpolating over sample
	 * points which are located at x-coords: -1.5, -0.5, +0.5, +1.5.
	 * And oversample points at x-coords: -0.3, -0.1, 0.1, 0.3. */
	const __m128 m3 = _mm_set_ps(-0.155915f, 0.935489f, 0.233872f, -0.103943f);
	const __m128 m1 = _mm_set_ps(-0.216236f, 0.756827f, 0.504551f, -0.189207f);
	const __m128 p1 = _mm_set_ps(-0.189207f, 0.504551f, 0.756827f, -0.216236f);
	const __m128 p3 = _mm_set_ps(-0.103943f, 0.233872f, 0.935489f, -0.155915f);

	__m128 work = previous_samples;
	__m128 peak = previous_samples;
	for (size_t i = 0; (i + 3) < nr_samples; i += 4) {
		__m128 new_work = _mm_load_ps(&samples[i]);
		__m128 intrp_samples;

		/* Include the actual sample values in the peak. */
		__m128 abs_new_work = abs_ps(new_work);
		peak = _mm_max_ps(peak, abs_new_work);

		/* Shift in the next point. */
		SHIFT_RIGHT_2PS(new_work, work);
		VECTOR_MATRIX_CROSS_PS(intrp_samples, work, m3, m1, p1, p3);
		peak = _mm_max_ps(peak, abs_ps(intrp_samples));

		SHIFT_RIGHT_2PS(new_work, work);
		VECTOR_MATRIX_CROSS_PS(intrp_samples, work, m3, m1, p1, p3);
		peak = _mm_max_ps(peak, abs_ps(intrp_samples));

		SHIFT_RIGHT_2PS(new_work, work);
		VECTOR_MATRIX_CROSS_PS(intrp_samples, work, m3, m1, p1, p3);
		peak = _mm_max_ps(peak, abs_ps(intrp_samples));

		SHIFT_RIGHT_2PS(new_work, work);
		VECTOR_MATRIX_CROSS_PS(intrp_samples, work, m3, m1, p1, p3);
		peak = _mm_max_ps(peak, abs_ps(intrp_samples));
	}

	float r;
	hmax_ps(r, peak);
	return r;
}

/* points contain the first four samples to calculate the sinc interpolation
 * over. They will have come from a previous iteration.
 */
static float get_sample_peak(__m128 previous_samples, const float *samples, size_t nr_samples)
{
	__m128 peak = previous_samples;
	for (size_t i = 0; (i + 3) < nr_samples; i += 4) {
		__m128 new_work = _mm_load_ps(&samples[i]);
		peak = _mm_max_ps(peak, abs_ps(new_work));
	}

	float r;
	hmax_ps(r, peak);
	return r;
}

static void volmeter_process_peak_last_samples(volmeter_t *volmeter, int channel_nr, float *samples, size_t nr_samples)
{
	/* Take the last 4 samples that need to be used for the next peak
	 * calculation. If there are less than 4 samples in total the new
	 * samples shift out the old samples. */

	switch (nr_samples) {
	case 0:
		break;
	case 1:
		volmeter->prev_samples[channel_nr][0] = volmeter->prev_samples[channel_nr][1];
		volmeter->prev_samples[channel_nr][1] = volmeter->prev_samples[channel_nr][2];
		volmeter->prev_samples[channel_nr][2] = volmeter->prev_samples[channel_nr][3];
		volmeter->prev_samples[channel_nr][3] = samples[nr_samples - 1];
		break;
	case 2:
		volmeter->prev_samples[channel_nr][0] = volmeter->prev_samples[channel_nr][2];
		volmeter->prev_samples[channel_nr][1] = volmeter->prev_samples[channel_nr][3];
		volmeter->prev_samples[channel_nr][2] = samples[nr_samples - 2];
		volmeter->prev_samples[channel_nr][3] = samples[nr_samples - 1];
		break;
	case 3:
		volmeter->prev_samples[channel_nr][0] = volmeter->prev_samples[channel_nr][3];
		volmeter->prev_samples[channel_nr][1] = samples[nr_samples - 3];
		volmeter->prev_samples[channel_nr][2] = samples[nr_samples - 2];
		volmeter->prev_samples[channel_nr][3] = samples[nr_samples - 1];
		break;
	default:
		volmeter->prev_samples[channel_nr][0] = samples[nr_samples - 4];
		volmeter->prev_samples[channel_nr][1] = samples[nr_samples - 3];
		volmeter->prev_samples[channel_nr][2] = samples[nr_samples - 2];
		volmeter->prev_samples[channel_nr][3] = samples[nr_samples - 1];
	}
}

static void volmeter_process_peak(volmeter_t *volmeter, const struct audio_data *data, int nr_channels)
{
	int nr_samples = data->frames;
	int channel_nr = 0;
	for (int plane_nr = 0; channel_nr < nr_channels; plane_nr++) {
		float *samples = (float *)data->data[plane_nr];
		if (!samples) {
			continue;
		}
		if (((uintptr_t)samples & 0xf) > 0) {
			blog(LOG_WARNING, "Audio plane %i is not aligned %p skipping peak volume measurement.",
			     plane_nr, samples);
			volmeter->peak[channel_nr] = 1.0;
			channel_nr++;
			continue;
		}

		/* volmeter->prev_samples may not be aligned to 16 bytes;
		 * use unaligned load. */
		__m128 previous_samples = _mm_loadu_ps(volmeter->prev_samples[channel_nr]);

		float peak;
		switch (volmeter->peak_meter_type) {
		case TRUE_PEAK_METER:
			peak = get_true_peak(previous_samples, samples, nr_samples);
			break;

		case SAMPLE_PEAK_METER:
		default:
			peak = get_sample_peak(previous_samples, samples, nr_samples);
			break;
		}

		volmeter_process_peak_last_samples(volmeter, channel_nr, samples, nr_samples);

		volmeter->peak[channel_nr] = peak;

		channel_nr++;
	}

	/* Clear the peak of the channels that have not been handled. */
	for (; channel_nr < MAX_AUDIO_CHANNELS; channel_nr++) {
		volmeter->peak[channel_nr] = 0.0;
	}
}

static void volmeter_process_magnitude(volmeter_t *volmeter, const struct audio_data *data, int nr_channels)
{
	size_t nr_samples = data->frames;

	int channel_nr = 0;
	for (int plane_nr = 0; channel_nr < nr_channels; plane_nr++) {
		float *samples = (float *)data->data[plane_nr];
		if (!samples) {
			continue;
		}

		float sum = 0.0;
		for (size_t i = 0; i < nr_samples; i++) {
			float sample = samples[i];
			sum += sample * sample;
		}
		volmeter->magnitude[channel_nr] = sqrtf(sum / nr_samples);

		channel_nr++;
	}
}

static void volmeter_process_audio_data(volmeter_t *volmeter, const struct audio_data *data)
{
	int nr_channels = get_nr_channels_from_audio_data(data);

	volmeter_process_peak(volmeter, data, nr_channels);
	volmeter_process_magnitude(volmeter, data, nr_channels);
}

void volmeter_push_audio_data(volmeter_t *volmeter, const struct audio_data *data)
{
	float magnitude[MAX_AUDIO_CHANNELS];
	float peak[MAX_AUDIO_CHANNELS];

	pthread_mutex_lock(&volmeter->mutex);

	volmeter_process_audio_data(volmeter, data);

	for (int channel_nr = 0; channel_nr < MAX_AUDIO_CHANNELS; channel_nr++) {
		magnitude[channel_nr] = mul_to_db(volmeter->magnitude[channel_nr]);
		peak[channel_nr] = mul_to_db(volmeter->peak[channel_nr]);
	}

	pthread_mutex_unlock(&volmeter->mutex);

	signal_levels_updated(volmeter, magnitude, peak, peak);
}

volmeter_t *volmeter_create()
{
	volmeter_t *volmeter = bzalloc(sizeof(volmeter_t));
	if (!volmeter)
		return NULL;

	if (pthread_mutex_init(&volmeter->mutex, NULL) != 0)
		goto fail1;
	if (pthread_mutex_init(&volmeter->callback_mutex, NULL) != 0)
		goto fail2;

	return volmeter;

fail2:
	pthread_mutex_destroy(&volmeter->mutex);
fail1:
	bfree(volmeter);
	return NULL;
}

void volmeter_destroy(volmeter_t *volmeter)
{
	if (!volmeter)
		return;

	da_free(volmeter->callbacks);
	pthread_mutex_destroy(&volmeter->callback_mutex);
	pthread_mutex_destroy(&volmeter->mutex);

	bfree(volmeter);
}

void volmeter_set_peak_meter_type(volmeter_t *volmeter, enum obs_peak_meter_type peak_meter_type)
{
	pthread_mutex_lock(&volmeter->mutex);
	volmeter->peak_meter_type = peak_meter_type;
	pthread_mutex_unlock(&volmeter->mutex);
}

uint32_t volmeter_get_nr_channels(volmeter_t *volmeter)
{
	UNUSED_PARAMETER(volmeter);

	struct obs_audio_info audio_info;
	if (obs_get_audio_info(&audio_info)) {
		return get_audio_channels(audio_info.speakers);
	}
	else {
		return 2;
	}
}

void volmeter_add_callback(volmeter_t *volmeter, obs_volmeter_updated_t callback, void *param)
{
	struct meter_cb cb = {callback, param};

	if (!obs_ptr_valid(volmeter, "volmeter_add_callback"))
		return;

	pthread_mutex_lock(&volmeter->callback_mutex);
	da_push_back(volmeter->callbacks, &cb);
	pthread_mutex_unlock(&volmeter->callback_mutex);
}

void volmeter_remove_callback(volmeter_t *volmeter, obs_volmeter_updated_t callback, void *param)
{
	struct meter_cb cb = {callback, param};

	if (!obs_ptr_valid(volmeter, "volmeter_remove_callback"))
		return;

	pthread_mutex_lock(&volmeter->callback_mutex);
	da_erase_item(volmeter->callbacks, &cb);
	pthread_mutex_unlock(&volmeter->callback_mutex);
}
