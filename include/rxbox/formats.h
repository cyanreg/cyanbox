/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_FORMATS_H
#define RXBOX_FORMATS_H

/* The on-stick formats, each a port of the matching dj module and checked
 * against the same rekordbox fixtures (tests/data). Details and sources:
 * docs/pdb.md, docs/anlz.md, docs/onelibrary.md, docs/settings.md.
 *
 * Every writer renders from an RXPlan (below), which the export module
 * builds from the library; the readers are used by inspect and pull. The
 * internal structures of each format (row layouts, page builders) live in
 * src/formats and are not part of this header. */

#include "rxbox/model.h"
#include "rxbox/library.h"
#include "rxbox/analysis.h"
#include "rxbox/audio.h"

/* ------------------------------------------------------------------- plan */

/* A cue as it goes into the ANLZ files: resolved and coloured. */
typedef struct RXPlanCue {
    double   time;              /* seconds */
    double   loop_end;          /* < 0 when not a loop */
    int      hot;               /* -1 memory, 0..7 hot */
    bool     active;
    int      color_code;        /* hot cues: 0..63 palette code; memory cues: 0..8 track colour */
    uint32_t rgb;               /* 0xRRGGBB */
    char     name[RX_CUE_NAME_MAX];
} RXPlanCue;

typedef struct RXPlanTrack {
    uint32_t     id;            /* the library id (fits 32 bits) */
    const RXTrack *track;       /* in the library */
    char        *source;        /* absolute path of the file to copy (the transcode when used) */
    char        *usb_path;      /* "/Contents/Artist/Album/01 Track.flac" (FAT-safe, unique) */
    char        *anlz_dir;      /* "/PIONEER/USBANLZ/P0xx/xxxxxxxx" */
    int          anlz_index;    /* nnnn of ANLZnnnn.DAT: tracks hashing to one directory count up (docs/anlz.md 1.1) */
    int          file_type;     /* DeviceSQL file type: mp3 1, aac 4, flac 5, alac 6, wav 11, aiff 12 */
    int          bit_rate, sample_rate, bit_depth;
    int64_t      file_size;
    double       duration;
    int64_t      total_samples; /* players' timeline */
    uint32_t     artist_id, album_id, genre_id, label_id, key_id, color_id,
                 remixer_id, composer_id, original_artist_id, artwork_id;
    char         key_name[RX_KEY_MAX];   /* as displayed, in the configured notation */
    RXGrid       grid;
    RXPlanCue   *cues; int nb_cues;
    uint32_t    *tag_ids; int nb_tag_ids;   /* My Tags */
    RXProbe      probe;         /* of `source` */
} RXPlanTrack;

typedef struct RXPlanName { uint32_t id; char *name; uint32_t artist_id; } RXPlanName;   /* artist_id for albums only */
typedef struct RXPlanPlaylist {
    uint32_t  id, parent_id;    /* parent 0 = root */
    int       sort_order;       /* 0-based among siblings (docs/pdb.md playlist_tree) */
    bool      is_folder;
    char     *name;
    uint32_t *track_ids; int nb;
} RXPlanPlaylist;
typedef struct RXPlanTag { uint32_t id; char *name; int category; int position; bool is_category; } RXPlanTag;
typedef struct RXPlanTagTrack { uint32_t track_id, tag_id; } RXPlanTagTrack;
typedef struct RXPlanArtwork { uint32_t id; char *path80; char *path240; uint8_t *jpeg80; size_t size80; uint8_t *jpeg240; size_t size240; } RXPlanArtwork;

