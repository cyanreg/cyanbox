/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_FORMATS_H
#define CYANBOX_FORMATS_H

/* The on-stick formats, each a port of the matching dj module and checked
 * against the same rekordbox fixtures (tests/data). Details and sources:
 * docs/pdb.md, docs/anlz.md, docs/onelibrary.md, docs/settings.md.
 *
 * Every writer renders from an CBPlan (below), which the export module
 * builds from the library; the readers are used by inspect and pull. The
 * internal structures of each format (row layouts, page builders) live in
 * src/formats and are not part of this header. */

#include "cyanbox/model.h"
#include "cyanbox/library.h"
#include "cyanbox/analysis.h"
#include "cyanbox/audio.h"

/* ------------------------------------------------------------------- plan */

/* A cue as it goes into the ANLZ files: resolved and coloured. */
typedef struct CBPlanCue {
    double   time;              /* seconds */
    double   loop_end;          /* < 0 when not a loop */
    int      hot;               /* -1 memory, 0..7 hot */
    bool     active;
    int      color_code;        /* hot cues: 0..63 palette code; memory cues: 0..8 track colour */
    uint32_t rgb;               /* 0xRRGGBB */
    char     name[CB_CUE_NAME_MAX];
} CBPlanCue;

/* A track the export writes encoded rather than copied (CBStickFormat):
 * how, for cb_encode. */
typedef struct CBPlanEncode {
    bool         on;
    int          format;        /* enum CBEncodeFormat */
    int          bits;          /* 16 or 24 */
    bool         exact;         /* the source's samples fit `bits` as they are */
    int          max_rate;      /* 0: the source's */
    int          channels;      /* 0: the source's */
} CBPlanEncode;

typedef struct CBPlanTrack {
    uint32_t     id;            /* the library id (fits 32 bits) */
    const CBTrack *track;       /* in the library */
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
    char         key_name[CB_KEY_MAX];   /* as displayed, in the configured notation */
    CBGrid       grid;
    CBPlanCue   *cues; int nb_cues;
    uint32_t    *tag_ids; int nb_tag_ids;   /* My Tags */
    CBProbe      probe;         /* of `source`, or for `pcm` of the WAV served */
    /* Link (CB_TRANSCODE_LINK): served as a 16-bit stereo WAV of `source`,
     * the track's own file, decoded as a player reads it (src/link/wav.c),
     * total_samples frames long; probe, file_type and file_size are the
     * WAV's. Never set for an export. */
    bool         pcm;
    /* An export's track written encoded (CB_TRANSCODE_STICK_*): `source`
     * is the track's own file; probe, file_type and file_size are the
     * encoded file's, the size an estimate until it is written. */
    CBPlanEncode encode;
} CBPlanTrack;

typedef struct CBPlanName { uint32_t id; char *name; uint32_t artist_id; } CBPlanName;   /* artist_id for albums only */
typedef struct CBPlanPlaylist {
    uint32_t  id, parent_id;    /* parent 0 = root */
    int       sort_order;       /* 0-based among siblings (docs/pdb.md playlist_tree) */
    bool      is_folder;
    char     *name;
    uint32_t *track_ids; int nb;
} CBPlanPlaylist;
typedef struct CBPlanTag { uint32_t id; char *name; int category; int position; bool is_category; } CBPlanTag;
typedef struct CBPlanTagTrack { uint32_t track_id, tag_id; } CBPlanTagTrack;
typedef struct CBPlanArtwork { uint32_t id; char *path80; char *path240; uint8_t *jpeg80; size_t size80; uint8_t *jpeg240; size_t size240; } CBPlanArtwork;

typedef struct CBPlan {
    CBPlanTrack    *tracks;    int nb_tracks;       /* sorted by id */
    CBPlanName     *artists;   int nb_artists;
    CBPlanName     *albums;    int nb_albums;
    CBPlanName     *genres;    int nb_genres;
    CBPlanName     *labels;    int nb_labels;
    CBPlanName     *keys;      int nb_keys;
    CBPlanName     *remixers;  int nb_remixers;
    CBPlanName     *composers; int nb_composers;
    CBPlanName     *original_artists; int nb_original_artists;
    CBPlanArtwork  *artworks;  int nb_artworks;
    CBPlanPlaylist *playlists; int nb_playlists;    /* folders and playlists, parents first */
    CBPlanTag      *tags;      int nb_tags;
    CBPlanTagTrack *tag_tracks; int nb_tag_tracks;
    char            date[16];       /* "2026-10-13" */
    char           *device_name;
    char            key_notation[8];
    bool            hot_cue_auto_load;
    int             background_color;
    uint32_t        library_id;     /* one constant per library: track row 0x18, OneLibrary masterDbId */
    const CBConfig *cfg;
} CBPlan;

