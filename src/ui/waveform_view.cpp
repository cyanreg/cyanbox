/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The waveforms: the player's overview (the whole track) and detail (a
// window of `zoom_bars` bars under a fixed centre playhead), and the table's
// row previews. The waveform itself is always rx_wave_draw(): the ANLZ
// column bytes go to the GPU once and the shader colours every pixel the
// way rekordbox does, at any zoom. Everything over it (beat grid, cue
// markers, loop bands, playhead) is ImDrawList.
//
// What goes where follows rekordbox and the CDJ-3000 as rbxport measured
// them (src/canvas/waveform.ts, src/views/player): hot cues are a square
// badge in the cue's colour with its letter, its left edge on the cue;
// memory cues a red downward triangle centred on the cue; loops a
// translucent orange band; the played part of the overview dimmed.
//
// The detail window is computed in bars, not seconds: `zoom_bars` bars
// either side of the playhead converted to seconds through the grid
// (rx_grid_offset_beats), so a 174 BPM track and a 120 BPM track show the
// same amount of music. Without a grid 120 BPM is assumed.

#include "ui_internal.h"

#include <math.h>
#include <stdio.h>

// The 3-band overview's gain (rx_wave_draw). PWV6 at rekordbox's levels
// (encoder version 3) has a low band of about 22 on average and 39 at the
// 95th percentile on loud tracks, so value/127 would draw a thin strip;
// rekordbox's overview fills its height, which 127/42 does, with only
// the loudest columns clipped.
#define UI_OVERVIEW_GAIN (127.0f / 42.0f)

/* ------------------------------------------------------------- grid helpers */

double ui_zoom_step(double bars, int dir)
{
    int at = -1;
    for (int i = 0; i < UI_NB_ZOOM; i++) {
        if (fabs(UI_ZOOM_STEPS[i] - bars) < 1e-9)
            at = i;
    }
    if (at < 0) {
        // Not a step (an old setting): the nearest one.
        at = 6;
        for (int i = 0; i < UI_NB_ZOOM; i++) {
            if (fabs(UI_ZOOM_STEPS[i] - bars) < fabs(UI_ZOOM_STEPS[at] - bars))
                at = i;
        }
    }
    return UI_ZOOM_STEPS[ImClamp(at + dir, 0, UI_NB_ZOOM - 1)];
}

static uint64_t grid_sig(const RXTrack *t)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void *p, size_t n) {
        const uint8_t *b = (const uint8_t *)p;
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    mix(&t->id, sizeof(t->id));
    mix(&t->duration, sizeof(t->duration));
    mix(&t->grid.nb_sections, sizeof(int));
    mix(&t->grid.beats_per_bar, sizeof(int));
    for (int i = 0; i < t->grid.nb_sections; i++) {
        mix(&t->grid.sections[i].start, sizeof(double));
        mix(&t->grid.sections[i].bpm, sizeof(double));
        mix(&t->grid.sections[i].beat, sizeof(int));
    }
    return h;
}

// Every beat of the loaded track's grid, rebuilt when the grid changes.
const RXBeat *ui_deck_beats(UiState *ui, int deck, int *nb)
{
    UiDeck *ud = &ui->decks[deck];
    RXTrack *t = ui->app->decks[deck].track;
    *nb = 0;
    if (!t || !rx_track_analysed(t) || !(t->duration > 0))
        return nullptr;
    uint64_t sig = grid_sig(t);
    if (sig != ud->beats_sig || !ud->beats) {
        free(ud->beats);
        ud->beats = nullptr;
        ud->nb_beats = 0;
        ud->beats_sig = sig;
        if (rx_grid_beats(&t->grid, t->duration, &ud->beats, &ud->nb_beats) < 0) {
            ud->beats = nullptr;
            ud->nb_beats = 0;
        }
    }
    *nb = ud->nb_beats;
    return ud->beats;
}

