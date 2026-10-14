// Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

// The Link decks (docs/UI.md, "Link decks"). With --pro-link the deck's
// title row shows the players on the link instead of the deck's track
// (deck_view.cpp, ui_deck_panel), one cell each, side by side: the width
// is shared equally, each at most half of it, so two fill it and one alone
// takes its side (odd numbers left, even right); with none, it says so. A
// cell has the track's
// overview for its background, the part played shaded and the playhead on
// it when the player says where it is; over it, the cover and the title,
// the player's state and model, and right-aligned the key, the tempo at
// the player's pitch and the time left. The rest of the deck is unchanged:
// rxbox's own, for editing tracks before they go to a player.
//
// Where the player is: the beat number of its status on the track's grid
// (the beats Link serves it: rx_grid_beats, anlz.c PQTZ; 0 before the
// first), carried forward between statuses (about five a second) at the
// pitch it plays at and kept within the beat the last one named. A
// pre-nexus player (a CDJ-900, a CDJ-2000) sends no beat number: no
// playhead, no time left.
//
// Tracks dropped on a cell, and the table's "Load in deck" items, go to
// the player as a load command (rx_app_link_load: it loads the track as
// one dragged onto it in rekordbox), but not while it plays with the
// players' EJECT/LOAD LOCK on (player.eject_lock).
//
// RXBOX_UI_LINK_DECKS (the headless tests) makes up the players instead:
// a comma-separated list of NUMBER:NAME:TRACK:FLAGS:BEAT, TRACK a library
// id (0 nothing loaded), FLAGS any of p (plays), a (on air), m (master),
// s (synced), BEAT its first beat number (-1 none sent); a playing one's
// beat number moves on at its track's tempo.

#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>


// The players heard, by number: their indices in link_state.devices.
static int players(const RXApp *app, int *idx, int max)
{
    const auto &ls = app->link_state;
    int n = 0;
    if (!ls.on)
        return 0;
    for (int i = 0; i < ls.nb_devices && n < max; i++) {
        const RXLinkPlayer &d = ls.devices[i];
        if (d.type == RX_LINK_DEVICE_PLAYER && d.number >= 1 && d.number < UI_LINK_DECKS)
            idx[n++] = i;
    }
    std::sort(idx, idx + n, [&](int a, int b) { return ls.devices[a].number < ls.devices[b].number; });
    return n;
}

// The library track a player has loaded, when it is ours: a rekordbox
// track loaded from our device number on the player's own link.
static RXTrack *ours(RXApp *app, int index)
{
    const auto &ls = app->link_state;
    const RXLinkPlayer &d = ls.devices[index];
    int w = ls.device_if[index];
    const RXLinkServed *on = w >= 0 && w < ls.nb_interfaces ? &ls.interfaces[w] : nullptr;
    if (!on || !on->running || on->state != RX_LINK_UP || !d.has_status || d.track_type != 1 ||
        d.track_device != on->number || !d.track_id)
        return nullptr;
    return rx_library_find_id(&app->lib, d.track_id);
}

// The players' EJECT/LOAD LOCK keeps a track off a deck that plays.
static bool locked(UiState *ui, const RXLinkPlayer &d)
{
    return d.playing && !strcmp(ui_my_setting(ui, "player.eject_lock"), "true");
}

static void load_on(UiState *ui, int index, RXTrack *t)
{
    RXApp *app = ui->app;
    const RXLinkPlayer &d = app->link_state.devices[index];
    int ret = rx_app_link_load(app, index, t->id);
    if (ret >= 0)
        rx_app_set_status(app, "Deck %d (%s): loading %s", d.number, d.name, ui_title(t));
    else if (ret == RX_ERROR(EAGAIN))
        rx_app_set_status(app, "Deck %d: not on its link yet", d.number);
    else
        rx_app_set_status(app, "Deck %d is not on the link any more", d.number);
}

