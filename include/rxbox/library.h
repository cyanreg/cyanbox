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
 *                  rating, hue (REAL, the track colour: the HSV/HSL hue angle in
 *                  degrees, 0 <= hue < 360, 0 red, 60 yellow, 120 green, 180 cyan,
 *                  240 blue, 300 magenta; NULL = none; RXMeta.hue, rxbox/model.h.
 *                  Schema 4 replaced the colour name with it), key, key_confidence,
 *                  grid_confidence, grid_flags, grid_locked, added, analysed,
 *                  play_count, last_played, missing,
 *                  beats_version, key_version, waves_version (the analyser that made
 *                  the grid, key and waveforms: RX_*_VERSION, 0 before schema 2),
 *                  tags_version, ml_club, ml_intensity, ml_valence, ml_vocals (the
 *                  model's tagging, rxbox/ml.h: RX_ML_TAGS_VERSION, 0 never, and its
 *                  measures, NULL never; schema 5), ml_colors, ml_legend (the
 *                  probability it gave each of the eight colours, NULL none, and the
 *                  hash of the legend it picked from, 0 none; schema 7)
 *   grid_section   track_id, idx, start, bpm, beat, measured (1: one measured beat of
 *                  a dynamic grid, RXTempoSection.measured; schema 6)
 *   cue            id, track_id, time, grid_beat, hot, loop_end, loop_beats, active, name, color
 *   track_tag      track_id, position, tag
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
typedef struct RXKv { char *key; char *value; } RXKv;

/* The settings, read from the `setting` table at open and written back by
 * rx_library_config_save. Keys in the table are "analysis.bpm_min",
 * "export.key_notation", "player.quantize" (a My Settings name),
 * "ui.traffic_light"... The interface's waveform colour, key notation and
 * the deck's startup state are the players' My Settings (device.*,
 * player.*), not ui.* keys: rxbox shows what the stick will carry. */
typedef struct RXConfig {
    /* analysis.* */
    double bpm_min, bpm_max;    /* 0 = unset */
    bool   snap_bpm;            /* default true */
    bool   auto_cues;           /* memory cue on the first downbeat, default true */
    bool   allow_sections;      /* tempo sections where the tempo or phase really changes,
                                   default true (RXAnalysisOptions.allow_sections) */
    unsigned analyse_what;      /* RXAnalyseWhat the interface's Analyse asks for,
                                   default RX_ANALYSE_ALL */

    /* import.* */
    bool   import_comment;      /* take the file's comment tag when a track is added
                                   (or its file re-read); default false: the comment
                                   is the user's, and starts empty */

    /* export.* */
    char   key_notation[8];     /* classic (default), camelot, open */
    bool   export_pdb, export_onelibrary;   /* both default true */
    bool   compat_transcode;    /* AAC and hi-res to 16/44.1 FLAC on the stick */
    char  *device_name;         /* NULL = stick's label */

    /* player.* mixer.* device.*: My Settings values the user set */
    RXKv *player; int nb_player;
    RXKv *mixer;  int nb_mixer;
    RXKv *device; int nb_device;

    /* audio.* */
    char *audio_sink;           /* PipeWire node name, NULL = default */

    /* ml.* */
    char *ml_model;             /* the model's GGUF file for tagging (rxbox/ml.h), NULL:
                                   $RXBOX_ML_MODEL, else models/ (rx_app_ml_model) */

    /* color.*: the legend, what each track colour stands for, by colour id
     * - 1 (color.pink .. color.purple, rxbox/colors.h); the tagging model
     * picks the colour whose words fit a track best. NULL: the default,
     * RX_TRACK_VIBE_*. Read through rx_config_vibe. */
    char *vibes[8];

    /* ui.* */
    char  traffic_light[12];    /* the browser's key highlight (rx_key_reach_name): off,
                                   same, related1, related2, related3 (default) */
    int   detail_bars;          /* 12 */
    float split_player, split_tree;   /* remembered splitter positions, 0 = default */
} RXConfig;

void rx_config_free(RXConfig *c);
void rx_config_defaults(RXConfig *c);
/* One of RXConfig's own settings by its key as the setting table holds it
 * (analysis.*, import.comment, export.*, audio.sink, ml.model, color.*,
 * ui.traffic_light, ui.detail_bars, ui.split_*): 0 when `value` is valid
 * for the key and now in c, an empty one (or NULL) putting the default
 * back; -EINVAL when it is not valid (-E2BIG for a description too long),
 * c unchanged; -ENOENT for any other key (the My Settings player.*,
 * mixer.*, device.* are rxbox/formats.h's to check, and other keys are
 * the interface's own, free for rx_library_setting_set). */
int  rx_config_set(RXConfig *c, const char *key, const char *value);
/* What a key of RXConfig takes, in words ("true or false"); NULL for any
 * other key. */
const char *rx_config_takes(const char *key);
/* The legend: the description of track colour `id` (1..8) and its setting
 * key ("color.pink"); NULL for another id. rx_config_vibe is the user's
 * words or the default's. */