typedef struct RXPlan {
    RXPlanTrack    *tracks;    int nb_tracks;       /* sorted by id */
    RXPlanName     *artists;   int nb_artists;
    RXPlanName     *albums;    int nb_albums;
    RXPlanName     *genres;    int nb_genres;
    RXPlanName     *labels;    int nb_labels;
    RXPlanName     *keys;      int nb_keys;
    RXPlanName     *remixers;  int nb_remixers;
    RXPlanName     *composers; int nb_composers;
    RXPlanName     *original_artists; int nb_original_artists;
    RXPlanArtwork  *artworks;  int nb_artworks;
    RXPlanPlaylist *playlists; int nb_playlists;    /* folders and playlists, parents first */
    RXPlanTag      *tags;      int nb_tags;
    RXPlanTagTrack *tag_tracks; int nb_tag_tracks;
    char            date[16];       /* "2026-10-13" */
    char           *device_name;
    char            key_notation[8];
    bool            hot_cue_auto_load;
    int             background_color;
    uint32_t        library_id;     /* one constant per library: track row 0x18, OneLibrary masterDbId */
    const RXConfig *cfg;
} RXPlan;

/* Builds the plan (ids, names, paths, cues, artwork) for the given tracks
 * and playlists. `waves_for(id)` is not needed here: ANLZ files are built
 * per track with rx_anlz_build. */
int  rx_plan_build(const RXLibrary *lib, RXTrack *const *tracks, int nb_tracks,
                   const RXPlaylist *const *playlists, int nb_playlists,
                   const char *device_name, bool compat_transcode, RXPlan *out);
void rx_plan_free(RXPlan *p);
/* The FAT-safe, unique on-stick path dj used ("Contents/Artist/Album/file"). */
char *rx_plan_usb_path(const RXTrack *t, const char *source, const char *const *used, int nb_used);

/* -------------------------------------------------------------------- pdb */

int  rx_pdb_build(const RXPlan *plan, uint8_t **out, size_t *size);        /* export.pdb */
int  rx_pdb_ext_build(const RXPlan *plan, uint8_t **out, size_t *size);    /* exportExt.pdb */
/* Too much text for a page? Returns the number of bytes over, 0 when fine. */
int  rx_pdb_track_row_overflow(const RXPlanTrack *t, const RXPlan *plan);

/* Reading, for inspect and pull. */
typedef struct RXPdbTrack {
    uint32_t id;
    char *path;                 /* on-stick path */
    char *anlz_path;            /* "/PIONEER/USBANLZ/.../ANLZ0000.DAT" */
    char *title, *artist;       /* resolved names */
    int   bpm_x100;
    int   duration;             /* seconds */
    uint32_t artwork_id, key_id, color_id;
} RXPdbTrack;
typedef struct RXPdbPlaylist { uint32_t id, parent_id; bool is_folder; char *name; uint32_t *track_ids; int nb; } RXPdbPlaylist;
typedef struct RXPdb {
    RXPdbTrack    *tracks;    int nb_tracks;
    RXPdbPlaylist *playlists; int nb_playlists;
    int nb_tables;
    unsigned next_unused_page, sequence;
} RXPdb;
int  rx_pdb_read(const uint8_t *data, size_t size, RXPdb *out);
void rx_pdb_free(RXPdb *p);
/* DeviceSQL string helpers (shared with the tests). */
int  rx_pdb_string_encode(const char *utf8, uint8_t **out, size_t *size);
int  rx_pdb_string_decode(const uint8_t *data, size_t size, char **utf8);

/* ------------------------------------------------------------------- anlz */

typedef struct RXAnlzFiles {
    uint8_t *dat; size_t dat_size;   /* ANLZ0000.DAT */
    uint8_t *ext; size_t ext_size;   /* ANLZ0000.EXT */
    uint8_t *ex2; size_t ex2_size;   /* ANLZ0000.2EX */
} RXAnlzFiles;

/* The USBANLZ directory for a path on the stick ("/PIONEER/USBANLZ/P0xx/xxxxxxxx"). */
int  rx_anlz_dir(const char *usb_path, char *buf, size_t size);
/* Builds the three files for a track. `waves` may be NULL (no waveform
 * tags, which the players accept). */
int  rx_anlz_build(const RXPlanTrack *t, const RXWaveforms *waves, RXAnlzFiles *out);
void rx_anlz_files_free(RXAnlzFiles *f);

