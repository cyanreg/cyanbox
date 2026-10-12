/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_LIBRARY_H
#define RXBOX_LIBRARY_H

/* The library: a directory with rxbox.toml, tracks/ (sidecars), playlists/
 * (m3u8 files and folders) and .cache/. Owned by one thread (the main
 * thread in the interface, the CLI otherwise). Background work never
 * touches it; results are applied here by the owner.
 *
 *   <root>/rxbox.toml
 *   <root>/tracks/home/lynne/Music/Album/01 Track.flac.toml   (mirrors the absolute path)
 *   <root>/playlists/Warmup.m3u8, <root>/playlists/Folder/Peak.m3u8
 *   <root>/.cache/wave/<id>.rxw, <root>/.cache/art/<id>.png, <root>/.cache/transcode/...
 *
 * Playlist entries are absolute audio paths (m3u8 is written with paths
 * relative to the playlist file, resolved on read, exactly as dj did). */

#include "rxbox/model.h"

/* A My Settings preset as the config holds it: name/value pairs, names as
 * `rxbox settings` lists them. */
typedef struct RxKv { char *key; char *value; } RxKv;

typedef struct RxConfig {
    char *root;                 /* absolute library directory */
    char *sidecars;             /* absolute: tracks/ */
    char *playlists;            /* absolute: playlists/ */
    char *cache;                /* absolute: .cache/ */

    /* [analysis] */
    double bpm_min, bpm_max;    /* 0 = unset */
    bool   snap_bpm;            /* default true */
    bool   auto_cues;           /* memory cue on the first downbeat, default true */

    /* [export] */
    char   key_notation[8];     /* classic (default), camelot, open */
    bool   export_pdb, export_onelibrary;   /* both default true */
    bool   compat_transcode;    /* AAC and hi-res to 16/44.1 FLAC on the stick */
    char  *device_name;         /* NULL = stick's label */

    /* [player] [mixer] [device]: only the keys the user wrote */
    RxKv *player; int nb_player;
    RxKv *mixer;  int nb_mixer;
    RxKv *device; int nb_device;

    /* [audio] */
    char *audio_sink;           /* PipeWire node name, NULL = default */

    /* [ui] remembered interface state */
    char  wave_palette[8];      /* 3band (default), blue, rgb */
    int   detail_bars;          /* 12 */
} RxConfig;

void rx_config_free(RxConfig *c);
/* Reads <root>/rxbox.toml (defaults when absent). */
int  rx_config_read(const char *root, RxConfig *c);
int  rx_config_write(const RxConfig *c);
/* Where the library is: $RXBOX_LIBRARY, else ~/.local/share/rxbox. Allocating. */
char *rx_library_default_root(void);

/* ---------------------------------------------------------------- playlists */

typedef struct RxPlaylist {
    char     *name;             /* shown; the file name without ".m3u8" and a leading "NN " order prefix */
    char     *path;             /* absolute .m3u8 path */
    uint64_t *track_ids;        /* in order; 0 for entries whose file is not in the library */
    char    **track_paths;      /* parallel: absolute paths as resolved from the file */
    int       nb;
} RxPlaylist;

typedef struct RxPlaylistFolder {
    char *name;                 /* "" for the root */
    char *path;                 /* absolute directory */
    struct RxPlaylistFolder **folders; int nb_folders;
    RxPlaylist **playlists;              int nb_playlists;
} RxPlaylistFolder;

/* ------------------------------------------------------------------ library */

typedef struct RxLibrary {
    RxConfig  cfg;
    RxTrack **tracks;           /* owned; sorted by id */
    int       nb_tracks;
    uint64_t  next_id;
    RxPlaylistFolder *root;     /* playlist tree, owned */
    unsigned  generation;       /* bumped on every change; the interface watches it */
} RxLibrary;

/* Opens (creating when absent) the library at `root` (NULL = default).
 * Reads every sidecar and playlist. Missing audio files are kept and
 * flagged `missing`. */
int  rx_library_open(RxLibrary *lib, const char *root);
void rx_library_close(RxLibrary *lib);

RxTrack *rx_library_find_id(const RxLibrary *lib, uint64_t id);
RxTrack *rx_library_find_path(const RxLibrary *lib, const char *abs_path);
int      rx_library_index_of(const RxLibrary *lib, const RxTrack *t);

/* Adds one audio file: probes it, assigns an id, writes its sidecar (meta
 * from the file, no grid yet). Returns the track (existing one if already
 * in the library), or NULL with *err set. */
RxTrack *rx_library_add_file(RxLibrary *lib, const char *path, int *err);
/* Adds a file or everything under a directory with a known audio extension
 * (rx_is_audio_path). Calls `cb` per file added or skipped. Returns the
 * number added or a negative errno. */
typedef void (*rx_add_cb)(void *opaque, const char *path, RxTrack *added, int err);
int      rx_library_add_path(RxLibrary *lib, const char *path, rx_add_cb cb, void *opaque);
bool     rx_is_audio_path(const char *path);
/* Removes a track: deletes its sidecar and cache files, drops it from every
 * playlist. */
int      rx_library_remove(RxLibrary *lib, RxTrack *t);
/* Writes a track's sidecar (after any edit: cues, grid, meta). */
int      rx_library_save_track(RxLibrary *lib, RxTrack *t);
/* Re-probes a track whose file changed (mtime); keeps grid/key/cues. */
int      rx_library_refresh_track(RxLibrary *lib, RxTrack *t);

/* Cache paths (allocating). */
char *rx_library_cache_path(const RxLibrary *lib, const char *kind, uint64_t id, const char *ext);

/* Playlists. Every edit rewrites the file(s) involved and bumps generation. */
RxPlaylist       *rx_library_playlist_create(RxLibrary *lib, RxPlaylistFolder *in, const char *name);
RxPlaylistFolder *rx_library_folder_create(RxLibrary *lib, RxPlaylistFolder *in, const char *name);
int   rx_library_playlist_rename(RxLibrary *lib, RxPlaylist *p, const char *name);
int   rx_library_folder_rename(RxLibrary *lib, RxPlaylistFolder *f, const char *name);
int   rx_library_playlist_delete(RxLibrary *lib, RxPlaylist *p);
int   rx_library_folder_delete(RxLibrary *lib, RxPlaylistFolder *f);     /* and everything in it */
int   rx_library_playlist_add(RxLibrary *lib, RxPlaylist *p, const RxTrack *t, int at);   /* at = -1: append */
int   rx_library_playlist_remove_at(RxLibrary *lib, RxPlaylist *p, int index);
int   rx_library_playlist_move(RxLibrary *lib, RxPlaylist *p, int from, int to);
int   rx_library_playlist_save(RxLibrary *lib, RxPlaylist *p);
/* Every playlist, depth first, for lookups: returns count, fills up to max. */
int   rx_library_playlists_flat(const RxLibrary *lib, RxPlaylist **out, int max);
RxPlaylistFolder *rx_library_folder_of(const RxLibrary *lib, const RxPlaylist *p);

/* m3u8 helpers used by the playlist code and the stick writer. */
int   rx_m3u_read(const char *path, char ***abs_paths, int *nb);
int   rx_m3u_write(const char *path, const char *const *abs_paths, int nb);

#endif /* RXBOX_LIBRARY_H */
