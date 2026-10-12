/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The settings (rxbox/library.h RXConfig) and where the library lives.
 *
 * Settings are rows of the `setting` table, text keys and text values:
 *
 *   analysis.bpm_min  analysis.bpm_max  analysis.snap_bpm  analysis.auto_cues
 *   analysis.allow_sections  analysis.what
 *   export.key_notation  export.pdb  export.onelibrary  export.compat_transcode
 *   export.device_name
 *   player.<name>  mixer.<name>  device.<name>     My Settings, names as
 *                                                  `rxbox settings` lists them
 *   audio.sink
 *   ui.wave_palette  ui.detail_bars  ui.split_player  ui.split_tree
 *
 * Booleans are "true"/"false", numbers their shortest exact decimal form,
 * analysis.what the names `rxbox analyze --what` takes ("beats,key,waves").
 * A value that does not parse is logged and the default kept, so a bad row
 * never stops the library from opening. Other keys are free for
 * rx_library_setting_get/set (the interface's column layout and such) and
 * are left alone by rx_library_config_save. */

#include "core.h"

#include <math.h>
#include <stdio.h>

#include "rxbox/analysis.h"

void rx_config_defaults(RXConfig *c)
{
    *c = (RXConfig) {
        .snap_bpm          = true,
        .auto_cues         = true,
        .allow_sections    = true,
        .analyse_what      = RX_ANALYSE_ALL,
        .key_notation      = "classic",
        .export_pdb        = true,
        .export_onelibrary = true,
        .wave_palette      = "3band",
        .detail_bars       = 12,
    };
}

static void kv_free(RXKv **kv, int *nb)
{
    for (int i = 0; i < *nb; i++) {
        free((*kv)[i].key);
        free((*kv)[i].value);
    }
    free(*kv);
    *kv = NULL;
    *nb = 0;
}

void rx_config_free(RXConfig *c)
{
    if (!c)
        return;
    free(c->device_name);
    free(c->audio_sink);
    kv_free(&c->player, &c->nb_player);
    kv_free(&c->mixer, &c->nb_mixer);
    kv_free(&c->device, &c->nb_device);
    rx_config_defaults(c);
}

char *rx_library_default_root(void)
{
    const char *env = getenv("RXBOX_LIBRARY");
    if (env && *env)
        return rx_path_expand(env);
    const char *home = getenv("HOME");
    if (!home || !*home)
        return NULL;
    return rx_asprintf("%s/.local/share/rxbox", home);
}

const char *rx_core_fmt_double(double v, char *buf, size_t size)
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
    { "beats", RX_ANALYSE_BEATS },
    { "key",   RX_ANALYSE_KEY   },
    { "waves", RX_ANALYSE_WAVES },
};

static bool parse_what(const char *s, unsigned *out)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", s);
    unsigned what = 0;
    char *save = NULL;
    for (char *w = strtok_r(buf, ", ", &save); w; w = strtok_r(NULL, ", ", &save)) {
        size_t i = 0;
        while (i < RX_ARRAY_ELEMS(what_names) && strcmp(w, what_names[i].name))
            i++;
        if (i == RX_ARRAY_ELEMS(what_names))
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
    for (size_t i = 0; i < RX_ARRAY_ELEMS(what_names); i++) {
        size_t len = strlen(buf);
        if (what & what_names[i].bit)
            snprintf(buf + len, size - len, "%s%s", len ? "," : "", what_names[i].name);
    }
    /* Nothing asked: the row goes and the default comes back. */
    return buf[0] ? buf : NULL;
}

static int kv_push(RXKv **kv, int *nb, const char *key, const char *value)
{
    RXKv *n = realloc(*kv, (size_t)(*nb + 1) * sizeof(*n));
    if (!n)
        return RX_ERROR(ENOMEM);
    *kv = n;
    char *k = rx_strdup(key), *v = rx_strdup(value);
    if (!k || !v) {
        free(k);
        free(v);
        return RX_ERROR(ENOMEM);
    }
    n[(*nb)++] = (RXKv) {
        .key   = k,
        .value = v,
    };
    return 0;
}

static int set_string(char **field, const char *value)
{
    char *copy = *value ? rx_strdup(value) : NULL;
    if (*value && !copy)
        return RX_ERROR(ENOMEM);
    free(*field);
    *field = copy;
    return 0;
}

