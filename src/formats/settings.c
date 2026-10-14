/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* My Settings files: MYSETTING.DAT, MYSETTING2.DAT (players),
 * DJMMYSETTING.DAT (mixer) and DEVSETTING.DAT (per stick display), a port
 * of dj's formats/settings.py; layout, CRC and every value in
 * docs/settings.md.
 *
 * Payloads start from rekordbox 7.2.8's defaults (the fixture in
 * tests/data/settings), but for the tempo range, and each setting is one
 * byte chosen from a fixed set. Encoding the fixture's decoded values gives
 * the fixture back byte for byte (tests/formats/test_settings.c). Players
 * apply [player] and [mixer] only when the DJ loads My Settings;
 * DEVSETTING applies directly.
 *
 * The tempo range defaults to +-6 % where rekordbox writes +-10 %: rxbox's
 * deck starts at the range this setting holds (rx_settings_get), as a CDJ
 * switched on starts at +-6 %, so the default is the range wanted on both
 * (docs/settings.md 3). The waveform colour defaults to 3-band where
 * rekordbox writes blue, for the same reason: the interface draws the
 * palette this setting holds, and the user wants 3-band on both.
 *
 * The players' background colour is not in any of these files: rekordbox
 * keeps it in export.pdb's property row and exportLibrary.db's property
 * table (rbxport, observed with rekordbox 7.2.14), so it is an export
 * setting (export.background_color, src/core/config.c), not one of these. */

#include "rxbox/formats.h"

#include <stdio.h>
#include <strings.h>


typedef struct Option {
    const char *name;
    uint8_t     byte;
} Option;

typedef struct Field {
    const char  *name;
    int          offset;          /* in the payload */
    bool         onoff;           /* true/false (and on/off, 1/0) */
    const Option opts[20];        /* names in dj's order; ends at name NULL */
} Field;

#define ONOFF(nm, off, ...) { .name = nm, .offset = off, .onoff = true, \
                              .opts = { { "off", 0x80 }, { "on", 0x81 }, __VA_ARGS__ } }

/* MYSETTING.DAT (docs/settings.md 3). */
static const Field player_fields[] = {
    ONOFF("on_air_display", 8),
    { "lcd_brightness", 9, false, { { "1", 0x81 }, { "2", 0x82 }, { "3", 0x83 }, { "4", 0x84 },
                                    { "5", 0x85 } } },
    ONOFF("quantize", 10),
    { "auto_cue_level", 11, false, { { "-36", 0x80 }, { "-42", 0x81 }, { "-48", 0x82 }, { "-54", 0x83 },
                                     { "-60", 0x84 }, { "-66", 0x85 }, { "-72", 0x86 }, { "-78", 0x87 },
                                     { "memory", 0x88 } } },
    { "language", 12, false, { { "english", 0x81 }, { "french", 0x82 }, { "german", 0x83 },
                               { "italian", 0x84 }, { "dutch", 0x85 }, { "spanish", 0x86 },
                               { "russian", 0x87 }, { "korean", 0x88 }, { "chinese_simplified", 0x89 },
                               { "chinese_traditional", 0x8A }, { "japanese", 0x8B },
                               { "portuguese", 0x8C }, { "swedish", 0x8D }, { "czech", 0x8E },
                               { "hungarian", 0x8F }, { "danish", 0x90 }, { "greek", 0x91 },
                               { "turkish", 0x92 } } },
    { "jog_ring_brightness", 14, false, { { "off", 0x80 }, { "1", 0x81 }, { "2", 0x82 } } },
    ONOFF("jog_ring_indicator", 15),
    ONOFF("slip_flashing", 16),
    { "disc_slot_illumination", 20, false, { { "off", 0x80 }, { "dark", 0x81 }, { "bright", 0x82 } } },
    ONOFF("eject_lock", 21),
    ONOFF("sync", 22),
    { "play_mode", 23, false, { { "continue", 0x80 }, { "single", 0x81 } } },
    { "quantize_beat_value", 24, false, { { "1", 0x80 }, { "1/2", 0x81 }, { "1/4", 0x82 },
                                          { "1/8", 0x83 } } },
    /* 0x82 is "rekordbox SETTING": the track's own auto-load flag decides
     * (pyrekordbox's doc says 0x83; its code and the real file use 0x82). */
    ONOFF("hot_cue_autoload", 25, { "rekordbox", 0x82 }),
    ONOFF("hot_cue_color", 26),
    ONOFF("needle_lock", 29),
    { "time_mode", 32, false, { { "elapsed", 0x80 }, { "remain", 0x81 } } },
    { "jog_mode", 33, false, { { "cdj", 0x80 }, { "vinyl", 0x81 } } },
    ONOFF("auto_cue", 34),
    ONOFF("master_tempo", 35),
    { "tempo_range", 36, false, { { "6", 0x80 }, { "10", 0x81 }, { "16", 0x82 }, { "wide", 0x83 } } },
    { "phase_meter", 37, false, { { "1", 0x80 }, { "2", 0x81 } } },
};

