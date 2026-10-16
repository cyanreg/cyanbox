/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The settings (cyanbox/library.h CBConfig) and where the library lives.
 *
 * Settings are rows of the `setting` table, text keys and text values:
 *
 *   analysis.bpm_min  analysis.bpm_max  analysis.snap_bpm  analysis.auto_cues
 *   analysis.allow_sections  analysis.what
 *   import.comment                 take the file's comment tag on import
 *                                  (default false: the comment field is the
 *                                  user's and starts empty)
 *   export.key_notation  export.pdb  export.onelibrary  export.compat_transcode
 *   export.format                  the stick's audio: copy (as it is), flac,
 *                                  wav or aiff (cyanbox/export.h CBStickFormat)
 *   export.device_name
 *   export.background_color        the players' background for the stick's
 *                                  library: default or one of the eight
 *                                  track colours (export.pdb's property row,
 *                                  exportLibrary.db's property table)
 *   player.<name>  mixer.<name>  device.<name>     My Settings, names as
 *                                                  `cyanbox settings` lists them
 *   link.mp3                       Link serves what the players cannot all
 *                                  read as 320 kbps MP3 instead of 16-bit
 *                                  WAV (default false; docs/LINK.md)
 *   audio.sink
 *   ml.model                       the model's GGUF file for tagging tracks
 *                                  (cyanbox/ml.h; unset: $CYANBOX_ML_MODEL, else
 *                                  models/ beside the executable)
 *   color.pink ... color.purple    the legend: the words the tagging model
 *                                  picks a track's colour by (cyanbox/ml.h);
 *                                  unset: CB_TRACK_VIBE_* (cyanbox/colors.h)
 *   ui.detail_bars  ui.split_player  ui.split_tree  ui.traffic_light
 *
 * Booleans are "true"/"false", numbers their shortest exact decimal form,
 * analysis.what the names `cyanbox analyze --what` takes ("beats,key,waves").
 * cb_config_set checks a value for its key (`cyanbox config` refuses one that
 * does not parse, rather than storing a row every command then warns of);
 * an empty value is the key's default. A row that does not parse all the
 * same (an older cyanbox stored any) leaves the default in place and is
 * removed once the library has opened (cb_core_config_clean), as is a
 * key cyanbox does not know: a bad row never stops the library from
 * opening, and does not stay. The interface's ui.* keys and the sync's
 * sync.* records are free for cb_library_setting_get/set and left alone
 * by cb_library_config_save.
 *
 * The interface's waveform colour and key notation are the players'
 * device.waveform_color and device.key_display (one setting for cyanbox and
 * the stick). */

#include "core.h"

#include <math.h>
#include <stdio.h>

#include "cyanbox/analysis.h"
#include "cyanbox/colors.h"
#include "cyanbox/notation.h"

void cb_config_defaults(CBConfig *c)
{
    *c = (CBConfig) {
        .snap_bpm          = true,
        .auto_cues         = true,
        .allow_sections    = true,
        .analyse_what      = CB_ANALYSE_ALL,
        .key_notation      = "classic",
        .export_pdb        = true,
        .export_onelibrary = true,
        .compat_transcode  = true,
        .traffic_light     = "related3",
        .detail_bars       = 12,
    };
}

static void kv_free(CBKv **kv, int *nb)
{
    for (int i = 0; i < *nb; i++) {
        free((*kv)[i].key);
        free((*kv)[i].value);
    }
    free(*kv);
    *kv = NULL;
    *nb = 0;
}

void cb_config_free(CBConfig *c)
{
    if (!c)
        return;
    free(c->device_name);
    free(c->audio_sink);
    free(c->ml_model);
    for (int i = 0; i < CB_TRACK_COLORS; i++)
        free(c->vibes[i]);
    kv_free(&c->player, &c->nb_player);
    kv_free(&c->mixer, &c->nb_mixer);
    kv_free(&c->device, &c->nb_device);
    cb_config_defaults(c);
}

/* -------------------------------------------------------------- legend */

/* The keys spell rekordbox's colour names out: cb_track_color_names is the
 * formats module's, which core does not link. */
static const char *const vibe_keys[CB_TRACK_COLORS] = {
    "color.pink", "color.red", "color.orange", "color.yellow",
    "color.green", "color.aqua", "color.blue", "color.purple",
};

static const char *const vibe_defaults[CB_TRACK_COLORS] = {
    CB_TRACK_VIBE_PINK,  CB_TRACK_VIBE_RED,  CB_TRACK_VIBE_ORANGE, CB_TRACK_VIBE_YELLOW,
    CB_TRACK_VIBE_GREEN, CB_TRACK_VIBE_AQUA, CB_TRACK_VIBE_BLUE,   CB_TRACK_VIBE_PURPLE,
};

static_assert(CB_ARRAY_ELEMS(((CBConfig *)0)->vibes) == CB_TRACK_COLORS,
              "a description per track colour");

const char *cb_config_vibe_default(int id)
{
    return id >= 1 && id <= CB_TRACK_COLORS ? vibe_defaults[id - 1] : NULL;
}

const char *cb_config_vibe_key(int id)
{
    return id >= 1 && id <= CB_TRACK_COLORS ? vibe_keys[id - 1] : NULL;
}

const char *cb_config_vibe(const CBConfig *c, int id)
{
    if (id < 1 || id > CB_TRACK_COLORS)
        return NULL;
    return c->vibes[id - 1] ? c->vibes[id - 1] : vibe_defaults[id - 1];
}

int cb_config_set_vibe(CBConfig *c, int id, const char *text)
{
    if (id < 1 || id > CB_TRACK_COLORS)
        return CB_ERROR(EINVAL);
    if (!text)
        text = "";
    while (*text == ' ' || *text == '\t')
        text++;
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\n'))
        n--;
    if (n > CB_CONFIG_VIBE_MAX)
        return CB_ERROR(E2BIG);
    char *v = NULL;
    if (n && (strlen(vibe_defaults[id - 1]) != n || strncmp(text, vibe_defaults[id - 1], n))) {
        if (!(v = strndup(text, n)))
            return CB_ERROR(ENOMEM);
    }
    free(c->vibes[id - 1]);
    c->vibes[id - 1] = v;
    return 0;
}

void cb_config_legend(const CBConfig *c, const char *legend[CB_TRACK_COLORS])
{
    for (int i = 0; i < CB_TRACK_COLORS; i++)
        legend[i] = cb_config_vibe(c, i + 1);
}

/* ------------------------------------------------------------------ root */

char *cb_library_default_root(void)
{
    const char *env = getenv("CYANBOX_LIBRARY");
    if (env && *env)
        return cb_path_expand(env);
    const char *home = getenv("HOME");
    if (!home || !*home)
        return NULL;
    return cb_asprintf("%s/.local/share/cyanbox", home);
}

const char *cb_core_fmt_double(double v, char *buf, size_t size)
{
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, size, "%.*g", prec, v);
        if (strtod(buf, NULL) == v)
            break;
    }
    return buf;
}

