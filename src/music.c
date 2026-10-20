/*
 * Music for the recompiled Nitemare-3D.
 *
 * The Windows game handed its Standard MIDI Files (stored in SND.DAT) to the
 * MCI sequencer. Here a small MIDI sequencer drives two emulated OPL2 chips
 * (18 FM voices) using the General MIDI instrument bank (IBK) that ships in
 * SND.DAT, which gives the AdLib sound of the era without external files.
 *
 * If a General MIDI SoundFont (.sf2) is available the same events go to
 * TinySoundFont instead, approximating a wavetable card / GM synth, which is
 * what the Windows version sent its MIDI to. A Downloadable Sounds bank
 * (.dls, e.g. the Windows gm.dls) is converted to a SoundFont in memory.
 *
 * Pre-rendered tuneNN.ogg / tuneNN.wav files (tools/render_music.c) in the
 * music folder replace the synth for the tunes they cover.
 */
#include "music.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _MSC_VER
#include <strings.h>
#endif

#include "dls.h"
#include "msadlib.h"
#include "opl3.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#define TSF_IMPLEMENTATION
#include "tsf.h"

#define NVOICES 18 /* most voices of any model (OPL3) */
#define FM_GAIN 1.0

/* ------------------------------------------------------------- instrument bank */
typedef struct {
    uint8_t mod_char, car_char, mod_scale, car_scale, mod_ad, car_ad, mod_sr, car_sr, mod_wave, car_wave, fb_conn;
    int8_t transpose;
} patch_t;

static patch_t bank[128];
static int bank_loaded;

static uint8_t *tunes[MUSIC_MAX_TUNES]; /* SND.DAT entries 1..15: Standard MIDI Files */
static size_t tune_len[MUSIC_MAX_TUNES];

static void load_bank(const char *data_dir) {
    char path[1200];
    FILE *f = NULL;
    const char *names[] = {"SND.DAT", "snd.dat", "Snd.dat"};
    for (int i = 0; i < 3 && !f; i++) {
        snprintf(path, sizeof path, "%s/%s", data_dir, names[i]);
        f = fopen(path, "rb");
    }
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *dat = fsize > 6 ? malloc((size_t)fsize) : NULL;
    if (!dat || fread(dat, 1, (size_t)fsize, f) != (size_t)fsize) { free(dat); fclose(f); return; }
    fclose(f);
    /* entry table: u16 length + u32 offset each; the table ends where the data starts */
    uint32_t first = dat[2] | dat[3] << 8 | (uint32_t)dat[4] << 16 | (uint32_t)dat[5] << 24;
    int nent = first <= (uint32_t)fsize ? (int)(first / 6) : 0;
#define ENT_OFF(i) (dat[(i) * 6 + 2] | dat[(i) * 6 + 3] << 8 | (uint32_t)dat[(i) * 6 + 4] << 16 | (uint32_t)dat[(i) * 6 + 5] << 24)
    for (int i = 0; i < nent && i < MUSIC_MAX_TUNES; i++) {
        uint32_t off = ENT_OFF(i), len = dat[i * 6] | dat[i * 6 + 1] << 8;
        /* u16 lengths can overflow: the entry really ends at the next larger offset */
        uint32_t end = (uint32_t)fsize;
        for (int j = 0; j < nent; j++)
            if (ENT_OFF(j) > off && ENT_OFF(j) < end) end = ENT_OFF(j);
        if (off >= (uint32_t)fsize || end <= off) continue;
        if (i == 0) {
            if (end - off >= 4 + 128 * 16 && !memcmp(dat + off, "IBK\x1a", 4)) {
                for (int k = 0; k < 128; k++) {
                    const uint8_t *p = dat + off + 4 + k * 16;
                    bank[k] = (patch_t){p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], (int8_t)p[12]};
                }
                bank_loaded = 1;
            }
        } else if (end - off >= 14 && !memcmp(dat + off, "MThd", 4)) {
            if (len < 14 || len > end - off) len = end - off;
            tunes[i] = malloc(len);
            memcpy(tunes[i], dat + off, len);
            tune_len[i] = len;
        }
    }
#undef ENT_OFF
    free(dat);
}

/* Which SND.DAT tune is this MIDI file? (the game copies the entry to a temp file) */
static int tune_index(const uint8_t *data, size_t len) {
    for (int i = 1; i < MUSIC_MAX_TUNES; i++)
        if (tunes[i] && tune_len[i] == len && !memcmp(tunes[i], data, len)) return i;
    /* tolerate padding/truncation of the temp file */
    for (int i = 1; i < MUSIC_MAX_TUNES; i++) {
        size_t n = len < tune_len[i] ? len : tune_len[i];
        if (tunes[i] && n >= 256 && !memcmp(tunes[i], data, n)) return i;
    }
    return 0;
}

