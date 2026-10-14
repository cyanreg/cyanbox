// Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

// The Link decks (docs/UI.md, "Link decks"). With players on the link
// (--pro-link), the top panel shows them in place of rxbox's own deck,
// side by side on the panel's background: the width is shared equally,
// each at most half of it, so two fill it and one alone takes its side
// (odd numbers left, even right). Each says which player it is, whether
// it plays, is on air, master or synced, and what it has loaded; for a
// track of ours, its title, key, tempo at the player's pitch and time,
// its overview, and the detail waveform around where the player is.
//
// That place is the beat number of the player's status on the track's
// grid (the beats Link serves it: rx_grid_beats, anlz.c PQTZ; 0 before
// the first), carried
// forward between statuses (about five a second) at the pitch it plays
// at and kept within the beat the last one named. A pre-nexus player (a
// CDJ-900, a CDJ-2000) sends no beat number: its place is not shown.
//
// Tracks dropped on a deck, and the table's "Load in deck" items, go to
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

#define LINK_READOUT_H  44      // the readout row under the title row
#define LINK_DETAIL_BARS 8      // the detail waveform's width, at most
#define LINK_BEAT_PX    16      // and at least so many pixels a beat

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

bool ui_link_decks_shown(UiState *ui)
{
    int idx[UI_LINK_DECKS];
    return players(ui->app, idx, UI_LINK_DECKS) > 0;
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

// A label over a value, right-aligned at `right`, as the deck's readouts.
static void readout(UiState *ui, ImDrawList *dl, float right, float y, const char *label,
                    const char *value, ImU32 col, const char *extra = nullptr)
{
    ui_text_right(ui, dl, ui->font_bold, 10.0f, ImVec2(right, y), UI_DIM, label);
    if (extra) {
        float lw = ui_text_width(ui, ui->font_bold, 10.0f, label);
        ui_text_right(ui, dl, nullptr, 10.5f, ImVec2(right - lw - PX(5), y - PX(0.5f)), UI_DIM, extra);
    }
    ui_text_right(ui, dl, ui->font_bold, 20.0f, ImVec2(right, y + PX(13)), col, value);
}

// A state word, after the last, in its colour.
static float chip(UiState *ui, ImDrawList *dl, float x, float y, const char *text, ImU32 col)
{
    ui_text(ui, dl, ui->font_bold, 10.5f, ImVec2(x, y), col, text);
    return x + ui_text_width(ui, ui->font_bold, 10.5f, text) + PX(9);
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

// The detail waveform around `pos`, LINK_DETAIL_BARS bars wide or fewer
// on a narrow deck, the playhead in the middle; beats, downbeats
// brighter, and cues over it.
static void detail(UiState *ui, const RXLinkPlayer &d, const RXTrack *t, double pos, ImVec2 a,
                   ImVec2 b)
{
    RXApp *app = ui->app;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    UiLinkDeck *ld = &ui->link_decks[d.number];
    dl->AddRectFilled(a, b, UI_WAVE_BG, PX(2));
    double bpm = rx_grid_bpm_at(&t->grid, pos);
    float w = b.x - a.x;
    int bars = ImClamp((int)(w / PX(LINK_BEAT_PX * 4)), 2, LINK_DETAIL_BARS);
    double span = bars * 4 * 60.0 / (bpm > 0 ? bpm : 120.0);
    double w0 = pos - span * 0.5, w1 = pos + span * 0.5;
    auto xof = [&](double sec) { return a.x + (float)((sec - w0) / span) * w; };
    float top = a.y + PX(12);

    if (RXWaveGpu *g = rx_app_detail_wave(app, t)) {
        RXWaveDraw wd = {};
        wd.x0 = a.x;
        wd.y0 = a.y;
        wd.x1 = b.x;
        wd.y1 = b.y;
        wd.col0 = w0 * RX_WAVE_COLUMNS_PER_SECOND;
        wd.col1 = w1 * RX_WAVE_COLUMNS_PER_SECOND;
        wd.mode = ui->detail_half ? RX_WAVE_HALF_OVERLAID : RX_WAVE_CENTRED;
        wd.inset_top = top - a.y;
        wd.inset_bottom = PX(3);
        wd.detail = true;
        wd.alpha = 1.0f;
        rx_wave_draw(app->window, dl, g, &wd);
    } else {
        ui_want_frames(ui, 2);
    }

    for (int i = 0; i < ld->nb_beats; i++) {
        const RXBeat &bt = ld->beats[i];
        if (bt.time < w0 || bt.time > w1)
            continue;
        float x = floorf(xof(bt.time)) + 0.5f;
        bool down = bt.number == 1;
        dl->AddLine(ImVec2(x, top), ImVec2(x, b.y), down ? IM_COL32(0x9A, 0x9A, 0x9A, 0xC0)
                                                           : IM_COL32(0x4C, 0x4C, 0x4C, 0xC0));
    }
    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        if (c->time < w0 || c->time > w1)
            continue;
        float x = floorf(xof(c->time)) + 0.5f;
        bool hot = rx_cue_is_hot(c);
        ImU32 col = ui_rgb(hot ? ui_hot_cue_rgb(ui, c) : ui_memory_cue_rgb(c));
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), ui_alpha(col, 0.8f), PX(1.5f));
        if (hot) {
            char l[2] = { (char)('A' + c->hot), 0 };
            ImVec2 p(x, a.y + PX(1));
            dl->AddRectFilled(p, ImVec2(p.x + PX(11), p.y + PX(11)), col, PX(1));
            ui_text_centre(ui, dl, ui->font_bold, 9.0f, p, ImVec2(p.x + PX(11), p.y + PX(11)),
                           ui_text_on(hot ? ui_hot_cue_rgb(ui, c) : ui_memory_cue_rgb(c)), l);
        } else {
            dl->AddTriangleFilled(ImVec2(x - PX(4), a.y), ImVec2(x + PX(4), a.y),
                                  ImVec2(x, a.y + PX(6)), col);
        }
    }
    float cx = floorf(a.x + w * 0.5f) + 0.5f;
    dl->AddLine(ImVec2(cx, a.y), ImVec2(cx, b.y), UI_TEXT, PX(2));
}