/* ------------------------------------------------------------------ load */

static bool parse_bool(const char *s, bool *out)
{
    if (!strcmp(s, "true") || !strcmp(s, "1") || !strcmp(s, "yes") || !strcmp(s, "on"))
        *out = true;
    else if (!strcmp(s, "false") || !strcmp(s, "0") || !strcmp(s, "no") || !strcmp(s, "off"))
        *out = false;
    else
        return false;
    return true;
}

static bool parse_double(const char *s, double *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end || !isfinite(v))
        return false;
    *out = v;
    return true;
}

static const struct {
    const char *name;
    unsigned    bit;
} what_names[] = {
    { "beats", CB_ANALYSE_BEATS },
    { "key",   CB_ANALYSE_KEY   },
    { "waves", CB_ANALYSE_WAVES },
};

static bool parse_what(const char *s, unsigned *out)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", s);
    unsigned what = 0;
    char *save = NULL;
    for (char *w = strtok_r(buf, ", ", &save); w; w = strtok_r(NULL, ", ", &save)) {
        size_t i = 0;
        while (i < CB_ARRAY_ELEMS(what_names) && strcmp(w, what_names[i].name))
            i++;
        if (i == CB_ARRAY_ELEMS(what_names))
            return false;
        what |= what_names[i].bit;
    }
    if (!what)
        return false;
    *out = what;
    return true;
}

static const char *fmt_what(unsigned what, char *buf, size_t size)
{
    buf[0] = '\0';
    for (size_t i = 0; i < CB_ARRAY_ELEMS(what_names); i++) {
        size_t len = strlen(buf);
        if (what & what_names[i].bit)
            snprintf(buf + len, size - len, "%s%s", len ? "," : "", what_names[i].name);
    }
    /* Nothing asked: the row goes and the default comes back. */
    return buf[0] ? buf : NULL;
}

