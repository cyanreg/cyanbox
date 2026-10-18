/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_LIBRARY_H
#define CYANBOX_LIBRARY_H

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
 * The schema is src/core/db.c's, which describes it table by table: a
 * track's colour is its hue (CBMeta.hue, cyanbox/model.h), and the
 * analysers' versions (CB_*_VERSION) mark what each result was made by.
 * A database not made by cyanbox at this schema is not opened.
 *
 * Every public edit is one transaction and bumps `generation`, which the
 * interface watches to rebuild its rows. The whole track list is kept in
 * memory (a few thousand rows) and written through; the database is the
 * truth on disk, the arrays are its image. */

#include "cyanbox/model.h"

/* A My Settings preset as the settings hold it: name/value pairs, names as
 * `cyanbox settings` lists them. */
typedef struct CBKv { char *key; char *value; } CBKv;

/* The settings, read from the `setting` table at open and written back by
 * cb_library_config_save. Keys in the table are "analysis.bpm_min",
 * "export.key_notation", "player.quantize" (a My Settings name),
 * "ui.traffic_light"... The interface's waveform colour, key notation and
 * the deck's startup state are the players' My Settings (device.*,
 * player.*), not ui.* keys: cyanbox shows what the stick will carry. */
typedef struct CBConfig {
    /* analysis.* */
    double bpm_min, bpm_max;    /* 0 = unset */
    bool   snap_bpm;            /* default true */
    bool   auto_cues;           /* memory cue where the drums come in, default true */
    bool   allow_sections;      /* tempo sections where the tempo or phase really changes,
                                   default true (CBAnalysisOptions.allow_sections) */
    unsigned analyse_what;      /* CBAnalyseWhat the interface's Analyse asks for,
                                   default CB_ANALYSE_ALL */

    /* import.* */
    bool   import_comment;      /* take the file's comment tag when a track is added
                                   (or its file re-read); default false: the comment
                                   is the user's, and starts empty */

    /* export.* */
    char   key_notation[8];     /* classic (default), camelot, open */
    bool   export_pdb, export_onelibrary;   /* both default true */
    bool   compat_transcode;    /* AAC and hi-res to 16/44.1 FLAC on the stick */
    int    export_format;       /* the stick's audio: 0 copy, 1 flac, 2 wav, 3 aiff
                                   (cyanbox/export.h CBStickFormat) */
    char  *device_name;         /* NULL = stick's label */
    int    background_color;    /* the players' background for the stick's library:
                                   0 default, 1..8 pink..purple (cyanbox/colors.h) */

    /* link.* */
    bool   link_mp3;            /* Link converts to 320 kbps MP3 instead of 16-bit WAV */

    /* player.* mixer.* device.*: My Settings values the user set */
    CBKv *player; int nb_player;
    CBKv *mixer;  int nb_mixer;
    CBKv *device; int nb_device;

    /* audio.* */
    char *audio_sink;           /* PipeWire node name, NULL = default */

    /* ml.* */
    char *ml_model;             /* the model's GGUF file for tagging (cyanbox/ml.h), NULL:
                                   $CYANBOX_ML_MODEL, else models/ (cb_app_ml_model) */

    /* color.*: the legend, what each track colour stands for, by colour id
     * - 1 (color.pink .. color.purple, cyanbox/colors.h); the tagging model
     * picks the colour whose words fit a track best. NULL: the default,
     * CB_TRACK_VIBE_*. Read through cb_config_vibe. */
    char *vibes[8];

    /* ui.* */
    char  traffic_light[12];    /* the browser's key highlight (cb_key_reach_name): off,
                                   same, related1, related2, related3 (default) */
    int   detail_bars;          /* 12 */
    float split_player, split_tree;   /* remembered splitter positions, 0 = default */
    bool  rekordbox_colours;    /* the 3-band waveforms drawn as rekordbox draws them
                                   (its colours, whole core and cream top slab) rather
                                   than in cyanbox's own palette; default false */
} CBConfig;