const char *rx_config_vibe(const RXConfig *c, int id);
const char *rx_config_vibe_default(int id);
const char *rx_config_vibe_key(int id);
/* The longest description the tagger is given, in bytes: what the
 * interface's field holds, and room in the model's question. */
#define RX_CONFIG_VIBE_MAX 127
/* Sets colour `id`'s description, trimmed; NULL, blank or the default's
 * own words go back to the default (and the row away at the next save).
 * -EINVAL for another id, -E2BIG for one longer than RX_CONFIG_VIBE_MAX
 * once trimmed. */
int  rx_config_set_vibe(RXConfig *c, int id, const char *text);
/* All eight, by id - 1, as rx_ml_tag takes them. */
void rx_config_legend(const RXConfig *c, const char *legend[8]);
/* Where the library is: $RXBOX_LIBRARY, else ~/.local/share/rxbox. Allocating. */
char *rx_library_default_root(void);

/* ---------------------------------------------------------------- playlists */

typedef struct RXPlaylist {
    int64_t   id;
    char     *name;
    uint64_t *track_ids;        /* in order */
    int       nb;
} RXPlaylist;

typedef struct RXPlaylistFolder {
    int64_t id;                 /* 0 for the root */
    char   *name;               /* "" for the root */
    struct RXPlaylistFolder **folders; int nb_folders;   /* in position order */
    RXPlaylist **playlists;              int nb_playlists;
} RXPlaylistFolder;

/* ------------------------------------------------------------------ library */

typedef struct RXDb RXDb;       /* src/core/db.c: the sqlite3 handle and statements */
typedef struct RXPathIndex RXPathIndex;   /* src/core/library.c: the tracks by path */

typedef struct RXLibrary {
    char     *root;
    RXConfig  cfg;
    RXDb     *db;
    RXTrack **tracks;           /* owned; sorted by id */
    int       nb_tracks;
    RXPlaylistFolder *root_folder;   /* playlist tree, owned */
    unsigned  generation;       /* bumped on every change; the interface watches it */
    RXPathIndex *by_path;       /* rx_library_find_path's; kept by the library's own edits */
} RXLibrary;

/* Opens (creating when absent) the library at `root` (NULL = default),
 * converts one of schema 8 (src/core/db.c), loads every track and
 * playlist, checks which audio files are missing (stat; such tracks are
 * kept, flagged `missing` and shown greyed out; they come back when the
 * file does). */
int  rx_library_open(RXLibrary *lib, const char *root);
void rx_library_close(RXLibrary *lib);
/* Re-checks `missing` on every track (also called by open). Returns how
 * many changed state. */
int  rx_library_rescan_missing(RXLibrary *lib);
/* Runs SQLite's integrity_check and foreign_key_check: 0 when both pass,
 * -EIO with every problem logged otherwise. */
int  rx_library_check(const RXLibrary *lib);

RXTrack *rx_library_find_id(const RXLibrary *lib, uint64_t id);
RXTrack *rx_library_find_path(const RXLibrary *lib, const char *abs_path);
int      rx_library_index_of(const RXLibrary *lib, const RXTrack *t);

/* Adds one audio file: probes it, fills file_meta from its tags (the tag
 * names dj's library.track_from_probe used; the comment only when
 * cfg.import_comment is set, the file itself is never written), inserts
 * it. Returns the track
 * (the existing one if the path is already in the library; its `missing`
 * flag is cleared when the file is back), or NULL with *err set. */
RXTrack *rx_library_add_file(RXLibrary *lib, const char *path, int *err);
/* Adds a file or everything under a directory with a known audio extension
 * (rx_is_audio_path), in one transaction per directory, then its
 * subdirectories by name. A directory's files go in album by album (an
 * album is an album tag with its album artist's; the albums in the order
 * of their first files by name), each album in the order of its disc and
 * track tags (unnumbered ones after, by name), so that each album goes in
 * as it runs even in a folder of several; files without an album tag go
 * last, by name. Calls `cb` per file added or skipped. Returns the number
 * added or a negative errno. */
typedef void (*rx_add_cb)(void *opaque, const char *path, RXTrack *added, int err);
int      rx_library_add_path(RXLibrary *lib, const char *path, rx_add_cb cb, void *opaque);
bool     rx_is_audio_path(const char *path);
/* Removes a track and everything that refers to it (cues, waveform,
 * artwork, plays, playlist entries). */
int      rx_library_remove(RXLibrary *lib, RXTrack *t);
/* Writes a track's editable state (meta overrides, rating, hue, tags,
 * key, grid, cues, locked, the analyser versions, the model's tagging) in
 * one transaction.
 * Call after any edit. */
int      rx_library_save_track(RXLibrary *lib, RXTrack *t);
/* Re-probes a track whose file changed (mtime); keeps grid/key/cues.
 * Without cfg.import_comment the comment the library holds stays as it is. */