static int kv_push(CBKv **kv, int *nb, const char *key, const char *value)
{
    CBKv *n = realloc(*kv, (size_t)(*nb + 1) * sizeof(*n));
    if (!n)
        return CB_ERROR(ENOMEM);
    *kv = n;
    char *k = cb_strdup(key), *v = cb_strdup(value);
    if (!k || !v) {
        free(k);
        free(v);
        return CB_ERROR(ENOMEM);
    }
    n[(*nb)++] = (CBKv) {
        .key   = k,
        .value = v,
    };
    return 0;
}

static int set_string(char **field, const char *value)
{
    char *copy = *value ? cb_strdup(value) : NULL;
    if (*value && !copy)
        return CB_ERROR(ENOMEM);
    free(*field);
    *field = copy;
    return 0;
}

/* CBConfig's own keys (not the My Settings), what each takes, and how it
 * is read. */
typedef enum Kind {
    K_BOOL, K_BPM, K_WHAT, K_NOTATION, K_TEXT, K_VIBE, K_TRAFFIC, K_BARS, K_SPLIT, K_BACKGROUND,
    K_FORMAT,
} Kind;

/* export.background_color's values, by id: cyanbox/colors.h's order (the
 * formats module's table, which core does not link). */
static const char *const background_names[CB_TRACK_COLORS + 1] = {
    "default", "pink", "red", "orange", "yellow", "green", "aqua", "blue", "purple",
};

static const struct {
    const char *key;
    Kind        kind;
} known_keys[] = {
    { "analysis.bpm_min",        K_BPM },
    { "analysis.bpm_max",        K_BPM },
    { "analysis.snap_bpm",       K_BOOL },
    { "analysis.auto_cues",      K_BOOL },
    { "analysis.allow_sections", K_BOOL },
    { "analysis.what",           K_WHAT },
    { "import.comment",          K_BOOL },
    { "export.key_notation",     K_NOTATION },
    { "export.pdb",              K_BOOL },
    { "export.onelibrary",       K_BOOL },
    { "export.compat_transcode", K_BOOL },
    { "export.format",           K_FORMAT },
    { "export.device_name",      K_TEXT },
    { "export.background_color", K_BACKGROUND },
    { "link.mp3",                K_BOOL },
    { "audio.sink",              K_TEXT },
    { "ml.model",                K_TEXT },
    { "color.pink",              K_VIBE }, { "color.red",    K_VIBE },
    { "color.orange",            K_VIBE }, { "color.yellow", K_VIBE },
    { "color.green",             K_VIBE }, { "color.aqua",   K_VIBE },
    { "color.blue",              K_VIBE }, { "color.purple", K_VIBE },
    { "ui.traffic_light",        K_TRAFFIC },
    { "ui.detail_bars",          K_BARS },
    { "ui.split_player",         K_SPLIT },
    { "ui.split_tree",           K_SPLIT },
};

static int key_kind(const char *key)
{
    for (size_t i = 0; key && i < CB_ARRAY_ELEMS(known_keys); i++) {
        if (!strcmp(key, known_keys[i].key))
            return (int)known_keys[i].kind;
    }
    return -1;
}

static bool *bool_field(CBConfig *c, const char *key)
{
    return !strcmp(key, "analysis.snap_bpm")       ? &c->snap_bpm :
           !strcmp(key, "analysis.auto_cues")      ? &c->auto_cues :
           !strcmp(key, "analysis.allow_sections") ? &c->allow_sections :
           !strcmp(key, "import.comment")          ? &c->import_comment :
           !strcmp(key, "export.pdb")              ? &c->export_pdb :
           !strcmp(key, "export.onelibrary")       ? &c->export_onelibrary :
           !strcmp(key, "link.mp3")                ? &c->link_mp3 : &c->compat_transcode;
}

#define STR_(x) #x
#define STR(x) STR_(x)

/* export.format's values, by CBStickFormat (cyanbox/export.h). */
static const char *const format_names[] = { "copy", "flac", "wav", "aiff" };

const char *cb_config_takes(const char *key)
{
    switch (key_kind(key)) {
    case K_BOOL:     return "true or false";
    case K_BPM:      return "a tempo in BPM up to 1000, or 0 for none";
    case K_WHAT:     return "beats, key and waves, one or more, separated by commas";
    case K_NOTATION: return "classic, camelot or open";
    case K_TEXT:     return "any text";
    case K_VIBE:     return "a description of at most " STR(CB_CONFIG_VIBE_MAX) " bytes";
    case K_TRAFFIC:  return "off, same, related1, related2 or related3";
    case K_BARS:     return "a whole number of bars from 1 to 1024";
    case K_SPLIT:    return "a position, 0 or more (0: the default)";
    case K_BACKGROUND:
        return "default, pink, red, orange, yellow, green, aqua, blue or purple";
    case K_FORMAT:   return "copy, flac, wav or aiff";
    default:         return NULL;
    }
}