static int first_beat_at(const RXBeat *beats, int nb, double t)
{
    int lo = 0, hi = nb;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (beats[mid].time < t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int beats_per_bar(const RXTrack *t)
{
    return t && t->grid.beats_per_bar > 0 ? t->grid.beats_per_bar : 4;
}

// Half the detail window in seconds: zoom/2 bars each side of the playhead.
static double detail_half_seconds(const RXTrack *t, double pos, double bars)
{
    double half_beats = bars * beats_per_bar(t) * 0.5;
    if (t && rx_track_analysed(t)) {
        double t0 = rx_grid_offset_beats(&t->grid, pos, -half_beats);
        double t1 = rx_grid_offset_beats(&t->grid, pos, half_beats);
        // Symmetric so the playhead stays at the centre across a tempo change.
        if (t1 > t0)
            return (t1 - t0) * 0.5;
        double bpm = rx_grid_bpm_at(&t->grid, pos);
        if (bpm > 0)
            return half_beats * 60.0 / bpm;
    }
    return half_beats * 0.5;
}

/* ------------------------------------------------------------------ markers */

// A hot cue badge: a square in the cue's colour, the letter in it, its left
// edge on the cue (pulled inside the strip at the far end).
static void hot_badge(UiState *ui, ImDrawList *dl, float x, float y, float size, float x_max,
                      const RXCue *c)
{
    uint32_t rgb = ui_hot_cue_rgb(c);
    float bx = ImMin(x, x_max - size);
    ImVec2 p0(floorf(bx), floorf(y)), p1(p0.x + size, p0.y + size);
    dl->AddRectFilled(p0, p1, ui_rgb(rgb), PX(1.5f));
    char letter[2] = { rx_cue_letter(c), 0 };
    ui_text_centre(ui, dl, ui->font_bold, size / ui->s * 0.78f, p0, p1, ui_text_on(rgb), letter);
}

// A memory cue head: a downward triangle centred on the cue.
static void memory_head(ImDrawList *dl, float x, float y, float half, float h, ImU32 col)
{
    dl->AddTriangleFilled(ImVec2(x - half, y), ImVec2(x + half, y), ImVec2(x, y + h), col);
}

// The cue point a CUE press returns to: a small orange triangle at the
// bottom edge, as a CDJ marks it.
static void cue_point_mark(ImDrawList *dl, float x, float y, float half, ImU32 col)
{
    dl->AddTriangleFilled(ImVec2(x, y - half * 1.4f), ImVec2(x + half, y), ImVec2(x - half, y), col);
}

static void loop_band(ImDrawList *dl, float x0, float x1, float y0, float y1, float alpha)
{
    if (x1 - x0 < 1)
        x1 = x0 + 1;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ui_alpha(UI_ACCENT, alpha));
}

// Scrubbing shared by both waveforms: a click (no drag) seeks; a drag
// scrubs, the engine playing grains at the drag position.
static void scrub(UiState *ui, int deck, int area, double click_time, double drag_time, bool loaded)
{
    RXApp *app = ui->app;
    UiDeck *ud = &ui->decks[deck];
    RXDeckCtl *c = &app->decks[deck].ctl;
    ImGuiIO &io = ImGui::GetIO();
    if (!loaded || !app->player)
        return;
    if (ImGui::IsItemActivated()) {
        ud->drag_area = area;
        ud->drag_x0 = io.MousePos.x;
        ud->scrubbing = false;
    }
    if (ud->drag_area != area)
        return;
    if (ImGui::IsItemActive()) {
        if (!ud->scrubbing && fabsf(io.MousePos.x - ud->drag_x0) > PX(3)) {
            rx_deck_scrub_begin(app->player, deck);
            ud->scrubbing = true;
        }
        if (ud->scrubbing)
            rx_deck_scrub_to(app->player, deck, drag_time);
    }
    if (ImGui::IsItemDeactivated()) {
        if (ud->scrubbing)
            rx_deck_scrub_end(app->player, deck);
        else
            rx_ctl_seek(c, click_time);
        ud->scrubbing = false;
        ud->drag_area = 0;
    }
}

/* ----------------------------------------------------------------- overview */

void ui_overview(UiState *ui, int deck, ImVec2 a, ImVec2 b)
{
    RXApp *app = ui->app;
    RXDeckView *dv = &app->decks[deck];
    RXTrack *t = dv->track;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    RXDeckState st;
    double pos = ui_deck_position(ui, deck, &st);
    bool loaded = st.loaded && t && !dv->loading_request;
    double dur = st.duration > 0 ? st.duration : t ? t->duration : 0;
    float w = b.x - a.x;
    dl->AddRectFilled(a, b, UI_WAVE_BG, PX(2));

    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##overview", ImVec2(w, b.y - a.y));
    bool hovered = ImGui::IsItemHovered();
    double mouse_t = ImClamp((ImGui::GetIO().MousePos.x - a.x) / w, 0.0f, 1.0f) * dur;
    double click_t = ImClamp((ui->decks[deck].drag_x0 - a.x) / w, 0.0f, 1.0f) * dur;
    scrub(ui, deck, 1, click_t, mouse_t, loaded && dur > 0);
    if (!loaded || !(dur > 0))
        return;
    auto xof = [&](double sec) { return a.x + (float)(sec / dur) * w; };

    // Loop bands under the waveform's markers: memory loops, then the
    // deck's current loop, brighter while it plays.
    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        if (rx_cue_is_loop(c))
            loop_band(dl, xof(c->time), xof(c->loop_end), a.y, b.y, 0.22f);
    }

    if (dv->wave_preview) {
        double cols = rx_wave_columns(dv->wave_preview);
        RXWaveDraw d = {};
        d.x0 = a.x;
        d.y0 = a.y;
        d.x1 = b.x;
        d.y1 = b.y;
        d.col0 = 0;
        d.col1 = cols;
        d.mode = RX_WAVE_CENTRED;
        d.inset_top = PX(8);
        d.inset_bottom = PX(2);
        d.alpha = 1.0f;
        // The played part at 55 % brightness, as rekordbox dims it.
        d.dim = 0.45f;
        d.dim_until = pos / dur * cols;
        // PWV6 at rekordbox's levels is drawn up to fill the strip, as
        // rekordbox's overview does (UI_OVERVIEW_GAIN); PWAV and PWV4
        // heights already fill it.
        if (app->wave_palette == 0)
            d.gain = UI_OVERVIEW_GAIN;
        rx_wave_draw(app->window, dl, dv->wave_preview, &d);
    } else {
        ui_text_centre(ui, dl, nullptr, 11.0f, a, b, UI_FAINT, t->has_waveforms ? "" : "not analysed");
    }

    if (st.loop_in >= 0 && st.loop_out > st.loop_in) {
        loop_band(dl, xof(st.loop_in), xof(st.loop_out), a.y, b.y, st.loop_enabled ? 0.45f : 0.25f);
        if (st.loop_enabled) {
            dl->AddLine(ImVec2(xof(st.loop_in), a.y), ImVec2(xof(st.loop_in), b.y), UI_ACCENT);
            dl->AddLine(ImVec2(xof(st.loop_out), a.y), ImVec2(xof(st.loop_out), b.y), UI_ACCENT);
        }
    }

    // Memory cues: a red head and a 1 px line; hot cues: their badge on top.
    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        if (rx_cue_is_hot(c))
            continue;
        float x = floorf(xof(c->time)) + 0.5f;
        ImU32 col = ui_rgb(ui_memory_cue_rgb(c));
        dl->AddLine(ImVec2(x, a.y + PX(5)), ImVec2(x, b.y), ui_alpha(col, 0.75f));
        memory_head(dl, x, a.y, PX(4), PX(6), col);
    }
    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        if (!rx_cue_is_hot(c))
            continue;
        float x = floorf(xof(c->time));
        dl->AddLine(ImVec2(x + 0.5f, a.y), ImVec2(x + 0.5f, b.y), ui_rgb(ui_hot_cue_rgb(c), 0.55f));
        hot_badge(ui, dl, x, a.y, PX(11), b.x, c);
    }
    if (dv->ctl.cue_point >= 0)
        cue_point_mark(dl, xof(dv->ctl.cue_point), b.y, PX(4), UI_ACCENT);

    // The playhead: 1 px, 2 px while playing.
    float px = floorf(xof(pos));
    float pw = st.playing ? PX(2) : PX(1);
    dl->AddRectFilled(ImVec2(px - (pw > 1 ? pw * 0.5f : 0), a.y),
                      ImVec2(px + (pw > 1 ? pw * 0.5f : 1), b.y), UI_TEXT);

    if (hovered && !ImGui::IsItemActive()) {
        float hx = floorf(xof(mouse_t)) + 0.5f;
        dl->AddLine(ImVec2(hx, a.y), ImVec2(hx, b.y), IM_COL32(0xFF, 0xFF, 0xFF, 0x50));
        char buf[32];
        ui_fmt_time(mouse_t, buf, sizeof(buf), true);
        float tw = ui_text_width(ui, nullptr, 10.0f, buf);
        float tx = hx + PX(4) + tw > b.x ? hx - PX(4) - tw : hx + PX(4);
        dl->AddRectFilled(ImVec2(tx - PX(2), b.y - PX(14)), ImVec2(tx + tw + PX(2), b.y - PX(1)),
                          IM_COL32(0, 0, 0, 0xB0));
        ui_text(ui, dl, nullptr, 10.0f, ImVec2(tx, b.y - PX(14)), UI_TEXT, buf);
    }
}

