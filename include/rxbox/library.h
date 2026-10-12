/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_LIBRARY_H
#define RXBOX_LIBRARY_H

/* The library: one SQLite database that holds everything about every
 * track: file facts and tags, the user's overrides, grid, key, cues,
 * rating, colour, My Tags, play history, the waveform bytes, the artwork
 * thumbnail, the playlists and folders, and the settings. Owned by one
 * thread (the main thread in the interface, the CLI otherwise); background
 * work never touches it, the owner applies results.
 *
 *   <root>/library.db          (WAL mode, foreign keys on, user_version = schema version)
 *   <root>/cache/transcode/    (compatibility transcodes for export; disposable)
 *
 * Schema (src/core/db.c owns it; migrations bump user_version):
 *   track          id, path (unique, absolute), mtime, file_size, codec, container,
 *                  duration, total_samples, sample_rate, channels, bit_rate, bit_depth,
 *                  has_artwork, file_* (the tags as read from the file), the override
 *                  columns (NULL = use the file's), year, track_number, disc_number,
 *                  rating, color, key, key_confidence, grid_confidence, grid_flags,
 *                  grid_locked, added, analysed, play_count, last_played, missing
 *   grid_section   track_id, idx, start, bpm, beat
 *   cue            id, track_id, time, grid_beat, hot, loop_end, loop_beats, active, name, color
 *   track_tag      track_id, tag
 *   waveform       track_id, data (the packed tags, rx_waveforms_pack)
 *   artwork        track_id, jpeg, width, height (a 240 px thumbnail)
 *   play           track_id, played_at
 *   playlist       id, parent_id (NULL = root), name, is_folder, position
 *   playlist_entry playlist_id, position, track_id
 *   setting        key, value (text)
 *
 * Every public edit is one transaction and bumps `generation`, which the
 * interface watches to rebuild its rows. The whole track list is kept in
 * memory (a few thousand rows) and written through; the database is the
 * truth on disk, the arrays are its image. */

#include "rxbox/model.h"

/* A My Settings preset as the settings hold it: name/value pairs, names as
 * `rxbox settings` lists them. */
typedef struct RxKv { char *key; char *value; } RxKv;

/* The settings, read from the `setting` table at open and written back by
 * rx_library_config_save. Keys in the table are "analysis.bpm_min",
 * "export.key_notation", "player.quantize" (a My Settings name), "ui.wave_palette"... */
typedef struct RxConfig {
    /* analysis.* */
    double bpm_min, bpm_max;    /* 0 = unset */
    bool   snap_bpm;            /* default true */
    bool   auto_cues;           /* memory cue on the first downbeat, default true */

    /* export.* */
    char   key_notation[8];     /* classic (default), camelot, open */
    bool   export_pdb, export_onelibrary;   /* both default true */
    bool   compat_transcode;    /* AAC and hi-res to 16/44.1 FLAC on the stick */
    char  *device_name;         /* NULL = stick's label */

    /* player.* mixer.* device.*: My Settings values the user set */
    RxKv *player; int nb_player;
    RxKv *mixer;  int nb_mixer;
    RxKv *device; int nb_device;

    /* audio.* */
    char *audio_sink;           /* PipeWire node name, NULL = default */

    /* ui.* */
    char  wave_palette[8];      /* 3band (default), blue, rgb */
    int   detail_bars;          /* 12 */
    float split_player, split_tree;   /* remembered splitter positions, 0 = default */
} RxConfig;

void rx_config_free(RxConfig *c);
void rx_config_defaults(RxConfig *c);
/* Where the library is: $RXBOX_LIBRARY, else ~/.local/share/rxbox. Allocating. */
char *rx_library_default_root(void);

/* ---------------------------------------------------------------- playlists */

typedef struct RxPlaylist {
    int64_t   id;
    char     *name;
    uint64_t *track_ids;        /* in order */
    int       nb;
} RxPlaylist;

typedef struct RxPlaylistFolder {
    int64_t id;                 /* 0 for the root */
    char   *name;               /* "" for the root */
    struct RxPlaylistFolder **folders; int nb_folders;   /* in position order */
    RxPlaylist **playlists;              int nb_playlists;
} RxPlaylistFolder;

/* ------------------------------------------------------------------ library */

typedef struct RxDb RxDb;       /* src/core/db.c: the sqlite3 handle and statements */

typedef struct RxLibrary {
    char     *root;
    RxConfig  cfg;
    RxDb     *db;
    RxTrack **tracks;           /* owned; sorted by id */
    int       nb_tracks;
    RxPlaylistFolder *root_folder;   /* playlist tree, owned */
    unsigned  generation;       /* bumped on every change; the interface watches it */
} RxLibrary;

/* Opens (creating when absent) the library at `root` (NULL = default),
 * migrates the schema if needed, loads every track and playlist, checks
 * which audio files are missing (stat; such tracks are kept, flagged
 * `missing` and shown greyed out; they come back when the file does). */
int  rx_library_open(RxLibrary *lib, const char *root);
void rx_library_close(RxLibrary *lib);
/* Re-checks `missing` on every track (also called by open). Returns how
 * many changed state. */
int  rx_library_rescan_missing(RxLibrary *lib);