int      rx_library_refresh_track(RXLibrary *lib, RXTrack *t);
/* Records a play now (play_count, last_played, the play table). */
int      rx_library_record_play(RXLibrary *lib, RXTrack *t);
/* Moves a track to a new path (the file was moved by the user). */
int      rx_library_relocate(RXLibrary *lib, RXTrack *t, const char *new_abs_path);

/* Waveforms and artwork, by track id. Load returns -ENOENT when absent. */
struct RXWaveforms;
int  rx_library_waveforms_save(RXLibrary *lib, uint64_t id, const struct RXWaveforms *w);
int  rx_library_waveforms_load(const RXLibrary *lib, uint64_t id, struct RXWaveforms *w);
/* The previews alone (nb_detail 0, no detail tags): a few kB, where the
 * whole is a few hundred, for what draws only the previews. */
int  rx_library_waveforms_load_previews(const RXLibrary *lib, uint64_t id, struct RXWaveforms *w);
bool rx_library_has_waveforms(const RXLibrary *lib, uint64_t id);
/* Whether the stored waveforms are of the format version this build reads,
 * from their header alone (cheap enough for the whole library): 0 when
 * they are, -EPROTO when another version (load would refuse them: make
 * them again), -EINVAL when not waveforms at all, -ENOENT when none. */
int  rx_library_waveforms_check(const RXLibrary *lib, uint64_t id);
/* A track's cover thumbnail. Stored once however many tracks have it:
 * saving the same bytes for another track links it to the stored one,
 * and a cover goes with the last track that has it (removed, or given
 * another cover). */
int  rx_library_artwork_save(RXLibrary *lib, uint64_t id, const uint8_t *jpeg, size_t size, int width, int height);
int  rx_library_artwork_load(const RXLibrary *lib, uint64_t id, uint8_t **jpeg, size_t *size, int *width, int *height);
/* The id of a track's cover, which the tracks sharing it share: > 0, 0
 * when the track has none, or a negative errno. */
int64_t rx_library_artwork_id(const RXLibrary *lib, uint64_t id);

/* Settings. */
int  rx_library_config_save(RXLibrary *lib);          /* writes lib->cfg to the setting table */
/* Raw access for anything not in RXConfig (the interface's column layout...). */
int  rx_library_setting_get(const RXLibrary *lib, const char *key, char **value);   /* -ENOENT when absent */
int  rx_library_setting_set(RXLibrary *lib, const char *key, const char *value);

/* Disposable files (transcodes): <root>/cache/<kind>/<id>.<ext>, directories created. */
char *rx_library_cache_path(const RXLibrary *lib, const char *kind, uint64_t id, const char *ext);

/* Playlists. Every edit is a transaction and bumps generation. Positions
 * among siblings are kept dense. */
RXPlaylist       *rx_library_playlist_create(RXLibrary *lib, RXPlaylistFolder *in, const char *name);
RXPlaylistFolder *rx_library_folder_create(RXLibrary *lib, RXPlaylistFolder *in, const char *name);
int   rx_library_playlist_rename(RXLibrary *lib, RXPlaylist *p, const char *name);
int   rx_library_folder_rename(RXLibrary *lib, RXPlaylistFolder *f, const char *name);
int   rx_library_playlist_delete(RXLibrary *lib, RXPlaylist *p);
int   rx_library_folder_delete(RXLibrary *lib, RXPlaylistFolder *f);     /* and everything in it */
int   rx_library_playlist_add(RXLibrary *lib, RXPlaylist *p, const RXTrack *t, int at);   /* at = -1: append; a track may appear once per playlist */
int   rx_library_playlist_remove_at(RXLibrary *lib, RXPlaylist *p, int index);
int   rx_library_playlist_move(RXLibrary *lib, RXPlaylist *p, int from, int to);
/* Moves a playlist or folder under another folder, at `position` (-1: last). */
int   rx_library_playlist_reparent(RXLibrary *lib, RXPlaylist *p, RXPlaylistFolder *to, int position);
int   rx_library_folder_reparent(RXLibrary *lib, RXPlaylistFolder *f, RXPlaylistFolder *to, int position);
/* Every playlist, depth first: returns the count, fills up to max. */
int   rx_library_playlists_flat(const RXLibrary *lib, RXPlaylist **out, int max);
RXPlaylistFolder *rx_library_folder_of(const RXLibrary *lib, const RXPlaylist *p);
RXPlaylist       *rx_library_playlist_find(const RXLibrary *lib, const char *name);   /* first by name, depth first */

/* m3u8 import/export (paths relative to the file, resolved on read). */
int   rx_library_playlist_import_m3u(RXLibrary *lib, RXPlaylistFolder *in, const char *m3u_path, RXPlaylist **out);
int   rx_library_playlist_export_m3u(const RXLibrary *lib, const RXPlaylist *p, const char *m3u_path);

#endif /* RXBOX_LIBRARY_H */
