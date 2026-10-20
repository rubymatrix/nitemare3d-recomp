/*
 * Model of MSADLIB.DRV, the Windows 3.1 "Ad Lib" MIDI driver.
 *
 * Under Windows 3.1 a game's General MIDI went through the MIDI Mapper to
 * this driver, which played it on the OPL2 at port 388h: the FM sound of an
 * AdLib or Sound Blaster under Windows. Its behaviour follows the driver
 * source in the Windows 3.1 DDK (multimed/adlib: midimain.c, adlib.c,
 * init.c), re-implemented here:
 *
 *  - OPL2 rhythm mode: 6 melodic voices plus bass drum, snare, tom, cymbal
 *    and hi-hat; melodic voices are taken free-first, else the oldest is stolen
 *  - drums on MIDI channel 16 through the drum kit (key -> patch, fixed note)
 *  - velocity only (through the driver's "loudervol" boost); volume, pan,
 *    expression and sustain controllers are ignored, as the driver did
 *  - per-program octave corrections, 2 semitone pitch bend in 1/25 semitone
 *    steps, and the driver's own F-number table (semitones approximated as
 *    a factor of 1.06)
 *
 * The instruments are not included: they are loaded from the user's own
 * MSADLIB.DRV (NE resources: type 256 id 1 = an AdLib .BNK bank, type 257
 * id 1 = drum kit of {key, patch, note} records), or from a .BNK file.
 */
#include "msadlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXPATCH 180
#define DRUMCH 15
#define FIRSTDRUM 35
#define NDRUMS (81 - 35 + 1)
#define BD 6
#define SD 7
#define TOM 8
#define NSTEP 25 /* pitch bend steps per semitone */
#define MAXVOL 0x7f

enum { P_KSL, P_MULT, P_FB, P_AR, P_SL, P_SS, P_DR, P_RR, P_LEVEL, P_AM, P_VIB, P_KSR, P_FM, P_WAVE, NPARAM };

typedef struct {
    uint8_t mode, perc_voice; /* mode 1: rhythm patch on voice perc_voice (6..10) */
    uint8_t op0[13], op1[13];
    uint8_t wave0, wave1;
} timbre_t;

static timbre_t patches[MAXPATCH];
static struct { uint8_t patch, note; } drumkit[NDRUMS];
static int have_drumkit;

static void (*out)(uint8_t reg, uint8_t val);

/* ---------------------------------------------------------------- chip level (adlib.c) */
static const uint8_t offset_slot[18] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21};
static const uint8_t oper_slot[18] = {0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1};
static const uint8_t voice_slot[18] = {0, 1, 2, 0, 1, 2, 3, 4, 5, 3, 4, 5, 6, 7, 8, 6, 7, 8};
static const uint8_t slot_perc[5] = {12, 16, 14, 17, 13}; /* BD (and 15), SD, TOM, CYMB, HH */
static const uint8_t slot_voice[9][2] = {{0, 3}, {1, 4}, {2, 5}, {6, 9}, {7, 10}, {8, 11}, {12, 15}, {13, 16}, {14, 17}};
static const uint8_t perc_masks[5] = {0x10, 0x08, 0x04, 0x02, 0x01};

static uint8_t param_slot[18][NPARAM];
static uint8_t slot_rel_volume[18];
static uint8_t perc_bits, note_pitch[11], voice_key_on[11];
static uint16_t fnum_notes[NSTEP][12];
static int fnum_row[11], half_tone_offset[11];
static int pitch_range_step = 2 * NSTEP;

static void am_vib_rhythm(void) { out(0xbd, (uint8_t)(0x20 | perc_bits)); } /* rhythm mode, no AM/vib depth */

static void ksl_level(int slot) {
    unsigned t = 63 - (param_slot[slot][P_LEVEL] & 0x3f);
    t = slot_rel_volume[slot] * t;
    t += t + MAXVOL;
    t = 63 - t / (2 * MAXVOL);
    out((uint8_t)(0x40 | offset_slot[slot]), (uint8_t)(t | (unsigned)param_slot[slot][P_KSL] << 6));
}