void cb_config_free(CBConfig *c);
void cb_config_defaults(CBConfig *c);
/* One of CBConfig's own settings by its key as the setting table holds it
 * (analysis.*, import.comment, export.*, audio.sink, ml.model, color.*,
 * ui.traffic_light, ui.detail_bars, ui.split_*, ui.rekordbox_colours): 0
 * when `value` is valid
 * for the key and now in c, an empty one (or NULL) putting the default
 * back; -EINVAL when it is not valid (-E2BIG for a description too long),
 * c unchanged; -ENOENT for any other key (the My Settings player.*,
 * mixer.*, device.* are cyanbox/formats.h's to check, and other keys are
 * the interface's own, free for cb_library_setting_set). */
int  cb_config_set(CBConfig *c, const char *key, const char *value);
/* What a key of CBConfig takes, in words ("true or false"); NULL for any
 * other key. */
const char *cb_config_takes(const char *key);
/* The legend: the description of track colour `id` (1..8) and its setting
 * key ("color.pink"); NULL for another id. cb_config_vibe is the user's
 * words or the default's. */
const char *cb_config_vibe(const CBConfig *c, int id);
const char *cb_config_vibe_default(int id);
const char *cb_config_vibe_key(int id);
/* The longest description the tagger is given, in bytes: what the
 * interface's field holds, and room in the model's question. */
#define CB_CONFIG_VIBE_MAX 127
/* Sets colour `id`'s description, trimmed; NULL, blank or the default's
 * own words go back to the default (and the row away at the next save).
 * -EINVAL for another id, -E2BIG for one longer than CB_CONFIG_VIBE_MAX
 * once trimmed. */
int  cb_config_set_vibe(CBConfig *c, int id, const char *text);
/* All eight, by id - 1, as cb_ml_tag takes them. */
void cb_config_legend(const CBConfig *c, const char *legend[8]);
/* Where the library is: $CYANBOX_LIBRARY, else ~/.local/share/cyanbox. Allocating. */
char *cb_library_default_root(void);

/* ---------------------------------------------------------------- playlists */

typedef struct CBPlaylist {
    int64_t   id;
    char     *name;
    uint64_t *track_ids;        /* in order */
    int       nb;
} CBPlaylist;

typedef struct CBPlaylistFolder {
    int64_t id;                 /* 0 for the root */
    char   *name;               /* "" for the root */
    struct CBPlaylistFolder **folders; int nb_folders;   /* in position order */
    CBPlaylist **playlists;              int nb_playlists;
} CBPlaylistFolder;

/* ------------------------------------------------------------------ library */

typedef struct CBDb CBDb;       /* src/core/db.c: the sqlite3 handle and statements */
typedef struct CBPathIndex CBPathIndex;   /* src/core/library.c: the tracks by path */

typedef struct CBLibrary {
    char     *root;
    CBConfig  cfg;
    CBDb     *db;
    CBTrack **tracks;           /* owned; sorted by id */
    int       nb_tracks;
    CBPlaylistFolder *root_folder;   /* playlist tree, owned */
    unsigned  generation;       /* bumped on every change; the interface watches it */
    /* What changed since cb_library_take_changes: the tracks whose own
     * state did (an edit saved, a play, waveforms), or everything else
     * (`changed_all`: tracks added, removed, moved or read again from
     * their files, playlists, settings, artwork). PRO DJ LINK reads only
     * those tracks again (src/app/link.c). */
    uint64_t *changed; int nb_changed;
    bool      changed_all;
    CBPathIndex *by_path;       /* cb_library_find_path's; kept by the library's own edits */
} CBLibrary;

/* The tracks the record of changes holds before it says everything. */
#define CB_LIBRARY_CHANGES_KEPT 512

/* Opens (creating when absent) the library at `root` (NULL = default),
 * loads every track and playlist, checks which audio files are missing
 * (stat; such tracks are kept, flagged `missing` and shown greyed out;
 * they come back when the file does). */
int  cb_library_open(CBLibrary *lib, const char *root);
void cb_library_close(CBLibrary *lib);
/* Re-checks `missing` on every track (also called by open). Returns how
 * many changed state. */
int  cb_library_rescan_missing(CBLibrary *lib);
/* Runs SQLite's integrity_check and foreign_key_check: 0 when both pass,
 * -EIO with every problem logged otherwise. */
int  cb_library_check(const CBLibrary *lib);

CBTrack *cb_library_find_id(const CBLibrary *lib, uint64_t id);
CBTrack *cb_library_find_path(const CBLibrary *lib, const char *abs_path);
int      cb_library_index_of(const CBLibrary *lib, const CBTrack *t);

