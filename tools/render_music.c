/*
 * n3d-render-music: pre-renders Nitemare-3D's MIDI tunes to audio files.
 *
 * Extracts the 15 Standard MIDI Files from SND.DAT and plays each through the
 * same sequencer the game uses (src/music.c): either the OPL2 FM synth with
 * the game's own IBK instrument bank, or a General MIDI SoundFont (.sf2) or
 * DLS bank (.dls, e.g. Windows' gm.dls). Each tune is written as
 * tuneNN.wav — one pass of the song followed by the release tail of its last
 * notes, with the loop (start of the song to the end of the pass) stored in a
 * 'smpl' chunk — and, when oggenc or ffmpeg is installed, as tuneNN.ogg with
 * LOOPSTART/LOOPLENGTH comments. The game plays these files instead of the
 * live synth (see README, "Pre-rendered music").
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define DEVNULL "NUL"
#else
#define MKDIR(p) mkdir(p, 0755)
#define DEVNULL "/dev/null"
#endif

#include "dls.h"
#include "music.h"

#define RATE 44100
#define BLOCK 1024

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --data DIR [options]\n"
            "  --data DIR        folder with SND.DAT (the original game data)\n"
            "  --soundfont FILE  General MIDI bank: .sf2 or .dls (default: the OPL2 FM synth\n"
            "                    with the game's own instrument bank from SND.DAT)\n"
            "  --sb MODEL        Sound Blaster whose FM chip and output filter to emulate for the\n"
            "                    FM synth: sb1, sb2, sbpro1, sbpro2, sb16 (default sb16)\n"
            "  --fm-driver FILE  play the FM like Windows 3.1's Ad Lib driver, with the instruments\n"
            "                    from your MSADLIB.DRV (or an AdLib .BNK bank)\n"
            "  --out DIR         output folder (default: <data>/music, where the game looks)\n"
            "  --format F        wav, ogg or both (default: both; ogg needs oggenc or ffmpeg)\n"
            "  --quality Q       Vorbis quality -1..10 (default 6)\n"
            "  --tunes LIST      e.g. 1,4,7 (default: all)\n"
            "  --tail SECONDS    longest release tail to keep after the song (default 8)\n"
            "  --gain DB         SoundFont output gain (default: as loud as possible without clipping)\n"
            "  --export-sf2 OUT  only convert the --soundfont .dls to a .sf2 file and exit\n",
            argv0);
}

static void w16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); }
static void w32(FILE *f, uint32_t v) { w16(f, v & 0xffff); w16(f, v >> 16); }

static int write_wav(const char *path, const int16_t *pcm, uint32_t frames, uint32_t loop_end) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t data = frames * 4, smpl = 36 + 24;
    fwrite("RIFF", 1, 4, f);
    w32(f, 4 + (8 + 16) + (8 + smpl) + (8 + data));
    fwrite("WAVEfmt ", 1, 8, f);
    w32(f, 16); w16(f, 1); w16(f, 2); w32(f, RATE); w32(f, RATE * 4); w16(f, 4); w16(f, 16);
    /* sampler chunk: one forward loop over the song; the frames after it are the release tail */
    fwrite("smpl", 1, 4, f);
    w32(f, smpl);
    w32(f, 0); w32(f, 0); w32(f, 1000000000u / RATE); w32(f, 60); w32(f, 0); w32(f, 0); w32(f, 0);
    w32(f, 1); w32(f, 0);
    w32(f, 0); w32(f, 0); w32(f, 0); w32(f, loop_end - 1); w32(f, 0); w32(f, 0);
    fwrite("data", 1, 4, f);
    w32(f, data);
    for (uint32_t i = 0; i < frames * 2; i++) w16(f, (uint16_t)pcm[i]);
    int ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

static const char *find_encoder(void) {
    if (system("oggenc --version >" DEVNULL " 2>&1") == 0) return "oggenc";
    if (system("ffmpeg -version >" DEVNULL " 2>&1") == 0) return "ffmpeg";
    return NULL;
}

static int encode_ogg(const char *enc, const char *wav, const char *ogg, int quality, uint32_t loop_len) {
    char cmd[4096];
    if (!strcmp(enc, "oggenc"))
        snprintf(cmd, sizeof cmd, "oggenc -Q -q %d -c LOOPSTART=0 -c LOOPLENGTH=%u -o \"%s\" \"%s\"", quality, loop_len,
                 ogg, wav);
    else
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -y -hide_banner -loglevel error -i \"%s\" -c:a libvorbis -q:a %d "
                 "-metadata LOOPSTART=0 -metadata LOOPLENGTH=%u \"%s\"",
                 wav, quality, loop_len, ogg);
    return system(cmd) == 0;
}