/* MYSETTING2.DAT (docs/settings.md 4). */
static const Field player2_fields[] = {
    { "vinyl_speed_adjust", 0, false, { { "touch_release", 0x80 }, { "touch", 0x81 }, { "release", 0x82 } } },
    { "jog_display_mode", 1, false, { { "auto", 0x80 }, { "info", 0x81 }, { "simple", 0x82 },
                                      { "artwork", 0x83 } } },
    { "pad_brightness", 2, false, { { "1", 0x81 }, { "2", 0x82 }, { "3", 0x83 }, { "4", 0x84 } } },
    { "jog_lcd_brightness", 3, false, { { "1", 0x81 }, { "2", 0x82 }, { "3", 0x83 }, { "4", 0x84 },
                                        { "5", 0x85 } } },
    { "waveform_divisions", 4, false, { { "time", 0x80 }, { "phrase", 0x81 } } },
    { "waveform_area", 10, false, { { "waveform", 0x80 }, { "phase_meter", 0x81 } } },
    { "beat_jump_beats", 12, false, { { "1/2", 0x80 }, { "1", 0x81 }, { "2", 0x82 }, { "4", 0x83 },
                                      { "8", 0x84 }, { "16", 0x85 }, { "32", 0x86 }, { "64", 0x87 } } },
};

/* DJMMYSETTING.DAT (docs/settings.md 5). */
static const Field mixer_fields[] = {
    { "channel_fader_curve", 12, false, { { "steep_top", 0x80 }, { "linear", 0x81 },
                                          { "steep_bottom", 0x82 } } },
    { "crossfader_curve", 13, false, { { "constant_power", 0x80 }, { "slow_cut", 0x81 },
                                       { "fast_cut", 0x82 } } },
    ONOFF("headphones_pre_eq", 14),
    ONOFF("headphones_mono_split", 15),
    ONOFF("beat_fx_quantize", 16),
    ONOFF("mic_low_cut", 17),
    { "talk_over_mode", 18, false, { { "advanced", 0x80 }, { "normal", 0x81 } } },
    { "talk_over_level", 19, false, { { "-24", 0x80 }, { "-18", 0x81 }, { "-12", 0x82 }, { "-6", 0x83 } } },
    { "midi_channel", 20, false, { { "1", 0x80 }, { "2", 0x81 }, { "3", 0x82 }, { "4", 0x83 }, { "5", 0x84 },
                                   { "6", 0x85 }, { "7", 0x86 }, { "8", 0x87 }, { "9", 0x88 }, { "10", 0x89 },
                                   { "11", 0x8A }, { "12", 0x8B }, { "13", 0x8C }, { "14", 0x8D },
                                   { "15", 0x8E }, { "16", 0x8F } } },
    { "midi_button_type", 21, false, { { "toggle", 0x80 }, { "trigger", 0x81 } } },
    { "display_brightness", 22, false, { { "white", 0x80 }, { "1", 0x81 }, { "2", 0x82 }, { "3", 0x83 },
                                         { "4", 0x84 }, { "5", 0x85 } } },
    { "indicator_brightness", 23, false, { { "1", 0x80 }, { "2", 0x81 }, { "3", 0x82 } } },
    { "long_fader_curve", 24, false, { { "exponential", 0x80 }, { "smooth", 0x81 }, { "linear", 0x82 } } },
};

/* DEVSETTING.DAT (docs/settings.md 6): base 0x01, no 0x02 waveform colour. */
static const Field device_fields[] = {
    { "overview_waveform", 9, false, { { "half", 1 }, { "full", 2 } } },
    { "waveform_color", 10, false, { { "blue", 1 }, { "rgb", 3 }, { "3band", 4 } } },
    { "key_display", 12, false, { { "classic", 1 }, { "alphanumeric", 2 } } },
    { "waveform_position", 13, false, { { "center", 1 }, { "left", 2 } } },
};

