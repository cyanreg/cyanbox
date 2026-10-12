/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_FORMATS_H
#define RXBOX_FORMATS_H

/* The on-stick formats, each a port of the matching dj module and checked
 * against the same rekordbox fixtures (tests/data). Details and sources:
 * docs/pdb.md, docs/anlz.md, docs/onelibrary.md, docs/settings.md.
 *
 * Every writer renders from an RxPlan (below), which the export module
 * builds from the library; the readers are used by inspect and pull. The
 * internal structures of each format (row layouts, page builders) live in
 * src/formats and are not part of this header. */

#include "rxbox/model.h"
#include "rxbox/library.h"
#include "rxbox/analysis.h"
#include "rxbox/audio.h"

/* ------------------------------------------------------------------- plan */

/* A cue as it goes into the ANLZ files: resolved and coloured. */
typedef struct RxPlanCue {
    double   time;              /* seconds */
    double   loop_end;          /* < 0 when not a loop */
    int      hot;               /* -1 memory, 0..7 hot */
    bool     active;
    int      color_code;        /* hot cues: 0..63 palette code; memory cues: 0..8 track colour */
    uint32_t rgb;               /* 0xRRGGBB */
    char     name[RX_CUE_NAME_MAX];
} RxPlanCue;

typedef struct RxPlanTrack {
    uint32_t     id;            /* the library id (fits 32 bits) */
    const RxTrack *track;       /* in the library */
    char        *source;        /* absolute path of the file to copy (the transcode when used) */
    char        *usb_path;      /* "/Contents/Artist/Album/01 Track.flac" (FAT-safe, unique) */
    char        *anlz_dir;      /* "/PIONEER/USBANLZ/P0xx/xxxxxxxx" */
    int          file_type;     /* DeviceSQL file type: mp3 1, aac 4, flac 5, alac 6, wav 11, aiff 12 */
    int          bit_rate, sample_rate, bit_depth;
    int64_t      file_size;
    double       duration;
    int64_t      total_samples; /* players' timeline */
    uint32_t     artist_id, album_id, genre_id, label_id, key_id, color_id,
                 remixer_id, composer_id, original_artist_id, artwork_id;
    char         key_name[RX_KEY_MAX];   /* as displayed, in the configured notation */
    RxGrid       grid;
    RxPlanCue   *cues; int nb_cues;
    uint32_t    *tag_ids; int nb_tag_ids;   /* My Tags */
    RxProbe      probe;         /* of `source` */
} RxPlanTrack;

typedef struct RxPlanName { uint32_t id; char *name; uint32_t artist_id; } RxPlanName;   /* artist_id for albums only */
typedef struct RxPlanPlaylist {
    uint32_t  id, parent_id;    /* parent 0 = root */
    int       sort_order;       /* 1-based among siblings */
    bool      is_folder;
    char     *name;
    uint32_t *track_ids; int nb;
} RxPlanPlaylist;
typedef struct RxPlanTag { uint32_t id; char *name; int category; int position; bool is_category; } RxPlanTag;
typedef struct RxPlanTagTrack { uint32_t track_id, tag_id; } RxPlanTagTrack;
typedef struct RxPlanArtwork { uint32_t id; char *path80; char *path240; uint8_t *jpeg80; size_t size80; uint8_t *jpeg240; size_t size240; } RxPlanArtwork;

typedef struct RxPlan {
    RxPlanTrack    *tracks;    int nb_tracks;       /* sorted by id */
    RxPlanName     *artists;   int nb_artists;
    RxPlanName     *albums;    int nb_albums;
    RxPlanName     *genres;    int nb_genres;
    RxPlanName     *labels;    int nb_labels;
    RxPlanName     *keys;      int nb_keys;
    RxPlanName     *remixers;  int nb_remixers;
    RxPlanName     *composers; int nb_composers;
    RxPlanName     *original_artists; int nb_original_artists;
    RxPlanArtwork  *artworks;  int nb_artworks;
    RxPlanPlaylist *playlists; int nb_playlists;    /* folders and playlists, parents first */
    RxPlanTag      *tags;      int nb_tags;
    RxPlanTagTrack *tag_tracks; int nb_tag_tracks;
    char            date[16];       /* "2026-10-13" */
    char           *device_name;
    char            key_notation[8];
    bool            hot_cue_auto_load;
    int             background_color;
    const RxConfig *cfg;
} RxPlan;

/* Builds the plan (ids, names, paths, cues, artwork) for the given tracks
 * and playlists. `waves_for(id)` is not needed here: ANLZ files are built
 * per track with rx_anlz_build. */
int  rx_plan_build(const RxLibrary *lib, RxTrack *const *tracks, int nb_tracks,
                   const RxPlaylist *const *playlists, int nb_playlists,
                   const char *device_name, bool compat_transcode, RxPlan *out);
void rx_plan_free(RxPlan *p);
/* The FAT-safe, unique on-stick path dj used ("Contents/Artist/Album/file"). */
char *rx_plan_usb_path(const RxTrack *t, const char *source, const char *const *used, int nb_used);

/* -------------------------------------------------------------------- pdb */

int  rx_pdb_build(const RxPlan *plan, uint8_t **out, size_t *size);        /* export.pdb */
int  rx_pdb_ext_build(const RxPlan *plan, uint8_t **out, size_t *size);    /* exportExt.pdb */
/* Too much text for a page? Returns the number of bytes over, 0 when fine. */
int  rx_pdb_track_row_overflow(const RxPlanTrack *t, const RxPlan *plan);

