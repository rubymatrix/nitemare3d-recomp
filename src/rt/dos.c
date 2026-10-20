/* DOS INT 21h services used by the C runtime (DOS3Call) and the game. */
#include "rt/rt.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_p(p) _mkdir(p)
#else
#include <dirent.h>
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0755)
#endif

static char g_data_dir[1024] = ".";
static char g_user_dir[1024] = ".";

void dos_set_paths(const char *data_dir, const char *user_dir) {
    snprintf(g_data_dir, sizeof g_data_dir, "%s", data_dir);
    snprintf(g_user_dir, sizeof g_user_dir, "%s", user_dir);
    mkdir_p(g_user_dir);
}
const char *dos_data_dir(void) { return g_data_dir; }
const char *dos_user_dir(void) { return g_user_dir; }

/* Strip drive and directories from a DOS path; the game only uses bare names. */
static void dos_basename(const char *dospath, char *out, size_t n) {
    const char *p = dospath;
    for (const char *q = dospath; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') p = q + 1;
    size_t i = 0;
    for (; p[i] && i + 1 < n; i++) out[i] = (char)tolower((unsigned char)p[i]);
    out[i] = 0;
}

/* Find a file case-insensitively inside dir. Returns 1 and fills out on success. */
static int find_ci(const char *dir, const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", dir, name);
    struct stat st;
    if (stat(out, &st) == 0) return 1;
#ifndef _WIN32
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcasecmp(e->d_name, name) == 0) {
            snprintf(out, n, "%s/%s", dir, e->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
#endif
    return 0;
}

void *dos_fopen(const char *dospath, const char *mode) {
    char base[256], path[1400];
    dos_basename(dospath, base, sizeof base);
    int writing = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    if (writing) {
        /* Writes always go to the user directory; seed it from the data dir for r+ modes. */
        if (!find_ci(g_user_dir, base, path, sizeof path) && strchr(mode, '+') && !strchr(mode, 'w')) {
            char src[1400];
            if (find_ci(g_data_dir, base, src, sizeof src)) {
                FILE *in = fopen(src, "rb"), *out;
                snprintf(path, sizeof path, "%s/%s", g_user_dir, base);
                if (in && (out = fopen(path, "wb"))) {
                    char buf[8192];
                    size_t k;
                    while ((k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, out);
                    fclose(out);
                }
                if (in) fclose(in);
            }
        }
        snprintf(path, sizeof path, "%s/%s", g_user_dir, base);
        find_ci(g_user_dir, base, path, sizeof path);
        return fopen(path, mode);
    }
    if (find_ci(g_user_dir, base, path, sizeof path) || find_ci(g_data_dir, base, path, sizeof path))
        return fopen(path, mode);
    return NULL;
}

#define MAX_HANDLES 64
static FILE *handles[MAX_HANDLES];

static void set_cf(int c) { rCF = (uint8_t)(c != 0); }
static void dos_error(uint16_t code) { rAX = code; set_cf(1); }

static uint32_t dsdx(void) { return SEGLIN(rDS, rDX); }

static int alloc_handle(FILE *f) {
    for (int h = 5; h < MAX_HANDLES; h++)
        if (!handles[h]) { handles[h] = f; return h; }
    return -1;
}

void dos_int21(void) {
    uint8_t ah = rAH;
    switch (ah) {
    case 0x30: /* get DOS version: report 6.22 */
        rAX = 0x1606; rBX = 0; rCX = 0; set_cf(0); break;
    case 0x19: /* current drive = C: */
        rAL = 2; break;
    case 0x0e: /* select drive */
        rAL = 26; break;
    case 0x2a: { /* date */
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        rCX = (uint16_t)(tm->tm_year + 1900); rDH = (uint8_t)(tm->tm_mon + 1); rDL = (uint8_t)tm->tm_mday; rAL = (uint8_t)tm->tm_wday;
        break;
    }
    case 0x2c: { /* time */
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        rCH = (uint8_t)tm->tm_hour; rCL = (uint8_t)tm->tm_min; rDH = (uint8_t)tm->tm_sec; rDL = 0;
        break;
    }
    case 0x47: /* get current directory -> empty (root) */
        MEMW8(SEGLIN(rDS, rSI), 0); set_cf(0); rAX = 0x0100; break;
    case 0x3b: /* chdir */
        set_cf(0); break;
    case 0x3c: /* create */
    case 0x5b: { /* create new */
        char *name = rt_strdup_far(dsdx());
        FILE *f = dos_fopen(name, "w+b");
        free(name);
        int h = f ? alloc_handle(f) : -1;
        if (h < 0) { if (f) fclose(f); dos_error(f ? 4 : 3); break; }
        rAX = (uint16_t)h; set_cf(0);
        break;
    }
    case 0x3d: { /* open */
        char *name = rt_strdup_far(dsdx());
        int mode = rAL & 7;
        FILE *f = dos_fopen(name, mode == 0 ? "rb" : "r+b");
        if (!f && mode != 0) f = dos_fopen(name, "rb");
        free(name);
        int h = f ? alloc_handle(f) : -1;
        if (h < 0) { if (f) fclose(f); dos_error(2); break; }
        rAX = (uint16_t)h; set_cf(0);
        break;
    }
    case 0x3e: { /* close */
        uint16_t h = rBX;
        if (h < MAX_HANDLES && handles[h]) { fclose(handles[h]); handles[h] = NULL; }
        set_cf(0);
        break;
    }
    case 0x3f: { /* read */
        uint16_t h = rBX, n = rCX;
        if (h >= MAX_HANDLES || !handles[h]) {
            if (h <= 2) { rAX = 0; set_cf(0); break; }
            dos_error(6); break;
        }
        uint32_t dst = dsdx();
        size_t got = 0;
        uint8_t buf[4096];
        while (got < n) {
            size_t want = n - got > sizeof buf ? sizeof buf : n - got;
            size_t k = fread(buf, 1, want, handles[h]);
            for (size_t i = 0; i < k; i++) MEMW8(SEGLIN(rDS, (uint16_t)(rDX + got + i)), buf[i]);
            got += k;
            if (k < want) break;
        }
        (void)dst;
        rAX = (uint16_t)got; set_cf(0);
        break;
    }
    case 0x40: { /* write */
        uint16_t h = rBX, n = rCX;
        if (h <= 2) {
            for (uint16_t i = 0; i < n; i++) fputc(MEMR8(SEGLIN(rDS, (uint16_t)(rDX + i))), stderr);
            rAX = n; set_cf(0); break;
        }
        if (h >= MAX_HANDLES || !handles[h]) { dos_error(6); break; }
        if (n == 0) { /* truncate at current position */
            fflush(handles[h]);
            rAX = 0; set_cf(0); break;
        }
        for (uint16_t i = 0; i < n; i++) fputc(MEMR8(SEGLIN(rDS, (uint16_t)(rDX + i))), handles[h]);
        rAX = n; set_cf(0);
        break;
    }
    case 0x41: { /* delete */
        char *name = rt_strdup_far(dsdx());
        char base[256], path[1400];
        dos_basename(name, base, sizeof base);
        free(name);
        snprintf(path, sizeof path, "%s/%s", g_user_dir, base);
        if (remove(path) != 0) dos_error(2); else set_cf(0);
        break;
    }
    case 0x42: { /* lseek */
        uint16_t h = rBX;
        if (h >= MAX_HANDLES || !handles[h]) { dos_error(6); break; }
        int32_t off = (int32_t)(((uint32_t)rCX << 16) | rDX);
        int whence = rAL == 0 ? SEEK_SET : rAL == 1 ? SEEK_CUR : SEEK_END;
        if (fseek(handles[h], off, whence) != 0) { dos_error(25); break; }
        long pos = ftell(handles[h]);
        rDX = (uint16_t)((uint32_t)pos >> 16); rAX = (uint16_t)pos; set_cf(0);
        break;
    }
    case 0x43: { /* get/set attributes */
        char *name = rt_strdup_far(dsdx());
        FILE *f = dos_fopen(name, "rb");
        free(name);
        if (!f) { dos_error(2); break; }
        fclose(f);
        rCX = 0x20; set_cf(0);
        break;
    }
    case 0x44: /* IOCTL */
        if (rAL == 0) { rDX = rBX <= 2 ? 0x80d3 : 0x0002; set_cf(0); }
        else { rAX = 0; set_cf(0); }
        break;
    case 0x45: { /* dup */
        uint16_t h = rBX;
        if (h >= MAX_HANDLES || !handles[h]) { dos_error(6); break; }
        rAX = h; set_cf(0); /* shallow: same FILE* would double close; avoid by reusing handle */
        break;
    }
    case 0x4e: case 0x4f: /* find first/next: nothing */
        dos_error(18); break;
    case 0x56: dos_error(5); break;
    case 0x57: /* file date/time */
        rCX = 0; rDX = 0; set_cf(0); break;
    case 0x59: rAX = 0; rBX = 0; rCX = 0; set_cf(0); break;
    case 0x4c: rt_log("program requested exit (%d)", rAL); exit(rAL);
    case 0x25: case 0x35: rBX = 0; rES = 0; break; /* interrupt vectors: ignore */
    case 0x2f: rES = rDS; rBX = 0x80; break;
    case 0x48: dos_error(8); break;
    default:
        rt_log("warning: unhandled INT 21h AH=%02x AX=%04x", ah, rAX);
        set_cf(1);
        break;
    }
}