static void set_slot_param(int slot, const uint8_t *prm, uint8_t wave) {
    uint8_t *p = param_slot[slot];
    memcpy(p, prm, 13);
    p[P_WAVE] = wave & 3;
    am_vib_rhythm();
    out(0x08, 0);
    ksl_level(slot);
    if (!oper_slot[slot])
        out((uint8_t)(0xc0 | voice_slot[slot]), (uint8_t)(p[P_FB] << 1 | (p[P_FM] ? 0 : 1)));
    out((uint8_t)(0x60 | offset_slot[slot]), (uint8_t)(p[P_AR] << 4 | (p[P_DR] & 15)));
    out((uint8_t)(0x80 | offset_slot[slot]), (uint8_t)(p[P_SL] << 4 | (p[P_RR] & 15)));
    out((uint8_t)(0x20 | offset_slot[slot]),
        (uint8_t)((p[P_AM] ? 0x80 : 0) | (p[P_VIB] ? 0x40 : 0) | (p[P_SS] ? 0x20 : 0) | (p[P_KSR] ? 0x10 : 0) | (p[P_MULT] & 15)));
    out((uint8_t)(0xe0 | offset_slot[slot]), p[P_WAVE]);
}

static void set_freq(int voice, uint8_t pitch, int key_on) {
    voice_key_on[voice] = (uint8_t)key_on;
    note_pitch[voice] = pitch;
    pitch = (uint8_t)(pitch + half_tone_offset[voice]); /* BYTE arithmetic, as in the driver */
    if (pitch > 95) pitch = 95;
    uint16_t fnum = fnum_notes[fnum_row[voice]][pitch % 12];
    out((uint8_t)(0xa0 | voice), (uint8_t)fnum);
    out((uint8_t)(0xb0 | voice), (uint8_t)((key_on ? 32 : 0) + (pitch / 12 << 2) + (3 & fnum >> 8)));
}

static void set_voice_timbre(int voice, const timbre_t *t) {
    if (voice < BD) {
        set_slot_param(slot_voice[voice][0], t->op0, t->wave0);
        set_slot_param(slot_voice[voice][1], t->op1, t->wave1);
    } else if (voice == BD) {
        set_slot_param(12, t->op0, t->wave0);
        set_slot_param(15, t->op1, t->wave1);
    } else {
        set_slot_param(slot_perc[voice - BD], t->op0, t->wave0);
    }
}

static void change_pitch(int voice, unsigned bend) {
    uint32_t dw = (uint32_t)((long)((int)bend - 0x2000) * pitch_range_step);
    int t1 = (int16_t)(dw >> 8 & 0xffff) >> 5;
    int delta;
    if (t1 < 0) {
        int t2 = NSTEP - 1 - t1;
        half_tone_offset[voice] = -(t2 / NSTEP);
        delta = (t2 - NSTEP + 1) % NSTEP;
        if (delta) delta = NSTEP - delta;
    } else {
        half_tone_offset[voice] = t1 / NSTEP;
        delta = t1 % NSTEP;
    }
    fnum_row[voice] = delta;
}

static void set_voice_pitch(int voice, unsigned bend) {
    if (voice <= BD) {
        if (bend > 0x3fff) bend = 0x3fff;
        change_pitch(voice, bend);
        set_freq(voice, note_pitch[voice], voice_key_on[voice]);
    }
}

static void set_voice_volume(int voice, uint8_t vol) {
    if (vol > MAXVOL) vol = MAXVOL;
    if (voice <= BD) {
        const uint8_t *s = slot_voice[voice];
        slot_rel_volume[s[1]] = vol;
        ksl_level(s[1]);
        if (!param_slot[s[0]][P_FM]) { /* additive: the modulator is heard too */
            slot_rel_volume[s[0]] = vol;
            ksl_level(s[0]);
        }
    } else {
        int slot = slot_perc[voice - BD];
        slot_rel_volume[slot] = vol;
        ksl_level(slot);
    }
}