const uint8_t *music_tune(int i, size_t *len) {
    if (i < 1 || i >= MUSIC_MAX_TUNES || !tunes[i]) return NULL;
    if (len) *len = tune_len[i];
    return tunes[i];
}

/* GM percussion (channel 10) mapped onto melodic bank instruments. */
typedef struct { uint8_t program, note; } drum_t;
static drum_t drum_map(int note) {
    switch (note) {
    case 35: case 36: return (drum_t){116, 30};                       /* bass drum: Taiko */
    case 37: return (drum_t){115, 72};                                /* side stick: WoodBlok */
    case 38: case 40: return (drum_t){118, 60};                       /* snare: SynDrum */
    case 39: return (drum_t){126, 72};                                /* hand clap: Applause */
    case 41: case 43: case 45: case 47: case 48: case 50:              /* toms */
        return (drum_t){117, (uint8_t)(note + 4)};
    case 42: case 44: case 46: return (drum_t){127, (uint8_t)(note + 40)}; /* hi-hats: GunShot noise */
    case 49: case 52: case 55: case 57: return (drum_t){119, 72};     /* crash: RevCymbl */
    case 51: case 53: case 59: return (drum_t){112, 84};              /* ride: TinklBel */
    case 54: case 56: return (drum_t){113, 80};                       /* tambourine/cowbell: Agogo */
    default: return (drum_t){118, (uint8_t)note};
    }
}

/* ------------------------------------------------------------- OPL voices */
/*
 * The FM chip of a Sound Blaster, emulated with Nuked OPL3 (cycle accurate,
 * as in DOSBox Staging), in the configuration of the chosen card:
 *   sb1, sb2  one OPL2, 9 voices, mono
 *   sbpro1    two OPL2s (left/right), 9 voices, panned hard left/centre/right
 *   sbpro2    OPL3, 18 voices, stereo
 *   sb16      OPL3, 18 voices, stereo (default)
 * followed by the card's analog low-pass filter on the FM output
 * (DOSBox Staging's opl_filter "auto" values: 1st order, 12 kHz on the
 * SB 1.0/2.0, 8 kHz on the SB Pro, none on the SB 16).
 */
enum { SB1, SB2, SBPRO1, SBPRO2, SB16 };
static const struct { const char *name; int nchips, opl3, nvoices; double lpf_hz; } sb_models[] = {
    {"sb1", 1, 0, 9, 12000}, {"sb2", 1, 0, 9, 12000}, {"sbpro1", 2, 0, 9, 8000},
    {"sbpro2", 1, 1, 18, 8000}, {"sb16", 1, 1, 18, 0},
};
static int sb_model = SB16;
static int use_drv; /* FM played by the Windows Ad Lib driver model (msadlib.c) */
static opl3_chip opl[2];
static int opl_ready, nvoices = 18;
static double lpf_a, lpf_b, lpf_x[2], lpf_y[2]; /* 1st order low-pass (bilinear) */
static double dc_r = 0.9993, dc_x[2], dc_y[2];   /* DC-blocking high-pass, ~5 Hz */
static int out_rate;
static const uint8_t op_off[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};

typedef struct {
    int active, channel, note, program, velocity;
    uint32_t age;
    uint8_t block_fnum_hi; /* last value written to B0 (without key-on) */
    uint8_t sides;         /* bit 0 left, bit 1 right */
} voice_t;
static voice_t voices[NVOICES];
static uint32_t voice_clock;

typedef struct { int program, volume, expression, pan, bend, sustain; } chan_t;
static chan_t chans[16];
static uint8_t sustained[NVOICES];

/* register write for voice v: OPL3 voices 9..17 are in the second register
 * bank; on the dual OPL2 every voice exists on both chips */
static void opl_w(int v, int reg, uint8_t val) {
    if (sb_models[sb_model].opl3) OPL3_WriteReg(&opl[0], (uint16_t)(reg | (v >= 9 ? 0x100 : 0)), val);
    else
        for (int c = 0; c < sb_models[sb_model].nchips; c++) OPL3_WriteReg(&opl[c], (uint16_t)reg, val);
}
/* B0 (key-on) write: on the dual OPL2 only the chips of the voice's sides sound */
static void opl_w_key(int v, uint8_t val) {
    int c9 = v % 9;
    if (sb_model != SBPRO1) { opl_w(v, 0xb0 + c9, val); return; }
    for (int c = 0; c < 2; c++)
        OPL3_WriteReg(&opl[c], (uint16_t)(0xb0 + c9), (voices[v].sides >> c & 1) ? val : (uint8_t)(val & ~0x20));
}