RxTrack *rx_library_find_id(const RxLibrary *lib, uint64_t id);
RxTrack *rx_library_find_path(const RxLibrary *lib, const char *abs_path);
int      rx_library_index_of(const RxLibrary *lib, const RxTrack *t);

/* Adds one audio file: probes it, fills file_meta from its tags (the tag
 * names dj's library.track_from_probe used), inserts it. Returns the track
 * (the existing one if the path is already in the library; its `missing`
 * flag is cleared when the file is back), or NULL with *err set. */
RxTrack *rx_library_add_file(RxLibrary *lib, const char *path, int *err);
/* Adds a file or everything under a directory with a known audio extension
 * (rx_is_audio_path), in one transaction per directory. Calls `cb` per file
 * added or skipped. Returns the number added or a negative errno. */
typedef void (*rx_add_cb)(void *opaque, const char *path, RxTrack *added, int err);
int      rx_library_add_path(RxLibrary *lib, const char *path, rx_add_cb cb, void *opaque);
bool     rx_is_audio_path(const char *path);
/* Removes a track and everything that refers to it (cues, waveform,
 * artwork, plays, playlist entries). */
int      rx_library_remove(RxLibrary *lib, RxTrack *t);
/* Writes a track's editable state (meta overrides, rating, colour, tags,
 * key, grid, cues, locked) in one transaction. Call after any edit. */
int      rx_library_save_track(RxLibrary *lib, RxTrack *t);
/* Re-probes a track whose file changed (mtime); keeps grid/key/cues. */
int      rx_library_refresh_track(RxLibrary *lib, RxTrack *t);
/* Records a play now (play_count, last_played, the play table). */
int      rx_library_record_play(RxLibrary *lib, RxTrack *t);
/* Moves a track to a new path (the file was moved by the user). */
int      rx_library_relocate(RxLibrary *lib, RxTrack *t, const char *new_abs_path);

/* Waveforms and artwork, by track id. Load returns -ENOENT when absent. */
struct RxWaveforms;
int  rx_library_waveforms_save(RxLibrary *lib, uint64_t id, const struct RxWaveforms *w);
int  rx_library_waveforms_load(const RxLibrary *lib, uint64_t id, struct RxWaveforms *w);
bool rx_library_has_waveforms(const RxLibrary *lib, uint64_t id);
int  rx_library_artwork_save(RxLibrary *lib, uint64_t id, const uint8_t *jpeg, size_t size, int width, int height);
int  rx_library_artwork_load(const RxLibrary *lib, uint64_t id, uint8_t **jpeg, size_t *size, int *width, int *height);

/* Settings. */
int  rx_library_config_save(RxLibrary *lib);          /* writes lib->cfg to the setting table */
/* Raw access for anything not in RxConfig (the interface's column layout...). */
int  rx_library_setting_get(const RxLibrary *lib, const char *key, char **value);   /* -ENOENT when absent */
int  rx_library_setting_set(RxLibrary *lib, const char *key, const char *value);

/* Disposable files (transcodes): <root>/cache/<kind>/<id>.<ext>, directories created. */
char *rx_library_cache_path(const RxLibrary *lib, const char *kind, uint64_t id, const char *ext);

/* Playlists. Every edit is a transaction and bumps generation. Positions
 * among siblings are kept dense. */
RxPlaylist       *rx_library_playlist_create(RxLibrary *lib, RxPlaylistFolder *in, const char *name);
RxPlaylistFolder *rx_library_folder_create(RxLibrary *lib, RxPlaylistFolder *in, const char *name);
int   rx_library_playlist_rename(RxLibrary *lib, RxPlaylist *p, const char *name);
int   rx_library_folder_rename(RxLibrary *lib, RxPlaylistFolder *f, const char *name);
int   rx_library_playlist_delete(RxLibrary *lib, RxPlaylist *p);
int   rx_library_folder_delete(RxLibrary *lib, RxPlaylistFolder *f);     /* and everything in it */
int   rx_library_playlist_add(RxLibrary *lib, RxPlaylist *p, const RxTrack *t, int at);   /* at = -1: append; a track may appear once per playlist */
int   rx_library_playlist_remove_at(RxLibrary *lib, RxPlaylist *p, int index);
int   rx_library_playlist_move(RxLibrary *lib, RxPlaylist *p, int from, int to);
/* Moves a playlist or folder under another folder, at `position` (-1: last). */
int   rx_library_playlist_reparent(RxLibrary *lib, RxPlaylist *p, RxPlaylistFolder *to, int position);
int   rx_library_folder_reparent(RxLibrary *lib, RxPlaylistFolder *f, RxPlaylistFolder *to, int position);
/* Every playlist, depth first: returns the count, fills up to max. */
int   rx_library_playlists_flat(const RxLibrary *lib, RxPlaylist **out, int max);
RxPlaylistFolder *rx_library_folder_of(const RxLibrary *lib, const RxPlaylist *p);
RxPlaylist       *rx_library_playlist_find(const RxLibrary *lib, const char *name);   /* first by name, depth first */

/* m3u8 import/export (paths relative to the file, resolved on read). */
int   rx_library_playlist_import_m3u(RxLibrary *lib, RxPlaylistFolder *in, const char *m3u_path, RxPlaylist **out);
int   rx_library_playlist_export_m3u(const RxLibrary *lib, const RxPlaylist *p, const char *m3u_path);

#endif /* RXBOX_LIBRARY_H */