int cb_config_set(CBConfig *c, const char *key, const char *value)
{
    int kind = key_kind(key);
    if (kind < 0)
        return CB_ERROR(ENOENT);
    if (!value)
        value = "";
    CBConfig def;
    cb_config_defaults(&def);
    bool reset = !*value;
    double d;
    bool b;
    unsigned what;

    switch ((Kind)kind) {
    case K_BOOL:
        if (!reset && !parse_bool(value, &b))
            return CB_ERROR(EINVAL);
        *bool_field(c, key) = reset ? *bool_field(&def, key) : b;
        return 0;
    case K_BPM:
        if (!reset && (!parse_double(value, &d) || d < 0 || d > 1000))
            return CB_ERROR(EINVAL);
        if (reset)
            d = 0.0;
        *(!strcmp(key, "analysis.bpm_min") ? &c->bpm_min : &c->bpm_max) = d;
        return 0;
    case K_WHAT:
        if (!reset && !parse_what(value, &what))
            return CB_ERROR(EINVAL);
        c->analyse_what = reset ? def.analyse_what : what;
        return 0;
    case K_FORMAT:
        for (int i = 0; !reset && i < (int)CB_ARRAY_ELEMS(format_names); i++)
            if (!strcmp(value, format_names[i])) {
                c->export_format = i;
                return 0;
            }
        if (!reset)
            return CB_ERROR(EINVAL);
        c->export_format = def.export_format;
        return 0;
    case K_NOTATION:
        if (!reset && strcmp(value, "classic") && strcmp(value, "camelot") && strcmp(value, "open"))
            return CB_ERROR(EINVAL);
        snprintf(c->key_notation, sizeof(c->key_notation), "%s", reset ? def.key_notation : value);
        return 0;
    case K_TEXT:
        return set_string(!strcmp(key, "export.device_name") ? &c->device_name :
                          !strcmp(key, "audio.sink")         ? &c->audio_sink : &c->ml_model, value);
    case K_VIBE:
        for (int i = 0; i < CB_TRACK_COLORS; i++) {
            if (!strcmp(key, vibe_keys[i]))
                return cb_config_set_vibe(c, i + 1, value);
        }
        return CB_ERROR(ENOENT);
    case K_TRAFFIC:
        if (!reset && cb_key_reach_parse(value) < 0)
            return CB_ERROR(EINVAL);
        snprintf(c->traffic_light, sizeof(c->traffic_light), "%s", reset ? def.traffic_light : value);
        return 0;
    case K_BARS:
        if (!reset && (!parse_double(value, &d) || d < 1 || d > 1024 || d != trunc(d)))
            return CB_ERROR(EINVAL);
        c->detail_bars = reset ? def.detail_bars : (int)d;
        return 0;
    case K_SPLIT:
        if (!reset && (!parse_double(value, &d) || d < 0 || d > 1e6))
            return CB_ERROR(EINVAL);
        if (reset)
            d = 0.0;
        *(!strcmp(key, "ui.split_player") ? &c->split_player : &c->split_tree) = (float)d;
        return 0;
    case K_BACKGROUND:
        for (int i = 0; i <= CB_TRACK_COLORS; i++) {
            if (reset || !cb_strcasecmp(value, background_names[i])) {
                c->background_color = reset ? 0 : i;
                return 0;
            }
        }
        return CB_ERROR(EINVAL);
    }
    return CB_ERROR(ENOENT);
}

static int put(CBLibrary *lib, const char *key, const char *value);

/* One row of the setting table at load: a value that is not valid for
 * its key is said and the default kept, so a bad row never stops the
 * library from opening; a key that is none of CBConfig's is someone
 * else's (the interface's, the sync's). */
static int apply_setting(CBConfig *c, const char *key, const char *value)
{
    if (cb_str_starts(key, "player."))
        return kv_push(&c->player, &c->nb_player, key + 7, value);
    if (cb_str_starts(key, "mixer."))
        return kv_push(&c->mixer, &c->nb_mixer, key + 6, value);
    if (cb_str_starts(key, "device."))
        return kv_push(&c->device, &c->nb_device, key + 7, value);

    /* A value the key does not take is the default here, and its row goes
     * once the library has opened (cb_core_config_clean says so). */
    int ret = cb_config_set(c, key, value);
    if (ret == CB_ERROR(EINVAL) || ret == CB_ERROR(E2BIG))
        ret = 0;
    return ret == CB_ERROR(ENOENT) ? 0 : ret;
}