static void chip_note_on(int voice, uint8_t pitch) {
    pitch = pitch < 12 ? 0 : (uint8_t)(pitch - 12); /* MIDI middle C 60 = chip 48 */
    if (voice < BD) {
        set_freq(voice, pitch, 1);
        return;
    }
    if (voice == BD) set_freq(BD, pitch, 0);
    else if (voice == TOM) {
        set_freq(TOM, pitch, 0); /* the snare's frequency follows the tom */
        set_freq(SD, (uint8_t)(pitch + 7), 0);
    }
    perc_bits |= perc_masks[voice - BD];
    am_vib_rhythm();
}

static void chip_note_off(int voice) {
    if (voice < BD) set_freq(voice, note_pitch[voice], 0);
    else {
        perc_bits &= (uint8_t)~perc_masks[voice - BD];
        am_vib_rhythm();
    }
}

static long prem_fnum(int num, int den) {
    long d100 = den * 100L;
    long f8 = (d100 + 6L * num) * (26044L * 2L);
    f8 /= d100 * 25;
    long fnum8 = f8 * 16384;
    fnum8 *= 9L;
    fnum8 /= 179L * 625L;
    return fnum8;
}

/* ---------------------------------------------------------------- MIDI level (midimain.c) */
typedef struct { uint8_t alloc, note, channel, volume; uint32_t stamp; } voice_t;
static voice_t voices[11];
static struct { uint8_t patch; uint16_t bend; } channels[16];
static uint32_t age;

/* octave corrections per GM program, from the driver */
static const int8_t patch_key_offset[128] = {
    0, -12, 12, 0, 0, 12, -12, 0, 0, -24, 0, 0, 0, 0, 0, 0, 0, 0, -12, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 12, 12, 12, 0, 0, 12, 12, 0, -12, -12, 0, 12, -12, -12, 0, 12, 0, 0, -12, 0,
    0, 0, 12, 12, 0, 0, 12, 0, 0, 0, 12, 0, 0, 0, 12, 12, 0, 12, 0, 0, -12, 0, -12, -12, 0, 0,
    -12, -12, 0, 0, 0, 0, 0, -12, -19, 0, 0, -12, 0, 0, 0, 0, 0, 0, -31, -12, 0, 12, 12, 12, 12, 0,
    12, 0, 12, 0, 0, 0, 0, 12, 0, 0, 0, 0, 12, 12, 12, 0, 0, 0, 0, 0, -24, -36, 0, 0};

/* "hack to overcome how quiet this thing is in relation to wave output" */
static uint8_t louder_vol(uint8_t v) { return v < 2 ? 0 : (uint8_t)(64 + v / 2); }

static uint8_t octave_reg(uint8_t ch, uint8_t note) {
    if (ch == DRUMCH || channels[ch].patch > 127) return note;
    int n = note + patch_key_offset[channels[ch].patch];
    return n < 0 || n > 127 ? note : (uint8_t)n;
}

static int find_voice(uint8_t note, uint8_t ch) {
    if (ch == DRUMCH) {
        int i = patches[channels[DRUMCH].patch].perc_voice;
        if (i < 11 && voices[i].alloc) return i;
        return -1;
    }
    for (int i = 0; i < 11; i++)
        if (voices[i].alloc && voices[i].note == note && voices[i].channel == ch) {
            voices[i].stamp = age++;
            return i;
        }
    return -1;
}