typedef struct File {
    const char  *name;
    const char  *brand, *version;
    int          len;
    bool         crc_header;      /* DJMMYSETTING's CRC covers the header too */
    const uint8_t *defaults;
    const Field *fields;
    int          nb_fields;
} File;

/* rekordbox 7.2.8's default payloads (dj's DEFAULTS; docs/settings.md 3-7),
 * but MYSETTING's tempo range (byte 36) at 0x80, +-6 %, not 0x81. */
static const uint8_t mysetting_default[40] = {
    0x78, 0x56, 0x34, 0x12, 0x02, 0x00, 0x00, 0x00, 0x81, 0x83, 0x81, 0x88, 0x81, 0x01, 0x82, 0x81,
    0x81, 0x01, 0x01, 0x01, 0x82, 0x80, 0x80, 0x81, 0x80, 0x81, 0x80, 0x00, 0x00, 0x81, 0x00, 0x00,
    0x81, 0x81, 0x81, 0x80, 0x80, 0x80, 0x00, 0x00,
};
static const uint8_t mysetting2_default[40] = {
    0x81, 0x80, 0x83, 0x83, 0x81, 0x80, 0x80, 0x00, 0x00, 0x00, 0x80, 0x81, 0x85,
};
static const uint8_t djmmysetting_default[52] = {
    0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x81, 0x82, 0x80, 0x80,
    0x81, 0x81, 0x80, 0x81, 0x80, 0x80, 0x85, 0x82, 0x80, 0x80, 0x82, 0x84, 0x81, 0x81,
};
static const uint8_t devsetting_default[32] = {
    0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x04, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01,
};

static const File files[] = {
    { "MYSETTING.DAT", "PIONEER", "0.001", 40, false, mysetting_default,
      player_fields, RX_ARRAY_ELEMS(player_fields) },
    { "MYSETTING2.DAT", "PIONEER", "0.001", 40, false, mysetting2_default,
      player2_fields, RX_ARRAY_ELEMS(player2_fields) },
    { "DJMMYSETTING.DAT", "PioneerDJ", "1.000", 52, true, djmmysetting_default,
      mixer_fields, RX_ARRAY_ELEMS(mixer_fields) },
    { "DEVSETTING.DAT", "PIONEER DJ", "7.2.8", 32, false, devsetting_default,
      device_fields, RX_ARRAY_ELEMS(device_fields) },
};

static const File *find_file(const char *name)
{
    for (size_t i = 0; i < RX_ARRAY_ELEMS(files); i++)
        if (!strcasecmp(files[i].name, name))
            return &files[i];
    return NULL;
}

static const Field *find_field(const File *f, const char *key)
{
    for (int i = 0; i < f->nb_fields; i++)
        if (!strcmp(f->fields[i].name, key))
            return &f->fields[i];
    return NULL;
}

/* CRC-16/XMODEM: poly 0x1021, init 0 (docs/settings.md 2.1). */
static uint16_t crc16_xmodem(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++)
            crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* dj's _norm(): lower case, spaces to '_', '&' and '%' dropped, "__" to
 * "_", and an integer in canonical form ("+6" and "06" are 6). */