static void opl_reset_chips(void) {
    for (int c = 0; c < 2; c++) {
        OPL3_Reset(&opl[c], (uint32_t)out_rate);
        if (sb_models[sb_model].opl3 && !use_drv) OPL3_WriteReg(&opl[c], 0x105, 0x01); /* OPL3 mode */
        else OPL3_WriteReg(&opl[c], 0x01, 0x20);                          /* OPL2 waveform select */
    }
    lpf_x[0] = lpf_x[1] = lpf_y[0] = lpf_y[1] = 0;
    dc_x[0] = dc_x[1] = dc_y[0] = dc_y[1] = 0;
    double fc = sb_models[sb_model].lpf_hz, k = fc > 0 ? tan(3.14159265358979 * fc / out_rate) : 0;
    lpf_a = k / (1 + k);
    lpf_b = (1 - k) / (1 + k);
}

int music_set_fm_driver(const char *path) {
    use_drv = path && *path && msadlib_load(path);
    return use_drv;
}

int music_set_sb_model(const char *name) {
    for (int i = 0; i < (int)(sizeof sb_models / sizeof sb_models[0]); i++)
        if (!strcasecmp(name, sb_models[i].name)) {
            sb_model = i;
            nvoices = sb_models[i].nvoices;
            if (opl_ready) opl_reset_chips();
            return 1;
        }
    return 0;
}

static int total_level(int base_tl, int vel, int ch) {
    /* attenuation in 0.75 dB OPL steps from velocity * volume * expression */
    double g = (vel / 127.0) * (chans[ch].volume / 127.0) * (chans[ch].expression / 127.0);
    int add = g <= 0.0001 ? 63 : (int)(-20.0 * log10(g) / 0.75);
    int tl = (base_tl & 63) + add;
    return tl > 63 ? 63 : tl;
}

static void voice_program(int v, const patch_t *p, int ch, int vel) {
    int c = v % 9, m = op_off[c], k = m + 3;
    opl_w(v, 0x20 + m, p->mod_char);
    opl_w(v, 0x20 + k, p->car_char);
    int conn = p->fb_conn & 1;
    int mod_tl = conn ? total_level(p->mod_scale, vel, ch) : (p->mod_scale & 63);
    opl_w(v, 0x40 + m, (uint8_t)((p->mod_scale & 0xc0) | mod_tl));
    opl_w(v, 0x40 + k, (uint8_t)((p->car_scale & 0xc0) | total_level(p->car_scale, vel, ch)));
    opl_w(v, 0x60 + m, p->mod_ad);
    opl_w(v, 0x60 + k, p->car_ad);
    opl_w(v, 0x80 + m, p->mod_sr);
    opl_w(v, 0x80 + k, p->car_sr);
    int wmask = sb_models[sb_model].opl3 ? 7 : 3;
    opl_w(v, 0xe0 + m, p->mod_wave & wmask);
    opl_w(v, 0xe0 + k, p->car_wave & wmask);
    /* C0 bits 4/5 route an OPL3 voice to the left/right output */
    opl_w(v, 0xc0 + c, (uint8_t)((p->fb_conn & 0x0f) | (sb_models[sb_model].opl3 ? voices[v].sides << 4 : 0)));
}

static void voice_freq(int v, double note, int keyon) {
    double freq = 440.0 * pow(2.0, (note - 69.0) / 12.0);
    int block = 0;
    double fn = freq * (double)(1 << 20) / 49716.0;
    while (fn >= 1024.0 && block < 7) { fn /= 2.0; block++; }
    int fnum = (int)(fn + 0.5);
    if (fnum > 1023) fnum = 1023;
    int c = v % 9;
    voices[v].block_fnum_hi = (uint8_t)((block << 2) | (fnum >> 8));
    opl_w(v, 0xa0 + c, (uint8_t)(fnum & 0xff));
    opl_w_key(v, (uint8_t)(voices[v].block_fnum_hi | (keyon ? 0x20 : 0)));
}

static void voice_off(int v) {
    if (!voices[v].active) return;
    opl_w_key(v, voices[v].block_fnum_hi);
    voices[v].active = 0;
    sustained[v] = 0;
}

static double voice_pitch(int v) {
    const voice_t *vo = &voices[v];
    double n = vo->note;
    if (vo->channel != 9) n += chans[vo->channel].bend / 8192.0 * 2.0;
    return n;
}

