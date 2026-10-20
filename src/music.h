/* Music: plays the game's MIDI tunes (from SND.DAT) through a software synth,
 * or pre-rendered tuneNN.ogg / tuneNN.wav files from a music folder. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* FM synth: Sound Blaster model whose FM chip and output filter to emulate,
 * "sb1", "sb2", "sbpro1", "sbpro2" or "sb16" (default). Returns 0 if unknown. */
int music_set_sb_model(const char *name);
/* FM synth: play like the Windows 3.1 Ad Lib driver, with the patches from
 * the user's MSADLIB.DRV (or an AdLib .BNK). Call before music_init. Returns 0 on failure. */
int music_set_fm_driver(const char *path);
/* soundfont: path to a General MIDI .sf2 or .dls, or NULL for the OPL2 FM synth.
 * Also selects the default music folder: <data_dir>/music, else ./music. */
void music_init(const char *data_dir, const char *soundfont);
/* Folder with pre-rendered tuneNN.ogg / tuneNN.wav (NULL: synth only). */
void music_set_dir(const char *dir);
/* Optional, outside the audio lock: decodes the pre-rendered file for this
 * MIDI ahead of music_load_midi so the audio thread is not stalled. */
void music_prefetch(const uint8_t *data, size_t len);
void music_load_midi(const uint8_t *data, size_t len); /* starts playback, loops */
void music_stop(void);
void music_set_volume(int vol); /* 0..65535 */
/* Mix (add) interleaved stereo s16 frames into out. Called from the audio thread. */
void music_render(int16_t *out, int frames, int rate);

/* --- for the offline renderer (tools/render_music.c) --- */
#define MUSIC_MAX_TUNES 16
/* Tune i (1..15) as stored in SND.DAT, or NULL. */
const uint8_t *music_tune(int i, size_t *len);
/* 0: play the song once; when it ends all notes are released and
 * music_song_end() returns the frame (counted from music_load_midi) it ended at. */
void music_set_looping(int loop);
long music_song_end(void); /* -1 while still playing */
int music_using_soundfont(void); /* 0: OPL2 FM synth */
void music_set_sf_gain(float gain); /* SoundFont output gain (default 1) */
float music_take_peak(void); /* SoundFont peak since the last call, 1.0 = full scale; resets it */
/* Called between slices of decoding a pre-rendered tune (host: keep audio fed). */
extern void (*music_decode_yield)(void);