static int new_voice(uint8_t note, uint8_t ch) {
    uint8_t patch = channels[ch].patch;
    const timbre_t *t = &patches[patch];
    if (t->mode) { /* rhythm patch: its fixed voice */
        int v = t->perc_voice >= BD && t->perc_voice <= 10 ? t->perc_voice : BD;
        voices[v] = (voice_t){1, note, ch, voices[v].volume, patch};
        set_voice_timbre(v, t);
        return v;
    }
    int use = 0;
    uint32_t oldest = age;
    for (int i = 0; i < 6; i++) {
        if (!voices[i].alloc) { use = i; break; }
        if (voices[i].stamp < oldest) { oldest = voices[i].stamp; use = i; }
    }
    if (voices[use].alloc) chip_note_off(use);
    voices[use] = (voice_t){1, note, ch, voices[use].volume, age++};
    set_voice_timbre(use, t);
    return use;
}

static void note_off(uint8_t ch, uint8_t note) {
    note = octave_reg(ch, note);
    if (ch == DRUMCH) {
        if (note < FIRSTDRUM || note >= FIRSTDRUM + NDRUMS) return;
        uint8_t patch = drumkit[note - FIRSTDRUM].patch;
        int v = find_voice(drumkit[note - FIRSTDRUM].note, ch);
        if (v >= 0 && (voices[v].stamp & 0xffff) == patch) {
            chip_note_off(v);
            voices[v].alloc = 0;
        }
        return;
    }
    int v = find_voice(note, ch);
    if (v >= 0 && voices[v].note) {
        chip_note_off(v);
        voices[v].alloc = 0;
    }
}

static void note_on(uint8_t ch, uint8_t note, uint8_t vel) {
    if (!vel) { note_off(ch, note); return; }
    note = octave_reg(ch, note);
    vel = louder_vol(vel);
    int v;
    if (ch == DRUMCH) {
        if (note < FIRSTDRUM || note >= FIRSTDRUM + NDRUMS) return;
        channels[DRUMCH].patch = drumkit[note - FIRSTDRUM].patch;
        note = drumkit[note - FIRSTDRUM].note;
        if ((v = find_voice(note, ch)) >= 0) chip_note_off(v);
        v = new_voice(note, ch);
    } else {
        v = find_voice(note, ch);
        if (v < 0) v = new_voice(note, ch);
        else chip_note_off(v);
    }
    if (voices[v].volume != vel) {
        set_voice_volume(v, vel);
        voices[v].volume = vel;
    }
    set_voice_pitch(v, channels[ch].bend);
    chip_note_on(v, note);
}

void msadlib_all_notes_off(void) {
    if (!out) return;
    for (int v = 0; v < 11; v++)
        if (voices[v].alloc) note_off(voices[v].channel, voices[v].note);
}

void msadlib_midi(uint8_t st, uint8_t a, uint8_t b) {
    uint8_t ch = st & 15;
    if (!out) return;
    switch (st & 0xf0) {
    case 0x80: note_off(ch, a); break;
    case 0x90: note_on(ch, a, b); break;
    case 0xb0: if (a >= 123) msadlib_all_notes_off(); break;
    case 0xc0:
        if (ch == DRUMCH) break; /* the drum kit is fixed */
        for (int v = 0; v < 11; v++)
            if (voices[v].alloc && voices[v].channel == ch && voices[v].note) {
                chip_note_off(v);
                voices[v].alloc = 0;
            }
        channels[ch].patch = a;
        break;
    case 0xe0: {
        unsigned bend = (unsigned)b << 7 | a;
        for (int v = 0; v < 11; v++)
            if (voices[v].alloc && voices[v].channel == ch) set_voice_pitch(v, bend);
        channels[ch].bend = (uint16_t)bend;
        break;
    }
    }
}

