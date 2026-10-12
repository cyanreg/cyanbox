/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The rxbox half of tests/export/port_check.sh: builds a library from the
 * same description the script turns into dj sidecars, and exports it.
 *
 *   rx_port_export SPEC LIBRARY_ROOT STICK
 *
 * SPEC, one item per line (paths absolute):
 *   track <path> <bpm> <downbeat s> <key>
 *   cue <path> <A..H or -> <time s> <loop beats, 0 for none> <name or ->
 *   playlist <name> <path> [<path> ...]
 *   setting <player|mixer|device>.<name> <value>
 *
 * Tracks are added in path order so their library ids are dj's (1..n in
 * path order); the library root is dj's, so the per-library constant
 * (crc32 of the root) agrees; the device name is empty, as dj wrote it.
 * Waveforms are computed from the files as the analysis module does. */

#include <stdio.h>

#include "rxbox/export.h"

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool progress(void *opaque, const char *stage, int done, int total, const char *item)
{
    if (item && strcmp(stage, "copy") && strcmp(stage, "skip") && strcmp(stage, "analysis") &&
        strcmp(stage, "artwork"))
        fprintf(stderr, "[%s] %s\n", stage, item);
    return true;
}

static int add_kv(RXKv **kv, int *nb, const char *key, const char *value)
{
    RXKv *n = realloc(*kv, (size_t)(*nb + 1) * sizeof(*n));
    if (!n)
        return RX_ERROR(ENOMEM);
    *kv = n;
    n[*nb] = (RXKv) { .key = rx_strdup(key), .value = rx_strdup(value) };
    (*nb)++;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s SPEC LIBRARY_ROOT STICK\n", argv[0]);
        return 2;
    }
    uint8_t *spec;
    size_t size;
    if (rx_file_read(argv[1], &spec, &size) < 0) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    char **lines = NULL;
    int nl = 0;
    char *save = NULL;
    for (char *l = strtok_r((char *)spec, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        lines = realloc(lines, (size_t)(nl + 1) * sizeof(*lines));
        lines[nl++] = l;
    }

    RXLibrary lib = { 0 };
    if (rx_library_open(&lib, argv[2]) < 0) {
        fprintf(stderr, "cannot open the library at %s\n", argv[2]);
        return 1;
    }
    /* Tracks first, in path order. */
    char **tracks = NULL;
    int nt = 0;
    for (int i = 0; i < nl; i++) {
        if (strncmp(lines[i], "track ", 6))
            continue;
        tracks = realloc(tracks, (size_t)(nt + 1) * sizeof(*tracks));
        tracks[nt++] = lines[i] + 6;
    }
    qsort(tracks, (size_t)nt, sizeof(*tracks), cmp_str);
    for (int i = 0; i < nt; i++) {
        char path[1024], key[RX_KEY_MAX];
        double bpm, downbeat;
        if (sscanf(tracks[i], "%1023s %lf %lf %7s", path, &bpm, &downbeat, key) != 4)
            continue;
        int err = 0;
        RXTrack *t = rx_library_add_file(&lib, path, &err);
        if (!t) {
            fprintf(stderr, "%s: %s\n", path, strerror(-err));
            return 1;
        }
        rx_grid_init(&t->grid, downbeat, bpm);
        snprintf(t->key, sizeof(t->key), "%s", key);
        RXAudio a;
        RXWaveforms w;
        if (rx_decode(t->path, 1, 0, &a) >= 0 && rx_waveforms_compute(a.samples, a.nb_frames, a.rate, &w) >= 0) {
            rx_library_waveforms_save(&lib, t->id, &w);
            rx_waveforms_free(&w);
        }
        rx_audio_free(&a);
        rx_library_save_track(&lib, t);
    }
    for (int i = 0; i < nl; i++) {
        char path[1024], hot[4], name[128];
        double time, loop;
        if (sscanf(lines[i], "cue %1023s %3s %lf %lf %127s", path, hot, &time, &loop, name) == 5) {
            RXTrack *t = rx_library_find_path(&lib, path);
            if (!t)
                continue;
            RXCue c;
            rx_cue_init(&c, time);
            c.hot = hot[0] >= 'A' && hot[0] <= 'H' ? hot[0] - 'A' : -1;
            if (loop > 0)
                c.loop_end = time + loop * 60.0 / rx_grid_bpm_at(&t->grid, time);
            if (strcmp(name, "-"))
                snprintf(c.name, sizeof(c.name), "%s", name);
            rx_track_add_cue(t, &c);
            rx_library_save_track(&lib, t);
        } else if (!strncmp(lines[i], "playlist ", 9)) {
            char *rest = lines[i] + 9, *sp = strchr(rest, ' ');
            if (!sp)
                continue;
            *sp = '\0';
            RXPlaylist *p = rx_library_playlist_create(&lib, lib.root_folder, rest);
            char *s2 = NULL;
            for (char *tp = strtok_r(sp + 1, " ", &s2); p && tp; tp = strtok_r(NULL, " ", &s2)) {
                RXTrack *t = rx_library_find_path(&lib, tp);
                if (t)
                    rx_library_playlist_add(&lib, p, t, -1);
            }
        } else if (!strncmp(lines[i], "setting ", 8)) {
            char k[64], v[64];
            if (sscanf(lines[i] + 8, "%63s %63s", k, v) != 2)
                continue;
            char *dot = strchr(k, '.');
            if (!dot)
                continue;
            *dot = '\0';
            RXConfig *c = &lib.cfg;
            if (!strcmp(k, "player"))
                add_kv(&c->player, &c->nb_player, dot + 1, v);
            else if (!strcmp(k, "mixer"))
                add_kv(&c->mixer, &c->nb_mixer, dot + 1, v);
            else if (!strcmp(k, "device"))
                add_kv(&c->device, &c->nb_device, dot + 1, v);
        }
    }
    rx_library_config_save(&lib);

    RXExportOptions opt;
    rx_export_options_default(&opt);
    opt.device_name = "";
    opt.whole_library = true;          /* dj exported every track */
    opt.ignore_unsynced = true;
    int ret = rx_export(&lib, argv[3], &opt, progress, NULL);
    if (ret < 0)
        fprintf(stderr, "export failed: %s\n", strerror(-ret));
    rx_library_close(&lib);
    free(tracks);
    free(lines);
    free(spec);
    return ret < 0;
}
