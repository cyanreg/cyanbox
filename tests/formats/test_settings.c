/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* My Settings: dj's test_settings_match_rekordbox (the fixture stick's
 * owner changed tempo range and waveform colour; everything else is
 * rekordbox 7.2.8's default) and test_settings_errors, plus decoding each
 * fixture and encoding the decoded values back to the same bytes, and the
 * files the config's sections produce. */

#include <stdio.h>

#include "cyanbox/formats.h"

#include "tests/formats/test.h"

static const char *const files[4] = { "MYSETTING.DAT", "MYSETTING2.DAT", "DJMMYSETTING.DAT", "DEVSETTING.DAT" };

static int read_fixture(const char *name, uint8_t **b, size_t *n)
{
    char path[128];
    snprintf(path, sizeof(path), "tests/data/settings/%s", name);
    int ret = cb_file_read(path, b, n);
    if (ret < 0) {
        fprintf(stderr, "cannot read %s\n", path);
        failures++;
    }
    return ret;
}

static void match_rekordbox(void)
{
    CBKv changes[4][1] = {
        { { "tempo_range", "6" } }, { { 0 } }, { { 0 } }, { { "waveform_color", "rgb" } },
    };
    int nb[4] = { 1, 0, 0, 1 };
    for (int i = 0; i < 4; i++) {
        uint8_t *ref, *ours;
        size_t rn, on;
        if (read_fixture(files[i], &ref, &rn) < 0)
            continue;
        CHECK_EQ(cb_settings_encode(files[i], changes[i], nb[i], &ours, &on), 0);
        CHECK(on == rn && !memcmp(ours, ref, rn));
        free(ours);

        /* Decoding and encoding every value gives the file back. */
        CBKv *kv;
        int n;
        CHECK_EQ(cb_settings_decode(files[i], ref, rn, &kv, &n), 0);
        CHECK(n > 0);
        CHECK_EQ(cb_settings_encode(files[i], kv, n, &ours, &on), 0);
        CHECK(on == rn && !memcmp(ours, ref, rn));
        free(ours);
        for (int k = 0; k < n; k++) {
            if (i == 0 && !strcmp(kv[k].key, "tempo_range"))
                CHECK_STR(kv[k].value, "6");
            if (i == 0 && !strcmp(kv[k].key, "quantize"))
                CHECK_STR(kv[k].value, "true");
            if (i == 3 && !strcmp(kv[k].key, "waveform_color"))
                CHECK_STR(kv[k].value, "rgb");
        }
        cb_kv_free(kv, n);
        free(ref);
    }
}

static void errors(void)
{
    uint8_t *b;
    size_t n;
    CBKv bad_value = { "tempo_range", "12" };
    CHECK_EQ(cb_settings_encode("MYSETTING.DAT", &bad_value, 1, &b, &n), CB_ERROR(EINVAL));
    CBKv not_bool = { "tempo_range", "true" };
    CHECK_EQ(cb_settings_encode("MYSETTING.DAT", &not_bool, 1, &b, &n), CB_ERROR(EINVAL));
    CBKv unknown = { "bogus", "1" };
    CHECK_EQ(cb_settings_encode("MYSETTING.DAT", &unknown, 1, &b, &n), CB_ERROR(EINVAL));

    CBConfig cfg = { .player = &unknown, .nb_player = 1 };
    CBSettingsFile out[4];
    int nf;
    CHECK_EQ(cb_settings_from_config(&cfg, out, &nf), CB_ERROR(EINVAL));
    CHECK_EQ(nf, 0);

    /* dj's normalisation: case, spaces, '&' and '%' do not matter. */
    CBKv loose[2] = { { "vinyl_speed_adjust", "Touch & Release" }, { "beat_jump_beats", "16" } };
    CHECK_EQ(cb_settings_encode("MYSETTING2.DAT", loose, 2, &b, &n), 0);
    CHECK(n == 148 && b[0x68] == 0x80 && b[0x68 + 12] == 0x85);
    free(b);
    CBKv onoff[3] = { { "quantize", "off" }, { "sync", "1" }, { "quantize_beat_value", "1/2" } };
    CHECK_EQ(cb_settings_encode("MYSETTING.DAT", onoff, 3, &b, &n), 0);
    CHECK(b[0x68 + 10] == 0x80 && b[0x68 + 22] == 0x81 && b[0x68 + 24] == 0x81);
    free(b);
}