static void note_on(int ch, int note, int vel) {
    if (vel == 0) {
        for (int v = 0; v < nvoices; v++)
            if (voices[v].active && voices[v].channel == ch && voices[v].note == note) {
                if (chans[ch].sustain) sustained[v] = 1; else voice_off(v);
            }
        return;
    }
    int program = chans[ch].program, play_note = note;
    if (ch == 9) {
        drum_t d = drum_map(note);
        program = d.program;
        play_note = d.note;
    }
    /* pick a free voice, else the oldest */
    int best = -1;
    for (int v = 0; v < nvoices; v++)
        if (!voices[v].active) { best = v; break; }
    if (best < 0) {
        uint32_t oldest = UINT32_MAX;
        for (int v = 0; v < nvoices; v++)
            if (voices[v].age < oldest) { oldest = voices[v].age; best = v; }
        voice_off(best);
    }
    const patch_t *p = &bank[program & 127];
    /* MIDI pan -> left / both / right, the resolution of the stereo cards */
    int pan = chans[ch].pan;
    uint8_t sides = !sb_models[sb_model].opl3 && sb_model != SBPRO1 ? 3 : pan < 43 ? 1 : pan > 85 ? 2 : 3;
    voices[best] = (voice_t){1, ch, note, program, vel, ++voice_clock, 0, sides};
    voice_program(best, p, ch, vel);
    sustained[best] = 0;
    double pitch = (ch == 9 ? play_note : note) + p->transpose;
    if (ch != 9) pitch += chans[ch].bend / 8192.0 * 2.0;
    voice_freq(best, pitch, 1);
}

static void note_off(int ch, int note) { note_on(ch, note, 0); }

static void all_notes_off(void) {
    for (int v = 0; v < nvoices; v++) voice_off(v);
    if (use_drv) msadlib_all_notes_off();
}

/* The Windows Ad Lib driver writes port 388h: every chip of the card gets
 * it, and an OPL3 stays in its OPL2-compatible mode. */
static void drv_out(uint8_t reg, uint8_t val) {
    for (int c = 0; c < sb_models[sb_model].nchips; c++) OPL3_WriteReg(&opl[c], reg, val);
}

/* ------------------------------------------------------------- MIDI sequencer */
typedef struct { const uint8_t *p, *end; uint32_t next_tick; uint8_t status; int done; } track_t;

static tsf *sf; /* non-NULL: SoundFont backend */
static void sf_reset_channels(void);

static uint8_t *song;
static size_t song_len;
static track_t tracks[32];
static int ntracks, division = 96;
static double samples_per_tick, sample_acc;
static uint32_t cur_tick;
static int playing;
static int volume = 65535;
static int looping = 1;
static long song_end = -1;
static uint64_t frame_pos; /* frames rendered since music_load_midi */
static float sf_gain = 1.0f, peak; /* SoundFont gain; peak level before clipping (1.0 = full scale) */

