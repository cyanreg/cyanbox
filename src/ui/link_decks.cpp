// Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

// The Link decks (docs/UI.md, "Link decks"). With --pro-link the deck's
// title row shows the players on the link instead of the deck's track
// (deck_view.cpp, ui_deck_panel), one cell each, side by side: the width
// is shared equally, each at most half of it, so two fill it and one alone
// takes its side (odd numbers left, even right); with none, it says so. The
// cells go by deck number, left to right: the number the DJ gave the player
// in the status bar's Link menu (below), else its own. A cell has the
// track's overview for its background, the part played shaded and the
// playhead on it when the player says where it is; over it, the cover and
// the title, the player's state and model, and right-aligned the key, the
// tempo at the player's pitch and the time left. The rest of the deck is
// unchanged: cyanbox's own, for editing tracks before they go to a player.
//
// Where the player is: the beat number of its status on the track's grid
// (the beats Link serves it: cb_grid_beats, anlz.c PQTZ; 0 before the
// first), carried forward between statuses (about five a second) at the
// pitch it plays at and kept within the beat the last one named. A
// pre-nexus player (a CDJ-900, a CDJ-2000) sends no beat number: no
// playhead, no time left.
//
// Tracks dropped on a cell, and the table's "Load in deck" items, go to
// the player as a load command (cb_app_link_load: it loads the track as
// one dragged onto it in rekordbox), but not while it plays with the
// players' EJECT/LOAD LOCK on (player.eject_lock).
//
// CYANBOX_UI_LINK_DECKS (the headless tests) makes up the players instead:
// a comma-separated list of NUMBER:NAME:TRACK:FLAGS:BEAT[:MAC], TRACK a
// library id (0 nothing loaded), FLAGS any of p (plays), a (on air), m
// (master), s (synced), BEAT its first beat number (-1 none sent), MAC the
// last byte of its hardware address 02:00:00:00:00:MAC (default NUMBER); a
// playing one's beat number moves on at its track's tempo. With them,
// CYANBOX_UI_LINK_CUE=NAME:ON:SOURCE makes up a mixer's LINK CUE (ON 1 or
// 0, SOURCE the device it elected, 17 cyanbox, 0 none), and
// CYANBOX_UI_LINK_JOINING=1 has cyanbox still joining the link.
//
// The deck numbers: a player's number on the link is the one it took when
// it started (the first free, in the order the players came up), so it
// says nothing of where the player stands. In the status bar's Link menu
// the DJ gives each player the number it is to show here, a click on one
// of a row of numbers, the player showing that number taking the clicked
// one's old number. A number is given to the player's hardware address
// (its keep-alives' MAC), kept in the library (ui.link_deck.N): the same
// player has it again when it comes up as another number. A player given
// none shows its own number while no other player was given that, else
// the lowest free. It is cyanbox's numbering only: the players and the
// mixer go on with theirs.

#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>


// The players heard: their indices in link_state.devices.
static int heard(const CBApp *app, int *idx, int max)
{
    const auto &ls = app->link_state;
    int n = 0;
    if (!ls.on)
        return 0;
    for (int i = 0; i < ls.nb_devices && n < max; i++) {
        const CBLinkPlayer &d = ls.devices[i];
        if (d.type == CB_LINK_DEVICE_PLAYER && d.number >= 1 && d.number < UI_LINK_DECKS)
            idx[n++] = i;
    }
    return n;
}

static bool no_mac(const uint8_t mac[6])
{
    static const uint8_t zero[6] = {};
    return !memcmp(mac, zero, 6);
}