/* Adds one audio file: probes it, fills file_meta from its tags (the tag
 * names dj's library.track_from_probe used; the comment only when
 * cfg.import_comment is set, the file itself is never written), inserts
 * it. Returns the track
 * (the existing one if the path is already in the library; its `missing`
 * flag is cleared when the file is back), or NULL with *err set. */
CBTrack *cb_library_add_file(CBLibrary *lib, const char *path, int *err);
/* Adds a file or everything under a directory with a known audio extension
 * (cb_is_audio_path), in one transaction per directory, then its
 * subdirectories by name. A directory's files go in album by album (an
 * album is an album tag with its album artist's; the albums in the order
 * of their first files by name), each album in the order of its disc and
 * track tags (unnumbered ones after, by name), so that each album goes in
 * as it runs even in a folder of several; files without an album tag go
 * last, by name. Calls `cb` per file added or skipped. Returns the number
 * added or a negative errno. */
typedef void (*cb_add_cb)(void *opaque, const char *path, CBTrack *added, int err);
int      cb_library_add_path(CBLibrary *lib, const char *path, cb_add_cb cb, void *opaque);
bool     cb_is_audio_path(const char *path);
/* Removes a track and everything that refers to it (cues, waveform,
 * artwork, plays, playlist entries). */
int      cb_library_remove(CBLibrary *lib, CBTrack *t);
/* Writes a track's editable state (meta overrides, rating, hue, tags,
 * key, grid, cues, locked, the analyser versions, the model's tagging) in
 * one transaction.
 * Call after any edit. */
int      cb_library_save_track(CBLibrary *lib, CBTrack *t);
/* Re-probes a track whose file changed (mtime); keeps grid/key/cues.
 * Without cfg.import_comment the comment the library holds stays as it is. */
int      cb_library_refresh_track(CBLibrary *lib, CBTrack *t);
/* Records a play now (play_count, last_played, the play table), or at
 * `when` (epoch seconds: a player's history). */
int      cb_library_record_play(CBLibrary *lib, CBTrack *t);
int      cb_library_record_play_at(CBLibrary *lib, CBTrack *t, int64_t when);
/* Every play the library recorded, oldest first (ties as recorded); the
 * array is the caller's to free. */
typedef struct CBPlay { int64_t track_id, played_at; } CBPlay;
int      cb_library_plays(CBLibrary *lib, CBPlay **out, int *nb);
/* The History: a row for each track a deck on PRO DJ LINK played straight
 * on for 1.5 minutes (src/app/link.c), when it started, and for each one
 * added to it by hand, kept in the library. */
typedef struct CBHistoryEntry {
    int64_t  id;
    uint64_t track_id;
    int64_t  played_at;         /* epoch seconds */
    int      player;            /* the deck's number, 0 added by hand */
} CBHistoryEntry;
int      cb_library_history_add(CBLibrary *lib, const CBTrack *t, int64_t played_at, int player);
/* Every row, oldest first (by played_at, then as added); the caller frees
 * the array. */
int      cb_library_history(CBLibrary *lib, CBHistoryEntry **out, int *nb);
/* Every row of a track: how many there were. */
int      cb_library_history_remove(CBLibrary *lib, uint64_t track_id);
/* Moves a track to a new path (the file was moved by the user). */
int      cb_library_relocate(CBLibrary *lib, CBTrack *t, const char *new_abs_path);
/* The tracks changed since the last call (`*ids`, the caller's to free;
 * NULL and 0 for none): 0; or 1 when everything is to be taken as
 * changed. The record starts again either way. */
int      cb_library_take_changes(CBLibrary *lib, uint64_t **ids, int *nb);
/* A track as the database holds it now, which another connection may
 * have written since this one loaded it (the interface's, for Link's):
 * its row, grid, cues and My Tags, not added to the library; the caller
 * frees it. NULL with *err -ENOENT when there is no such track. */
CBTrack *cb_library_read_track(CBLibrary *lib, uint64_t id, int *err);

/* Waveforms and artwork, by track id. Load returns -ENOENT when absent. */
struct CBWaveforms;
int  cb_library_waveforms_save(CBLibrary *lib, uint64_t id, const struct CBWaveforms *w);
int  cb_library_waveforms_load(const CBLibrary *lib, uint64_t id, struct CBWaveforms *w);
/* The previews alone (nb_detail 0, no detail tags): a few kB, where the
 * whole is a few hundred, for what draws only the previews. */