/* Reading. */
typedef struct RXAnlzTag { char fourcc[5]; const uint8_t *body; size_t size; const uint8_t *raw; size_t raw_size; } RXAnlzTag;
int  rx_anlz_parse(const uint8_t *data, size_t size, RXAnlzTag **tags, int *nb);   /* tags malloc'd; bodies point into data */
int  rx_anlz_read_path(const uint8_t *data, size_t size, char **path);              /* PPTH */
int  rx_anlz_read_beats(const uint8_t *data, size_t size, RXBeat **beats, int *nb);  /* PQTZ */
/* PCOB/PCO2 cues: hot (PCO2 kind 1) or memory (kind 0). */
int  rx_anlz_read_cues(const uint8_t *data, size_t size, RXPlanCue **cues, int *nb);
/* The waveform tags found in a file, each malloc'd into `w` (missing ones
 * left zero). */
int  rx_anlz_read_waveforms(const uint8_t *data, size_t size, RXWaveforms *w);

/* ------------------------------------------------------------- onelibrary */

/* exportLibrary.db: the SQLite database rekordbox writes for OneLibrary
 * players, SQLCipher-encrypted with the known passphrase. */
int  rx_onelibrary_build(const RXPlan *plan, uint8_t **out, size_t *size);
/* The plain (decrypted) database, for the tests and inspect. */
int  rx_onelibrary_build_plain(const RXPlan *plan, uint8_t **out, size_t *size);
/* SQLCipher v4 page encryption (AES-256-CBC, HMAC-SHA512, PBKDF2 256000). */
int  rx_sqlcipher_encrypt(const uint8_t *plain, size_t size, const char *passphrase,
                          const uint8_t salt[16], uint8_t **out, size_t *out_size);
int  rx_sqlcipher_decrypt(const uint8_t *data, size_t size, const char *passphrase,
                          const uint8_t *wal, size_t wal_size, uint8_t **out, size_t *out_size);
extern const char rx_onelibrary_passphrase[];
/* Reads cues and track ids back from a decrypted exportLibrary.db, for
 * inspect. */
typedef struct RXOneLibTrack { uint32_t id; char *path; char *title; int nb_cues; } RXOneLibTrack;
int  rx_onelibrary_read(const uint8_t *plain, size_t size, RXOneLibTrack **tracks, int *nb);

/* --------------------------------------------------------------- settings */

/* My Settings files: MYSETTING.DAT, MYSETTING2.DAT, DJMMYSETTING.DAT,
 * DEVSETTING.DAT. Names and values are those `rxbox settings` lists. */
int  rx_settings_encode(const char *file_name, const RXKv *kv, int nb, uint8_t **out, size_t *size);
int  rx_settings_decode(const char *file_name, const uint8_t *data, size_t size, RXKv **kv, int *nb);
/* The files the config's [player]/[mixer]/[device] sections produce: up to
 * four (name, bytes). Only sections with keys produce files. */
typedef struct RXSettingsFile { const char *name; uint8_t *data; size_t size; } RXSettingsFile;
int  rx_settings_from_config(const RXConfig *cfg, RXSettingsFile files[4], int *nb);
/* Lists every setting: calls `cb(name, file, values...)` for each. */
typedef void (*rx_settings_list_cb)(void *opaque, const char *file, const char *name,
                                    const char *const *values, int nb_values, const char *deflt);
void rx_settings_list(rx_settings_list_cb cb, void *opaque);
/* The value rx_settings_list lists that `value` of the setting `name` in
 * `file` stands for, as the encoder reads it ("on" and "1" are "true",
 * "Touch & Release" is "touch_release", "+6" is "6"); NULL when it is none
 * of them or the setting is unknown. The string is static. */
const char *rx_settings_value(const char *file, const char *name, const char *value);
void rx_kv_free(RXKv *kv, int nb);

#endif /* RXBOX_FORMATS_H */