/* Plays tune t once into *buf: frames [0, loop_end) are the song, the rest
 * up to total its release tail. */
static int render_tune(int t, int16_t **buf, size_t *cap, uint32_t *total_out, uint32_t *loop_end_out, double tail_max) {
    size_t len;
    const uint8_t *mid = music_tune(t, &len);
    music_load_midi(mid, len);
    uint32_t frames = 0, last_loud = 0;
    long end = -1;
    const uint32_t max_song = RATE * 60 * 20;
    for (;;) {
        if ((size_t)(frames + BLOCK) * 2 > *cap) {
            *cap = *cap ? *cap * 2 : (size_t)RATE * 2 * 120;
            *buf = realloc(*buf, *cap * sizeof **buf);
            if (!*buf) { fprintf(stderr, "out of memory\n"); exit(1); }
        }
        int16_t *blk = *buf + (size_t)frames * 2;
        memset(blk, 0, BLOCK * 4);
        music_render(blk, BLOCK, RATE);
        for (int i = 0; i < BLOCK; i++)
            if (abs(blk[2 * i]) > 4 || abs(blk[2 * i + 1]) > 4) last_loud = frames + (uint32_t)i + 1;
        frames += BLOCK;
        if (end < 0) {
            end = music_song_end();
            if (end < 0 && frames >= max_song) { fprintf(stderr, "tune %d: no end after 20 minutes\n", t); break; }
            continue;
        }
        /* release tail: stop after half a second of silence or tail_max */
        uint32_t quiet = frames > last_loud ? frames - last_loud : 0;
        if (quiet >= RATE / 2 || frames - (uint32_t)end >= (uint32_t)(tail_max * RATE)) break;
    }
    music_stop();
    if (end <= 0) return 0;
    *loop_end_out = (uint32_t)end;
    *total_out = last_loud > (uint32_t)end ? last_loud : (uint32_t)end;
    if (*total_out > frames) *total_out = frames;
    /* notes that never die away (release rate 0) are cut at tail_max: fade
     * the last 50 ms so the tail does not click where it ends */
    uint32_t tail = *total_out - (uint32_t)end, fade = tail < RATE / 20 ? tail : RATE / 20;
    for (uint32_t i = 0; i < fade; i++) {
        size_t k = (size_t)(*total_out - fade + i) * 2;
        double g = 1.0 - (i + 1.0) / fade;
        (*buf)[k] = (int16_t)((*buf)[k] * g);
        (*buf)[k + 1] = (int16_t)((*buf)[k + 1] * g);
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *data_dir = NULL, *soundfont = NULL, *out_dir = NULL, *format = "both", *export_sf2 = NULL;
    const char *tune_list = NULL, *sb = "sb16", *fm_driver = NULL;
    int quality = 6;
    double tail_max = 8.0;
    float gain_db = -10000; /* auto */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--soundfont") && i + 1 < argc) soundfont = argv[++i];
        else if (!strcmp(argv[i], "--fm-driver") && i + 1 < argc) {
            fm_driver = argv[++i];
            if (!music_set_fm_driver(fm_driver)) { fprintf(stderr, "%s: no Ad Lib driver bank found\n", fm_driver); return 1; }
        }
        else if (!strcmp(argv[i], "--sb") && i + 1 < argc) {
            sb = argv[++i];
            if (!music_set_sb_model(sb)) { usage(argv[0]); return 1; }
        }
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--format") && i + 1 < argc) format = argv[++i];
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tunes") && i + 1 < argc) tune_list = argv[++i];
        else if (!strcmp(argv[i], "--tail") && i + 1 < argc) tail_max = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain_db = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--export-sf2") && i + 1 < argc) export_sf2 = argv[++i];
        else { usage(argv[0]); return 1; }
    }

    if (export_sf2) {
        if (!soundfont) { fprintf(stderr, "--export-sf2 needs --soundfont FILE.dls\n"); return 1; }
        FILE *f = fopen(soundfont, "rb");
        if (!f) { perror(soundfont); return 1; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *d = malloc((size_t)n);
        size_t got = fread(d, 1, (size_t)n, f);
        fclose(f);
        size_t sn = 0;
        uint8_t *sf2 = got == (size_t)n ? dls_to_sf2(d, (size_t)n, &sn) : NULL;
        free(d);
        if (!sf2) { fprintf(stderr, "%s: not a DLS bank\n", soundfont); return 1; }
        f = fopen(export_sf2, "wb");
        if (!f || fwrite(sf2, 1, sn, f) != sn) { perror(export_sf2); return 1; }
        fclose(f);
        free(sf2);
        printf("wrote %s (%zu bytes)\n", export_sf2, sn);
        return 0;
    }

    if (!data_dir) { usage(argv[0]); return 1; }
    int want_wav = strcmp(format, "ogg") != 0, want_ogg = strcmp(format, "wav") != 0;
    if (!want_wav && !want_ogg) { usage(argv[0]); return 1; }
    char out_buf[1100];
    if (!out_dir) {
        snprintf(out_buf, sizeof out_buf, "%s/music", data_dir);
        out_dir = out_buf;
    }
    MKDIR(out_dir);

    music_init(data_dir, soundfont);
    if (soundfont && !music_using_soundfont()) return 1; /* music_init said why */
    music_set_dir(NULL);
    music_set_looping(0);
    music_set_volume(65535);
    if (!music_tune(1, NULL)) { fprintf(stderr, "no MIDI tunes found in %s/SND.DAT\n", data_dir); return 1; }

    const char *enc = want_ogg ? find_encoder() : NULL;
    if (want_ogg && !enc) {
        fprintf(stderr, "no Vorbis encoder (oggenc or ffmpeg) found: writing WAV only\n");
        want_ogg = 0;
        want_wav = 1;
    }
    if (soundfont) printf("source: %s\n", soundfont);
    else if (fm_driver) printf("source: FM (Nuked OPL3 as Sound Blaster '%s'), Windows Ad Lib driver model, %s\n", sb, fm_driver);
    else printf("source: FM (Nuked OPL3 as Sound Blaster '%s'), GM instrument bank from SND.DAT\n", sb);

    int tunes[MUSIC_MAX_TUNES], ntunes = 0;
    for (int t = 1; t < MUSIC_MAX_TUNES; t++) {
        if (!music_tune(t, NULL)) continue;
        int hit = !tune_list;
        for (const char *p = tune_list; p && *p;) {
            if (atoi(p) == t) hit = 1;
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
        if (hit) tunes[ntunes++] = t;
    }

    int16_t *buf = NULL;
    size_t cap = 0;
    uint32_t total, loop_end;
    if (music_using_soundfont()) {
        /* One gain for all tunes so they keep their relative levels: lower it
         * just enough that the loudest tune does not clip. */
        float gain = 1.0f;
        if (gain_db > -1000) {
            gain = powf(10.0f, gain_db / 20.0f);
        } else {
            float peak = 0;
            music_take_peak();
            for (int i = 0; i < ntunes; i++)
                if (render_tune(tunes[i], &buf, &cap, &total, &loop_end, tail_max)) {
                    float p = music_take_peak();
                    if (p > peak) peak = p;
                }
            if (peak > 0.98f) gain = 0.98f / peak;
            printf("peak %.2f of full scale -> gain %.1f dB\n", peak, 20 * log10f(gain));
        }
        music_set_sf_gain(gain);
    }

    int failures = 0;
    for (int i = 0; i < ntunes; i++) {
        int t = tunes[i];
        if (!render_tune(t, &buf, &cap, &total, &loop_end, tail_max)) { failures++; continue; }
        int peak = 0;
        for (uint32_t k = 0; k < total * 2; k++) if (abs(buf[k]) > peak) peak = abs(buf[k]);

        char wav[1300], ogg[1300];
        snprintf(wav, sizeof wav, "%s/tune%02d.wav", out_dir, t);
        snprintf(ogg, sizeof ogg, "%s/tune%02d.ogg", out_dir, t);
        int ok = write_wav(wav, buf, total, loop_end);
        if (ok && want_ogg) ok = encode_ogg(enc, wav, ogg, quality, loop_end);
        if (!want_wav) remove(wav);
        printf("tune%02d: %6.1f s loop + %4.1f s tail, peak %5.1f%%  %s%s%s\n", t, loop_end / (double)RATE,
               (total - loop_end) / (double)RATE, peak * 100.0 / 32768, want_wav ? wav : "",
               want_wav && want_ogg ? " " : "", want_ogg ? ogg : "");
        if (!ok) { fprintf(stderr, "tune %d: writing failed\n", t); failures++; }
    }
    free(buf);
    return failures ? 1 : 0;
}