static void bad_value(const char *key, const char *value)
{
    rx_warn("setting %s = \"%s\" is not valid; using the default", key, value);
}

static int apply_setting(RXConfig *c, const char *key, const char *value)
{
    double d;

    if (rx_str_starts(key, "player."))
        return kv_push(&c->player, &c->nb_player, key + 7, value);
    if (rx_str_starts(key, "mixer."))
        return kv_push(&c->mixer, &c->nb_mixer, key + 6, value);
    if (rx_str_starts(key, "device."))
        return kv_push(&c->device, &c->nb_device, key + 7, value);

    if (!strcmp(key, "analysis.bpm_min") || !strcmp(key, "analysis.bpm_max")) {
        if (!parse_double(value, &d) || d < 0)
            bad_value(key, value);
        else if (!strcmp(key, "analysis.bpm_min"))
            c->bpm_min = d;
        else
            c->bpm_max = d;
    } else if (!strcmp(key, "analysis.snap_bpm")) {
        if (!parse_bool(value, &c->snap_bpm))
            bad_value(key, value);
    } else if (!strcmp(key, "analysis.auto_cues")) {
        if (!parse_bool(value, &c->auto_cues))
            bad_value(key, value);
    } else if (!strcmp(key, "analysis.allow_sections")) {
        if (!parse_bool(value, &c->allow_sections))
            bad_value(key, value);
    } else if (!strcmp(key, "analysis.what")) {
        if (!parse_what(value, &c->analyse_what))
            bad_value(key, value);
    } else if (!strcmp(key, "export.key_notation")) {
        if (strcmp(value, "classic") && strcmp(value, "camelot") && strcmp(value, "open"))
            bad_value(key, value);
        else
            snprintf(c->key_notation, sizeof(c->key_notation), "%s", value);
    } else if (!strcmp(key, "export.pdb")) {
        if (!parse_bool(value, &c->export_pdb))
            bad_value(key, value);
    } else if (!strcmp(key, "export.onelibrary")) {
        if (!parse_bool(value, &c->export_onelibrary))
            bad_value(key, value);
    } else if (!strcmp(key, "export.compat_transcode")) {
        if (!parse_bool(value, &c->compat_transcode))
            bad_value(key, value);
    } else if (!strcmp(key, "export.device_name")) {
        return set_string(&c->device_name, value);
    } else if (!strcmp(key, "audio.sink")) {
        return set_string(&c->audio_sink, value);
    } else if (!strcmp(key, "ui.wave_palette")) {
        if (strcmp(value, "3band") && strcmp(value, "blue") && strcmp(value, "rgb"))
            bad_value(key, value);
        else
            snprintf(c->wave_palette, sizeof(c->wave_palette), "%s", value);
    } else if (!strcmp(key, "ui.detail_bars")) {
        if (!parse_double(value, &d) || d < 1 || d > 1024 || d != trunc(d))
            bad_value(key, value);
        else
            c->detail_bars = (int)d;
    } else if (!strcmp(key, "ui.split_player") || !strcmp(key, "ui.split_tree")) {
        if (!parse_double(value, &d) || d < 0)
            bad_value(key, value);
        else if (!strcmp(key, "ui.split_player"))
            c->split_player = (float)d;
        else
            c->split_tree = (float)d;
    }
    return 0;
}