void ui_link_load_menu(UiState *ui, RXTrack *t)
{
    RXApp *app = ui->app;
    int idx[UI_LINK_DECKS];
    int n = players(app, idx, UI_LINK_DECKS);
    if (!n)
        return;
    ui->link_menu_frame = ui->frame_no;
    ui->link_menu_locked.clear();
    for (int k = 0; k < n; k++) {
        const RXLinkPlayer &d = app->link_state.devices[idx[k]];
        char label[64];
        snprintf(label, sizeof(label), "Load in deck %d (%s)", d.number, d.name[0] ? d.name : "player");
        bool lock = locked(ui, d);
        if (lock)
            ui->link_menu_locked += (ui->link_menu_locked.empty() ? "" : " ") + std::to_string(d.number);
        if (ImGui::MenuItem(label, nullptr, false, !lock && !t->missing))
            load_on(ui, idx[k], t);
        if (lock && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Deck %d is playing and the players' Eject/Load Lock is on\n"
                              "(Player settings)", d.number);
    }
    ImGui::Separator();
}

// Where the player is in its track, in seconds; < 0 when it does not say.
static double position(UiState *ui, const RXLinkPlayer &d, const RXTrack *t)
{
    UiLinkDeck *ld = &ui->link_decks[d.number];
    int64_t now = rx_time_us();
    if (!t || d.beat < 0 || !rx_track_analysed(t)) {
        ld->pos = -1;
        return -1;
    }
    if (ld->beats_track != t->id) {
        free(ld->beats);
        ld->beats = nullptr;
        ld->nb_beats = 0;
        ld->beats_track = t->id;
        if (rx_grid_beats(&t->grid, t->duration, &ld->beats, &ld->nb_beats) < 0)
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
        const RXBeat *bt = &ld->beats[d.beat - 1];
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
    ui_text_right(ui, dl, ui->font_bold, LABEL_SIZE, ImVec2(right, y), UI_DIM, label);
    ui_text_right(ui, dl, ui->font_bold, VALUE_SIZE, ImVec2(right, y + PX(LABEL_SIZE + 3)), col,
                  value);
}

// The track's overview behind a cell, dimmed under the text, the part
// played shaded and the playhead on it.
static void background(UiState *ui, const RXTrack *t, double pos, ImVec2 a, ImVec2 b)
{
    RXApp *app = ui->app;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, UI_WAVE_BG, PX(3));
    if (!t)
        return;
    if (RXWaveGpu *g = t->has_waveforms ? rx_app_preview_wave(app, t) : nullptr) {
        RXWaveDraw d = {};
        d.x0 = a.x;
        d.y0 = a.y + PX(3);
        d.x1 = b.x;
        d.y1 = b.y - PX(3);
        d.col0 = 0;
        d.col1 = rx_wave_columns(g);
        d.mode = RX_WAVE_CENTRED;
        d.gain = 1.0f;
        d.alpha = 0.6f;
        rx_wave_draw(app->window, dl, g, &d);
    } else if (t->has_waveforms) {
        ui->previews_pending = true;
    }
    // Dark enough under the text to read it.
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 0x98), PX(3));
    if (pos >= 0 && t->duration > 0) {
        float x = floorf(a.x + (float)(pos / t->duration) * (b.x - a.x)) + 0.5f;
        dl->AddRectFilled(a, ImVec2(x, b.y), IM_COL32(0, 0, 0, 0x50), PX(3));
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), UI_TEXT, PX(2));
    }
}