static void from_config(void)
{
    CBKv player[2] = { { "quantize", "true" }, { "jog_display_mode", "artwork" } };
    CBKv device[1] = { { "waveform_color", "3band" } };
    CBConfig cfg = { .player = player, .nb_player = 2, .device = device, .nb_device = 1 };
    CBSettingsFile out[4];
    int nf;
    CHECK_EQ(cb_settings_from_config(&cfg, out, &nf), 0);
    CHECK_EQ(nf, 3);
    if (nf == 3) {
        CHECK_STR(out[0].name, "MYSETTING.DAT");
        CHECK_STR(out[1].name, "MYSETTING2.DAT");
        CHECK_STR(out[2].name, "DEVSETTING.DAT");
        CHECK(out[1].size == 148 && out[1].data[0x68 + 1] == 0x83);
        CHECK(out[2].size == 140 && out[2].data[0x68 + 10] == 4);
    }
    for (int i = 0; i < nf; i++)
        free(out[i].data);
}

static void list_cb(void *opaque, const char *file, const char *name, const char *const *values,
                    int nb_values, const char *deflt)
{
    int *count = opaque;
    (*count)++;
    CHECK(file && name && nb_values > 0 && deflt);
    bool found = false;
    for (int i = 0; i < nb_values; i++)
        found |= !strcmp(values[i], deflt);
    if (!found)
        fprintf(stderr, "%s %s: default %s not among the values\n", file, name, deflt);
    CHECK(found);
    /* Every listed value is its own listed form. */
    for (int i = 0; i < nb_values; i++) {
        const char *v = cb_settings_value(file, name, values[i]);
        if (!v || strcmp(v, values[i]))
            fprintf(stderr, "%s %s: %s reads as %s\n", file, name, values[i], v ? v : "nothing");
        CHECK(v && !strcmp(v, values[i]));
    }
}

/* The listed form of what `cyanbox config` may have been given. */
static void listed_values(void)
{
    CHECK_STR(cb_settings_value("MYSETTING.DAT", "quantize", "on"), "true");
    CHECK_STR(cb_settings_value("MYSETTING.DAT", "quantize", "0"), "false");
    CHECK_STR(cb_settings_value("MYSETTING.DAT", "hot_cue_autoload", "rekordbox"), "rekordbox");
    CHECK_STR(cb_settings_value("MYSETTING.DAT", "tempo_range", "+6"), "6");
    CHECK_STR(cb_settings_value("MYSETTING.DAT", "tempo_range", "WIDE"), "wide");
    CHECK_STR(cb_settings_value("MYSETTING2.DAT", "vinyl_speed_adjust", "Touch & Release"),
              "touch_release");
    CHECK(!cb_settings_value("MYSETTING.DAT", "tempo_range", "12"));
    CHECK(!cb_settings_value("MYSETTING.DAT", "tempo_range", "true"));
    CHECK(!cb_settings_value("MYSETTING.DAT", "bogus", "1"));
    CHECK(!cb_settings_value("MYSETTING2.DAT", "quantize", "on"));
}

/* What the interface follows: the library's value in its listed form, else
 * the default, the tempo range's being +-6 % (not rekordbox's +-10 %). */
static void settings_get(void)
{
    CBKv player[3] = { { "quantize", "off" }, { "beat_jump_beats", "1/2" }, { "time_mode", "12" } };
    CBKv device[1] = { { "overview_waveform", "Full" } };
    CBConfig cfg = { .player = player, .nb_player = 3, .device = device, .nb_device = 1 };
    CHECK_STR(cb_settings_get(&cfg, "player.quantize"), "false");
    CHECK_STR(cb_settings_get(&cfg, "player.beat_jump_beats"), "1/2");
    CHECK_STR(cb_settings_get(&cfg, "player.time_mode"), "remain");
    CHECK_STR(cb_settings_get(&cfg, "player.tempo_range"), "6");
    CHECK_STR(cb_settings_get(&cfg, "player.auto_cue_level"), "memory");
    CHECK_STR(cb_settings_get(&cfg, "player.hot_cue_color"), "false");
    CHECK_STR(cb_settings_get(&cfg, "device.overview_waveform"), "full");
    CHECK_STR(cb_settings_get(&cfg, "device.waveform_color"), "3band");
    CHECK_STR(cb_settings_get(&cfg, "device.key_display"), "classic");
    /* The databases' background colour is an export setting, not one of
     * these (export.background_color). */
    CHECK(!cb_settings_get(&cfg, "device.background_color"));
    CHECK_STR(cb_settings_get(&cfg, "mixer.channel_fader_curve"), "linear");
    CHECK_STR(cb_settings_get(NULL, "player.quantize"), "true");
    CHECK(!cb_settings_get(&cfg, "player.bogus"));
    CHECK(!cb_settings_get(&cfg, "ui.traffic_light"));
    CHECK(!cb_settings_get(&cfg, "device.quantize"));
}

int main(void)
{
    match_rekordbox();
    errors();
    from_config();
    listed_values();
    settings_get();
    int count = 0;
    cb_settings_list(list_cb, &count);
    /* 22 + 7 + 13 + 4 dj settings. */
    CHECK_EQ(count, 22 + 7 + 13 + 4);
    return test_result("settings");
}