int rx_core_config_load(RXLibrary *lib)
{
    rx_config_free(&lib->cfg);
    rx_config_defaults(&lib->cfg);

    sqlite3_stmt *st = rx_db_prepare(lib->db, "SELECT key, value FROM setting ORDER BY key");
    if (!st)
        return RX_ERROR(EIO);
    int ret;
    while ((ret = rx_db_row(lib->db, st, "loading settings")) > 0) {
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

static int put(RXLibrary *lib, const char *key, const char *value)
{
    sqlite3_stmt *st = rx_db_stmt(lib->db, value ? RX_STMT_SETTING_PUT : RX_STMT_SETTING_DELETE);
    rx_db_bind_text(lib->db, st, 1, key);
    if (value)
        rx_db_bind_text(lib->db, st, 2, value);
    return rx_db_done(lib->db, st, key);
}

static int put_group(RXLibrary *lib, const char *prefix, const RXKv *kv, int nb)
{
    /* Every key of the group goes, so a value the user cleared is gone:
     * "player." <= key < "player/" ('/' follows '.'). */
    char lo[32], hi[32];
    snprintf(lo, sizeof(lo), "%s.", prefix);
    snprintf(hi, sizeof(hi), "%s/", prefix);
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_SETTING_DELETE_RANGE);
    rx_db_bind_text(lib->db, st, 1, lo);
    rx_db_bind_text(lib->db, st, 2, hi);
    int ret = rx_db_done(lib->db, st, prefix);

    for (int i = 0; ret >= 0 && i < nb; i++) {
        char *key = rx_asprintf("%s.%s", prefix, kv[i].key);
        if (!key)
            return RX_ERROR(ENOMEM);
        ret = put(lib, key, kv[i].value ? kv[i].value : "");
        free(key);
    }
    return ret;
}

int rx_library_config_save(RXLibrary *lib)
{
    const RXConfig *c = &lib->cfg;
    char bpm_min[32], bpm_max[32], what[32], bars[32], split_p[32], split_t[32];
    const struct {
        const char *key, *value;
    } rows[] = {
        { "analysis.bpm_min",        rx_core_fmt_double(c->bpm_min, bpm_min, sizeof(bpm_min)) },
        { "analysis.bpm_max",        rx_core_fmt_double(c->bpm_max, bpm_max, sizeof(bpm_max)) },
        { "analysis.snap_bpm",       c->snap_bpm ? "true" : "false" },
        { "analysis.auto_cues",      c->auto_cues ? "true" : "false" },
        { "analysis.allow_sections", c->allow_sections ? "true" : "false" },
        { "analysis.what",           fmt_what(c->analyse_what, what, sizeof(what)) },
        { "export.key_notation",     c->key_notation },
        { "export.pdb",              c->export_pdb ? "true" : "false" },
        { "export.onelibrary",       c->export_onelibrary ? "true" : "false" },
        { "export.compat_transcode", c->compat_transcode ? "true" : "false" },
        { "export.device_name",      c->device_name },
        { "audio.sink",              c->audio_sink },
        { "ui.wave_palette",         c->wave_palette },
        { "ui.detail_bars",          rx_core_fmt_double(c->detail_bars, bars, sizeof(bars)) },
        { "ui.split_player",         rx_core_fmt_double(c->split_player, split_p, sizeof(split_p)) },
        { "ui.split_tree",           rx_core_fmt_double(c->split_tree, split_t, sizeof(split_t)) },
    };

    int ret = rx_db_begin(lib->db);
    if (ret < 0)
        return ret;
    for (size_t i = 0; ret >= 0 && i < RX_ARRAY_ELEMS(rows); i++)
        ret = put(lib, rows[i].key, rows[i].value);
    if (ret >= 0)
        ret = put_group(lib, "player", c->player, c->nb_player);
    if (ret >= 0)
        ret = put_group(lib, "mixer", c->mixer, c->nb_mixer);
    if (ret >= 0)
        ret = put_group(lib, "device", c->device, c->nb_device);
    if (ret < 0) {
        rx_db_rollback(lib->db);
        return ret;
    }

    ret = rx_db_commit(lib->db);
    if (ret >= 0)
        lib->generation++;
    return ret;
}

int rx_library_setting_get(const RXLibrary *lib, const char *key, char **value)
{
    *value = NULL;
    sqlite3_stmt *st = rx_db_stmt(lib->db, RX_STMT_SETTING_GET);
    rx_db_bind_text(lib->db, st, 1, key);
    int ret = rx_db_row(lib->db, st, key);
    if (ret <= 0)
        return ret < 0 ? ret : RX_ERROR(ENOENT);
    int err = 0;
    *value = rx_db_column_text(st, 0, &err);
    sqlite3_reset(st);
    if (!*value && !err)
        return RX_ERROR(ENOENT);
    return err;
}

int rx_library_setting_set(RXLibrary *lib, const char *key, const char *value)
{
    if (!key || !*key)
        return RX_ERROR(EINVAL);
    int ret = put(lib, key, value);
    if (ret >= 0)
        lib->generation++;
    return ret;
}