/* ------------------------------------------------------------------- detail */

static void detail_menu(UiState *ui, int deck)
{
    RXApp *app = ui->app;
    RXDeckView *dv = &app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXTrack *t = dv->ctl.track;
    if (!ImGui::BeginPopup("##cuemenu"))
        return;
    if (!t) {
        ImGui::EndPopup();
        return;
    }
    char buf[48];
    ImGui::TextDisabled("At %s", ui_fmt_time(ud->ctx_time, buf, sizeof(buf), true));
    ImGui::Separator();
    if (ImGui::MenuItem("Set memory cue here")) {
        RXCue c;
        rx_cue_init(&c, ud->ctx_time);
        rx_cue_attach_to_grid(&c, rx_track_analysed(t) ? &t->grid : nullptr);
        if (rx_track_add_cue(t, &c) >= 0)
            rx_app_save_track(app, t);
    }
    if (ImGui::BeginMenu("Set hot cue here")) {
        for (int slot = 0; slot < RX_HOT_CUE_SLOTS; slot++) {
            char label[64], at[32] = "";
            int idx = rx_track_hot_cue(t, slot);
            if (idx >= 0)
                snprintf(at, sizeof(at), "replaces %s",
                         ui_fmt_time(t->cues[idx].time, buf, sizeof(buf), true));
            snprintf(label, sizeof(label), "%c", 'A' + slot);
            if (ImGui::MenuItem(label, at)) {
                RXCue c;
                rx_cue_init(&c, ud->ctx_time);
                c.hot = slot;
                rx_cue_attach_to_grid(&c, rx_track_analysed(t) ? &t->grid : nullptr);
                if (rx_track_add_cue(t, &c) >= 0)
                    rx_app_save_track(app, t);
            }
        }
        ImGui::EndMenu();
    }
    int ci = ud->ctx_cue;
    if (ci >= 0 && ci < t->nb_cues) {
        RXCue *c = &t->cues[ci];
        ImGui::Separator();
        if (rx_cue_is_hot(c))
            ImGui::TextDisabled("Hot cue %c", rx_cue_letter(c));
        else
            ImGui::TextDisabled(rx_cue_is_loop(c) ? "Memory loop" : "Memory cue");
        ImGui::SetNextItemWidth(PX(200));
        if (ImGui::InputTextWithHint("##cname", "Name", ud->name_buf, sizeof(ud->name_buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            if (rx_cue_is_hot(c)) {
                rx_ctl_hot_cue_set(&dv->ctl, c->hot, ud->name_buf, c->color);
            } else {
                snprintf(c->name, sizeof(c->name), "%s", ud->name_buf);
                rx_app_save_track(app, t);
            }
        }
        char col[RX_COLOR_MAX];
        bool picked = rx_cue_is_hot(c) ? ui_hot_cue_colour_menu(ui, c->color, col)
                                       : ui_track_colour_menu(ui, c->color, col);
        if (picked) {
            if (rx_cue_is_hot(c)) {
                rx_ctl_hot_cue_set(&dv->ctl, c->hot, c->name, col);
            } else {
                snprintf(c->color, sizeof(c->color), "%s", col);
                rx_app_save_track(app, t);
            }
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Delete")) {
            if (rx_cue_is_hot(c)) {
                rx_ctl_hot_cue_delete(&dv->ctl, c->hot);
            } else if (rx_track_remove_cue(t, ci) >= 0) {
                if (dv->ctl.active_memory_loop == ci)
                    dv->ctl.active_memory_loop = -1;
                else if (dv->ctl.active_memory_loop > ci)
                    dv->ctl.active_memory_loop--;
                rx_app_save_track(app, t);
            }
        }
    }
    ImGui::EndPopup();
}

void ui_detail(UiState *ui, int deck, ImVec2 a, ImVec2 b)
{
    RXApp *app = ui->app;
    RXDeckView *dv = &app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXTrack *t = dv->track;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGuiIO &io = ImGui::GetIO();
    RXDeckState st;
    double pos = ui_deck_position(ui, deck, &st);
    bool loaded = st.loaded && t && !dv->loading_request;
    float w = b.x - a.x, h = b.y - a.y;
    float cx = floorf(a.x + w * 0.5f);
    dl->AddRectFilled(a, b, UI_WAVE_BG, PX(2));
    if (!(dv->zoom_bars > 0))
        dv->zoom_bars = 12;

    double half = detail_half_seconds(loaded ? t : nullptr, pos, dv->zoom_bars);
    double w0 = pos - half, w1 = pos + half;
    double spp = (w1 - w0) / w;
    auto xof = [&](double sec) { return a.x + (float)((sec - w0) / spp); };

    // Interaction first so the zoom buttons drawn later win the overlap.
    ImGui::SetCursorScreenPos(a);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##detail", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated())
        ud->drag_t0 = pos;
    double drag_t = ud->drag_t0 - (io.MousePos.x - ud->drag_x0) * spp;
    double click_t = pos + (ud->drag_x0 - cx) * spp;
    scrub(ui, deck, 2, click_t, ImClamp(drag_t, 0.0, st.duration > 0 ? st.duration : 1e9), loaded);
    if (hovered && io.MouseWheel != 0) {
        dv->zoom_bars = ui_zoom_step(dv->zoom_bars, io.MouseWheel > 0 ? -1 : 1);
        ui_want_frames(ui, 2);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && loaded) {
        double tt = pos + (io.MousePos.x - cx) * spp;
        if (dv->ctl.quantize && rx_track_analysed(t))
            tt = rx_grid_quantize(&t->grid, tt);
        ud->ctx_time = ImClamp(tt, 0.0, t->duration);
        ud->ctx_cue = -1;
        float best = PX(7);
        for (int i = 0; i < t->nb_cues; i++) {
            float d = fabsf(xof(t->cues[i].time) - io.MousePos.x);
            if (d <= best) {
                best = d;
                ud->ctx_cue = i;
            }
        }
        snprintf(ud->name_buf, sizeof(ud->name_buf), "%s",
                 ud->ctx_cue >= 0 ? t->cues[ud->ctx_cue].name : "");
        ImGui::OpenPopup("##cuemenu");
    }

    float top = a.y + PX(14);                   // badges and bar numbers above the waveform
    if (loaded) {
        int bpb = beats_per_bar(t);
        int nb = 0;
        const RXBeat *beats = ui_deck_beats(ui, deck, &nb);

        // Loop bands behind the markers.
        for (int i = 0; i < t->nb_cues; i++) {
            const RXCue *c = &t->cues[i];
            if (rx_cue_is_loop(c) && c->loop_end > w0 && c->time < w1)
                loop_band(dl, xof(c->time), xof(c->loop_end), top, b.y, 0.18f);
        }
        if (st.loop_in >= 0 && st.loop_out > st.loop_in && st.loop_out > w0 && st.loop_in < w1)
            loop_band(dl, xof(st.loop_in), xof(st.loop_out), top, b.y, st.loop_enabled ? 0.30f : 0.15f);

        if (dv->wave_detail) {
            RXWaveDraw d = {};
            d.x0 = a.x;
            d.y0 = a.y;
            d.x1 = b.x;
            d.y1 = b.y;
            d.col0 = w0 * RX_WAVE_COLUMNS_PER_SECOND;
            d.col1 = w1 * RX_WAVE_COLUMNS_PER_SECOND;
            d.mode = ui->detail_half ? RX_WAVE_HALF_OVERLAID : RX_WAVE_CENTRED;
            d.inset_top = top - a.y;
            d.inset_bottom = PX(3);
            d.detail = true;
            d.alpha = 1.0f;
            rx_wave_draw(app->window, dl, dv->wave_detail, &d);
        } else {
            ui_text_centre(ui, dl, nullptr, 12.0f, a, b, UI_FAINT, "No waveform: analyse the track");
        }

        // The beat grid over the waveform: beats in #4C4C4C, downbeats in
        // #9A9A9A, bar numbers above the downbeats. Beats closer than 4 px
        // and every beat at 64 bars are left out; bar numbers are thinned
        // to keep them legible.
        if (beats && nb > 0) {
            double beat_px = 60.0 / ImMax(rx_grid_bpm_at(&t->grid, pos), 1.0) / spp;
            bool every_beat = beat_px >= PX(4) && dv->zoom_bars < 64;
            int label_every = 1;
            while (beat_px * bpb * label_every < PX(26))
                label_every *= 2;
            for (int i = first_beat_at(beats, nb, w0 - spp); i < nb && beats[i].time <= w1 + spp; i++) {
                bool down = beats[i].number == 1;
                if (!down && !every_beat)
                    continue;
                float x = floorf(xof(beats[i].time)) + 0.5f;
                dl->AddLine(ImVec2(x, top), ImVec2(x, b.y),
                            down ? ui_alpha(UI_DOWNBEAT, 0.85f) : ui_alpha(UI_BEAT, 0.9f));
                dl->AddLine(ImVec2(x, a.y + PX(10)), ImVec2(x, top), down ? UI_DOWNBEAT : UI_BEAT);
                if (down) {
                    int bar = 0, bn = 0;
                    rx_grid_bar_of(&t->grid, beats[i].time + 1e-4, &bar, &bn);
                    // Left of the line: a hot cue badge on the downbeat
                    // starts at the line and goes right.
                    if ((bar - 1) % label_every == 0) {
                        char num[16];
                        snprintf(num, sizeof(num), "%d", bar);
                        ui_text_right(ui, dl, nullptr, 9.5f, ImVec2(x - PX(7), a.y + PX(1)), UI_DIM,
                                      num);
                    }
                }
            }
        }

        // The current loop's in and out in solid orange.
        if (st.loop_in >= 0 && st.loop_out > st.loop_in) {
            float li = floorf(xof(st.loop_in)), lo = floorf(xof(st.loop_out));
            if (li >= a.x && li <= b.x)
                dl->AddRectFilled(ImVec2(li, top), ImVec2(li + PX(2), b.y), UI_ACCENT);
            if (lo >= a.x && lo <= b.x)
                dl->AddRectFilled(ImVec2(lo - PX(2), top), ImVec2(lo, b.y), UI_ACCENT);
        }

        // Cue markers, taller than on the overview.
        for (int i = 0; i < t->nb_cues; i++) {
            const RXCue *c = &t->cues[i];
            if (rx_cue_is_hot(c) || c->time < w0 - 1 || c->time > w1 + 1)
                continue;
            float x = floorf(xof(c->time)) + 0.5f;
            ImU32 col = ui_rgb(ui_memory_cue_rgb(c));
            dl->AddLine(ImVec2(x, top), ImVec2(x, b.y), col);
            memory_head(dl, x, a.y + PX(2), PX(6), PX(10), col);
        }
        for (int i = 0; i < t->nb_cues; i++) {
            const RXCue *c = &t->cues[i];
            if (!rx_cue_is_hot(c) || c->time < w0 - 1 || c->time > w1 + 1)
                continue;
            float x = floorf(xof(c->time));
            ImU32 col = ui_rgb(ui_hot_cue_rgb(c));
            dl->AddLine(ImVec2(x + 0.5f, top), ImVec2(x + 0.5f, b.y), col);
            hot_badge(ui, dl, x, a.y + PX(1), PX(13), b.x + PX(13), c);
            if (c->name[0])
                ui_text(ui, dl, nullptr, 10.0f, ImVec2(x + PX(16), a.y + PX(1)),
                        ui_alpha(UI_TEXT, 0.85f), c->name, PX(120));
        }
        if (dv->ctl.cue_point >= w0 && dv->ctl.cue_point <= w1)
            cue_point_mark(dl, floorf(xof(dv->ctl.cue_point)) + 0.5f, b.y, PX(5), UI_ACCENT);
    }

    // The fixed centre playhead: 2 px white with a triangle at each end.
    dl->AddRectFilled(ImVec2(cx - PX(1), a.y), ImVec2(cx + PX(1), b.y), loaded ? UI_TEXT : UI_FAINT);
    if (loaded) {
        float k = PX(5);
        dl->AddTriangleFilled(ImVec2(cx - k, a.y), ImVec2(cx + k, a.y), ImVec2(cx, a.y + k), UI_TEXT);
        dl->AddTriangleFilled(ImVec2(cx - k, b.y), ImVec2(cx, b.y - k), ImVec2(cx + k, b.y), UI_TEXT);
    }

    // Zoom: + / - / RST at the right edge, and the zoom in bars.
    float bw = PX(24), bh = PX(18);
    float zx = b.x - bw - PX(4);
    float zy = a.y + PX(16);
    struct { const char *id; UiIcon icon; const char *label; const char *tip; } zb[3] = {
        { "zin", UI_ICON_PLUS, nullptr, "Zoom in (wheel)" },
        { "zout", UI_ICON_MINUS, nullptr, "Zoom out (wheel)" },
        { "zrst", UI_ICON_NONE, "RST", "Back to 12 bars" },
    };
    for (int i = 0; i < 3; i++) {
        ImGui::SetCursorScreenPos(ImVec2(zx, zy + i * (bh + PX(3))));
        UiButton btn = {};
        btn.icon = zb[i].icon;
        btn.label = zb[i].label;
        btn.font = 8.5f;
        btn.fill = IM_COL32(0x20, 0x20, 0x20, 0xD0);
        btn.rounding = -1;
        if (ui_button(ui, zb[i].id, ImVec2(bw, bh), btn)) {
            if (i == 0)
                dv->zoom_bars = ui_zoom_step(dv->zoom_bars, -1);
            else if (i == 1)
                dv->zoom_bars = ui_zoom_step(dv->zoom_bars, 1);
            else
                dv->zoom_bars = 12;
        }
        ui_tooltip(zb[i].tip);
    }
    char zoom[24];
    if (dv->zoom_bars < 1)
        snprintf(zoom, sizeof(zoom), "1/%d BAR", (int)lround(1.0 / dv->zoom_bars));
    else
        snprintf(zoom, sizeof(zoom), "%g BARS", dv->zoom_bars);
    ui_text_right(ui, dl, nullptr, 9.0f, ImVec2(b.x - PX(6), b.y - PX(15)), UI_DIM, zoom);

    detail_menu(ui, deck);
}

/* -------------------------------------------------------------- row preview */

// The table's Preview cell: the 3-band preview (PWV6) as a stacked half
// waveform, rekordbox's row preview (the shader holds rbxport's measured
// 128/256/128 scales), with hot cue badges and memory cue heads over it.
// The upload and its cache are app.c's (rx_app_preview_wave).
void ui_row_preview(UiState *ui, const RXTrack *t, ImVec2 a, ImVec2 b, bool analysing)
{
    RXApp *app = ui->app;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    RXWaveGpu *g = t->has_waveforms ? rx_app_preview_wave(app, t) : nullptr;
    if (!g && t->has_waveforms)
        ui->previews_pending = true;
    float w = b.x - a.x;
    if (g) {
        RXWaveDraw d = {};
        d.x0 = a.x;
        d.y0 = a.y;
        d.x1 = b.x;
        d.y1 = b.y;
        d.col0 = 0;
        d.col1 = rx_wave_columns(g);
        d.mode = RX_WAVE_HALF_STACKED;
        d.inset_top = PX(1);
        d.alpha = t->missing ? 0.3f : 1.0f;
        rx_wave_draw(app->window, dl, g, &d);
    } else if (!t->has_waveforms) {
        // Unanalysed: a thin flat line.
        float y = floorf(b.y - PX(2));
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(0x44, 0x44, 0x44, 0xFF));
    }
    if (t->duration > 0 && !t->missing) {
        float bsz = PX(7);
        for (int i = 0; i < t->nb_cues; i++) {
            const RXCue *c = &t->cues[i];
            float x = a.x + (float)(c->time / t->duration) * w;
            if (!rx_cue_is_hot(c))
                memory_head(dl, floorf(x) + 0.5f, a.y, PX(3), PX(4), ui_rgb(ui_memory_cue_rgb(c)));
        }
        for (int i = 0; i < t->nb_cues; i++) {
            const RXCue *c = &t->cues[i];
            if (!rx_cue_is_hot(c))
                continue;
            float x = floorf(ImMin(a.x + (float)(c->time / t->duration) * w, b.x - bsz));
            dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x + bsz, a.y + bsz), ui_rgb(ui_hot_cue_rgb(c)));
        }
    }
    if (analysing)
        ui_spinner(dl, ImVec2(b.x - PX(8), (a.y + b.y) * 0.5f), PX(5), UI_ACCENT);
}