/* The keys cyanbox knows besides CBConfig's and My Settings: the interface's
 * own (its layout, under ui.) and the stick sync's records (sync.). */
static bool free_key(const char *key)
{
    return cb_str_starts(key, "ui.") || cb_str_starts(key, "sync.");
}

int cb_core_config_clean(CBLibrary *lib, int *removed)
{
    *removed = 0;
    sqlite3_stmt *st = cb_db_prepare(lib->db, "SELECT key, value FROM setting ORDER BY key");
    if (!st)
        return CB_ERROR(EIO);
    CBArray gone;
    cb_array_init(&gone, sizeof(char *));
    int ret;
    while ((ret = cb_db_row(lib->db, st, "checking settings")) > 0) {
        const char *key = (const char *)sqlite3_column_text(st, 0);
        const char *value = (const char *)sqlite3_column_text(st, 1);
        if (!key || cb_str_starts(key, "player.") || cb_str_starts(key, "mixer.") ||
            cb_str_starts(key, "device.") || free_key(key))
            continue;
        CBConfig scratch;
        cb_config_defaults(&scratch);
        int r = cb_config_set(&scratch, key, value ? value : "");
        cb_config_free(&scratch);
        if (r >= 0)
            continue;
        if (r == CB_ERROR(ENOENT))
            cb_info("settings: %s is no setting cyanbox knows; removed", key);
        else
            cb_info("settings: %s = \"%s\" is not valid (it takes %s); removed, the default "
                    "stands", key, value ? value : "", cb_config_takes(key));
        char *k = cb_strdup(key);
        if (!k || !cb_array_push(&gone, &k)) {
            free(k);
            ret = CB_ERROR(ENOMEM);
            break;
        }
    }
    sqlite3_finalize(st);

    if (ret >= 0 && gone.nb) {
        ret = cb_db_begin(lib->db);
        for (size_t i = 0; ret >= 0 && i < gone.nb; i++)
            ret = put(lib, *(char **)cb_array_at(&gone, i), NULL);
        if (ret >= 0)
            ret = cb_db_commit(lib->db);
        else
            cb_db_rollback(lib->db);
        if (ret >= 0)
            *removed = (int)gone.nb;
    }
    for (size_t i = 0; i < gone.nb; i++)
        free(*(char **)cb_array_at(&gone, i));
    cb_array_free(&gone);
    return ret;
}

int cb_core_config_load(CBLibrary *lib)
{
    cb_config_free(&lib->cfg);
    cb_config_defaults(&lib->cfg);

    sqlite3_stmt *st = cb_db_prepare(lib->db, "SELECT key, value FROM setting ORDER BY key");
    if (!st)
        return CB_ERROR(EIO);
    int ret;
    while ((ret = cb_db_row(lib->db, st, "loading settings")) > 0) {
        const char *key = (const char *)sqlite3_column_text(st, 0);
        const char *value = (const char *)sqlite3_column_text(st, 1);
        ret = apply_setting(&lib->cfg, key, value ? value : "");
        if (ret < 0)
            break;
    }
    sqlite3_finalize(st);
    return ret;
}

/* ------------------------------------------------------------------ save */

static int put(CBLibrary *lib, const char *key, const char *value)
{
    sqlite3_stmt *st = cb_db_stmt(lib->db, value ? CB_STMT_SETTING_PUT : CB_STMT_SETTING_DELETE);
    cb_db_bind_text(lib->db, st, 1, key);
    if (value)
        cb_db_bind_text(lib->db, st, 2, value);
    return cb_db_done(lib->db, st, key);
}

static int put_group(CBLibrary *lib, const char *prefix, const CBKv *kv, int nb)
{
    /* Every key of the group goes, so a value the user cleared is gone:
     * "player." <= key < "player/" ('/' follows '.'). */
    char lo[32], hi[32];
    snprintf(lo, sizeof(lo), "%s.", prefix);
    snprintf(hi, sizeof(hi), "%s/", prefix);
    sqlite3_stmt *st = cb_db_stmt(lib->db, CB_STMT_SETTING_DELETE_RANGE);
    cb_db_bind_text(lib->db, st, 1, lo);
    cb_db_bind_text(lib->db, st, 2, hi);
    int ret = cb_db_done(lib->db, st, prefix);

    for (int i = 0; ret >= 0 && i < nb; i++) {
        char *key = cb_asprintf("%s.%s", prefix, kv[i].key);
        if (!key)
            return CB_ERROR(ENOMEM);
        ret = put(lib, key, kv[i].value ? kv[i].value : "");
        free(key);
    }
    return ret;
}