static void normalise(const char *in, char *out, size_t size)
{
    char tmp[64];
    size_t n = 0;
    while (*in == ' ' || *in == '\t')
        in++;
    for (; *in && n + 1 < sizeof(tmp); in++) {
        char c = *in;
        if (c == '&' || c == '%')
            continue;
        if (c == ' ')
            c = '_';
        tmp[n++] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    while (n && (tmp[n - 1] == '_' || tmp[n - 1] == '\t'))
        n--;
    tmp[n] = '\0';

    size_t o = 0;
    for (size_t i = 0; tmp[i] && o + 1 < size; i++) {
        if (tmp[i] == '_' && tmp[i + 1] == '_')
            i++;
        out[o++] = tmp[i];
    }
    out[o] = '\0';

    char *end;
    long v = strtol(out, &end, 10);
    if (end != out && !*end)
        snprintf(out, size, "%ld", v);
}

static int lookup(const Field *f, const char *value, uint8_t *byte)
{
    char v[64];
    normalise(value, v, sizeof(v));
    if (f->onoff) {
        if (!strcmp(v, "true") || !strcmp(v, "1"))
            snprintf(v, sizeof(v), "on");
        else if (!strcmp(v, "false") || !strcmp(v, "0"))
            snprintf(v, sizeof(v), "off");
    } else if (!strcmp(v, "true") || !strcmp(v, "false")) {
        return RX_ERROR(EINVAL);
    }
    for (int i = 0; f->opts[i].name; i++) {
        char o[64];
        normalise(f->opts[i].name, o, sizeof(o));
        if (!strcmp(o, v)) {
            *byte = f->opts[i].byte;
            return 0;
        }
    }
    return RX_ERROR(EINVAL);
}

static void options_text(const Field *f, char *buf, size_t size)
{
    size_t o = 0;
    buf[0] = '\0';
    if (f->onoff) {
        o += (size_t)snprintf(buf, size, "true, false");
        for (int i = 2; f->opts[i].name && o < size; i++)
            o += (size_t)snprintf(buf + o, size - o, ", %s", f->opts[i].name);
        return;
    }
    for (int i = 0; f->opts[i].name && o < size; i++)
        o += (size_t)snprintf(buf + o, size - o, "%s%s", i ? ", " : "", f->opts[i].name);
}

int rx_settings_encode(const char *file_name, const RXKv *kv, int nb, uint8_t **out, size_t *size)
{
    const File *f = find_file(file_name);
    if (!f)
        return RX_ERROR(EINVAL);

    uint8_t payload[64];
    memcpy(payload, f->defaults, (size_t)f->len);
    for (int i = 0; i < nb; i++) {
        const Field *fl = find_field(f, kv[i].key);
        if (!fl) {
            rx_err("%s: unknown setting '%s'; see `rxbox settings`", f->name, kv[i].key);
            return RX_ERROR(EINVAL);
        }
        uint8_t byte;
        if (lookup(fl, kv[i].value ? kv[i].value : "", &byte) < 0) {
            char opts[512];
            options_text(fl, opts, sizeof(opts));
            rx_err("%s: %s = '%s' is not one of %s", f->name, fl->name,
                   kv[i].value ? kv[i].value : "", opts);
            return RX_ERROR(EINVAL);
        }
        payload[fl->offset] = byte;
    }

    size_t total = 0x68 + (size_t)f->len + 4;
    uint8_t *b = calloc(1, total);
    if (!b)
        return RX_ERROR(ENOMEM);
    /* Header: len_strings 0x60, three NUL-padded 32-byte strings, len_data
     * (docs/settings.md 2). */
    b[0] = 0x60;
    memcpy(b + 0x04, f->brand, strlen(f->brand));
    memcpy(b + 0x24, "rekordbox", 9);
    memcpy(b + 0x44, f->version, strlen(f->version));
    b[0x64] = (uint8_t)f->len;
    memcpy(b + 0x68, payload, (size_t)f->len);
    uint16_t crc = f->crc_header ? crc16_xmodem(b, 0x68 + (size_t)f->len)
                                 : crc16_xmodem(payload, (size_t)f->len);
    b[0x68 + f->len] = (uint8_t)crc;
    b[0x69 + f->len] = (uint8_t)(crc >> 8);

    *out = b;
    *size = total;
    return 0;
}

/* The listed form of a byte (dj's decode: the first name for it); NULL
 * for a byte the field has no name for. */
static const char *byte_listed(const Field *f, uint8_t byte)
{
    for (int i = 0; f->opts[i].name; i++) {
        if (f->opts[i].byte != byte)
            continue;
        if (f->onoff && i < 2)
            return i ? "true" : "false";
        return f->opts[i].name;
    }
    return NULL;
}

/* The config form of a stored byte; "0x99" for one without a name. */
static void byte_name(const Field *f, uint8_t byte, char *buf, size_t size)
{
    const char *name = byte_listed(f, byte);
    if (name)
        snprintf(buf, size, "%s", name);
    else
        snprintf(buf, size, "0x%02x", byte);
}

void rx_kv_free(RXKv *kv, int nb)
{
    if (!kv)
        return;
    for (int i = 0; i < nb; i++) {
        free(kv[i].key);
        free(kv[i].value);
    }
    free(kv);
}

int rx_settings_decode(const char *file_name, const uint8_t *data, size_t size, RXKv **kv, int *nb)
{
    *kv = NULL;
    *nb = 0;
    const File *f = find_file(file_name);
    if (!f || size < 0x68 + (size_t)f->len)
        return RX_ERROR(EINVAL);
    RXKv *out = calloc((size_t)f->nb_fields, sizeof(*out));
    if (!out)
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < f->nb_fields; i++) {
        char v[32];
        byte_name(&f->fields[i], data[0x68 + f->fields[i].offset], v, sizeof(v));
        out[i].key = rx_strdup(f->fields[i].name);
        out[i].value = rx_strdup(v);
        if (!out[i].key || !out[i].value) {
            rx_kv_free(out, f->nb_fields);
            return RX_ERROR(ENOMEM);
        }
    }
    *kv = out;
    *nb = f->nb_fields;
    return 0;
}