int  cb_library_waveforms_load_previews(const CBLibrary *lib, uint64_t id, struct CBWaveforms *w);
bool cb_library_has_waveforms(const CBLibrary *lib, uint64_t id);
/* Whether the stored waveforms are of the format version this build reads,
 * from their header alone (cheap enough for the whole library): 0 when
 * they are, -EPROTO when another version (load would refuse them: make
 * them again), -EINVAL when not waveforms at all, -ENOENT when none. */
int  cb_library_waveforms_check(const CBLibrary *lib, uint64_t id);
/* A track's cover thumbnail. Stored once however many tracks have it:
 * saving the same bytes for another track links it to the stored one,
 * and a cover goes with the last track that has it (removed, or given
 * another cover). */
int  cb_library_artwork_save(CBLibrary *lib, uint64_t id, const uint8_t *jpeg, size_t size, int width, int height);
int  cb_library_artwork_load(const CBLibrary *lib, uint64_t id, uint8_t **jpeg, size_t *size, int *width, int *height);
/* The id of a track's cover, which the tracks sharing it share: > 0, 0
 * when the track has none, or a negative errno. */
int64_t cb_library_artwork_id(const CBLibrary *lib, uint64_t id);

/* Settings. */
int  cb_library_config_save(CBLibrary *lib);          /* writes lib->cfg to the setting table */
/* Raw access for anything not in CBConfig (the interface's column layout...). */
int  cb_library_setting_get(const CBLibrary *lib, const char *key, char **value);   /* -ENOENT when absent */
int  cb_library_setting_set(CBLibrary *lib, const char *key, const char *value);

/* Disposable files (transcodes): <root>/cache/<kind>/<id>.<ext>, directories created. */
char *cb_library_cache_path(const CBLibrary *lib, const char *kind, uint64_t id, const char *ext);

/* Playlists. Every edit is a transaction and bumps generation. Positions
 * among siblings are kept dense. */
CBPlaylist       *cb_library_playlist_create(CBLibrary *lib, CBPlaylistFolder *in, const char *name);
CBPlaylistFolder *cb_library_folder_create(CBLibrary *lib, CBPlaylistFolder *in, const char *name);
int   cb_library_playlist_rename(CBLibrary *lib, CBPlaylist *p, const char *name);
int   cb_library_folder_rename(CBLibrary *lib, CBPlaylistFolder *f, const char *name);
int   cb_library_playlist_delete(CBLibrary *lib, CBPlaylist *p);
int   cb_library_folder_delete(CBLibrary *lib, CBPlaylistFolder *f);     /* and everything in it */
int   cb_library_playlist_add(CBLibrary *lib, CBPlaylist *p, const CBTrack *t, int at);   /* at = -1: append; a track may appear once per playlist */
int   cb_library_playlist_remove_at(CBLibrary *lib, CBPlaylist *p, int index);
int   cb_library_playlist_move(CBLibrary *lib, CBPlaylist *p, int from, int to);
/* Moves a playlist or folder under another folder, at `position` (-1: last). */
int   cb_library_playlist_reparent(CBLibrary *lib, CBPlaylist *p, CBPlaylistFolder *to, int position);
int   cb_library_folder_reparent(CBLibrary *lib, CBPlaylistFolder *f, CBPlaylistFolder *to, int position);
/* Every playlist, depth first: returns the count, fills up to max. */
int   cb_library_playlists_flat(const CBLibrary *lib, CBPlaylist **out, int max);
CBPlaylistFolder *cb_library_folder_of(const CBLibrary *lib, const CBPlaylist *p);
CBPlaylist       *cb_library_playlist_find(const CBLibrary *lib, const char *name);   /* first by name, depth first */

/* m3u8 import/export (paths relative to the file, resolved on read). */
int   cb_library_playlist_import_m3u(CBLibrary *lib, CBPlaylistFolder *in, const char *m3u_path, CBPlaylist **out);
int   cb_library_playlist_export_m3u(const CBLibrary *lib, const CBPlaylist *p, const char *m3u_path);

#endif /* CYANBOX_LIBRARY_H */