/* Reading, for inspect and pull. */
typedef struct RxPdbTrack {
    uint32_t id;
    char *path;                 /* on-stick path */
    char *anlz_path;            /* "/PIONEER/USBANLZ/.../ANLZ0000.DAT" */
    char *title, *artist;       /* resolved names */
    int   bpm_x100;
    int   duration;             /* seconds */
    uint32_t artwork_id, key_id, color_id;
} RxPdbTrack;
typedef struct RxPdbPlaylist { uint32_t id, parent_id; bool is_folder; char *name; uint32_t *track_ids; int nb; } RxPdbPlaylist;
typedef struct RxPdb {
    RxPdbTrack    *tracks;    int nb_tracks;
    RxPdbPlaylist *playlists; int nb_playlists;
    int nb_tables;
    unsigned next_unused_page, sequence;
} RxPdb;
int  rx_pdb_read(const uint8_t *data, size_t size, RxPdb *out);
void rx_pdb_free(RxPdb *p);
/* DeviceSQL string helpers (shared with the tests). */
int  rx_pdb_string_encode(const char *utf8, uint8_t **out, size_t *size);
int  rx_pdb_string_decode(const uint8_t *data, size_t size, char **utf8);

/* ------------------------------------------------------------------- anlz */

typedef struct RxAnlzFiles {
    uint8_t *dat; size_t dat_size;   /* ANLZ0000.DAT */
    uint8_t *ext; size_t ext_size;   /* ANLZ0000.EXT */
    uint8_t *ex2; size_t ex2_size;   /* ANLZ0000.2EX */
} RxAnlzFiles;

/* The USBANLZ directory for a path on the stick ("/PIONEER/USBANLZ/P0xx/xxxxxxxx"). */
int  rx_anlz_dir(const char *usb_path, char *buf, size_t size);
/* Builds the three files for a track. `waves` may be NULL (no waveform
 * tags, which the players accept). */
int  rx_anlz_build(const RxPlanTrack *t, const RxWaveforms *waves, RxAnlzFiles *out);
void rx_anlz_files_free(RxAnlzFiles *f);

/* Reading. */
typedef struct RxAnlzTag { char fourcc[5]; const uint8_t *body; size_t size; const uint8_t *raw; size_t raw_size; } RxAnlzTag;
int  rx_anlz_parse(const uint8_t *data, size_t size, RxAnlzTag **tags, int *nb);   /* tags malloc'd; bodies point into data */
int  rx_anlz_read_path(const uint8_t *data, size_t size, char **path);              /* PPTH */
int  rx_anlz_read_beats(const uint8_t *data, size_t size, RxBeat **beats, int *nb);  /* PQTZ */
/* PCOB/PCO2 cues: hot (PCO2 kind 1) or memory (kind 0). */
int  rx_anlz_read_cues(const uint8_t *data, size_t size, RxPlanCue **cues, int *nb);
/* The waveform tags found in a file, each malloc'd into `w` (missing ones
 * left zero). */
int  rx_anlz_read_waveforms(const uint8_t *data, size_t size, RxWaveforms *w);

/* ------------------------------------------------------------- onelibrary */

/* exportLibrary.db: the SQLite database rekordbox writes for OneLibrary
 * players, SQLCipher-encrypted with the known passphrase. */
int  rx_onelibrary_build(const RxPlan *plan, uint8_t **out, size_t *size);
/* The plain (decrypted) database, for the tests and inspect. */
int  rx_onelibrary_build_plain(const RxPlan *plan, uint8_t **out, size_t *size);
/* SQLCipher v4 page encryption (AES-256-CBC, HMAC-SHA512, PBKDF2 256000). */
int  rx_sqlcipher_encrypt(const uint8_t *plain, size_t size, const char *passphrase,
                          const uint8_t salt[16], uint8_t **out, size_t *out_size);
int  rx_sqlcipher_decrypt(const uint8_t *data, size_t size, const char *passphrase,
                          const uint8_t *wal, size_t wal_size, uint8_t **out, size_t *out_size);
extern const char rx_onelibrary_passphrase[];
/* Reads cues and track ids back from a decrypted exportLibrary.db, for
 * inspect. */
typedef struct RxOneLibTrack { uint32_t id; char *path; char *title; int nb_cues; } RxOneLibTrack;
int  rx_onelibrary_read(const uint8_t *plain, size_t size, RxOneLibTrack **tracks, int *nb);

/* --------------------------------------------------------------- settings */

/* My Settings files: MYSETTING.DAT, MYSETTING2.DAT, DJMMYSETTING.DAT,
 * DEVSETTING.DAT. Names and values are those `rxbox settings` lists. */
int  rx_settings_encode(const char *file_name, const RxKv *kv, int nb, uint8_t **out, size_t *size);
int  rx_settings_decode(const char *file_name, const uint8_t *data, size_t size, RxKv **kv, int *nb);
/* The files the config's [player]/[mixer]/[device] sections produce: up to
 * four (name, bytes). Only sections with keys produce files. */
typedef struct RxSettingsFile { const char *name; uint8_t *data; size_t size; } RxSettingsFile;
int  rx_settings_from_config(const RxConfig *cfg, RxSettingsFile files[4], int *nb);
/* Lists every setting: calls `cb(name, file, values...)` for each. */
typedef void (*rx_settings_list_cb)(void *opaque, const char *file, const char *name,
                                    const char *const *values, int nb_values, const char *deflt);
void rx_settings_list(rx_settings_list_cb cb, void *opaque);
void rx_kv_free(RxKv *kv, int nb);

#endif /* RXBOX_FORMATS_H */