/* Builds the plan (ids, names, paths, cues, artwork) for the given tracks
 * and playlists. `waves_for(id)` is not needed here: ANLZ files are built
 * per track with cb_anlz_build. */
int  cb_plan_build(const CBLibrary *lib, CBTrack *const *tracks, int nb_tracks,
                   const CBPlaylist *const *playlists, int nb_playlists,
                   const char *device_name, bool compat_transcode, CBPlan *out);
void cb_plan_free(CBPlan *p);
/* The FAT-safe, unique on-stick path dj used ("Contents/Artist/Album/file"). */
char *cb_plan_usb_path(const CBTrack *t, const char *source, const char *const *used, int nb_used);

/* -------------------------------------------------------------------- pdb */

int  cb_pdb_build(const CBPlan *plan, uint8_t **out, size_t *size);        /* export.pdb */
int  cb_pdb_ext_build(const CBPlan *plan, uint8_t **out, size_t *size);    /* exportExt.pdb */
/* Too much text for a page? Returns the number of bytes over, 0 when fine. */
int  cb_pdb_track_row_overflow(const CBPlanTrack *t, const CBPlan *plan);

/* Reading, for inspect and pull. */
typedef struct CBPdbTrack {
    uint32_t id;
    char *path;                 /* on-stick path */
    char *anlz_path;            /* "/PIONEER/USBANLZ/.../ANLZ0000.DAT" */
    char *title, *artist;       /* resolved names */
    int   bpm_x100;
    int   duration;             /* seconds */
    uint32_t artwork_id, key_id, color_id;
} CBPdbTrack;
typedef struct CBPdbPlaylist { uint32_t id, parent_id; bool is_folder; char *name; uint32_t *track_ids; int nb; } CBPdbPlaylist;
typedef struct CBPdb {
    CBPdbTrack    *tracks;    int nb_tracks;
    CBPdbPlaylist *playlists; int nb_playlists;
    int nb_tables;
    unsigned next_unused_page, sequence;
} CBPdb;
int  cb_pdb_read(const uint8_t *data, size_t size, CBPdb *out);
void cb_pdb_free(CBPdb *p);
/* DeviceSQL string helpers (shared with the tests). */
int  cb_pdb_string_encode(const char *utf8, uint8_t **out, size_t *size);
int  cb_pdb_string_decode(const uint8_t *data, size_t size, char **utf8);

/* ------------------------------------------------------------------- anlz */

typedef struct CBAnlzFiles {
    uint8_t *dat; size_t dat_size;   /* ANLZ0000.DAT */
    uint8_t *ext; size_t ext_size;   /* ANLZ0000.EXT */
    uint8_t *ex2; size_t ex2_size;   /* ANLZ0000.2EX */
} CBAnlzFiles;

/* The USBANLZ directory for a path on the stick ("/PIONEER/USBANLZ/P0xx/xxxxxxxx"). */
int  cb_anlz_dir(const char *usb_path, char *buf, size_t size);
/* Builds the three files for a track. `waves` may be NULL (no waveform
 * tags, which the players accept). */
int  cb_anlz_build(const CBPlanTrack *t, const CBWaveforms *waves, CBAnlzFiles *out);
void cb_anlz_files_free(CBAnlzFiles *f);

/* Reading. */
typedef struct CBAnlzTag { char fourcc[5]; const uint8_t *body; size_t size; const uint8_t *raw; size_t raw_size; } CBAnlzTag;
int  cb_anlz_parse(const uint8_t *data, size_t size, CBAnlzTag **tags, int *nb);   /* tags malloc'd; bodies point into data */
int  cb_anlz_read_path(const uint8_t *data, size_t size, char **path);              /* PPTH */
int  cb_anlz_read_beats(const uint8_t *data, size_t size, CBBeat **beats, int *nb);  /* PQTZ */
/* PCOB/PCO2 cues: hot (PCO2 kind 1) or memory (kind 0). */
int  cb_anlz_read_cues(const uint8_t *data, size_t size, CBPlanCue **cues, int *nb);
/* The waveform tags found in a file, each malloc'd into `w` (missing ones
 * left zero). */
int  cb_anlz_read_waveforms(const uint8_t *data, size_t size, CBWaveforms *w);

/* ------------------------------------------------------------- onelibrary */