static void deck_strip(UiState *ui, int index, ImVec2 a, ImVec2 b)
{
    RXApp *app = ui->app;
    const RXLinkPlayer &d = app->link_state.devices[index];
    RXTrack *t = ours(app, index);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    double pos = position(ui, d, t);
    bool loaded = d.has_status && d.track_id;
    float gap = PX(4), width = b.x - a.x;
    char buf[96];

    // The title row, tinted by the deck's side: the number, red on air;
    // the sleeve; the title and the artist.
    float title_h = PX(UI_TITLE_ROW_H);
    ImVec2 ta = a, tb(b.x, a.y + title_h);
    ImU32 tint = d.number % 2 ? UI_DECK_A : UI_DECK_B;
    dl->AddRectFilledMultiColor(ta, tb, ui_alpha(tint, 0.55f), ui_alpha(tint, 0.08f),
                                ui_alpha(tint, 0.08f), ui_alpha(tint, 0.55f));
    snprintf(buf, sizeof(buf), "%d", d.number);
    float nw = PX(40);
    ui_text_centre(ui, dl, ui->font_bold, 30.0f, ta, ImVec2(ta.x + nw, tb.y), d.on_air ? UI_RED : UI_TEXT,
                   buf);
    float art = PX(52);
    ImVec2 p0(ta.x + nw, floorf((ta.y + tb.y - art) * 0.5f)), p1(p0.x + art, p0.y + art);
    uint64_t tex = t ? rx_app_artwork_texture(app, t) : 0;
    if (tex)
        dl->AddImage(ImTextureRef((ImTextureID)tex), p0, p1);
    else
        ui_record_icon(dl, p0, p1);
    float tx = p1.x + PX(12), ty = floorf((ta.y + tb.y) * 0.5f - PX(21)), max_w = tb.x - tx - PX(8);
    if (t) {
        ui_text(ui, dl, ui->font_bold, 22.0f, ImVec2(tx, ty), UI_TEXT, ui_title(t), max_w);
        ui_text(ui, dl, nullptr, 14.0f, ImVec2(tx, ty + PX(26)), UI_DIM, ui_artist(t), max_w);
    } else {
        const char *what = !d.has_status ? "Waiting for its status" : !loaded ? "No track loaded" : buf;
        if (loaded && d.track_device == d.number)
            snprintf(buf, sizeof(buf), "Track %u from its %s", d.track_id, slot_name(d.track_slot));
        else if (loaded)
            snprintf(buf, sizeof(buf), "Track %u from device %d's %s", d.track_id, d.track_device,
                     slot_name(d.track_slot));
        ui_text(ui, dl, ui->font_bold, 22.0f, ImVec2(tx, ty), UI_FAINT, what, max_w);
        ui_text(ui, dl, nullptr, 14.0f, ImVec2(tx, ty + PX(26)), UI_FAINT,
                loaded ? "Not one of rxbox's tracks" : "Drop a track here", max_w);
    }

    // The readout row: the player, its state; time, tempo and key.
    float y = tb.y + gap;
    ImVec2 ra(a.x, y), rb(b.x, y + PX(LINK_READOUT_H));
    ui_text(ui, dl, ui->font_bold, 12.0f, ImVec2(ra.x + PX(8), ra.y + PX(5)), UI_TEXT,
            d.name[0] ? d.name : "Player", PX(150));
    float cx = ra.x + PX(8), cy = ra.y + PX(24);
    if (d.has_status && loaded)
        cx = chip(ui, dl, cx, cy, d.playing ? "PLAYING" : "PAUSED", d.playing ? UI_PLAY : UI_DIM);
    if (d.on_air)
        cx = chip(ui, dl, cx, cy, "ON AIR", UI_RED);
    if (d.master)
        cx = chip(ui, dl, cx, cy, "MASTER", UI_ACCENT);
    if (d.synced)
        cx = chip(ui, dl, cx, cy, "SYNC", UI_DIM);

    float x = rb.x - PX(8), ry = ra.y + PX(3);
    if (t && pos >= 0)
        ui_fmt_time(-(t->duration - pos), buf, sizeof(buf), true);
    else
        snprintf(buf, sizeof(buf), "-:--.-");
    readout(ui, dl, x, ry, "REMAIN", buf, t && pos >= 0 ? UI_TEXT : UI_FAINT);
    x -= PX(104);
    char pitch[24] = "";
    if (loaded && d.bpm > 0) {
        snprintf(buf, sizeof(buf), "%.2f", d.bpm * (1.0 + d.pitch / 100.0));
        if (fabs(d.pitch) > 0.005)
            snprintf(pitch, sizeof(pitch), "%+.2f%%", d.pitch);
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    readout(ui, dl, x, ry, "BPM", buf, pitch[0] ? UI_ACCENT : UI_TEXT, pitch[0] ? pitch : nullptr);
    x -= PX(112);
    // The key where there is room for it after the player's name and state.
    if (x - PX(56) > ImMax(cx, ra.x + PX(160))) {
        char key[16] = "";
        const char *kt = t ? ui_key_text(ui, t->key, key, sizeof(key)) : "";
        readout(ui, dl, x, ry, "KEY", kt[0] ? kt : "-", UI_TEXT);
    }

    // The overview, the part played shaded and the playhead on it.
    y = rb.y + gap;
    ImVec2 oa(a.x, y), ob(b.x, y + PX(UI_OVERVIEW_H));
    dl->AddRectFilled(oa, ob, UI_WAVE_BG, PX(2));
    if (t) {
        ui_row_preview(ui, t, oa, ob, false);
        if (pos >= 0 && t->duration > 0) {
            float px = floorf(oa.x + (float)(pos / t->duration) * (ob.x - oa.x)) + 0.5f;
            dl->AddRectFilled(oa, ImVec2(px, ob.y), IM_COL32(0, 0, 0, 0x60));
            dl->AddLine(ImVec2(px, oa.y), ImVec2(px, ob.y), UI_TEXT, PX(2));
        }
    }

    // The detail waveform around the player's place, or why there is none.
    y = ob.y + gap;
    ImVec2 da(a.x, y), db(b.x, b.y);
    if (db.y - da.y > PX(24)) {
        if (t && pos >= 0) {
            detail(ui, d, t, pos, da, db);
        } else {
            dl->AddRectFilled(da, db, UI_WAVE_BG, PX(2));
            const char *why = !t ? "" : !rx_track_analysed(t) ? "Not analysed: no beat grid"
                            : d.beat < 0 ? "This player does not send where it is in the track"
                                         : "";
            if (why[0])
                ui_text_centre(ui, dl, nullptr, 12.0f, da, db, UI_FAINT, why);
        }
    }

    // The frame glows orange while the player plays.
    if (d.playing && loaded)
        dl->AddRect(ImVec2(a.x - PX(2), a.y - PX(2)), ImVec2(b.x + PX(2), b.y + PX(2)), UI_ACCENT, PX(4),
                    PX(2));

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

void ui_link_decks_panel(UiState *ui, ImVec2 size)
{
    RXApp *app = ui->app;
    int idx[UI_LINK_DECKS];
    int n = players(app, idx, UI_LINK_DECKS);
    ImGui::BeginChild("##linkdecks", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetWindowPos(), b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, UI_PANEL, PX(4));
    dl->AddRect(a, b, IM_COL32(0x2A, 0x2A, 0x2A, 0xFF), PX(4));

    // Equal shares, at most half the width each: two fill it.
    float pad = PX(8);
    int slots = std::max(n, 2);
    float share = (size.x - pad) / (float)slots;
    bool moving = false;
    for (int k = 0; k < n; k++) {
        const RXLinkPlayer &d = app->link_state.devices[idx[k]];
        int slot = n == 1 ? (d.number % 2 ? 0 : 1) : k;
        ImVec2 sa(floorf(a.x + pad + slot * share), a.y + pad);
        ImVec2 sb(floorf(sa.x + share - pad), b.y - pad);
        ImGui::PushID(d.number);
        deck_strip(ui, idx[k], sa, sb);
        ImGui::PopID();
        moving |= d.playing && ui->link_decks[d.number].pos >= 0;
    }
    ui->link_decks_drawn = n;
    if (moving)
        ui_want_frames(ui, 2);
    ImGui::EndChild();
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