void msadlib_reset(void (*o)(uint8_t, uint8_t)) {
    out = o;
    memset(voices, 0, sizeof voices);
    for (int c = 0; c < 16; c++) channels[c].patch = 0, channels[c].bend = 0x2000;
    channels[DRUMCH].patch = 129;
    age = 0;
    perc_bits = 0;
    /* SoundWarmInit */
    am_vib_rhythm();
    out(0x08, 0);
    for (int i = 0; i < 18; i++) slot_rel_volume[i] = MAXVOL;
    for (int row = 0, num = 0; row < NSTEP; row++, num += 100 / NSTEP) {
        long val = prem_fnum(num, 100);
        fnum_notes[row][0] = (uint16_t)((4 + val) >> 3);
        for (int i = 1; i < 12; i++) {
            val *= 106;
            fnum_notes[row][i] = (uint16_t)((4 + (val /= 100)) >> 3);
        }
    }
    for (int i = 0; i < 11; i++) fnum_row[i] = half_tone_offset[i] = 0;
    for (int v = 0; v <= 8; v++) { out((uint8_t)(0xa0 | v), 0); out((uint8_t)(0xb0 | v), 0); }
    set_freq(TOM, 24, 0); /* rhythm mode: tom and snare tuning */
    set_freq(SD, 24 + 7, 0);
    perc_bits = 0;
    pitch_range_step = 2 * NSTEP;
    for (int i = 0; i < 18; i++) out((uint8_t)(0xe0 | offset_slot[i]), 0);
    out(0x01, 0x20); /* wave select on */
}

/* ---------------------------------------------------------------- loading */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int load_bank(const uint8_t *d, size_t n) {
    if (n < 28 || memcmp(d + 2, "ADLIB-", 6)) return 0;
    uint32_t off = rd32(d + 12 + 4);
    int count = 0;
    memset(patches, 0, sizeof patches);
    for (int i = 0; i < MAXPATCH && off + 30u * (i + 1) <= n; i++, count++) memcpy(&patches[i], d + off + 30u * i, 30);
    return count >= 128;
}

/* NE resource lookup: integer type and id */
static const uint8_t *ne_resource(const uint8_t *d, size_t n, int type, int id, size_t *len) {
    if (n < 0x40 || memcmp(d, "MZ", 2)) return NULL;
    uint32_t ne = rd32(d + 0x3c);
    if (ne + 0x40 > n || memcmp(d + ne, "NE", 2)) return NULL;
    size_t p = ne + rd16(d + ne + 0x24);
    if (p + 2 > n) return NULL;
    int shift = rd16(d + p);
    p += 2;
    while (p + 8 <= n && rd16(d + p)) {
        int t = rd16(d + p), count = rd16(d + p + 2);
        p += 8;
        for (int i = 0; i < count && p + 12 <= n; i++, p += 12) {
            if (t == (0x8000 | type) && rd16(d + p + 6) == (0x8000 | id)) {
                size_t off = (size_t)rd16(d + p) << shift, sz = (size_t)rd16(d + p + 2) << shift;
                if (off >= n) return NULL;
                if (off + sz > n) sz = n - off;
                *len = sz;
                return d + off;
            }
        }
    }
    return NULL;
}

int msadlib_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return 0; }
    fclose(f);
    int ok = 0;
    size_t len = 0;
    const uint8_t *bank = ne_resource(d, (size_t)n, 256, 1, &len);
    if (bank) {
        ok = load_bank(bank, len);
        const uint8_t *kit = ne_resource(d, (size_t)n, 257, 1, &len);
        memset(drumkit, 0, sizeof drumkit);
        have_drumkit = 0;
        for (size_t i = 0; kit && i + 3 <= len; i += 3) {
            int key = kit[i];
            if (key >= FIRSTDRUM && key < FIRSTDRUM + NDRUMS) {
                drumkit[key - FIRSTDRUM].patch = kit[i + 1];
                drumkit[key - FIRSTDRUM].note = kit[i + 2];
                have_drumkit = 1;
            }
        }
    } else {
        ok = load_bank(d, (size_t)n);
    }
    if (ok && !have_drumkit) /* bank only: drum patch per key (the bank's order), played at its key */
        for (int k = 0; k < NDRUMS; k++) drumkit[k].patch = (uint8_t)(128 + k), drumkit[k].note = (uint8_t)(FIRSTDRUM + k);
    free(d);
    if (ok) fprintf(stderr, "music: Windows Ad Lib driver patches from %s%s\n", path, have_drumkit ? "" : " (no drum kit: approximated)");
    return ok;
}