static int add_file(RXSettingsFile files_out[4], int *nb, const char *name, const RXKv *kv, int nb_kv)
{
    const File *f = find_file(name);
    uint8_t *data;
    size_t size;
    int ret = rx_settings_encode(name, kv, nb_kv, &data, &size);
    if (ret < 0)
        return ret;
    files_out[*nb] = (RXSettingsFile) {
        .name = f->name,
        .data = data,
        .size = size,
    };
    (*nb)++;
    return 0;
}

int rx_settings_from_config(const RXConfig *cfg, RXSettingsFile files_out[4], int *nb)
{
    int ret = 0;
    *nb = 0;
    RXKv *p1 = NULL, *p2 = NULL;
    int n1 = 0, n2 = 0;

    /* [player] covers MYSETTING.DAT and MYSETTING2.DAT (dj's
     * files_from_config); a key in neither is an error. */
    if (cfg->nb_player) {
        p1 = calloc((size_t)cfg->nb_player, sizeof(*p1));
        p2 = calloc((size_t)cfg->nb_player, sizeof(*p2));
        if (!p1 || !p2) {
            ret = RX_ERROR(ENOMEM);
            goto fail;
        }
        for (int i = 0; i < cfg->nb_player; i++) {
            if (find_field(&files[0], cfg->player[i].key)) {
                p1[n1++] = cfg->player[i];
            } else if (find_field(&files[1], cfg->player[i].key)) {
                p2[n2++] = cfg->player[i];
            } else {
                rx_err("[player]: unknown setting '%s'; see `rxbox settings`", cfg->player[i].key);
                ret = RX_ERROR(EINVAL);
                goto fail;
            }
        }
        if ((ret = add_file(files_out, nb, "MYSETTING.DAT", p1, n1)) < 0 ||
            (ret = add_file(files_out, nb, "MYSETTING2.DAT", p2, n2)) < 0)
            goto fail;
    }
    if (cfg->nb_mixer && (ret = add_file(files_out, nb, "DJMMYSETTING.DAT", cfg->mixer, cfg->nb_mixer)) < 0)
        goto fail;
    if (cfg->nb_device && (ret = add_file(files_out, nb, "DEVSETTING.DAT", cfg->device,
                                          cfg->nb_device)) < 0)
        goto fail;
    free(p1);
    free(p2);
    return 0;

fail:
    free(p1);
    free(p2);
    for (int i = 0; i < *nb; i++)
        free(files_out[i].data);
    *nb = 0;
    return ret;
}

const char *rx_settings_value(const char *file, const char *name, const char *value)
{
    if (!file || !name || !value)
        return NULL;
    const File *f = find_file(file);
    if (!f)
        return NULL;
    const Field *fl = find_field(f, name);
    uint8_t byte;
    if (!fl || lookup(fl, value, &byte) < 0)
        return NULL;
    /* The first name for the byte, as byte_name() and the list give it. */
    return byte_listed(fl, byte);
}

bool rx_settings_valid(const char *key, const char *value)
{
    if (!key || !value)
        return false;
    const File *in[2] = { NULL, NULL };
    const char *name;
    if (rx_str_starts(key, "player.")) {
        name = key + 7;
        in[0] = find_file("MYSETTING.DAT");
        in[1] = find_file("MYSETTING2.DAT");
    } else if (rx_str_starts(key, "mixer.")) {
        name = key + 6;
        in[0] = find_file("DJMMYSETTING.DAT");
    } else if (rx_str_starts(key, "device.")) {
        name = key + 7;
        in[0] = find_file("DEVSETTING.DAT");
    } else {
        return false;
    }
    for (int i = 0; i < 2 && in[i]; i++) {
        if (find_field(in[i], name))
            return rx_settings_value(in[i]->name, name, value) != NULL;
    }
    return false;
}