static uint32_t read_vlq(const uint8_t **pp, const uint8_t *end) {
    uint32_t v = 0;
    while (*pp < end) {
        uint8_t b = *(*pp)++;
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return v;
}

static void set_tempo(uint32_t us_per_quarter) {
    samples_per_tick = (double)out_rate * us_per_quarter / 1e6 / division;
}

static void rewind_song(void) {
    const uint8_t *p = song + 14;
    const uint8_t *end = song + song_len;
    ntracks = 0;
    while (p + 8 <= end && ntracks < 32) {
        uint32_t len = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
        if (!memcmp(p, "MTrk", 4)) {
            track_t *t = &tracks[ntracks++];
            t->p = p + 8;
            t->end = p + 8 + len > end ? end : p + 8 + len;
            t->status = 0;
            t->done = 0;
            t->next_tick = read_vlq(&t->p, t->end);
        }
        p += 8 + len;
    }
    cur_tick = 0;
    sample_acc = 0;
    set_tempo(500000);
    for (int c = 0; c < 16; c++) chans[c] = (chan_t){0, 100, 127, 64, 0, 0};
    all_notes_off();
    if (sf) sf_reset_channels();
}


static void sf_reset_channels(void) {
    tsf_note_off_all(sf);
    for (int c = 0; c < 16; c++) {
        tsf_channel_midi_control(sf, c, 121, 0); /* reset controllers, bank select, pitch range */
        tsf_channel_set_presetnumber(sf, c, 0, c == 9);
        tsf_channel_set_volume(sf, c, 100 / 127.0f);
        tsf_channel_set_pan(sf, c, 0.5f);
        tsf_channel_set_pitchwheel(sf, c, 8192);
        tsf_channel_set_sustain(sf, c, 0);
    }
}

/* Route one channel event to the SoundFont synth. Returns 1 if handled. */
static int sf_event(uint8_t st, int a, int b) {
    if (!sf) return 0;
    int ch = st & 15;
    switch (st & 0xf0) {
    case 0x80: tsf_channel_note_off(sf, ch, a); break;
    case 0x90: if (b) tsf_channel_note_on(sf, ch, a, b / 127.0f); else tsf_channel_note_off(sf, ch, a); break;
    case 0xb0: tsf_channel_midi_control(sf, ch, a, b); break;
    case 0xc0: tsf_channel_set_presetnumber(sf, ch, a, ch == 9); break;
    case 0xe0: tsf_channel_set_pitchwheel(sf, ch, (b << 7) | a); break;
    }
    return 1;
}

static void track_event(track_t *t) {
    if (t->p >= t->end) { t->done = 1; return; }
    uint8_t b = *t->p;
    if (b & 0x80) { t->status = b; t->p++; }
    uint8_t st = t->status;
    int ch = st & 15;
    if ((sf || use_drv) && st < 0xf0) {
        int a = *t->p++, b = 0;
        if ((st & 0xf0) != 0xc0 && (st & 0xf0) != 0xd0) b = *t->p++;
        if (sf) sf_event(st, a, b);
        /* MIDI Mapper "Ad Lib general": GM drums (channel 10) go to the driver's channel 16 */
        else msadlib_midi((uint8_t)(ch == 9 ? (st & 0xf0) | 15 : ch == 15 ? (st & 0xf0) | 9 : st), (uint8_t)a, (uint8_t)b);
        if (t->p >= t->end) t->done = 1;
        if (!t->done) t->next_tick += read_vlq(&t->p, t->end);
        return;
    }
    switch (st & 0xf0) {
    case 0x80: { int n = *t->p++; t->p++; note_off(ch, n); break; }
    case 0x90: { int n = *t->p++; int v = *t->p++; note_on(ch, n, v); break; }
    case 0xa0: t->p += 2; break;
    case 0xb0: {
        int c = *t->p++, v = *t->p++;
        if (c == 7) chans[ch].volume = v;
        else if (c == 11) chans[ch].expression = v;
        else if (c == 10) chans[ch].pan = v;
        else if (c == 64) {
            chans[ch].sustain = v >= 64;
            if (!chans[ch].sustain)
                for (int k = 0; k < nvoices; k++)
                    if (sustained[k] && voices[k].channel == ch) voice_off(k);
        } else if (c == 123 || c == 120) {
            for (int k = 0; k < nvoices; k++)
                if (voices[k].active && voices[k].channel == ch) voice_off(k);
        }
        break;
    }
    case 0xc0: chans[ch].program = *t->p++; break;
    case 0xd0: t->p++; break;
    case 0xe0: {
        int lo = *t->p++, hi = *t->p++;
        chans[ch].bend = ((hi << 7) | lo) - 8192;
        for (int k = 0; k < nvoices; k++)
            if (voices[k].active && voices[k].channel == ch && ch != 9)
                voice_freq(k, voice_pitch(k) + bank[voices[k].program & 127].transpose, 1);
        break;
    }
    case 0xf0:
        if (st == 0xff) {
            uint8_t type = *t->p++;
            uint32_t len = read_vlq(&t->p, t->end);
            if (type == 0x51 && len == 3) set_tempo((uint32_t)t->p[0] << 16 | (uint32_t)t->p[1] << 8 | t->p[2]);
            if (type == 0x2f) t->done = 1;
            t->p += len;
        } else if (st == 0xf0 || st == 0xf7) {
            uint32_t len = read_vlq(&t->p, t->end);
            t->p += len;
        }
        break;
    }
    if (t->p >= t->end) t->done = 1;
    if (!t->done) t->next_tick += read_vlq(&t->p, t->end);
}

static void advance_tick(void) {
    int alive = 0;
    for (int i = 0; i < ntracks; i++) {
        track_t *t = &tracks[i];
        while (!t->done && t->next_tick <= cur_tick) track_event(t);
        if (!t->done) alive = 1;
    }
    cur_tick++;
    if (alive) return;
    if (looping) { rewind_song(); return; } /* the game always replays its tunes */
    playing = 0;
    song_end = (long)frame_pos;
    all_notes_off();
    if (sf) tsf_note_off_all(sf);
}

/* ------------------------------------------------------------- pre-rendered files */
/*
 * tools/render_music.c renders every tune to tuneNN.wav / tuneNN.ogg: one pass
 * of the song (the loop, frames [loop_start, loop_end)) followed by the
 * release tail of the notes still sounding when it ended. The loop points are
 * stored in a WAV 'smpl' chunk or as LOOPSTART/LOOPLENGTH Vorbis comments.
 * On a wrap the tail is mixed over the start of the next pass, which is what
 * the live synth does when it restarts the song.
 */
typedef struct {
    int tune;
    int16_t *pcm; /* stereo, MIX rate 44100 */
    uint32_t frames, loop_start, loop_end;
} pcm_t;

static char music_dir[1024];
static pcm_t cur_pcm;     /* owned by the audio thread while pcm_active */
static pcm_t pending_pcm; /* decoded by music_prefetch, swapped in by music_load_midi */
static int pcm_active;
static uint32_t pcm_pos, tail_pos; /* tail_pos 0: no tail playing */

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* in: interleaved s16 with ch channels at rate -> p->pcm stereo 44100 */
static int pcm_set(pcm_t *p, const int16_t *in, uint32_t frames, int ch, int rate) {
    if (!frames || ch < 1 || rate <= 0) return 0;
    uint32_t out_frames = rate == 44100 ? frames : (uint32_t)((uint64_t)frames * 44100 / (uint32_t)rate);
    p->pcm = malloc((size_t)out_frames * 4 + 4);
    if (!p->pcm) return 0;
    for (uint32_t i = 0; i < out_frames; i++) {
        double src = rate == 44100 ? i : (double)i * rate / 44100.0;
        uint32_t a = (uint32_t)src, b = a + 1 < frames ? a + 1 : a;
        double t = src - a;
        for (int c = 0; c < 2; c++) {
            int sc = c < ch ? c : ch - 1;
            double v = in[a * ch + sc] * (1 - t) + in[b * ch + sc] * t;
            p->pcm[2 * i + c] = (int16_t)v;
        }
    }
    if (rate != 44100) {
        p->loop_start = (uint32_t)((uint64_t)p->loop_start * 44100 / (uint32_t)rate);
        p->loop_end = (uint32_t)((uint64_t)p->loop_end * 44100 / (uint32_t)rate);
    }
    p->frames = out_frames;
    if (p->loop_end == 0 || p->loop_end > out_frames) p->loop_end = out_frames;
    if (p->loop_start >= p->loop_end) p->loop_start = 0;
    return 1;
}

static int decode_wav(pcm_t *p, const uint8_t *d, size_t n) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
    const uint8_t *fmt = NULL, *data = NULL;
    uint32_t data_len = 0;
    for (size_t o = 12; o + 8 <= n;) {
        uint32_t sz = rd32(d + o + 4);
        const uint8_t *c = d + o + 8;
        if (sz > n - o - 8) sz = (uint32_t)(n - o - 8);
        if (!memcmp(d + o, "fmt ", 4) && sz >= 16) fmt = c;
        else if (!memcmp(d + o, "data", 4)) { data = c; data_len = sz; }
        else if (!memcmp(d + o, "smpl", 4) && sz >= 36 + 24 && rd32(c + 28) >= 1) {
            p->loop_start = rd32(c + 36 + 8);
            p->loop_end = rd32(c + 36 + 12) + 1; /* smpl end is inclusive */
        }
        o += 8 + (size_t)sz + (sz & 1);
    }
    if (!fmt || !data) return 0;
    int tag = rd16(fmt), ch = rd16(fmt + 2), bits = rd16(fmt + 14);
    int rate = (int)rd32(fmt + 4);
    if ((tag != 1 && tag != 0xfffe) || (bits != 8 && bits != 16) || ch < 1) return 0;
    uint32_t frames = data_len / (uint32_t)(ch * bits / 8);
    int16_t *tmp = malloc((size_t)frames * ch * 2 + 2);
    if (!tmp) return 0;
    for (uint32_t i = 0; i < frames * (uint32_t)ch; i++)
        tmp[i] = bits == 16 ? (int16_t)rd16(data + 2 * i) : (int16_t)((data[i] - 128) << 8);
    int ok = pcm_set(p, tmp, frames, ch, rate);
    free(tmp);
    return ok;
}