int cb_library_config_save(CBLibrary *lib)
{
    const CBConfig *c = &lib->cfg;
    char bpm_min[32], bpm_max[32], what[32], bars[32], split_p[32], split_t[32];
    const struct {
        const char *key, *value;
    } rows[] = {
        { "analysis.bpm_min",        cb_core_fmt_double(c->bpm_min, bpm_min, sizeof(bpm_min)) },
        { "analysis.bpm_max",        cb_core_fmt_double(c->bpm_max, bpm_max, sizeof(bpm_max)) },
        { "analysis.snap_bpm",       c->snap_bpm ? "true" : "false" },
        { "analysis.auto_cues",      c->auto_cues ? "true" : "false" },
        { "analysis.allow_sections", c->allow_sections ? "true" : "false" },
        { "analysis.what",           fmt_what(c->analyse_what, what, sizeof(what)) },
        { "import.comment",          c->import_comment ? "true" : "false" },
        { "export.key_notation",     c->key_notation },
        { "export.pdb",              c->export_pdb ? "true" : "false" },
        { "export.onelibrary",       c->export_onelibrary ? "true" : "false" },
        { "export.compat_transcode", c->compat_transcode ? "true" : "false" },
        { "export.format",
          format_names[CB_CLAMP(c->export_format, 0, (int)CB_ARRAY_ELEMS(format_names) - 1)] },
        { "export.device_name",      c->device_name },
        { "export.background_color",
          background_names[CB_CLAMP(c->background_color, 0, CB_TRACK_COLORS)] },
        { "link.mp3",                c->link_mp3 ? "true" : "false" },
        { "audio.sink",              c->audio_sink },
        { "ml.model",                c->ml_model },
        { "ui.traffic_light",        c->traffic_light },
        { "ui.detail_bars",          cb_core_fmt_double(c->detail_bars, bars, sizeof(bars)) },
        { "ui.split_player",         cb_core_fmt_double(c->split_player, split_p, sizeof(split_p)) },
        { "ui.split_tree",           cb_core_fmt_double(c->split_tree, split_t, sizeof(split_t)) },
    };

    int ret = cb_db_begin(lib->db);
    if (ret < 0)
        return ret;
    for (size_t i = 0; ret >= 0 && i < CB_ARRAY_ELEMS(rows); i++)
        ret = put(lib, rows[i].key, rows[i].value);
    /* A description at its default has no row. */
    for (int i = 0; ret >= 0 && i < CB_TRACK_COLORS; i++)
        ret = put(lib, vibe_keys[i], c->vibes[i]);
    if (ret >= 0)
        ret = put_group(lib, "player", c->player, c->nb_player);
    if (ret >= 0)
        ret = put_group(lib, "mixer", c->mixer, c->nb_mixer);
    if (ret >= 0)
        ret = put_group(lib, "device", c->device, c->nb_device);
    if (ret < 0) {
        cb_db_rollback(lib->db);
        return ret;
    }

    ret = cb_db_commit(lib->db);
    if (ret >= 0)
        cb_core_changed(lib, 0);
    return ret;
}

int cb_library_setting_get(const CBLibrary *lib, const char *key, char **value)
{
    *value = NULL;
    sqlite3_stmt *st = cb_db_stmt(lib->db, CB_STMT_SETTING_GET);
    cb_db_bind_text(lib->db, st, 1, key);
    int ret = cb_db_row(lib->db, st, key);
    if (ret <= 0)
        return ret < 0 ? ret : CB_ERROR(ENOENT);
    int err = 0;
    *value = cb_db_column_text(st, 0, &err);
    sqlite3_reset(st);
    if (!*value && !err)
        return CB_ERROR(ENOENT);
    return err;
}

int cb_library_setting_set(CBLibrary *lib, const char *key, const char *value)
{
    if (!key || !*key)
        return CB_ERROR(EINVAL);
    int ret = put(lib, key, value);
    if (ret >= 0)
        cb_core_changed(lib, 0);
    return ret;
}