// The numbers given, from the library's rows, at the first use.
static void given_read(UiState *ui)
{
    if (ui->link_given_read)
        return;
    ui->link_given_read = true;
    for (int k = 1; k <= UI_LINK_NUMBERS; k++) {
        char key[32], *v = nullptr;
        unsigned m[6];
        snprintf(key, sizeof(key), "ui.link_deck.%d", k);
        if (cb_library_setting_get(&ui->app->lib, key, &v) == 0 && v &&
            sscanf(v, "%2x:%2x:%2x:%2x:%2x:%2x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            for (int i = 0; i < 6; i++)
                ui->link_given[k][i] = (uint8_t)m[i];
        }
        free(v);
    }
}

// The number given to a player's address, 0 none.
static int given(UiState *ui, const uint8_t mac[6])
{
    given_read(ui);
    for (int k = 1; !no_mac(mac) && k <= UI_LINK_NUMBERS; k++) {
        if (!memcmp(ui->link_given[k], mac, 6))
            return k;
    }
    return 0;
}

static void put_given(UiState *ui, int k, const uint8_t *mac)
{
    char key[32], v[24];
    snprintf(key, sizeof(key), "ui.link_deck.%d", k);
    if (mac) {
        memcpy(ui->link_given[k], mac, 6);
        snprintf(v, sizeof(v), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
    } else {
        memset(ui->link_given[k], 0, 6);
    }
    if (cb_library_setting_set(&ui->app->lib, key, mac ? v : nullptr) < 0)
        cb_warn("ui: could not save deck %d's player", k);
}

// Gives number k (0 none) to an address, taken from whichever had it, and
// takes the address's old number from it.
static void give(UiState *ui, const uint8_t mac[6], int k)
{
    given_read(ui);
    if (no_mac(mac))
        return;
    for (int j = 1; j <= UI_LINK_NUMBERS; j++) {
        if (j != k && !memcmp(ui->link_given[j], mac, 6))
            put_given(ui, j, nullptr);
    }
    if (k >= 1 && k <= UI_LINK_NUMBERS && memcmp(ui->link_given[k], mac, 6))
        put_given(ui, k, mac);
}

// The players heard, by deck number: their indices in link_state.devices,
// and the number each shows, by its number on the link (0: no player has
// that one). The numbers given first, then their own, then the lowest free.
static int players(UiState *ui, int *idx, int max, int shown[UI_LINK_DECKS])
{
    const auto &ls = ui->app->link_state;
    int n = heard(ui->app, idx, max);
    bool taken[UI_LINK_DECKS] = {};
    std::fill(shown, shown + UI_LINK_DECKS, 0);
    for (int k = 0; k < n; k++) {
        const CBLinkPlayer &d = ls.devices[idx[k]];
        int g = given(ui, d.mac);
        if (g && !taken[g]) {
            shown[d.number] = g;
            taken[g] = true;
        }
    }
    for (int k = 0; k < n; k++) {
        const int number = ls.devices[idx[k]].number;
        if (!shown[number] && !taken[number]) {
            shown[number] = number;
            taken[number] = true;
        }
    }
    for (int k = 0; k < n; k++) {
        const int number = ls.devices[idx[k]].number;
        int f = 1;
        while (!shown[number] && taken[f])
            f++;
        if (!shown[number]) {
            shown[number] = f;
            taken[f] = true;
        }
    }
    std::sort(idx, idx + n, [&](int a, int b) {
        return shown[ls.devices[a].number] < shown[ls.devices[b].number];
    });
    return n;
}

// Player `index` shows number k from now on: the player showing k takes
// its old one.
static void renumber(UiState *ui, int index, int k, const int *idx, int n,
                     const int shown[UI_LINK_DECKS])
{
    const auto &ls = ui->app->link_state;
    const CBLinkPlayer &d = ls.devices[index];
    const int was = shown[d.number];
    if (was == k)
        return;
    for (int j = 0; j < n; j++) {
        const CBLinkPlayer &o = ls.devices[idx[j]];
        if (idx[j] != index && shown[o.number] == k)
            give(ui, o.mac, was <= UI_LINK_NUMBERS ? was : 0);
    }
    give(ui, d.mac, k);
}

// The library track a player has loaded, when it is ours: a rekordbox
// track loaded from our device number on the player's own link.
static CBTrack *ours(CBApp *app, int index)
{
    const auto &ls = app->link_state;
    const CBLinkPlayer &d = ls.devices[index];
    int w = ls.device_if[index];
    const CBLinkServed *on = w >= 0 && w < ls.nb_interfaces ? &ls.interfaces[w] : nullptr;
    if (!on || !on->running || on->state != CB_LINK_UP || !d.has_status || d.track_type != 1 ||
        d.track_device != on->number || !d.track_id)
        return nullptr;
    return cb_library_find_id(&app->lib, d.track_id);
}

// The players' EJECT/LOAD LOCK keeps a track off a deck that plays.
static bool locked(UiState *ui, const CBLinkPlayer &d)
{
    return d.playing && !strcmp(ui_my_setting(ui, "player.eject_lock"), "true");
}

static void load_on(UiState *ui, int index, int deck, CBTrack *t)
{
    CBApp *app = ui->app;
    const CBLinkPlayer &d = app->link_state.devices[index];
    int ret = cb_app_link_load(app, index, t->id);
    if (ret >= 0)
        cb_app_set_status(app, "Deck %d (%s): loading %s", deck, d.name, ui_title(t));
    else if (ret == CB_ERROR(EAGAIN))
        cb_app_set_status(app, "Deck %d: not on its link yet", deck);
    else
        cb_app_set_status(app, "Deck %d is not on the link any more", deck);
}

void ui_link_load_menu(UiState *ui, CBTrack *t)
{
    CBApp *app = ui->app;
    int idx[UI_LINK_DECKS], shown[UI_LINK_DECKS];
    int n = players(ui, idx, UI_LINK_DECKS, shown);
    if (!n)
        return;
    ui->link_menu_frame = ui->frame_no;
    ui->link_menu_locked.clear();
    for (int k = 0; k < n; k++) {
        const CBLinkPlayer &d = app->link_state.devices[idx[k]];
        const int deck = shown[d.number];
        char label[64];
        snprintf(label, sizeof(label), "Load in deck %d (%s)", deck, d.name[0] ? d.name : "player");
        bool lock = locked(ui, d);
        if (lock)
            ui->link_menu_locked += (ui->link_menu_locked.empty() ? "" : " ") + std::to_string(deck);
        if (ImGui::MenuItem(label, nullptr, false, !lock && !t->missing))
            load_on(ui, idx[k], deck, t);
        if (lock && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Deck %d is playing and the players' Eject/Load Lock is on\n"
                              "(Player settings)", deck);
    }
    ImGui::Separator();
}

void ui_link_menu(UiState *ui, bool open, ImVec2 anchor)
{
    CBApp *app = ui->app;
    ui->link_menu_log.clear();
    if (open)
        ImGui::OpenPopup("##link_menu");
    if (ImGui::IsPopupOpen("##link_menu"))
        ImGui::SetNextWindowPos(anchor, ImGuiCond_Appearing, ImVec2(1.0f, 1.0f));
    if (!ImGui::BeginPopup("##link_menu"))
        return;
    auto log_item = [&](const char *what) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        char line[96];
        snprintf(line, sizeof(line), "%s at %.0f %.0f", what, (a.x + b.x) * 0.5f / ui->s,
                 (a.y + b.y) * 0.5f / ui->s);
        ui->link_menu_log.push_back(line);
    };

    int idx[UI_LINK_DECKS], shown[UI_LINK_DECKS];
    int n = players(ui, idx, UI_LINK_DECKS, shown);
    int pick = -1, pick_number = 0;
    if (!n) {
        ImGui::TextDisabled("No player on the link");
    } else {
        // Four numbers, or as many as the players need.
        int most = std::max(4, n);
        for (int k = 0; k < n; k++)
            most = std::max(most, shown[app->link_state.devices[idx[k]].number]);
        most = std::min(most, UI_LINK_NUMBERS);
        ImGui::TextDisabled("Deck numbers, left to right");
        if (ImGui::BeginTable("##link_numbers", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
            for (int k = 0; k < n; k++) {
                const CBLinkPlayer &d = app->link_state.devices[idx[k]];
                CBTrack *t = ours(app, idx[k]);
                ImGui::PushID(d.number);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                // The number on the player's screen.
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Player %d", d.number);
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", d.name[0] ? d.name : "player");
                if (t) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("\xC2\xB7 %s", ui_title(t));
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::BeginDisabled(no_mac(d.mac));
                for (int v = 1; v <= most; v++) {
                    char label[8];
                    snprintf(label, sizeof(label), "%d", v);
                    if (v > 1)
                        ImGui::SameLine(0, PX(2));
                    ImGui::PushID(v);
                    if (ImGui::Selectable(label, shown[d.number] == v,
                                          ImGuiSelectableFlags_NoAutoClosePopups,
                                          ImVec2(PX(26), ImGui::GetFrameHeight()))) {
                        pick = idx[k];
                        pick_number = v;
                    }
                    ImGui::PopID();
                    char what[48];
                    snprintf(what, sizeof(what), "player %d deck %d", d.number, v);
                    log_item(what);
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::PopStyleVar();
            ImGui::EndTable();
        }
        if (pick >= 0)
            renumber(ui, pick, pick_number, idx, n, shown);
        bool any = false;
        given_read(ui);
        for (int k = 1; k <= UI_LINK_NUMBERS; k++)
            any |= !no_mac(ui->link_given[k]);
        ImGui::Separator();
        if (ImGui::MenuItem("The players' own numbers", nullptr, false, any)) {
            for (int k = 1; k <= UI_LINK_NUMBERS; k++) {
                if (!no_mac(ui->link_given[k]))
                    put_given(ui, k, nullptr);
            }
        }
        log_item("own numbers");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Players..."))
        ui_open_link(ui);
    ImGui::EndPopup();
}

// Where the player is in its track, in seconds; < 0 when it does not say.
static double position(UiState *ui, const CBLinkPlayer &d, const CBTrack *t)
{
    UiLinkDeck *ld = &ui->link_decks[d.number];
    int64_t now = cb_time_us();
    if (!t || d.beat < 0 || !cb_track_analysed(t)) {
        ld->pos = -1;
        return -1;
    }
    if (ld->beats_track != t->id) {
        free(ld->beats);
        ld->beats = nullptr;
        ld->nb_beats = 0;
        ld->beats_track = t->id;
        if (cb_grid_beats(&t->grid, t->duration, &ld->beats, &ld->nb_beats) < 0)
            ld->nb_beats = 0;
    }
    if (d.beat > ld->nb_beats) {
        ld->pos = -1;
        return -1;
    }
    // Beat 0 is before the grid's first beat (a track loaded and played
    // from its start, as an NXS2 says).
    double b0 = 0, b1 = ld->nb_beats ? ld->beats[0].time : t->duration;
    if (d.beat > 0) {
        const CBBeat *bt = &ld->beats[d.beat - 1];
        b0 = bt->time;
        b1 = d.beat < ld->nb_beats ? ld->beats[d.beat].time : b0 + 60.0 / ImMax(bt->bpm, 1.0);
    }
    double rate = d.playing ? 1.0 + d.pitch / 100.0 : 0.0;
    if (ld->pos >= 0 && ld->track_id == d.track_id)
        ld->pos += (double)(now - ld->pos_us) * 1e-6 * rate;
    else
        ld->pos = b0;
    ld->track_id = d.track_id;
    ld->pos_us = now;
    // Within the beat the status names, with the time a status takes to
    // come of slack past its end while it plays.
    double slack = d.playing ? 0.25 * rate : 0.0;
    if (ld->pos < b0 || ld->pos > b1 + slack)
        ld->pos = b0;
    return ld->pos;
}

static const char *slot_name(int slot)
{
    switch (slot) {
    case 1:  return "CD";
    case 2:  return "SD";
    case 3:  return "USB";
    case 4:  return "rekordbox";
    default: return "media";
    }
}

// The cells' text sizes: twice the deck title row's.
#define LABEL_SIZE  19.0f       // the readouts' labels
#define VALUE_SIZE  34.0f       // and values
#define TITLE_SIZE  32.0f
#define LINE_SIZE   24.0f       // the artist, the model
#define STATE_SIZE  21.0f
#define NUMBER_SIZE 44.0f

// Text with a dark shadow, readable over the waveform.
static void stext(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col,
                  const char *text, float max_w = 0.0f)
{
    float o = PX(1.5f);
    ui_text(ui, dl, font, size, ImVec2(pos.x + o, pos.y + o), IM_COL32(0, 0, 0, 0xE0), text, max_w);
    ui_text(ui, dl, font, size, pos, col, text, max_w);
}

static void stext_right(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 right_top,
                        ImU32 col, const char *text)
{
    float o = PX(1.5f);
    ui_text_right(ui, dl, font, size, ImVec2(right_top.x + o, right_top.y + o),
                  IM_COL32(0, 0, 0, 0xE0), text);
    ui_text_right(ui, dl, font, size, right_top, col, text);
}

// How wide a readout is: its label's width or its value's.
static float readout_w(UiState *ui, const char *label, const char *value)
{
    return ImMax(ui_text_width(ui, ui->font_bold, LABEL_SIZE, label),
                 ui_text_width(ui, ui->font_bold, VALUE_SIZE, value));
}

// A label over a value, right-aligned at `right`.
static void readout(UiState *ui, ImDrawList *dl, float right, float y, const char *label,
                    const char *value, ImU32 col)
{
    stext_right(ui, dl, ui->font_bold, LABEL_SIZE, ImVec2(right, y), UI_DIM, label);
    stext_right(ui, dl, ui->font_bold, VALUE_SIZE, ImVec2(right, y + PX(LABEL_SIZE + 3)), col,
                  value);
}

// The track's overview behind a cell, drawn as the deck's overview is
// (the players' overview type), the part played dimmed as there and the
// playhead on it in red.
static void background(UiState *ui, const CBTrack *t, double pos, ImVec2 a, ImVec2 b)
{
    CBApp *app = ui->app;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, UI_WAVE_BG, PX(3));
    if (!t)
        return;
    if (CBWaveGpu *g = t->has_waveforms ? cb_app_preview_wave(app, t) : nullptr) {
        CBWaveDraw d = {};
        d.x0 = a.x;
        d.y0 = a.y + PX(3);
        d.x1 = b.x;
        d.y1 = b.y - PX(3);
        d.col0 = 0;
        d.col1 = cb_wave_columns(g);
        d.mode = ui->overview_full ? CB_WAVE_CENTRED : CB_WAVE_HALF_STACKED;
        d.gain = 1.0f;
        d.alpha = 1.0f;
        if (pos >= 0 && t->duration > 0) {
            d.dim = 0.45f;
            d.dim_until = pos / t->duration * cb_wave_columns(g);
        }
        cb_wave_draw(app->window, dl, g, &d);
    } else if (t->has_waveforms) {
        ui->previews_pending = true;
    }
    if (pos >= 0 && t->duration > 0) {
        float x = floorf(a.x + (float)(pos / t->duration) * (b.x - a.x));
        dl->AddRectFilled(ImVec2(x - PX(1), a.y), ImVec2(x + PX(1), b.y), UI_RED);
    }
}

static void deck_cell(UiState *ui, int index, int deck, ImVec2 a, ImVec2 b)
{
    CBApp *app = ui->app;
    const CBLinkPlayer &d = app->link_state.devices[index];
    CBTrack *t = ours(app, index);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    double pos = position(ui, d, t);
    bool loaded = d.has_status && d.track_id;
    float h = b.y - a.y, width = b.x - a.x;
    char buf[96];

    background(ui, t, pos, a, b);

    // The deck number on a dark backing (on air says so in the state);
    // the cover.
    snprintf(buf, sizeof(buf), "%d", deck);
    float nw = PX(48);
    dl->AddRectFilled(a, ImVec2(a.x + nw, b.y), IM_COL32(0, 0, 0, 0xB0), PX(3));
    ui_text_centre(ui, dl, ui->font_bold, NUMBER_SIZE, a, ImVec2(a.x + nw, b.y), UI_TEXT, buf);
    float art = h - PX(8);
    ImVec2 p0(a.x + nw + PX(4), a.y + PX(4)), p1(p0.x + art, p0.y + art);
    uint64_t tex = t ? cb_app_artwork_texture(app, t) : 0;
    if (tex)
        dl->AddImage(ImTextureRef((ImTextureID)tex), p0, p1);
    else
        ui_record_icon(dl, p0, p1);
    float tx = p1.x + PX(12);

    // The text goes on dark plates over the waveform, one a block, drawn in
    // a channel under it as each block's size is known.
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    float pad_x = PX(6), pad_y = PX(3);
    auto plate = [&](float x0, float y0, float x1, float y1) {
        if (x1 <= x0)
            return;
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(ImVec2(x0 - pad_x, y0 - pad_y), ImVec2(x1 + pad_x, y1 + pad_y),
                          IM_COL32(0, 0, 0, 0xA8), PX(4));
        dl->ChannelsSetCurrent(1);
    };

    // Right-aligned, from the edge: the time left, the tempo, the key,
    // each while the title keeps some room.
    char remain[16], bpm[16], key[16] = "";
    if (t && pos >= 0)
        ui_fmt_time(-(t->duration - pos), remain, sizeof(remain), false);
    else
        snprintf(remain, sizeof(remain), "-:--");
    if (loaded && d.bpm > 0)
        snprintf(bpm, sizeof(bpm), "%.2f", d.bpm * (1.0 + d.pitch / 100.0));
    else
        snprintf(bpm, sizeof(bpm), "-");
    const char *kt = t ? ui_key_text(ui, t->key, key, sizeof(key)) : "";
    const struct { const char *label, *value; ImU32 col; } reads[] = {
        { "REMAIN", remain, t && pos >= 0 ? UI_TEXT : UI_FAINT },
        { "BPM", bpm, fabs(d.pitch) > 0.005 ? UI_ACCENT : UI_TEXT },
        { "KEY", kt[0] ? kt : "-", UI_TEXT },
    };
    float x = b.x - PX(10), ry = a.y + floorf((h - PX(LABEL_SIZE + 3 + VALUE_SIZE)) * 0.5f);
    for (size_t i = 0; i < CB_ARRAY_ELEMS(reads); i++) {
        float rw = readout_w(ui, reads[i].label, reads[i].value);
        if (i && x - rw - tx < PX(120))
            break;
        readout(ui, dl, x, ry, reads[i].label, reads[i].value, reads[i].col);
        x -= rw + PX(20);
    }
    plate(x + PX(20), ry, b.x - PX(10), ry + PX(LABEL_SIZE + 3 + VALUE_SIZE));

    // The title over the artist; the player's state over its model beside
    // them before the readouts, or on a cell too narrow for that, in the
    // artist's place.
    const char *state = !d.has_status ? "" : !loaded ? "NO TRACK" : d.playing ? "PLAYING" : "PAUSED";
    ImU32 scol = d.playing && loaded ? UI_PLAY : UI_DIM;
    const char *model = d.name[0] ? d.name : "Player";
    float space = ui_text_width(ui, ui->font_bold, STATE_SIZE, " ");
    float sw = ui_text_width(ui, ui->font_bold, STATE_SIZE, state) +
               (d.on_air ? space + ui_text_width(ui, ui->font_bold, STATE_SIZE, "ON AIR") : 0) +
               (d.master ? space + ui_text_width(ui, ui->font_bold, STATE_SIZE, "MASTER") : 0);
    sw = ImMax(sw, ui_text_width(ui, nullptr, LINE_SIZE, model));
    bool beside = x - sw - PX(16) - tx >= PX(160);
    float ty = a.y + floorf(h * 0.5f) - PX((TITLE_SIZE + 4 + LINE_SIZE) * 0.5f);
    float ly = ty + PX(TITLE_SIZE + 4);
    float sx = beside ? x - sw : tx, sy = beside ? ty + PX(2) : ly;
    float max_w = (beside ? sx - PX(16) : x) - tx;
    // The state's words, each while it fits before the readouts.
    float cx = sx, right = beside ? x : x - PX(10);
    auto word = [&](const char *w, ImU32 col) {
        float ww = ui_text_width(ui, ui->font_bold, STATE_SIZE, w);
        if (cx + ww > right)
            return false;
        stext(ui, dl, ui->font_bold, STATE_SIZE, ImVec2(cx, sy), col, w);
        cx += ww + space;
        return true;
    };
    bool room = word(state, scol);
    room = room && (!d.on_air || word("ON AIR", UI_RED));
    room = room && (!d.master || word("MASTER", UI_ACCENT));
    if (beside) {
        stext(ui, dl, nullptr, LINE_SIZE, ImVec2(sx, ly), UI_DIM, model, sw);
        plate(sx, sy, sx + sw, ly + PX(LINE_SIZE));
    } else if (room && right - cx > PX(30)) {
        stext(ui, dl, nullptr, STATE_SIZE, ImVec2(cx, sy), UI_DIM, model, right - cx);
        cx += ImMin(ui_text_width(ui, nullptr, STATE_SIZE, model), right - cx) + space;
    }

    // The title over the artist (or the state), on one plate.
    if (loaded && d.track_device == d.number)
        snprintf(buf, sizeof(buf), "Track %u from its %s", d.track_id, slot_name(d.track_slot));
    else if (loaded)
        snprintf(buf, sizeof(buf), "Track %u from device %d's %s", d.track_id, d.track_device,
                 slot_name(d.track_slot));
    const char *line1 = t ? ui_title(t) : !d.has_status ? "Waiting for its status"
                      : loaded ? buf : "No track loaded";
    const char *line2 = t ? ui_artist(t) : loaded ? "Not one of cyanbox's tracks" : "Drop a track here";
    ImU32 col1 = t ? UI_TEXT : UI_FAINT, col2 = t ? UI_DIM : UI_FAINT;
    stext(ui, dl, ui->font_bold, TITLE_SIZE, ImVec2(tx, ty), col1, line1, max_w);
    float w1 = ImMin(ui_text_width(ui, ui->font_bold, TITLE_SIZE, line1), max_w), w2 = cx - space - tx;
    if (beside) {
        stext(ui, dl, nullptr, LINE_SIZE, ImVec2(tx, ly), col2, line2, max_w);
        w2 = ImMin(ui_text_width(ui, nullptr, LINE_SIZE, line2), max_w);
    }
    plate(tx, ty, tx + ImMax(w1, w2), ly + PX(LINE_SIZE));
    dl->ChannelsMerge();

    // A white triangle at the foot of the playhead, over everything, so
    // the place shows where the cover or the text hides the line.
    if (t && pos >= 0 && t->duration > 0) {
        float px = floorf(a.x + (float)(pos / t->duration) * width), s = PX(7);
        ImVec2 p[3] = { ImVec2(px - s, b.y - PX(1)), ImVec2(px + s, b.y - PX(1)),
                        ImVec2(px, b.y - PX(1) - s * 1.3f) };
        dl->AddTriangleFilled(p[0], p[1], p[2], UI_TEXT);
        dl->AddTriangle(p[0], p[1], p[2], IM_COL32(0, 0, 0, 0xC0), PX(1));
    }

    // Outlined orange while the player plays.
    dl->AddRect(a, b, d.playing && loaded ? UI_ACCENT : IM_COL32(0x2A, 0x2A, 0x2A, 0xFF), PX(3), PX(1.5f));

    // Tracks dropped on it go to the player, the first of them.
    ImGui::SetCursorScreenPos(a);
    ImGui::Dummy(ImVec2(width, 0));
    ImRect r(a, b);
    if (ImGui::BeginDragDropTargetCustom(r, ImGui::GetID("##linkdrop"))) {
        std::vector<uint64_t> ids;
        if (ui_accept_tracks(ui, ids) && !ids.empty()) {
            CBTrack *dropped = cb_library_find_id(&app->lib, ids[0]);
            if (locked(ui, d))
                cb_app_set_status(app, "Deck %d is playing and the players' Eject/Load Lock is on",
                                  deck);
            else if (dropped && !dropped->missing)
                load_on(ui, index, deck, dropped);
        }
        ImGui::EndDragDropTarget();
    }
}

void ui_link_decks_row(UiState *ui, ImVec2 a, ImVec2 b)
{
    CBApp *app = ui->app;
    int idx[UI_LINK_DECKS], shown[UI_LINK_DECKS];
    int n = players(ui, idx, UI_LINK_DECKS, shown);
    ui->link_decks_drawn = n;
    ui->link_decks_order.clear();
    if (!n) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        stext(ui, dl, nullptr, LINE_SIZE, ImVec2(a.x + PX(12), floorf((a.y + b.y) * 0.5f - PX(14))),
                UI_FAINT, app->link_state.on ? "No player on the link" : "PRO DJ LINK is off");
        return;
    }

    // Equal shares, at most half the width each: two fill it.
    float gap = PX(4);
    int slots = std::max(n, 2);
    float share = (b.x - a.x + gap) / (float)slots;
    bool moving = false;
    for (int k = 0; k < n; k++) {
        const CBLinkPlayer &d = app->link_state.devices[idx[k]];
        const int deck = shown[d.number];
        int slot = n == 1 ? (deck % 2 ? 0 : 1) : k;
        ImVec2 ca(floorf(a.x + slot * share), a.y);
        ImVec2 cb(floorf(ca.x + share - gap), b.y);
        ImGui::PushID(d.number);
        deck_cell(ui, idx[k], deck, ca, cb);
        ImGui::PopID();
        ui->link_decks_order +=
            (k ? " " : "") + std::to_string(deck) + "=" + std::to_string(d.number);
        moving |= d.playing && ui->link_decks[d.number].pos >= 0;
    }
    if (moving)
        ui_want_frames(ui, 2);
}

void ui_link_decks_shutdown(UiState *ui)
{
    for (int i = 0; i < UI_LINK_DECKS; i++) {
        free(ui->link_decks[i].beats);
        ui->link_decks[i] = UiLinkDeck();
    }
}

/* ----------------------------------------------------------- test hook */

void ui_link_decks_test_tick(UiState *ui)
{
    if (ui->link_decks_fake.empty())
        return;
    CBApp *app = ui->app;
    auto &ls = app->link_state;
    int64_t now = cb_time_us();
    if (!ui->link_decks_fake_us) {
        ui->link_decks_fake_us = now;
        ls = {};
        ls.enabled = ls.on = true;
        ls.nb_interfaces = 1;
        snprintf(ls.interfaces[0].interface, sizeof(ls.interfaces[0].interface), "lo");
        ls.interfaces[0].addr = 0x7F000001;
        ls.interfaces[0].running = true;
        ls.interfaces[0].state = ui->link_joining_fake ? CB_LINK_JOINING : CB_LINK_UP;
        ls.interfaces[0].number = 17;
        if (!ui->link_cue_fake.empty()) {
            CBLinkCueState &c = ls.cue;
            int on = 0;
            if (sscanf(ui->link_cue_fake.c_str(), "%20[^:]:%d:%d", c.name, &on, &c.source) == 3) {
                ls.cue_enabled = c.mixer = true;
                c.number = 33;
                c.on = on;
                c.ours = c.source == 17;
            } else {
                cb_warn("CYANBOX_UI_LINK_CUE: %s is not NAME:ON:SOURCE", ui->link_cue_fake.c_str());
            }
        }
        const char *s = ui->link_decks_fake.c_str();
        while (*s && ls.nb_devices < CB_APP_LINK_DEVICES) {
            int number = 0, beat = -1, used = 0;
            unsigned id = 0;
            char name[24] = "", flags[8] = "";
            if (sscanf(s, "%d:%23[^:]:%u:%n", &number, name, &id, &used) < 3 || !used) {
                cb_warn("CYANBOX_UI_LINK_DECKS: %s is not NUMBER:NAME:TRACK:FLAGS:BEAT", s);
                break;
            }
            // FLAGS may be empty, and MAC left out.
            sscanf(s + used, "%7[^:,]", flags);
            const char *comma = strchr(s + used, ',');
            const char *colon = strchr(s + used, ':');
            int mac = number;
            if (colon && (!comma || colon < comma)) {
                beat = atoi(colon + 1);
                const char *next = strchr(colon + 1, ':');
                if (next && (!comma || next < comma))
                    mac = atoi(next + 1);
            }
            CBLinkPlayer &d = ls.devices[ls.nb_devices];
            d = CBLinkPlayer();
            d.number = number;
            d.type = CB_LINK_DEVICE_PLAYER;
            snprintf(d.name, sizeof(d.name), "%s", name);
            d.addr = 0x7F000000u | (uint32_t)(10 + number);
            const uint8_t addr[6] = { 0x02, 0, 0, 0, 0, (uint8_t)mac };
            memcpy(d.mac, addr, 6);
            d.has_status = true;
            d.track_device = id ? 17 : 0;
            d.track_slot = id ? 4 : 0;
            d.track_type = id ? 1 : 0;
            d.track_id = id;
            d.playing = strchr(flags, 'p');
            d.on_air = strchr(flags, 'a');
            d.master = strchr(flags, 'm');
            d.synced = strchr(flags, 's');
            const CBTrack *t = id ? cb_library_find_id(&app->lib, id) : nullptr;
            d.bpm = t ? cb_grid_bpm(&t->grid) : 0;
            d.beat = beat;
            ui->link_decks_fake_beat[ls.nb_devices] = beat;
            ls.device_if[ls.nb_devices] = 0;
            ls.nb_devices++;
            s = comma ? comma + 1 : s + strlen(s);
        }
    }
    // A playing one's beat number moves on at its tempo, its status a
    // fifth of a second apart, as a player's.
    double since = (double)(now - ui->link_decks_fake_us) * 1e-6;
    for (int i = 0; i < ls.nb_devices; i++) {
        CBLinkPlayer &d = ls.devices[i];
        int b0 = ui->link_decks_fake_beat[i];
        double at = floor(since * 5.0) / 5.0;
        d.beat = b0 > 0 && d.playing && d.bpm > 0 ? b0 + (int)floor(at * d.bpm / 60.0) : b0;
        d.status_us = ui->link_decks_fake_us + (int64_t)(at * 1e6);
    }
    ui_want_frames(ui, 2);
}