/* The My Settings of one group that are valid, the others said and
 * dropped; true when any went. */
static bool clean_group(const char *prefix, RXKv **kv, int *nb, int *removed)
{
    int kept = 0;
    bool any = false;
    for (int i = 0; i < *nb; i++) {
        RXKv *e = &(*kv)[i];
        char key[96];
        snprintf(key, sizeof(key), "%s.%s", prefix, e->key ? e->key : "");
        if (e->key && rx_settings_valid(key, e->value ? e->value : "")) {
            (*kv)[kept++] = *e;
            continue;
        }
        if (e->key && rx_settings_get(NULL, key))
            rx_info("settings: %s = \"%s\" is no value it takes; removed, the default stands", key,
                    e->value ? e->value : "");
        else
            rx_info("settings: %s is no My Setting; removed", key);
        free(e->key);
        free(e->value);
        (*removed)++;
        any = true;
    }
    *nb = kept;
    return any;
}

int rx_settings_clean(RXLibrary *lib)
{
    RXConfig *c = &lib->cfg;
    int removed = 0;
    bool any = clean_group("player", &c->player, &c->nb_player, &removed);
    any |= clean_group("mixer", &c->mixer, &c->nb_mixer, &removed);
    any |= clean_group("device", &c->device, &c->nb_device, &removed);
    if (!any)
        return 0;
    int ret = rx_library_config_save(lib);
    if (ret < 0) {
        rx_warn("settings: the My Settings that are not valid could not be removed: %s",
                strerror(-ret));
        return ret;
    }
    return removed;
}

const char *rx_settings_get(const RXConfig *cfg, const char *key)
{
    if (!key)
        return NULL;
    const RXKv *kv = NULL;
    int nb = 0;
    const File *in[2] = { NULL, NULL };
    const char *name;
    if (rx_str_starts(key, "player.")) {
        name = key + 7;
        if (cfg) {
            kv = cfg->player;
            nb = cfg->nb_player;
        }
        in[0] = find_file("MYSETTING.DAT");
        in[1] = find_file("MYSETTING2.DAT");
    } else if (rx_str_starts(key, "mixer.")) {
        name = key + 6;
        if (cfg) {
            kv = cfg->mixer;
            nb = cfg->nb_mixer;
        }
        in[0] = find_file("DJMMYSETTING.DAT");
    } else if (rx_str_starts(key, "device.")) {
        name = key + 7;
        if (cfg) {
            kv = cfg->device;
            nb = cfg->nb_device;
        }
        in[0] = find_file("DEVSETTING.DAT");
    } else {
        return NULL;
    }

    const char *stored = NULL;
    for (int i = 0; i < nb; i++) {
        if (!strcmp(kv[i].key, name))
            stored = kv[i].value ? kv[i].value : "";
    }
    for (int i = 0; i < 2 && in[i]; i++) {
        const Field *fl = find_field(in[i], name);
        if (!fl)
            continue;
        const char *v = stored ? rx_settings_value(in[i]->name, name, stored) : NULL;
        return v ? v : byte_listed(fl, in[i]->defaults[fl->offset]);
    }
    return NULL;
}

void rx_settings_list(rx_settings_list_cb cb, void *opaque)
{
    for (size_t i = 0; i < RX_ARRAY_ELEMS(files); i++) {
        const File *f = &files[i];
        for (int k = 0; k < f->nb_fields; k++) {
            const Field *fl = &f->fields[k];
            const char *values[24];
            int n = 0;
            if (fl->onoff) {
                /* As decode prints them; dj listed plain on/off settings
                 * as true | false too. */
                values[n++] = "true";
                values[n++] = "false";
                for (int j = 2; fl->opts[j].name; j++)
                    values[n++] = fl->opts[j].name;
            } else {
                for (int j = 0; fl->opts[j].name; j++)
                    values[n++] = fl->opts[j].name;
            }
            char deflt[32];
            byte_name(fl, f->defaults[fl->offset], deflt, sizeof(deflt));
            cb(opaque, f->name, fl->name, values, n, deflt);
        }
    }
}