static void deck_cell(UiState *ui, int index, ImVec2 a, ImVec2 b)
{
    RXApp *app = ui->app;
    const RXLinkPlayer &d = app->link_state.devices[index];
    RXTrack *t = ours(app, index);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    double pos = position(ui, d, t);
    bool loaded = d.has_status && d.track_id;
    float h = b.y - a.y, width = b.x - a.x;
    char buf[96];

    background(ui, t, pos, a, b);

    // The number on a dark backing (on air says so in the state); the
    // cover.
    snprintf(buf, sizeof(buf), "%d", d.number);
    float nw = PX(48);
    dl->AddRectFilled(a, ImVec2(a.x + nw, b.y), IM_COL32(0, 0, 0, 0xB0), PX(3));
    ui_text_centre(ui, dl, ui->font_bold, NUMBER_SIZE, a, ImVec2(a.x + nw, b.y), UI_TEXT, buf);
    float art = h - PX(8);
    ImVec2 p0(a.x + nw + PX(4), a.y + PX(4)), p1(p0.x + art, p0.y + art);
    uint64_t tex = t ? rx_app_artwork_texture(app, t) : 0;
    if (tex)
        dl->AddImage(ImTextureRef((ImTextureID)tex), p0, p1);
    else
        ui_record_icon(dl, p0, p1);
    float tx = p1.x + PX(12);

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
    for (size_t i = 0; i < RX_ARRAY_ELEMS(reads); i++) {
        float rw = readout_w(ui, reads[i].label, reads[i].value);
        if (i && x - rw - tx < PX(120))
            break;
        readout(ui, dl, x, ry, reads[i].label, reads[i].value, reads[i].col);
        x -= rw + PX(20);
    }

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
        ui_text(ui, dl, ui->font_bold, STATE_SIZE, ImVec2(cx, sy), col, w);
        cx += ww + space;
        return true;
    };
    bool room = word(state, scol);
    room = room && (!d.on_air || word("ON AIR", UI_RED));
    room = room && (!d.master || word("MASTER", UI_ACCENT));
    if (beside)
        ui_text(ui, dl, nullptr, LINE_SIZE, ImVec2(sx, ly), UI_DIM, model, sw);
    else if (room && right - cx > PX(30))
        ui_text(ui, dl, nullptr, STATE_SIZE, ImVec2(cx, sy), UI_DIM, model, right - cx);

    if (t) {
        ui_text(ui, dl, ui->font_bold, TITLE_SIZE, ImVec2(tx, ty), UI_TEXT, ui_title(t), max_w);
        if (beside)
            ui_text(ui, dl, nullptr, LINE_SIZE, ImVec2(tx, ly), UI_DIM, ui_artist(t), max_w);
    } else {
        if (loaded && d.track_device == d.number)
            snprintf(buf, sizeof(buf), "Track %u from its %s", d.track_id, slot_name(d.track_slot));
        else if (loaded)
            snprintf(buf, sizeof(buf), "Track %u from device %d's %s", d.track_id, d.track_device,
                     slot_name(d.track_slot));
        ui_text(ui, dl, ui->font_bold, TITLE_SIZE, ImVec2(tx, ty), UI_FAINT,
                !d.has_status ? "Waiting for its status" : loaded ? buf : "No track loaded", max_w);
        if (beside)
            ui_text(ui, dl, nullptr, LINE_SIZE, ImVec2(tx, ly), UI_FAINT,
                    loaded ? "Not one of rxbox's tracks" : "Drop a track here", max_w);
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
            RXTrack *dropped = rx_library_find_id(&app->lib, ids[0]);
            if (locked(ui, d))
                rx_app_set_status(app, "Deck %d is playing and the players' Eject/Load Lock is on",
                                  d.number);
            else if (dropped && !dropped->missing)
                load_on(ui, index, dropped);
        }
        ImGui::EndDragDropTarget();
    }
}

