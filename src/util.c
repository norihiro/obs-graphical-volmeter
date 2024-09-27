#include <obs-module.h>
#include "plugin-macros.generated.h"
#include "util.h"

gs_effect_t *create_effect_from_module_file(const char *basename)
{
	char *f = obs_module_file(basename);
	gs_effect_t *effect = gs_effect_create_from_file(f, NULL);
	if (!effect)
		blog(LOG_ERROR, "Cannot load '%s'", f);
	bfree(f);
	return effect;
}
