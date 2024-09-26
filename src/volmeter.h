/*
Copyright (C) 2014 by Leonhard Oelke <leonhard@in-verted.de>
Modified by Norihiro Kamae <norihiro@nagater.net>
- Copied from obs-studio/libobs/obs-audio-controls.h
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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct volmeter_s volmeter_t;

volmeter_t *volmeter_create();
void volmeter_destroy(volmeter_t *volmeter);
void volmeter_set_peak_meter_type(volmeter_t *volmeter, enum obs_peak_meter_type peak_meter_type);
uint32_t volmeter_get_nr_channels(volmeter_t *volmeter);
void volmeter_add_callback(volmeter_t *volmeter, obs_volmeter_updated_t callback, void *param);
void volmeter_remove_callback(volmeter_t *volmeter, obs_volmeter_updated_t callback, void *param);
void volmeter_push_audio_data(volmeter_t *volmeter, const struct audio_data *data);

#ifdef __cplusplus
}
#endif