void (*music_decode_yield)(void);

static int decode_ogg(pcm_t *p, const uint8_t *d, size_t n) {
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(d, (int)n, &err, NULL);
    if (!v) return 0;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    stb_vorbis_comment cm = stb_vorbis_get_comment(v);
    long loop_len = -1;
    for (int i = 0; i < cm.comment_list_length; i++) {
        const char *c = cm.comment_list[i];
        if (!strncasecmp(c, "LOOPSTART=", 10)) p->loop_start = (uint32_t)strtoul(c + 10, NULL, 10);
        else if (!strncasecmp(c, "LOOPLENGTH=", 11)) loop_len = strtol(c + 11, NULL, 10);
        else if (!strncasecmp(c, "LOOPEND=", 8)) p->loop_end = (uint32_t)strtoul(c + 8, NULL, 10);
    }
    if (loop_len > 0) p->loop_end = p->loop_start + (uint32_t)loop_len;
    uint32_t total = stb_vorbis_stream_length_in_samples(v);
    int ch = info.channels;
    int16_t *tmp = malloc(((size_t)total + 4096) * ch * 2);
    uint32_t got = 0;
    if (tmp) {
        for (;;) {
            uint32_t want = total + 4096 - got;
            if (want > 4096) want = 4096; /* in slices, so the host can keep its audio going */
            int k = stb_vorbis_get_samples_short_interleaved(v, ch, tmp + (size_t)got * ch, (int)want * ch);
            if (k <= 0) break;
            got += (uint32_t)k;
            if (music_decode_yield) music_decode_yield();
            if (got >= total + 4096) break;
        }
    }
    stb_vorbis_close(v);
    int ok = tmp && pcm_set(p, tmp, got, ch, (int)info.sample_rate);
    free(tmp);
    return ok;
}