void ui_link_decks_row(UiState *ui, ImVec2 a, ImVec2 b)
{
    RXApp *app = ui->app;
    int idx[UI_LINK_DECKS];
    int n = players(app, idx, UI_LINK_DECKS);
    ui->link_decks_drawn = n;
    if (!n) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ui_text(ui, dl, nullptr, LINE_SIZE, ImVec2(a.x + PX(12), floorf((a.y + b.y) * 0.5f - PX(14))),
                UI_FAINT, app->link_state.on ? "No player on the link" : "PRO DJ LINK is off");
        return;
    }

    // Equal shares, at most half the width each: two fill it.
    float gap = PX(4);
    int slots = std::max(n, 2);
    float share = (b.x - a.x + gap) / (float)slots;
    bool moving = false;
    for (int k = 0; k < n; k++) {
        const RXLinkPlayer &d = app->link_state.devices[idx[k]];
        int slot = n == 1 ? (d.number % 2 ? 0 : 1) : k;
        ImVec2 ca(floorf(a.x + slot * share), a.y);
        ImVec2 cb(floorf(ca.x + share - gap), b.y);
        ImGui::PushID(d.number);
        deck_cell(ui, idx[k], ca, cb);
        ImGui::PopID();
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
    RXApp *app = ui->app;
    auto &ls = app->link_state;
    int64_t now = rx_time_us();
    if (!ui->link_decks_fake_us) {
        ui->link_decks_fake_us = now;
        ls = {};
        ls.enabled = ls.on = true;
        ls.nb_interfaces = 1;
        snprintf(ls.interfaces[0].interface, sizeof(ls.interfaces[0].interface), "lo");
        ls.interfaces[0].addr = 0x7F000001;
        ls.interfaces[0].running = true;
        ls.interfaces[0].state = RX_LINK_UP;
        ls.interfaces[0].number = 17;
        const char *s = ui->link_decks_fake.c_str();
        while (*s && ls.nb_devices < RX_APP_LINK_DEVICES) {
            int number = 0, beat = -1, used = 0;
            unsigned id = 0;
            char name[24] = "", flags[8] = "";
            if (sscanf(s, "%d:%23[^:]:%u:%n", &number, name, &id, &used) < 3 || !used) {
                rx_warn("RXBOX_UI_LINK_DECKS: %s is not NUMBER:NAME:TRACK:FLAGS:BEAT", s);
                break;
            }
            // FLAGS may be empty.
            sscanf(s + used, "%7[^:,]", flags);
            const char *colon = strchr(s + used, ':');
            if (colon && (!strchr(s + used, ',') || colon < strchr(s + used, ',')))
                beat = atoi(colon + 1);
            RXLinkPlayer &d = ls.devices[ls.nb_devices];
            d = RXLinkPlayer();
            d.number = number;
            d.type = RX_LINK_DEVICE_PLAYER;
            snprintf(d.name, sizeof(d.name), "%s", name);
            d.addr = 0x7F000000u | (uint32_t)(10 + number);
            d.has_status = true;
            d.track_device = id ? 17 : 0;
            d.track_slot = id ? 4 : 0;
            d.track_type = id ? 1 : 0;
            d.track_id = id;
            d.playing = strchr(flags, 'p');
            d.on_air = strchr(flags, 'a');
            d.master = strchr(flags, 'm');
            d.synced = strchr(flags, 's');
            const RXTrack *t = id ? rx_library_find_id(&app->lib, id) : nullptr;
            d.bpm = t ? rx_grid_bpm(&t->grid) : 0;
            d.beat = beat;
            ui->link_decks_fake_beat[ls.nb_devices] = beat;
            ls.device_if[ls.nb_devices] = 0;
            ls.nb_devices++;
            const char *comma = strchr(s, ',');
            s = comma ? comma + 1 : s + strlen(s);
        }
    }
    // A playing one's beat number moves on at its tempo, its status a
    // fifth of a second apart, as a player's.
    double since = (double)(now - ui->link_decks_fake_us) * 1e-6;
    for (int i = 0; i < ls.nb_devices; i++) {
        RXLinkPlayer &d = ls.devices[i];
        int b0 = ui->link_decks_fake_beat[i];
        double at = floor(since * 5.0) / 5.0;
        d.beat = b0 > 0 && d.playing && d.bpm > 0 ? b0 + (int)floor(at * d.bpm / 60.0) : b0;
        d.status_us = ui->link_decks_fake_us + (int64_t)(at * 1e6);
    }
    ui_want_frames(ui, 2);
}
