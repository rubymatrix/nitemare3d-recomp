/* Model of the Windows 3.1 "Ad Lib" MIDI driver (MSADLIB.DRV), the FM synth
 * Windows used for AdLib and Sound Blaster cards, playing the instrument bank
 * and drum kit loaded from the user's own MSADLIB.DRV (or an AdLib .BNK). */
#pragma once
#include <stdint.h>

/* Loads the patches. path: MSADLIB.DRV (bank + drum kit resources) or a
 * .BNK file (bank only; drums then play their own key). Returns 1 on success. */
int msadlib_load(const char *path);
/* Resets the chip the way the driver initialises it; out() writes one OPL2 register. */
void msadlib_reset(void (*out)(uint8_t reg, uint8_t val));
/* One MIDI channel message as the driver receives it (drums on channel 16). */
void msadlib_midi(uint8_t status, uint8_t a, uint8_t b);
void msadlib_all_notes_off(void);