static int load_tune_file(pcm_t *p, int tune) {
    static const char *ext[] = {"ogg", "wav", "OGG", "WAV"};
    *p = (pcm_t){0};
    if (!music_dir[0] || tune <= 0) return 0;
    for (int e = 0; e < 4; e++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/tune%02d.%s", music_dir, tune, ext[e]);
        size_t n = 0;
        uint8_t *d = read_file(path, &n);
        if (!d) continue;
        int ok = (e & 1) ? decode_wav(p, d, n) : decode_ogg(p, d, n);
        free(d);
        if (ok) {
            p->tune = tune;
            if (getenv("N3D_DEBUG"))
                fprintf(stderr, "music: %s (%u frames, loop %u..%u)\n", path, p->frames, p->loop_start, p->loop_end);
            return 1;
        }
        fprintf(stderr, "music: could not decode %s\n", path);
        free(p->pcm);
        *p = (pcm_t){0};
    }
    return 0;
}

void music_prefetch(const uint8_t *data, size_t len) {
    int t = tune_index(data, len);
    if (!t || !music_dir[0] || cur_pcm.tune == t || pending_pcm.tune == t) return;
    free(pending_pcm.pcm);
    load_tune_file(&pending_pcm, t);
}

static void pcm_render(int16_t *out, int frames) {
    for (int i = 0; i < frames; i++) {
        int32_t l = cur_pcm.pcm[2 * pcm_pos], r = cur_pcm.pcm[2 * pcm_pos + 1];
        pcm_pos++;
        if (tail_pos) {
            l += cur_pcm.pcm[2 * tail_pos];
            r += cur_pcm.pcm[2 * tail_pos + 1];
            if (++tail_pos >= cur_pcm.frames) tail_pos = 0;
        }
        if (pcm_pos >= cur_pcm.loop_end) {
            if (cur_pcm.loop_end < cur_pcm.frames) tail_pos = cur_pcm.loop_end;
            pcm_pos = cur_pcm.loop_start;
        }
        l = (int32_t)((int64_t)l * volume / 65535);
        r = (int32_t)((int64_t)r * volume / 65535);
        out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

static int has_tune_files(const char *dir) {
    for (int t = 1; t < MUSIC_MAX_TUNES; t++)
        for (int e = 0; e < 2; e++) {
            char path[1200];
            snprintf(path, sizeof path, "%s/tune%02d.%s", dir, t, e ? "wav" : "ogg");
            FILE *f = fopen(path, "rb");
            if (f) { fclose(f); return 1; }
        }
    return 0;
}

void music_set_dir(const char *dir) {
    if (!(dir && *dir) && music_dir[0]) fprintf(stderr, "music: pre-rendered tunes off\n");
    snprintf(music_dir, sizeof music_dir, "%s", dir ? dir : "");
    if (music_dir[0]) fprintf(stderr, "music: pre-rendered tunes from %s (synth for missing ones)\n", music_dir);
}

/* ------------------------------------------------------------- public API */
static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

void music_init(const char *data_dir, const char *soundfont) {
    load_bank(data_dir);
    if (soundfont && *soundfont) {
        if (ends_with(soundfont, ".dls")) {
            size_t n = 0, sn = 0;
            uint8_t *d = read_file(soundfont, &n);
            uint8_t *sf2 = d ? dls_to_sf2(d, n, &sn) : NULL;
            if (sf2) sf = tsf_load_memory(sf2, (int)sn);
            free(sf2);
            free(d);
        } else {
            sf = tsf_load_filename(soundfont);
        }
        if (sf) {
            tsf_set_output(sf, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
            tsf_set_max_voices(sf, 128);
            sf_reset_channels();
            fprintf(stderr, "music: using SoundFont %s\n", soundfont);
        } else {
            fprintf(stderr, "music: could not load SoundFont %s, using FM synth\n", soundfont);
        }
    }
    out_rate = 44100;
    opl_ready = 1;
    opl_reset_chips();
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/music", data_dir);
    if (has_tune_files(dir)) music_set_dir(dir);
    else if (has_tune_files("music")) music_set_dir("music");
}

void music_load_midi(const uint8_t *data, size_t len) {
    music_stop();
    int t = music_dir[0] ? tune_index(data, len) : 0;
    if (t) {
        if (cur_pcm.tune != t) {
            if (pending_pcm.tune != t) {
                free(pending_pcm.pcm);
                load_tune_file(&pending_pcm, t);
            }
            if (pending_pcm.tune == t) {
                free(cur_pcm.pcm);
                cur_pcm = pending_pcm;
                pending_pcm = (pcm_t){0};
            }
        }
        if (cur_pcm.tune == t && cur_pcm.pcm) {
            pcm_pos = 0;
            tail_pos = 0;
            pcm_active = 1;
            return;
        }
    }
    if ((!bank_loaded && !sf) || len < 14 || memcmp(data, "MThd", 4)) return;
    free(song);
    song = malloc(len);
    memcpy(song, data, len);
    song_len = len;
    division = (data[12] << 8) | data[13];
    if (division <= 0 || (division & 0x8000)) division = 96;
    /* a new tune starts from a silent synth */
    if (sf) tsf_reset(sf);
    opl_reset_chips();
    if (use_drv) msadlib_reset(drv_out);
    rewind_song();
    frame_pos = 0;
    song_end = -1;
    playing = 1;
}

void music_stop(void) {
    playing = 0;
    pcm_active = 0;
    if (opl_ready) all_notes_off();
    if (sf) tsf_note_off_all(sf);
}

void music_set_volume(int vol) { volume = vol < 0 ? 0 : vol > 65535 ? 65535 : vol; }
void music_set_looping(int loop) { looping = loop; }
long music_song_end(void) { return song_end; }
int music_using_soundfont(void) { return sf != NULL; }
void music_set_sf_gain(float gain) { sf_gain = gain; }
float music_take_peak(void) { float p = peak; peak = 0; return p; }

/* SoundFont output with gain/volume applied before clipping to 16 bits */
static void sf_render(int16_t *out, int frames) {
    float tmp[2 * 512];
    float g = sf_gain * (float)volume / 65535.0f;
    while (frames > 0) {
        int n = frames < 512 ? frames : 512;
        tsf_render_float(sf, tmp, n, 0);
        for (int i = 0; i < 2 * n; i++) {
            float a = fabsf(tmp[i]) * sf_gain;
            if (a > peak) peak = a;
            float v = tmp[i] * g * 32767.0f;
            out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        out += 2 * n;
        frames -= n;
    }
}

void music_render(int16_t *out, int frames, int rate) {
    (void)rate;
    if (pcm_active) { pcm_render(out, frames); return; }
    if (sf) {
        /* Render in chunks that end on MIDI tick boundaries so events land on time. */
        int done = 0;
        while (done < frames) {
            int n = frames - done;
            if (playing) {
                int k = (int)ceil(samples_per_tick - sample_acc);
                if (k < 1) k = 1;
                if (k < n) n = k;
            }
            sf_render(out + 2 * done, n);
            done += n;
            frame_pos += (uint64_t)n;
            if (playing) {
                sample_acc += n;
                while (playing && sample_acc >= samples_per_tick) {
                    sample_acc -= samples_per_tick;
                    advance_tick();
                }
            }
        }
        return;
    }
    if (!opl_ready) { memset(out, 0, (size_t)frames * 4); return; }
    int nchips = sb_models[sb_model].nchips, opl3 = sb_models[sb_model].opl3, filter = sb_models[sb_model].lpf_hz > 0;
    for (int i = 0; i < frames; i++) {
        if (playing) {
            sample_acc += 1.0;
            while (playing && sample_acc >= samples_per_tick) {
                sample_acc -= samples_per_tick;
                advance_tick();
            }
        }
        int16_t b[2][2];
        for (int c = 0; c < nchips; c++) OPL3_GenerateResampled(&opl[c], b[c]);
        /* dual OPL2: chip 0 is the left channel, chip 1 the right */
        /* SB 1/2 and the Windows driver (OPL2 writes to every chip): mono */
        double lr[2] = {b[0][0], use_drv ? b[0][0] : nchips == 2 ? b[1][1] : opl3 ? b[0][1] : b[0][0]};
        for (int c = 0; c < 2; c++) {
            /* AC coupling of the card's output: removes the chip's DC offset */
            double x = lr[c] - dc_x[c] + dc_r * dc_y[c];
            dc_x[c] = lr[c];
            dc_y[c] = x;
            x *= FM_GAIN;
            if (filter) {
                double y = lpf_a * (x + lpf_x[c]) + lpf_b * lpf_y[c];
                lpf_x[c] = x;
                lpf_y[c] = x = y;
            }
            x = x * volume / 65535;
            out[2 * i + c] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        frame_pos++;
    }
}