/* exportLibrary.db: the SQLite database rekordbox writes for OneLibrary
 * players, SQLCipher-encrypted with the known passphrase. */
int  cb_onelibrary_build(const CBPlan *plan, uint8_t **out, size_t *size);
/* The plain (decrypted) database, for the tests and inspect. */
int  cb_onelibrary_build_plain(const CBPlan *plan, uint8_t **out, size_t *size);
/* SQLCipher v4 page encryption (AES-256-CBC, HMAC-SHA512, PBKDF2 256000). */
int  cb_sqlcipher_encrypt(const uint8_t *plain, size_t size, const char *passphrase,
                          const uint8_t salt[16], uint8_t **out, size_t *out_size);
int  cb_sqlcipher_decrypt(const uint8_t *data, size_t size, const char *passphrase,
                          const uint8_t *wal, size_t wal_size, uint8_t **out, size_t *out_size);
extern const char cb_onelibrary_passphrase[];
/* Reads cues and track ids back from a decrypted exportLibrary.db, for
 * inspect. */
typedef struct CBOneLibTrack { uint32_t id; char *path; char *title; int nb_cues; } CBOneLibTrack;
int  cb_onelibrary_read(const uint8_t *plain, size_t size, CBOneLibTrack **tracks, int *nb);

/* --------------------------------------------------------------- settings */

/* My Settings files: MYSETTING.DAT, MYSETTING2.DAT, DJMMYSETTING.DAT,
 * DEVSETTING.DAT. Names and values are those `cyanbox settings` lists. */
int  cb_settings_encode(const char *file_name, const CBKv *kv, int nb, uint8_t **out, size_t *size);
int  cb_settings_decode(const char *file_name, const uint8_t *data, size_t size, CBKv **kv, int *nb);
/* The files the config's [player]/[mixer]/[device] sections produce: up to
 * four (name, bytes). Only sections with keys produce files. */
typedef struct CBSettingsFile { const char *name; uint8_t *data; size_t size; } CBSettingsFile;
int  cb_settings_from_config(const CBConfig *cfg, CBSettingsFile files[4], int *nb);
/* The four files' payloads (docs/settings.md sections 3 to 6) as the
 * config's sections make them, with each file's defaults where its
 * section sets nothing, as cb_settings_get reads them; has_devsetting
 * says whether [device] set anything. A value not valid fails it, the
 * defaults all through. What PRO DJ LINK serves and pushes (docs/LINK.md,
 * "My Settings over the link"). */
typedef struct CBSettingsPayloads {
    uint8_t mysetting[40], mysetting2[40], djmmysetting[52], devsetting[32];
    bool    has_devsetting;
} CBSettingsPayloads;
int  cb_settings_payloads(const CBConfig *cfg, CBSettingsPayloads *out);
/* Lists every setting: calls `cb(name, file, values...)` for each. */
typedef void (*cb_settings_list_cb)(void *opaque, const char *file, const char *name,
                                    const char *const *values, int nb_values, const char *deflt);
void cb_settings_list(cb_settings_list_cb cb, void *opaque);
/* The value cb_settings_list lists that `value` of the setting `name` in
 * `file` stands for, as the encoder reads it ("on" and "1" are "true",
 * "Touch & Release" is "touch_release", "+6" is "6"); NULL when it is none
 * of them or the setting is unknown. The string is static. */
const char *cb_settings_value(const char *file, const char *name, const char *value);
/* The value of the My Setting `key` ("player.quantize", "device.waveform_color":
 * the keys `cyanbox config` takes) that an export of `cfg` gives the players,
 * as cb_settings_list lists it: the library's, or the setting's default when
 * the library has none or one the export would refuse. What the interface
 * follows, so that it shows what the players will. NULL for a key that is
 * no My Setting. The string is static. */
const char *cb_settings_get(const CBConfig *cfg, const char *key);
/* A My Setting `key` ("player.quantize") exists and takes `value`. */
bool cb_settings_valid(const char *key, const char *value);
/* Removes the library's My Settings that are not valid (no such setting,
 * or a value it does not take), saying each: an invalid entry is removed,
 * not kept. Called after cb_library_open by the CLI and the app (the
 * library's own cleaning, src/core/clean.c, does the rest; core does not
 * link these tables). Returns how many went, or a negative errno when the
 * library could not be written (they are gone from memory all the same). */
int  cb_settings_clean(CBLibrary *lib);
void cb_kv_free(CBKv *kv, int nb);

#endif /* CYANBOX_FORMATS_H */
