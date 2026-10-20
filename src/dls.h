/* Downloadable Sounds (DLS level 1, e.g. Windows gm.dls) -> SoundFont 2 conversion. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Returns a malloc'd .sf2 image (free() it), or NULL if dls is not a usable DLS file. */
uint8_t *dls_to_sf2(const uint8_t *dls, size_t len, size_t *out_len);
