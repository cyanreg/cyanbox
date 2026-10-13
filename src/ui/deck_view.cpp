/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The player panel: rekordbox's 1 PLAYER deck across the full width. Rows
// top to bottom: the title row (sleeve, title/artist, key/BPM/time/bar
// readouts), the overview and detail waveforms (waveform_view.cpp), the
// control row and the grid tools strip. The control row shows every group
// at once, with no modes or tabs, in one row of pads so that the height it
// does not need goes to the detail waveform: transport, the HOT CUE pads,
// the BEAT LOOP pads over the loop strip, the MEMORY buttons, one list of
// every cue, tempo.
//
// The cue list keeps a selection: a click on a row selects the cue (and
// calls it), and while the list has the selection Delete deletes that cue
// rather than taking tracks out of the playlist (keys.cpp asks here).
//
// Every control is one call into the deck controls (rxbox/player.h): the
// engine owns the CDJ rules (what CUE does when held at the cue point, when
// a hot cue sets or calls, quantise), so nothing here second-guesses them.
// Positions are read from rx_deck_state() each frame; the interface never
// integrates time itself. Buttons that a CDJ acts on when pressed (CUE, the
// hot cue pads) send press on ImGui activation and release on deactivation.

#include "ui_internal.h"

#include <math.h>
#include <stdio.h>

// BEAT LOOP pads: the CDJ-3000's 1/4 .. 32 beat loops.
static const double LOOP_PADS[8] = { 0.25, 0.5, 1, 2, 4, 8, 16, 32 };
static const char *const LOOP_LABELS[8] = { "1/4", "1/2", "1", "2", "4", "8", "16", "32" };

// The control row's grid in logical pixels (UI_CONTROL_ROW_H tall): each
// group's caps label in the top band, one row of pads under it, the 24 px
// strips on the bottom line, which the transport row, the lower MEMORY
// buttons and MASTER TEMPO share. Pads are PAD_W_MIN to PAD_W_MAX wide by
// the room (control_row).
static const float ROW_LABEL_H = 18;
static const float ROW_BOTTOM  = 2;
static const float PAD_H       = 36;
static const float PAD_GAP     = 4;
static const float PAD_W_MIN   = 48;
static const float PAD_W_MAX   = 64;
static const float STRIP_H     = 24;
static const float GROUP_GAP   = 16;
static const float MEM_W       = 96;
static const float MEM_H       = 26;
static const float CUE_ROW_H   = 22;
static const float LIST_MIN_W  = 240;

// Flags from the analysis worth a DJ's attention (rxbox/analysis.h).
static const unsigned GRID_WARN_FLAGS = RX_GRID_FLAG_OCTAVE_UNCERTAIN | RX_GRID_FLAG_PHASE_UNCERTAIN |
                                        RX_GRID_FLAG_DOWNBEAT_WEAK | RX_GRID_FLAG_LOW_CONFIDENCE |
                                        RX_GRID_FLAG_NO_KICKS | RX_GRID_FLAG_UNSTEADY;

/* ------------------------------------------------------------------ helpers */

double ui_deck_position(UiState *ui, int deck, RXDeckState *st)
{
    RXDeckState tmp;
    if (!st)
        st = &tmp;
    memset(st, 0, sizeof(*st));
    st->tempo = 1.0;
    st->loop_in = st->loop_out = -1;
    if (ui->app->player)
        rx_deck_state(ui->app->player, deck, st);
    if (!(st->tempo > 0))
        st->tempo = 1.0;
    return st->loaded ? st->position : 0.0;
}

// A grid edit: the controls apply it to the deck, re-resolve the musical
// cues and save the sidecar (rx_ctl_grid_changed).
void ui_grid_edited(UiState *ui, int deck)
{
    rx_ctl_grid_changed(&ui->app->decks[deck].ctl);
    ui_want_frames(ui, 2);
}

// SHIFT GRID (rekordbox: Ctrl+cursor, the grid strip's nudge).
void ui_shift_grid(UiState *ui, int deck, double seconds)
{
    RXTrack *t = ui->app->decks[deck].ctl.track;
    if (!t || !rx_track_analysed(t) || t->grid_locked)
        return;
    rx_grid_shift(&t->grid, seconds);
    ui_grid_edited(ui, deck);
}

// Beat jump by the selected size; Fine moves 10 ms, a CDJ's fine search.
void ui_jump(UiState *ui, int deck, int dir)
{
    RXDeckCtl *c = &ui->app->decks[deck].ctl;
    double beats = UI_JUMP_SIZES[ui->decks[deck].jump_size].beats;
    if (!c->track)
        return;
    if (beats <= 0)
        rx_ctl_nudge(c, dir * UI_FINE_JUMP_SECONDS);
    else
        rx_ctl_beat_jump(c, dir * beats);
}

static void set_tempo(UiState *ui, int deck, double pct)
{
    if (ui->app->player)
        rx_deck_set_tempo(ui->app->player, deck, 1.0 + pct / 100.0);
}

static RXTrack *shown_track(UiState *ui, int deck)
{
    RXDeckView *dv = &ui->app->decks[deck];
    if (dv->loading_request && dv->loading_track_id) {
        RXTrack *t = rx_library_find_id(&ui->app->lib, dv->loading_track_id);
        if (t)
            return t;
    }
    return dv->track;
}

static void bar_beat_text(const RXTrack *t, double pos, char *buf, size_t size)
{
    if (!t || !rx_track_analysed(t)) {
        snprintf(buf, size, "-");
        return;
    }
    int bar = 0, beat = 0;
    rx_grid_bar_of(&t->grid, pos, &bar, &beat);
    snprintf(buf, size, "%d.%d", bar, beat);
}

// A loop's length in beats as rekordbox writes it: "1/4", "1/2", "1", "2" ...
static void loop_beats_text(double beats, char *buf, size_t size)
{
    if (beats > 0 && beats < 1)
        snprintf(buf, size, "1/%d", (int)lround(1.0 / beats));
    else if (fabs(beats - round(beats)) < 1e-6)
        snprintf(buf, size, "%d", (int)lround(beats));
    else
        snprintf(buf, size, "%.2f", beats);
}

static double cue_loop_beats(const RXTrack *t, const RXCue *c)
{
    if (!rx_cue_is_loop(c))
        return 0;
    if (!isnan(c->loop_beats))
        return c->loop_beats;
    if (rx_track_analysed(t))
        return rx_grid_time_to_beat(&t->grid, c->loop_end) - rx_grid_time_to_beat(&t->grid, c->time);
    return 0;
}

/* ---------------------------------------------------------------- title row */

// A readout: the caps label over the value, right-aligned at `right`. The
// block is 38 px tall from `y`.
static void readout(UiState *ui, ImDrawList *dl, float right, float y, const char *label,
                    const char *value, ImU32 col, const char *extra = nullptr)
{
    ui_text_right(ui, dl, ui->font_bold, 10.5f, ImVec2(right, y), UI_DIM, label);
    if (extra) {
        float lw = ui_text_width(ui, ui->font_bold, 10.5f, label);
        ui_text_right(ui, dl, nullptr, 11.0f, ImVec2(right - lw - PX(6), y - PX(0.5f)), UI_DIM, extra);
    }
    ui_text_right(ui, dl, ui->font_bold, 24.0f, ImVec2(right, y + PX(14)), col, value);
}

static void title_row(UiState *ui, int deck, ImVec2 a, ImVec2 b, const RXDeckState &st)
{
    RXApp *app = ui->app;
    RXDeckView *dv = &app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXTrack *t = shown_track(ui, deck);
    bool loaded = st.loaded && dv->track && !dv->loading_request;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // rekordbox tints deck 1's title row blue.
    dl->AddRectFilledMultiColor(a, b, ui_alpha(UI_DECK_A, 0.55f), ui_alpha(UI_DECK_A, 0.08f),
                                ui_alpha(UI_DECK_A, 0.08f), ui_alpha(UI_DECK_A, 0.55f));

    // The sleeve; clicking it ejects a stopped track, as rekordbox's does.
    float art = PX(56);
    ImVec2 p0(a.x + PX(4), floorf((a.y + b.y - art) * 0.5f)), p1(p0.x + art, p0.y + art);
    if (dv->artwork && loaded)
        dl->AddImage(ImTextureRef((ImTextureID)dv->artwork), p0, p1);
    else
        ui_record_icon(dl, p0, p1);
    ImGui::SetCursorScreenPos(p0);
    if (ImGui::InvisibleButton("##eject", ImVec2(art, art)) && loaded && !st.playing)
        rx_app_unload_deck(app, deck);
    if (loaded && ImGui::IsItemHovered()) {
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 0x90), PX(2));
        ui_text_centre(ui, dl, ui->font_bold, 10.5f, p0, p1, st.playing ? UI_FAINT : UI_TEXT, "EJECT");
        ui_tooltip(st.playing ? "Pause to eject" : "Eject the track");
    }

    // Readouts from the right edge: GRID, bar.beat, time, BPM, key. The
    // label and value block is centred on the row by its ink, which sits
    // high in ImGui's line boxes.
    float y = floorf((a.y + b.y) * 0.5f - PX(18));
    float x = b.x - PX(8);
    ImVec2 gs(PX(52), PX(24));
    ImGui::SetCursorScreenPos(ImVec2(x - gs.x, floorf((a.y + b.y - gs.y) * 0.5f)));
    UiButton gb = {};
    gb.label = "GRID";
    gb.lit = dv->grid_edit;
    gb.rounding = -1;
    if (ui_button(ui, "grid", gs, gb)) {
        dv->grid_edit = !dv->grid_edit;
        ImGui::MarkIniSettingsDirty();
    }
    ui_tooltip("Grid tools (Ctrl+G)");
    x -= gs.x + PX(20);

    char buf[64];
    double pos = st.position;
    float sep_y0 = a.y + PX(12), sep_y1 = b.y - PX(12);
    ImU32 sep = IM_COL32(0x3A, 0x3A, 0x3A, 0xFF);

    bar_beat_text(loaded ? t : nullptr, pos, buf, sizeof(buf));
    readout(ui, dl, x, y, "BAR", loaded ? buf : "-", UI_TEXT);
    x -= PX(88);
    dl->AddLine(ImVec2(x + PX(10), sep_y0), ImVec2(x + PX(10), sep_y1), sep);

    // Time: elapsed or remaining, clicked or T to toggle.
    double dur = loaded ? (st.duration > 0 ? st.duration : t->duration) : 0;
    if (loaded)
        ui_fmt_time(ud->time_remaining ? -(dur - pos) : pos, buf, sizeof(buf), true);
    else
        snprintf(buf, sizeof(buf), "-:--.-");
    readout(ui, dl, x, y, ud->time_remaining ? "REMAIN" : "ELAPSED", buf, UI_TEXT);
    ImGui::SetCursorScreenPos(ImVec2(x - PX(120), a.y + PX(4)));
    if (ImGui::InvisibleButton("##timemode", ImVec2(PX(120), b.y - a.y - PX(8))))
        ud->time_remaining = !ud->time_remaining;
    ui_tooltip("Elapsed / remaining (T)");
    x -= PX(136);
    dl->AddLine(ImVec2(x + PX(10), sep_y0), ImVec2(x + PX(10), sep_y1), sep);

    // BPM: the grid's tempo at the playhead; with the tempo slider off zero
    // the played tempo, in orange, as rekordbox shows it.
    double bpm = loaded && rx_track_analysed(t) ? rx_grid_bpm_at(&t->grid, pos) : 0;
    bool adjusted = fabs(st.tempo - 1.0) > 1e-7;
    char orig[32] = "";
    if (bpm > 0) {
        snprintf(buf, sizeof(buf), "%.2f", bpm * st.tempo);
        if (adjusted)
            snprintf(orig, sizeof(orig), "%.2f", bpm);
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    readout(ui, dl, x, y, "BPM", buf, adjusted && bpm > 0 ? UI_ACCENT : UI_TEXT,
            orig[0] ? orig : nullptr);
    if (loaded && rx_track_analysed(t) && (t->grid_flags & GRID_WARN_FLAGS)) {
        // The analyser was unsure: a small warning sign, details on hover.
        float bw = ui_text_width(ui, ui->font_bold, 24.0f, buf);
        ImVec2 c(x - bw - PX(14), y + PX(14) + PX(13));
        float r = PX(7);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y + r * 0.8f),
                              ImVec2(c.x - r, c.y + r * 0.8f), UI_ACCENT);
        ui_text_centre(ui, dl, ui->font_bold, 10.0f, ImVec2(c.x - r, c.y - r * 0.5f),
                       ImVec2(c.x + r, c.y + r), UI_BLACK, "!");
        if (ImGui::IsMouseHoveringRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r))) {
            std::string tip = "Grid: check by ear\n";
            char conf[64];
            snprintf(conf, sizeof(conf), "confidence %.2f\n", t->grid_confidence);
            tip += conf;
            for (unsigned bit = 1; bit <= RX_GRID_FLAG_UNSTEADY; bit <<= 1) {
                if (t->grid_flags & bit) {
                    const char *n = rx_grid_flag_name((RXGridFlag)bit);
                    tip += "  ";
                    tip += n ? n : "?";
                    tip += "\n";
                }
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }
    x -= PX(140);
    dl->AddLine(ImVec2(x + PX(10), sep_y0), ImVec2(x + PX(10), sep_y1), sep);

    char key[16] = "";
    const char *kt = loaded ? ui_key_text(ui, t->key, key, sizeof(key)) : "";
    readout(ui, dl, x, y, "KEY", kt[0] ? kt : "-", UI_TEXT);
    snprintf(ui->key_drawn, sizeof(ui->key_drawn), "%s", kt);
    x -= PX(76);

    // Title and artist, clipped before the readouts; like the readouts the
    // pair is placed by its ink.
    float tx = p1.x + PX(14);
    float ty = floorf((a.y + b.y) * 0.5f - PX(24));
    float max_w = x - tx - PX(12);
    if (t) {
        ui_text(ui, dl, ui->font_bold, 26.0f, ImVec2(tx, ty), UI_TEXT, ui_title(t), max_w);
        ui_text(ui, dl, nullptr, 15.0f, ImVec2(tx, ty + PX(29)), UI_DIM, ui_artist(t), max_w);
    } else {
        ui_text(ui, dl, ui->font_bold, 26.0f, ImVec2(tx, ty), UI_FAINT, "No track loaded", max_w);
        ui_text(ui, dl, nullptr, 15.0f, ImVec2(tx, ty + PX(29)), UI_FAINT,
                "Double-click a track in the browser or drag it here", max_w);
    }
}

/* ---------------------------------------------------------------- transport */

// A round CDJ button: CUE (orange ring) or PLAY/PAUSE (green ring).
static void round_button(UiState *ui, ImVec2 c, float r, ImU32 ring, bool lit, bool enabled,
                         UiIcon icon,
                         const char *label)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool down = ImGui::IsItemActive();
    bool hot = ImGui::IsItemHovered();
    ImU32 fill = down ? IM_COL32(0x40, 0x40, 0x40, 0xFF) : hot ? IM_COL32(0x30, 0x30, 0x30, 0xFF)
                                                            : IM_COL32(0x22, 0x22, 0x22, 0xFF);
    dl->AddCircleFilled(c, r, IM_COL32(0x0C, 0x0C, 0x0C, 0xFF), 48);
    dl->AddCircleFilled(c, r - PX(3), fill, 48);
    ImU32 ring_col = enabled ? (lit ? ring : ui_alpha(ring, 0.35f)) : IM_COL32(0x33, 0x33, 0x33, 0xFF);
    dl->AddCircle(c, r - PX(1.5f), ring_col, 48, PX(lit ? 3.0f : 2.0f));
    if (lit && enabled)
        dl->AddCircle(c, r + PX(1.5f), ui_alpha(ring, 0.25f), 48, PX(3.0f));
    ImU32 glyph = !enabled ? UI_FAINT : lit ? ring : UI_TEXT;
    if (label)
        ui_text_centre(ui, dl, ui->font_bold, 14.0f, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
                       glyph, label);
    else
        ui_icon(dl, icon, ImVec2(c.x - r * 0.62f, c.y - r * 0.62f),
                ImVec2(c.x + r * 0.62f, c.y + r * 0.62f), glyph);
}

static void transport(UiState *ui, int deck, ImVec2 a, ImVec2 b, const RXDeckState &st)
{
    RXDeckView *dv = &ui->app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXDeckCtl *c = &dv->ctl;
    bool has = c->track && st.loaded;
    float w = b.x - a.x;
    float rh = PX(STRIP_H);
    float ry = b.y - PX(ROW_BOTTOM) - rh;
    // The round buttons are centred on the pads beside them, as large as
    // the room above the Q and beat jump row (on the strips' line) allows.
    float d = ImMin(ImMin((w - PX(8)) * 0.5f, ry - PX(6) - a.y), PX(56));
    float pad_c = a.y + PX(ROW_LABEL_H + PAD_H * 0.5f);
    float top = floorf(ImClamp(pad_c - d * 0.5f, a.y, ry - PX(6) - d));

    // CUE: press and release go to the engine (hold at the cue point plays
    // while held; elsewhere while paused it sets the cue point).
    ImVec2 cc(a.x + d * 0.5f, top + d * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(cc.x - d * 0.5f, cc.y - d * 0.5f));
    if (!has)
        ImGui::BeginDisabled();
    ImGui::InvisibleButton("##cue", ImVec2(d, d));
    if (!has)
        ImGui::EndDisabled();
    if (ImGui::IsItemActivated() && has) {
        rx_ctl_cue_press(c);
        ud->cue_held_mouse = true;
    }
    if (ImGui::IsItemDeactivated() && ud->cue_held_mouse) {
        rx_ctl_cue_release(c);
        ud->cue_held_mouse = false;
    }
    round_button(ui, cc, d * 0.5f, UI_ACCENT, c->cue_point >= 0, has, UI_ICON_NONE, "CUE");
    ui_tooltip("CUE (C)");

    // PLAY/PAUSE acts on press, as a CDJ's does.
    ImVec2 pc(b.x - d * 0.5f, top + d * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(pc.x - d * 0.5f, pc.y - d * 0.5f));
    if (!has)
        ImGui::BeginDisabled();
    ImGui::InvisibleButton("##play", ImVec2(d, d));
    if (!has)
        ImGui::EndDisabled();
    if (ImGui::IsItemActivated() && has)
        rx_ctl_play_pause(c);
    round_button(ui, pc, d * 0.5f, UI_PLAY, st.playing, has, UI_ICON_PLAYPAUSE, nullptr);
    ui_tooltip("PLAY/PAUSE (Space)");

    // Q, then the beat jump pair with the size between; the arrows repeat
    // while held, as the keys do.
    float qw = PX(26), aw = PX(20);
    float sw = w - qw - aw * 2 - PX(8);
    ImGui::SetCursorScreenPos(ImVec2(a.x, ry));
    UiButton qb = {};
    qb.label = "Q";
    qb.lit = c->quantize;
    qb.lit_col = UI_QUANTIZE;
    qb.font = 13.0f;
    qb.rounding = -1;
    if (ui_button(ui, "q", ImVec2(qw, rh), qb))
        rx_ctl_set_quantize(c, !c->quantize);
    ui_tooltip("Quantize (Q): cues, loops and jumps snap to the beat grid");

    float x = a.x + qw + PX(4);
    UiButton jb = {};
    jb.icon = UI_ICON_LEFT;
    jb.disabled = !has;
    jb.repeat = true;
    jb.rounding = -1;
    ImGui::SetCursorScreenPos(ImVec2(x, ry));
    if (ui_button(ui, "jl", ImVec2(aw, rh), jb))
        ui_jump(ui, deck, -1);
    ui_tooltip("Beat jump back (Left; hold to repeat)");
    x += aw + PX(2);

    ImGui::SetCursorScreenPos(ImVec2(x, ry));
    UiButton sb = {};
    sb.label = UI_JUMP_SIZES[ud->jump_size].label;
    sb.fill = IM_COL32(0x18, 0x18, 0x18, 0xFF);
    sb.border = IM_COL32(0x3A, 0x3A, 0x3A, 0xFF);
    sb.rounding = -1;
    // The size the deck started with is the players' BEAT JUMP BEAT VALUE;
    // these change the running deck's only.
    if (ui_button(ui, "jsize", ImVec2(sw, rh), sb)) {
        ud->jump_size = (ud->jump_size + 1) % UI_NB_JUMP;
        c->jump_beats = UI_JUMP_SIZES[ud->jump_size].beats;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##jumpsizes");
    ui_tooltip("Beat jump size: click cycles, right-click picks");
    if (ImGui::BeginPopup("##jumpsizes")) {
        for (int i = 0; i < UI_NB_JUMP; i++) {
            if (ImGui::MenuItem(UI_JUMP_SIZES[i].label, nullptr, ud->jump_size == i)) {
                ud->jump_size = i;
                c->jump_beats = UI_JUMP_SIZES[i].beats;
            }
        }
        ImGui::EndPopup();
    }
    x += sw + PX(2);

    jb.icon = UI_ICON_RIGHT;
    ImGui::SetCursorScreenPos(ImVec2(x, ry));
    if (ui_button(ui, "jr", ImVec2(aw, rh), jb))
        ui_jump(ui, deck, 1);
    ui_tooltip("Beat jump forward (Right; hold to repeat)");
}

/* --------------------------------------------------------------- hot cues */

static void pad_popup(UiState *ui, int deck, int slot)
{
    RXDeckView *dv = &ui->app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXDeckCtl *c = &dv->ctl;
    char pid[32];
    snprintf(pid, sizeof(pid), "##padmenu%d", slot);
    if (!ImGui::BeginPopup(pid))
        return;
    RXTrack *t = c->track;
    int idx = t ? rx_track_hot_cue(t, slot) : -1;
    if (idx < 0) {
        ImGui::TextDisabled("Hot cue %c is empty", 'A' + slot);
        ImGui::EndPopup();
        return;
    }
    RXCue *cue = &t->cues[idx];
    char pos[32];
    ImGui::Text("Hot cue %c  %s", 'A' + slot, ui_fmt_time(cue->time, pos, sizeof(pos), true));
    ImGui::Separator();
    ImGui::TextDisabled("Name");
    ImGui::SetNextItemWidth(PX(220));
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    bool commit = ImGui::InputText("##name", ud->name_buf, sizeof(ud->name_buf),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    if (commit || ImGui::IsItemDeactivatedAfterEdit()) {
        rx_ctl_hot_cue_set(c, slot, ud->name_buf, cue->color);
        // The edit re-adds the cue: the list may have moved.
        idx = rx_track_hot_cue(t, slot);
        if (idx < 0) {
            ImGui::EndPopup();
            return;
        }
        cue = &t->cues[idx];
    }
    ImGui::TextDisabled("Colour");
    char col[RX_COLOR_MAX];
    if (ui_hot_cue_colour_menu(ui, cue->color, col)) {
        rx_ctl_hot_cue_set(c, slot, ud->name_buf, col);
        ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete hot cue")) {
        rx_ctl_hot_cue_delete(c, slot);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// One HOT CUE pad: dark with a dim letter when empty, filled in the cue's
// colour when set. Press/release go to the engine (an empty pad stores the
// playhead or the active loop; a set one calls and plays). Shift-click or
// the corner x deletes; right-click edits colour and name.
static void hot_cue_pad(UiState *ui, int deck, int slot, ImVec2 p, ImVec2 size)
{
    RXApp *app = ui->app;
    RXDeckView *dv = &app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXDeckCtl *c = &dv->ctl;
    RXTrack *t = c->track;
    int idx = t ? rx_track_hot_cue(t, slot) : -1;
    const RXCue *cue = idx >= 0 ? &t->cues[idx] : nullptr;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGuiIO &io = ImGui::GetIO();
    char id[16];
    snprintf(id, sizeof(id), "##pad%d", slot);

    ImGui::SetCursorScreenPos(p);
    ImGui::SetNextItemAllowOverlap();
    if (!t)
        ImGui::BeginDisabled();
    ImGui::InvisibleButton(id, size);
    if (!t)
        ImGui::EndDisabled();
    bool down = ImGui::IsItemActive();
    if (ImGui::IsItemActivated() && t) {
        if (io.KeyShift) {
            if (cue)
                rx_ctl_hot_cue_delete(c, slot);
        } else {
            rx_ctl_hot_cue_press(c, slot);
            ud->pads_down |= 1u << slot;
        }
    }
    if (ImGui::IsItemDeactivated() && (ud->pads_down & (1u << slot))) {
        ud->pads_down &= ~(1u << slot);
        rx_ctl_hot_cue_release(c, slot);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && cue) {
        snprintf(ud->name_buf, sizeof(ud->name_buf), "%s", cue->name);
        char pid[32];
        snprintf(pid, sizeof(pid), "##padmenu%d", slot);
        ImGui::OpenPopup(pid);
    }
    bool hovered = t && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + size.x, p.y + size.y)) &&
                   ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    ImVec2 q(p.x + size.x, p.y + size.y);
    float r = PX(4);
    char letter[2] = { (char)('A' + slot), 0 };
    // The letter at the top left, the name or position under it: both fit
    // the 36 px pad at 15 and 10.5 px.
    ImVec2 lp(p.x + PX(6), p.y + PX(2));
    if (cue) {
        uint32_t rgb = ui_hot_cue_rgb(ui, cue);
        ImU32 fill = ui_rgb(rgb, down ? 1.0f : hovered ? 0.92f : 0.82f);
        ImU32 ink = ui_text_on(rgb);
        dl->AddRectFilled(p, q, fill, r);
        if (down)
            dl->AddRect(p, q, UI_TEXT, r, PX(2));
        ui_text(ui, dl, ui->font_bold, 15.0f, lp, ink, letter);
        char pos[48];
        const char *sub = cue->name[0] ? cue->name : ui_fmt_time(cue->time, pos, sizeof(pos), true);
        ui_text(ui, dl, nullptr, 10.5f, ImVec2(p.x + PX(6), q.y - PX(15)), ui_alpha(ink, 0.85f), sub,
                size.x - PX(9));
        if (rx_cue_is_loop(cue) && !hovered)
            ui_icon(dl, UI_ICON_LOOP, ImVec2(q.x - PX(16), p.y + PX(4)),
                    ImVec2(q.x - PX(4), p.y + PX(16)), ink);
    } else {
        ImU32 fill = down ? IM_COL32(0x33, 0x33, 0x33, 0xFF) : IM_COL32(0x17, 0x17, 0x17, 0xFF);
        ImU32 edge = hovered ? IM_COL32(0x55, 0x55, 0x55, 0xFF) : IM_COL32(0x2C, 0x2C, 0x2C, 0xFF);
        ImU32 ink = t ? UI_FAINT : IM_COL32(0x33, 0x33, 0x33, 0xFF);
        dl->AddRectFilled(p, q, fill, r);
        dl->AddRect(p, q, edge, r);
        ui_text(ui, dl, ui->font_bold, 15.0f, lp, ink, letter);
    }

    // The corner x on a set pad (drawn over it; the overlap lets it win).
    if (cue && hovered) {
        ImVec2 xs(PX(14), PX(14));
        ImVec2 xp(q.x - xs.x - PX(2), p.y + PX(2));
        ImGui::SetCursorScreenPos(xp);
        char xid[16];
        snprintf(xid, sizeof(xid), "##padx%d", slot);
        if (ImGui::InvisibleButton(xid, xs))
            rx_ctl_hot_cue_delete(c, slot);
        bool xh = ImGui::IsItemHovered();
        dl->AddRectFilled(xp, ImVec2(xp.x + xs.x, xp.y + xs.y), IM_COL32(0, 0, 0, xh ? 0xC0 : 0x70),
                          PX(3));
        ui_icon(dl, UI_ICON_CROSS, xp, ImVec2(xp.x + xs.x, xp.y + xs.y), UI_TEXT);
        if (xh)
            ImGui::SetTooltip("Delete hot cue %c", 'A' + slot);
    }
    pad_popup(ui, deck, slot);
}

// A plain pad: the BEAT LOOP pads, lit orange for the running loop.
static bool mode_pad(UiState *ui, const char *id, ImVec2 p, ImVec2 size, const char *label,
                     const char *sub, bool lit, bool enabled)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(p);
    if (!enabled)
        ImGui::BeginDisabled();
    bool pressed = ImGui::InvisibleButton(id, size);
    if (!enabled)
        ImGui::EndDisabled();
    bool down = ImGui::IsItemActive(), hot = ImGui::IsItemHovered();
    ImVec2 q(p.x + size.x, p.y + size.y);
    float r = PX(4);
    ImU32 fill = lit ? ui_alpha(UI_ACCENT, down ? 1.0f : 0.85f)
               : down ? IM_COL32(0x3A, 0x3A, 0x3A, 0xFF) : IM_COL32(0x1C, 0x1C, 0x1C, 0xFF);
    dl->AddRectFilled(p, q, fill, r);
    dl->AddRect(p, q,
                lit ? UI_ACCENT : hot ? IM_COL32(0x60, 0x60, 0x60, 0xFF) : IM_COL32(0x30, 0x30, 0x30, 0xFF),
                r);
    ImU32 ink = !enabled ? UI_FAINT : lit ? UI_BLACK : UI_TEXT;
    float label_y = sub ? -PX(5) : 0;
    ui_text_centre(ui, dl, ui->font_bold, 14.0f, ImVec2(p.x, p.y + label_y), ImVec2(q.x, q.y + label_y),
                   ink, label);
    if (sub)
        ui_text_centre(ui, dl, nullptr, 10.0f, ImVec2(p.x, q.y - PX(14)), ImVec2(q.x, q.y - PX(2)),
                       ui_alpha(ink, 0.7f), sub);
    return pressed && enabled;
}

/* ------------------------------------------------------------- control row */

// A group's caps label, at the top of its band.
static void group_label(UiState *ui, ImDrawList *dl, ImVec2 a, const char *text)
{
    ui_text(ui, dl, ui->font_bold, 10.5f, ImVec2(a.x, a.y + PX(2)), UI_DIM, text);
}

// The width of a group of eight pads in one row.
static float pads_width(UiState *ui, float pad_w)
{
    return pad_w * 8 + PX(PAD_GAP) * 7;
}

// HOT CUE: pads A-H in one row.
static void hot_cue_group(UiState *ui, int deck, ImVec2 a, float pad_w)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    group_label(ui, dl, a, "HOT CUE");
    float py = a.y + PX(ROW_LABEL_H);
    for (int i = 0; i < 8; i++)
        hot_cue_pad(ui, deck, i, ImVec2(a.x + i * (pad_w + PX(PAD_GAP)), py), ImVec2(pad_w, PX(PAD_H)));
}

// BEAT LOOP: the auto loop pads in one row, the pad of the running loop lit
// (RXDeckCtl.last_hot_loop_beats), and the loop strip under them. Storing
// a loop as a memory loop is MEMORY's, next to it.
static void loop_group(UiState *ui, int deck, ImVec2 a, ImVec2 b, float pad_w, const RXDeckState &st)
{
    RXDeckCtl *c = &ui->app->decks[deck].ctl;
    bool has = c->track && st.loaded;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float w = pads_width(ui, pad_w);
    group_label(ui, dl, a, "BEAT LOOP");
    float py = a.y + PX(ROW_LABEL_H);
    for (int i = 0; i < 8; i++) {
        bool lit = c->looping && fabs(c->last_hot_loop_beats - LOOP_PADS[i]) < 1e-6;
        char id[16];
        snprintf(id, sizeof(id), "##loop%d", i);
        ImVec2 p(a.x + i * (pad_w + PX(PAD_GAP)), py);
        if (mode_pad(ui, id, p, ImVec2(pad_w, PX(PAD_H)), LOOP_LABELS[i], i < 2 ? "beat" : "beats", lit,
                     has))
            rx_ctl_auto_loop(c, LOOP_PADS[i]);
    }

    float sh = PX(STRIP_H), sy = b.y - PX(ROW_BOTTOM) - sh;
    float sg = PX(3);
    float bw = floorf((w - sg * 4) / 5);
    bool loop_set = c->loop_in >= 0 && c->loop_out > c->loop_in;
    // Halve and double are a drawn obelus and cross before the 2.
    struct { const char *label; UiIcon icon; const char *tip; } btn[5] = {
        { "IN", UI_ICON_NONE, "Loop in (I)" }, { "OUT", UI_ICON_NONE, "Loop out (O)" },
        { c->looping ? "EXIT" : "RELOOP", UI_ICON_NONE, "Exit / reloop (R)" },
        { "2", UI_ICON_DIVIDE, "Halve the loop (/)" }, { "2", UI_ICON_CROSS, "Double the loop (\\)" },
    };
    for (int i = 0; i < 5; i++) {
        float x = a.x + i * (bw + sg);
        ImGui::SetCursorScreenPos(ImVec2(x, sy));
        UiButton lb = {};
        lb.label = btn[i].label;
        lb.icon = btn[i].icon;
        lb.disabled = !has || (i >= 2 && !loop_set) || (i == 1 && c->loop_in < 0);
        lb.lit = (i == 2 && c->looping) || (i == 0 && c->loop_in >= 0 && !c->looping && c->loop_out < 0);
        lb.font = i >= 3 ? 12.0f : 0;
        lb.rounding = -1;
        char id[16];
        snprintf(id, sizeof(id), "ls%d", i);
        if (ui_button(ui, id, ImVec2(i == 4 ? a.x + w - x : bw, sh), lb)) {
            switch (i) {
            case 0: rx_ctl_loop_in(c); break;
            case 1: rx_ctl_loop_out(c); break;
            case 2: rx_ctl_exit_reloop(c); break;
            case 3: rx_ctl_loop_half(c); break;
            case 4: rx_ctl_loop_double(c); break;
            }
        }
        ui_tooltip(btn[i].tip);
    }
}

// MEMORY: store the cue point (or the active loop), delete the memory cue
// the playhead is on, call the one before or after the playhead (it
// becomes the cue point). A 2x2 block: the upper pair level with the pads,
// the lower on the strips' line.
static void memory_group(UiState *ui, int deck, ImVec2 a, ImVec2 b, const RXDeckState &st)
{
    RXDeckCtl *c = &ui->app->decks[deck].ctl;
    bool has = c->track && st.loaded;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    group_label(ui, dl, a, "MEMORY");
    float bw = floorf((b.x - a.x - PX(PAD_GAP)) * 0.5f), bh = PX(MEM_H);
    float y0 = a.y + PX(ROW_LABEL_H) + floorf((PX(PAD_H) - bh) * 0.5f);
    float y1 = b.y - PX(ROW_BOTTOM) - bh;
    struct { const char *id; const char *label; UiIcon icon; bool after; const char *tip; } btn[4] = {
        { "mset", "MEMORY", UI_ICON_NONE, false,
          "Store the cue point, or the active loop, as a memory cue (M)" },
        { "mdel", "DELETE", UI_ICON_NONE, false, "Delete the memory cue at the playhead (X)" },
        { "mprev", "CUE", UI_ICON_LEFT, false, "Call the previous memory cue (B)" },
        { "mnext", "CUE", UI_ICON_RIGHT, true, "Call the next memory cue (N)" },
    };
    for (int i = 0; i < 4; i++) {
        ImGui::SetCursorScreenPos(ImVec2(i & 1 ? b.x - bw : a.x, i < 2 ? y0 : y1));
        UiButton mb = {};
        mb.label = btn[i].label;
        mb.icon = btn[i].icon;
        mb.icon_after = btn[i].after;
        mb.disabled = !has;
        mb.rounding = -1;
        if (ui_button(ui, btn[i].id, ImVec2(bw, bh), mb)) {
            switch (i) {
            case 0: rx_ctl_memory_set(c); break;
            case 1: rx_ctl_memory_delete(c); break;
            case 2: rx_ctl_memory_prev(c); break;
            case 3: rx_ctl_memory_next(c); break;
            }
        }
        ui_tooltip(btn[i].tip);
    }
}

/* ----------------------------------------------------------------- cue list */

static void delete_cue(UiState *ui, int deck, int index)
{
    RXDeckView *dv = &ui->app->decks[deck];
    RXTrack *t = dv->ctl.track;
    if (!t || index < 0 || index >= t->nb_cues)
        return;
    if (rx_cue_is_hot(&t->cues[index])) {
        rx_ctl_hot_cue_delete(&dv->ctl, t->cues[index].hot);
        return;
    }
    if (rx_track_remove_cue(t, index) < 0)
        return;
    // The engaged memory loop is an index into the same list.
    if (dv->ctl.active_memory_loop == index)
        dv->ctl.active_memory_loop = -1;
    else if (dv->ctl.active_memory_loop > index)
        dv->ctl.active_memory_loop--;
    rx_app_save_track(ui->app, t);
}

// The selection is kept by what the cue is, not by its index: the list is
// sorted by time and a cue set or deleted elsewhere (a pad, M, X) moves
// the rows under it.
static void cue_select(UiState *ui, int deck, const RXTrack *t, int index)
{
    UiDeck *ud = &ui->decks[deck];
    const RXCue *c = &t->cues[index];
    ud->cue_focus = true;
    ud->cue_selected = true;
    ud->cue_track = t->id;
    ud->cue_hot = c->hot;
    ud->cue_time = c->time;
    ud->cue_end = c->loop_end;
}

// The selected cue's index, -1 when none. Another track in the player
// takes the keyboard back from the list; a cue gone (deleted, or a grid
// edit moved it) leaves the list the keyboard with nothing selected.
static int cue_selected_index(UiState *ui, int deck)
{
    UiDeck *ud = &ui->decks[deck];
    const RXTrack *t = ui->app->decks[deck].ctl.track;
    if (ud->cue_focus && (!t || t->id != ud->cue_track))
        ud->cue_focus = ud->cue_selected = false;
    if (!ud->cue_focus || !ud->cue_selected)
        return -1;
    int index = -1;
    if (ud->cue_hot >= 0) {
        index = rx_track_hot_cue(t, ud->cue_hot);
    } else {
        for (int i = 0; i < t->nb_cues && index < 0; i++) {
            const RXCue *c = &t->cues[i];
            if (!rx_cue_is_hot(c) && c->time == ud->cue_time && c->loop_end == ud->cue_end)
                index = i;
        }
    }
    if (index < 0)
        ud->cue_selected = false;
    return index;
}

bool ui_cue_list_has_keys(UiState *ui, int deck)
{
    cue_selected_index(ui, deck);
    return ui->decks[deck].cue_focus;
}

void ui_cue_list_release(UiState *ui, int deck)
{
    ui->decks[deck].cue_focus = false;
    ui->decks[deck].cue_selected = false;
}

// Delete: the selected cue goes and the list keeps the keyboard with
// nothing selected, so a second press neither deletes the next cue nor
// takes the track out of the playlist.
void ui_cue_list_delete(UiState *ui, int deck)
{
    UiDeck *ud = &ui->decks[deck];
    int index = cue_selected_index(ui, deck);
    if (index < 0)
        return;
    ud->cue_selected = false;
    if (ud->edit_cue >= index)
        ud->edit_cue = -1;
    delete_cue(ui, deck, index);
    ui_want_frames(ui, 2);
}

// Up/Down move the selection without calling the cue (a click calls it).
void ui_cue_list_move(UiState *ui, int deck, int dir)
{
    UiDeck *ud = &ui->decks[deck];
    const RXTrack *t = ui->app->decks[deck].ctl.track;
    if (!t || t->nb_cues <= 0)
        return;
    int index = cue_selected_index(ui, deck);
    index = index < 0 ? (dir > 0 ? 0 : t->nb_cues - 1) : ImClamp(index + dir, 0, t->nb_cues - 1);
    cue_select(ui, deck, t, index);
    ud->cue_scroll = true;
    ui_want_frames(ui, 2);
}

// What the cue list's rows asked for that changes the list: done once it
// has been drawn, since a hot cue edit re-adds the cue (and may move the
// array) and a deletion shifts the rows.
struct CueEdit {
    int  del;                  // cue index, -1
    int  hot_name;             // hot cue slot renamed to UiDeck.name_buf, -1
    int  hot_colour;           // hot cue slot given `colour`, -1
    char colour[RX_COLOR_MAX];
};

// A row's context menu: rename, colour (a hot cue takes the pads' sixteen,
// a memory cue the track colours), delete.
static void cue_row_menu(UiState *ui, int deck, int index, CueEdit *edit)
{
    RXDeckView *dv = &ui->app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXTrack *t = dv->ctl.track;
    RXCue *cue = &t->cues[index];
    if (ImGui::MenuItem("Rename")) {
        ud->edit_cue = index;
        ud->edit_focus = true;
        snprintf(ud->name_buf, sizeof(ud->name_buf), "%s", cue->name);
    }
    ImGui::TextDisabled("Colour");
    char col[RX_COLOR_MAX];
    if (rx_cue_is_hot(cue)) {
        if (ui_hot_cue_colour_menu(ui, cue->color, col)) {
            edit->hot_colour = cue->hot;
            snprintf(edit->colour, sizeof(edit->colour), "%s", col);
            ImGui::CloseCurrentPopup();
        }
    } else if (ui_track_colour_menu(ui, cue->color, col)) {
        snprintf(cue->color, sizeof(cue->color), "%s", col);
        rx_app_save_track(ui->app, t);
        ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete"))
        edit->del = index;
}

// Every cue of the track in one list, by time (the track keeps them
// sorted): memory cues with their colour dot, hot cues with their lettered
// badge, in 22 px rows that scroll. A click selects the cue and calls it
// (a hot cue as its pad does, press and release); a double-click edits the
// name in place. The cue at the playhead has an orange bar at its left.
// The bar.beat column shows only when the name keeps some room beside it.
static void cue_list(UiState *ui, int deck, ImVec2 a, ImVec2 b, double pos)
{
    RXApp *app = ui->app;
    RXDeckView *dv = &app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXDeckCtl *c = &dv->ctl;
    RXTrack *t = c->track;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // rekordbox keeps at most ten memory cues and loops, and eight hot cues.
    group_label(ui, dl, a, "CUES");
    if (t) {
        int nb_mem = 0, nb_hot = 0;
        for (int i = 0; i < t->nb_cues; i++) {
            if (rx_cue_is_hot(&t->cues[i]))
                nb_hot++;
            else
                nb_mem++;
        }
        char count[64];
        snprintf(count, sizeof(count), "%d/10 memory \xC2\xB7 %d/8 hot", nb_mem, nb_hot);
        ui_text_right(ui, dl, nullptr, 11.0f, ImVec2(b.x - PX(4), a.y + PX(1.5f)), UI_DIM, count);
    }

    ImVec2 la(a.x, a.y + PX(ROW_LABEL_H)), lb(b.x, b.y - PX(ROW_BOTTOM));
    dl->AddRectFilled(la, lb, IM_COL32(0x10, 0x10, 0x10, 0xFF), PX(3));
    ImGui::SetCursorScreenPos(la);
    // Rows abut: a Selectable grows into half the item spacing around it.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##cuelist", ImVec2(lb.x - la.x, lb.y - la.y), ImGuiChildFlags_None, 0);
    ImDrawList *ldl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float rh = PX(CUE_ROW_H), fs = 12.5f;
    float lw = ImGui::GetContentRegionAvail().x;
    bool show_bar = lw >= PX(290);
    int nb = t ? t->nb_cues : 0;
    int sel = cue_selected_index(ui, deck);
    CueEdit edit = { -1, -1, -1, "" };

    for (int i = 0; i < nb; i++) {
        RXCue *cue = &t->cues[i];
        bool hot = rx_cue_is_hot(cue);
        ImGui::PushID(i);
        ImVec2 p(o.x, o.y + i * rh), q(p.x + lw, p.y + rh);
        if (i & 1)
            ldl->AddRectFilled(p, q, IM_COL32(0xFF, 0xFF, 0xFF, 0x06));
        ImGui::SetCursorScreenPos(p);
        bool engaged = !hot && c->active_memory_loop == i && c->looping;
        // The selected row stays blue under the pointer that just clicked
        // it (ImGui would draw it in the plain hover grey).
        if (i == sel)
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetColorU32(ImGuiCol_HeaderActive));
        bool clicked = ImGui::Selectable("##row", i == sel,
                                         ImGuiSelectableFlags_AllowOverlap |
                                         ImGuiSelectableFlags_AllowDoubleClick, ImVec2(lw, rh));
        if (i == sel)
            ImGui::PopStyleColor();
        if (clicked) {
            cue_select(ui, deck, t, i);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ud->edit_cue = i;
                ud->edit_focus = true;
                snprintf(ud->name_buf, sizeof(ud->name_buf), "%s", cue->name);
            } else if (hot) {
                rx_ctl_hot_cue_press(c, cue->hot);
                rx_ctl_hot_cue_release(c, cue->hot);
            } else {
                rx_ctl_memory_call(c, i);
            }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            cue_select(ui, deck, t, i);
        if (i == sel && ud->cue_scroll)
            ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
        if (ImGui::BeginPopupContextItem("##rowmenu")) {
            cue_row_menu(ui, deck, i, &edit);
            ImGui::EndPopup();
        }
        if (fabs(cue->time - pos) < 0.02)
            ldl->AddRectFilled(p, ImVec2(p.x + PX(2), q.y), UI_ACCENT);

        float cy = (p.y + q.y) * 0.5f;
        float fy = floorf(cy - PX(fs) * 0.5f - PX(1));
        float x = p.x + PX(7);
        if (hot) {
            uint32_t rgb = ui_hot_cue_rgb(ui, cue);
            ImVec2 b0(x, floorf(cy - PX(7.5f))), b1(b0.x + PX(15), b0.y + PX(15));
            ldl->AddRectFilled(b0, b1, ui_rgb(rgb), PX(2));
            char letter[2] = { rx_cue_letter(cue), 0 };
            ui_text_centre(ui, ldl, ui->font_bold, 11.0f, b0, b1, ui_text_on(rgb), letter);
        } else {
            ldl->AddCircleFilled(ImVec2(x + PX(7.5f), cy), PX(4.5f), ui_rgb(ui_memory_cue_rgb(cue)), 16);
        }
        x += PX(23);
        char buf[64];
        double lb_beats = cue_loop_beats(t, cue);
        if (rx_cue_is_loop(cue)) {
            char beats[16];
            loop_beats_text(lb_beats, beats, sizeof(beats));
            snprintf(buf, sizeof(buf), lb_beats > 0 ? "LOOP %s" : "LOOP", beats);
        } else {
            snprintf(buf, sizeof(buf), "CUE");
        }
        ImU32 ink = engaged ? UI_ACCENT : UI_TEXT;
        ui_text(ui, ldl, ui->font_bold, fs, ImVec2(x, fy), ink, buf);
        x += PX(60);
        ui_text(ui, ldl, nullptr, fs, ImVec2(x, fy), ink, ui_fmt_time(cue->time, buf, sizeof(buf), true));
        x += PX(50);
        if (show_bar) {
            if (rx_track_analysed(t)) {
                int bar = 0, beat = 0;
                rx_grid_bar_of(&t->grid, cue->time, &bar, &beat);
                snprintf(buf, sizeof(buf), "%d.%d", bar, beat);
                ui_text(ui, ldl, nullptr, fs, ImVec2(x, fy), UI_DIM, buf);
            }
            x += PX(40);
        }

        // Right side: ACTIVE (memory loops) and the x.
        float rx = q.x - PX(5);
        ImVec2 xs(PX(20), PX(16));
        ImGui::SetCursorScreenPos(ImVec2(rx - xs.x, floorf(cy - xs.y * 0.5f)));
        UiButton xb = {};
        xb.icon = UI_ICON_CROSS;
        xb.fill = IM_COL32(0x22, 0x22, 0x22, 0xFF);
        xb.rounding = -1;
        if (ui_button(ui, "x", xs, xb))
            edit.del = i;
        ui_tooltip(hot ? "Delete the hot cue (Delete when selected)"
                       : "Delete the memory cue (Delete when selected)");
        rx -= xs.x + PX(5);
        if (!hot && rx_cue_is_loop(cue)) {
            // An active memory loop engages when the playhead reaches it.
            ImVec2 as(PX(50), PX(16));
            ImGui::SetCursorScreenPos(ImVec2(rx - as.x, floorf(cy - as.y * 0.5f)));
            UiButton ab = {};
            ab.label = "ACTIVE";
            ab.lit = cue->active;
            ab.font = 10.0f;
            ab.rounding = -1;
            if (ui_button(ui, "active", as, ab))
                rx_ctl_memory_set_active(c, i, !cue->active);
            ui_tooltip("Active loop: engages when the playhead reaches it");
            rx -= as.x + PX(6);
        }

        // Name, edited in place after a double-click or Rename. A hot cue's
        // goes through the controls (the pad popup's way), once the list is
        // drawn: the edit re-adds the cue.
        float nw = rx - x - PX(4);
        if (ud->edit_cue == i) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(PX(4), PX(1)));
            ImGui::SetCursorScreenPos(ImVec2(x, floorf(cy - ImGui::GetFrameHeight() * 0.5f)));
            ImGui::SetNextItemWidth(ImMax(nw, PX(80)));
            if (ud->edit_focus) {
                ImGui::SetKeyboardFocusHere();
                ud->edit_focus = false;
            }
            ImGuiInputTextFlags nf = ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_AutoSelectAll;
            bool enter = ImGui::InputText("##name", ud->name_buf, sizeof(ud->name_buf), nf);
            ImGui::PopStyleVar();
            if (enter || ImGui::IsItemDeactivated()) {
                if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && strcmp(cue->name, ud->name_buf)) {
                    if (hot) {
                        edit.hot_name = cue->hot;
                    } else {
                        snprintf(cue->name, sizeof(cue->name), "%s", ud->name_buf);
                        rx_app_save_track(app, t);
                    }
                }
                ud->edit_cue = -1;
            }
        } else if (cue->name[0] && nw > PX(24)) {
            ui_text(ui, ldl, nullptr, fs, ImVec2(x, fy), UI_TEXT, cue->name, nw);
        }
        ImGui::PopID();
    }
    ud->cue_scroll = false;
    if (t && !nb)
        ldl->AddText(ImGui::GetFont(), PX(12), ImVec2(o.x + PX(8), o.y + PX(6)), UI_FAINT,
                     "No cues. CUE while paused sets the cue point; MEMORY (M) stores it; "
                     "a pad stores a hot cue.", nullptr, lw - PX(16));
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + nb * rh));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    if (edit.hot_name >= 0)
        rx_ctl_hot_cue_set(c, edit.hot_name, ud->name_buf, nullptr);
    if (edit.hot_colour >= 0)
        rx_ctl_hot_cue_set(c, edit.hot_colour, nullptr, edit.colour);
    if (edit.del >= 0) {
        if (ud->edit_cue >= edit.del)
            ud->edit_cue = -1;
        delete_cue(ui, deck, edit.del);
    }
}

/* -------------------------------------------------------------------- tempo */

// The tempo slider: down is faster, as on a CDJ; a detent at 0; ranges
// +-6/10/16 % and WIDE. Dragging is relative to the grab so a click does
// not jump the tempo. Beside it the range, the readout, RESET and MASTER
// TEMPO, which asks the player to keep the pitch (rx_deck_set_master_tempo
// may refuse: the button then stays off, and lights only from the deck's
// state). With no caps label of its own the group takes the row's whole
// height, MT on the strips' line. The range and MASTER TEMPO start as the
// players' TEMPO RANGE (+-6 % by default, as a CDJ switched on) and MASTER
// TEMPO say (ui.cpp, follow_settings); the buttons change the running
// deck's only.
static void tempo(UiState *ui, int deck, ImVec2 a, ImVec2 b, const RXDeckState &st)
{
    RXApp *app = ui->app;
    UiDeck *ud = &ui->decks[deck];
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool has = st.loaded;
    double range = UI_TEMPO_RANGES[ud->tempo_range];
    double pct = (st.tempo - 1.0) * 100.0;
    double step = range <= 6 ? 0.02 : range <= 16 ? 0.05 : 0.5;
    float top = a.y + PX(1), bottom = b.y - PX(ROW_BOTTOM);

    float sw = PX(26);
    ImVec2 s0(a.x, top), s1(a.x + sw, bottom);
    float pad = PX(8);
    float ty0 = s0.y + pad, ty1 = s1.y - pad, cy = (ty0 + ty1) * 0.5f, half = (ty1 - ty0) * 0.5f;
    ImGui::SetCursorScreenPos(s0);
    if (!has)
        ImGui::BeginDisabled();
    ImGui::InvisibleButton("##tempo", ImVec2(sw, s1.y - s0.y));
    if (!has)
        ImGui::EndDisabled();
    static const ImGuiID drag_key = ImHashStr("rxbox.tempo.grab");
    ImGuiStorage *store = ImGui::GetStateStorage();
    if (ImGui::IsItemActivated()) {
        store->SetFloat(drag_key, (float)pct);
        store->SetFloat(drag_key + 1, ImGui::GetIO().MousePos.y);
    }
    if (ImGui::IsItemActive() && has) {
        double dy = ImGui::GetIO().MousePos.y - store->GetFloat(drag_key + 1);
        double v = store->GetFloat(drag_key) + dy / half * range;
        v = ImClamp(v, -range, range);
        if (fabs(v) < range * 0.02)
            v = 0;
        v = round(v / step) * step;
        if (fabs(v - pct) > 1e-9) {
            set_tempo(ui, deck, v);
            pct = v;
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && has) {
        set_tempo(ui, deck, 0);
        pct = 0;
    }
    ui_tooltip("Tempo: drag (down is faster), double-click resets");

    // Slot, ticks, centre detent, handle.
    float sx = (s0.x + s1.x) * 0.5f;
    dl->AddRectFilled(ImVec2(sx - PX(2), ty0), ImVec2(sx + PX(2), ty1),
                      IM_COL32(0x0C, 0x0C, 0x0C, 0xFF), PX(2));
    for (int i = -4; i <= 4; i++) {
        float y = cy + half * i / 4.0f;
        float k = i == 0 ? PX(9) : PX(5);
        dl->AddLine(ImVec2(sx - k - PX(4), y), ImVec2(sx - PX(5), y), i == 0 ? UI_PLAY : UI_FAINT);
        dl->AddLine(ImVec2(sx + PX(5), y), ImVec2(sx + k + PX(4), y), i == 0 ? UI_PLAY : UI_FAINT);
    }
    float hy = cy + (float)(ImClamp(pct / range, -1.0, 1.0)) * half;
    ImVec2 h0(s0.x + PX(2), hy - PX(5)), h1(s1.x - PX(2), hy + PX(5));
    dl->AddRectFilled(h0, h1, has ? IM_COL32(0x9A, 0x9A, 0x9A, 0xFF) : IM_COL32(0x44, 0x44, 0x44, 0xFF),
                      PX(2));
    dl->AddLine(ImVec2(h0.x + PX(1), hy), ImVec2(h1.x - PX(1), hy), pct == 0 ? UI_PLAY : UI_BLACK,
                PX(1.5f));

    // Range, readout, reset, master tempo.
    float cx = s1.x + PX(6);
    float cw = b.x - cx;
    float rh = PX(20), zh = PX(18), mh = PX(STRIP_H);
    char label[16];
    if (range >= 100)
        snprintf(label, sizeof(label), "WIDE");
    else
        snprintf(label, sizeof(label), "\xC2\xB1%d%%", (int)range);
    ImGui::SetCursorScreenPos(ImVec2(cx, top));
    UiButton rb = {};
    rb.label = label;
    rb.rounding = -1;
    if (ui_button(ui, "range", ImVec2(cw, rh), rb)) {
        ud->tempo_range = (ud->tempo_range + 1) % UI_NB_TEMPO_RANGES;
        double nr = UI_TEMPO_RANGES[ud->tempo_range];
        if (fabs(pct) > nr)
            set_tempo(ui, deck, pct > 0 ? nr : -nr);
    }
    ui_tooltip("Tempo range (each start at the Player settings' Tempo Range)");

    char val[24];
    snprintf(val, sizeof(val), "%+.2f%%", fabs(pct) < 0.005 ? 0.0 : pct);
    ui_text_centre(ui, dl, ui->font_bold, 14.0f, ImVec2(cx, top + PX(21)), ImVec2(cx + cw, top + PX(37)),
                   fabs(pct) >= 0.005 ? UI_ACCENT : UI_TEXT, val);
    ui_text_centre(ui, dl, ui->font_bold, 10.5f, ImVec2(cx, top + PX(35)), ImVec2(cx + cw, top + PX(46)),
                   UI_DIM, "TEMPO");

    ImGui::SetCursorScreenPos(ImVec2(cx, bottom - mh - PX(4) - zh));
    UiButton zb = {};
    zb.label = "RESET";
    zb.disabled = !has || pct == 0;
    zb.rounding = -1;
    if (ui_button(ui, "reset", ImVec2(cw, zh), zb))
        set_tempo(ui, deck, 0);
    ui_tooltip("Tempo back to 0");

    // The deck starts as the players' MASTER TEMPO says (ui.cpp); a
    // refusal leaves it off.
    ImGui::SetCursorScreenPos(ImVec2(cx, bottom - mh));
    UiButton mb = {};
    mb.label = "MT";
    mb.font = 12.0f;
    mb.lit = st.master_tempo;
    mb.lit_col = UI_ACCENT;
    mb.disabled = !app->player;
    mb.rounding = -1;
    if (ui_button(ui, "mt", ImVec2(cw, mh), mb)) {
        bool want = !st.master_tempo;
        bool ok = rx_deck_set_master_tempo(app->player, deck, want);
        ud->master_tempo = want && ok;
        if (want && !ok)
            rx_app_set_status(app, "Master tempo is not available: the player cannot keep the pitch");
    }
    ui_tooltip("Master tempo (key lock): the pitch stays while the tempo changes");
}

// The control row, every group at once, 16 px apart: transport, HOT CUE,
// BEAT LOOP, MEMORY, the cue list, tempo. The groups between transport and
// tempo share what those leave: the sixteen pads take up to 64 px each
// while the list keeps 240 px, and the list has whatever is left over.
// Narrower, the pads shrink first, down to 48 px; then the list gives way,
// and past it the groups are clipped short of tempo rather than drawn over
// it. At 1600 wide that is pads of 49 px and a list of 248, at 1920 pads
// of 64 and a list of 328.
static void control_row(UiState *ui, int deck, ImVec2 a, ImVec2 b, const RXDeckState &st)
{
    float gg = PX(GROUP_GAP), gap = PX(PAD_GAP);
    float tw = PX(120), tempo_w = PX(96), mem_w = PX(MEM_W) * 2 + gap, list_min = PX(LIST_MIN_W);
    transport(ui, deck, a, ImVec2(a.x + tw, b.y), st);
    tempo(ui, deck, ImVec2(b.x - tempo_w, a.y), b, st);

    float gx0 = a.x + tw + gg, gx1 = b.x - tempo_w - gg;
    if (gx1 <= gx0)
        return;
    float room = gx1 - gx0;
    float pads_room = room - gg * 3 - mem_w - list_min - gap * 7 * 2;
    float pad_w = floorf(ImClamp(pads_room / 16, PX(PAD_W_MIN), PX(PAD_W_MAX)));
    float pads_w = pads_width(ui, pad_w);
    ImGui::PushClipRect(ImVec2(gx0, a.y), ImVec2(gx1, b.y), true);
    float x = gx0;
    hot_cue_group(ui, deck, ImVec2(x, a.y), pad_w);
    x += pads_w + gg;
    loop_group(ui, deck, ImVec2(x, a.y), ImVec2(x + pads_w, b.y), pad_w, st);
    x += pads_w + gg;
    // With no room left for the list (about 1400 px wide), MEMORY narrows
    // into what there is, its labels shrinking, before anything is clipped.
    bool list = gx1 - (x + mem_w + gg) >= PX(80);
    if (!list)
        mem_w = ImClamp(gx1 - x, PX(120), mem_w);
    memory_group(ui, deck, ImVec2(x, a.y), ImVec2(x + mem_w, b.y), st);
    x += mem_w + gg;
    if (list)
        cue_list(ui, deck, ImVec2(x, a.y), ImVec2(gx1, b.y), st.position);
    ImGui::PopClipRect();
}

/* --------------------------------------------------------------- grid tools */

static void apply_bpm(UiState *ui, int deck, double bpm, double pos)
{
    RXTrack *t = ui->app->decks[deck].ctl.track;
    if (!t || t->grid_locked || !(bpm >= 20 && bpm <= 400))
        return;
    if (rx_track_analysed(t))
        rx_grid_set_bpm_at(&t->grid, pos, bpm);
    else if (rx_grid_init(&t->grid, pos, bpm) < 0)
        return;
    ui_grid_edited(ui, deck);
}

// Tap tempo: four taps or more set the BPM of the section under the
// playhead; SET then puts beat 1 at the first tap.
static void tap(UiState *ui, int deck, double pos)
{
    RXDeckView *dv = &ui->app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    int64_t now = rx_time_us();
    if (dv->nb_taps == 0 || now - ud->tap_last_us > 2000000) {
        dv->nb_taps = 0;
        ud->tap_first_pos = pos;
    }
    ud->tap_last_us = now;
    double sec = now / 1e6;
    int max = (int)(sizeof(dv->tap_times) / sizeof(dv->tap_times[0]));
    if (dv->nb_taps == max) {
        memmove(dv->tap_times, dv->tap_times + 1, sizeof(dv->tap_times[0]) * (max - 1));
        dv->nb_taps--;
    }
    dv->tap_times[dv->nb_taps++] = sec;
    if (dv->nb_taps >= 4) {
        double span = dv->tap_times[dv->nb_taps - 1] - dv->tap_times[0];
        double bpm = 60.0 * (dv->nb_taps - 1) / span;
        apply_bpm(ui, deck, round(bpm * 100) / 100, pos);
    }
}

static void tap_set_downbeat(UiState *ui, int deck)
{
    RXDeckView *dv = &ui->app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXTrack *t = dv->ctl.track;
    if (!t || !rx_track_analysed(t) || t->grid_locked)
        return;
    // Move the grid so a beat lies on the first tap, then make it beat 1.
    double near = rx_grid_nearest_beat(&t->grid, ud->tap_first_pos, nullptr);
    if (t->grid.nb_sections == 1)
        rx_grid_shift(&t->grid, ud->tap_first_pos - near);
    rx_grid_set_downbeat(&t->grid, ud->tap_first_pos);
    dv->nb_taps = 0;
    ui_grid_edited(ui, deck);
}

static void grid_strip(UiState *ui, int deck, ImVec2 a, ImVec2 b, const RXDeckState &st)
{
    RXApp *app = ui->app;
    RXDeckView *dv = &app->decks[deck];
    UiDeck *ud = &ui->decks[deck];
    RXTrack *t = dv->ctl.track;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool has = t && st.loaded;
    bool grid = has && rx_track_analysed(t);
    bool locked = has && t->grid_locked;
    double pos = st.position;
    float h = b.y - a.y;
    float bh = h - PX(6);
    float y = a.y + PX(3);
    float x = a.x + PX(6);
    dl->AddRectFilled(a, b, IM_COL32(0x17, 0x17, 0x17, 0xFF), PX(3));

    ui_text(ui, dl, ui->font_bold, 10.5f, ImVec2(x, y + (bh - PX(10.5f)) * 0.5f - PX(1)), UI_DIM,
            "GRID");
    x += PX(40);

    // BPM field: Enter applies to the section under the playhead.
    if (!ud->bpm_active) {
        if (grid)
            snprintf(ud->bpm_buf, sizeof(ud->bpm_buf), "%.2f", rx_grid_bpm_at(&t->grid, pos));
        else
            ud->bpm_buf[0] = 0;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::SetNextItemWidth(PX(76));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(PX(6), (bh - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0x0C, 0x0C, 0x0C, 0xFF));
    if (!has || locked)
        ImGui::BeginDisabled();
    ImGuiInputTextFlags bf = ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll;
    bool enter = ImGui::InputTextWithHint("##bpm", "BPM", ud->bpm_buf, sizeof(ud->bpm_buf), bf);
    if (!has || locked)
        ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ud->bpm_active = ImGui::IsItemActive();
    if (enter) {
        apply_bpm(ui, deck, atof(ud->bpm_buf), pos);
        ud->bpm_active = false;
    }
    ui_tooltip("BPM of the section under the playhead (Enter applies)");
    x += PX(76) + PX(6);

    struct Tool { const char *label; const char *tip; float w; bool enabled; bool lit; UiIcon icon; };
    bool taps_ready = dv->nb_taps >= 4 && rx_time_us() - ud->tap_last_us < 10000000;
    Tool tools[] = {
        { "TAP", "Tap tempo: four taps or more set the BPM", 46, has && !locked, false, UI_ICON_NONE },
        { "SET", "Beat 1 at the first tap", 42, taps_ready && grid && !locked, taps_ready,
         UI_ICON_NONE },
        { nullptr, "Grid 5 ms earlier (Shift: 1 ms; Ctrl+Left)", 28, grid && !locked, false,
         UI_ICON_LEFT },
        { nullptr, "Grid 5 ms later (Shift: 1 ms; Ctrl+Right)", 28, grid && !locked, false,
         UI_ICON_RIGHT },
        { "SET DOWNBEAT", "The beat nearest the playhead becomes beat 1", 112, grid && !locked, false,
         UI_ICON_NONE },
        { "SPLIT", "A new tempo section at the nearest beat", 58, grid && !locked, false,
         UI_ICON_NONE },
        { "MERGE", "Remove the section boundary nearest the playhead", 62,
          grid && !locked && t->grid.nb_sections > 1, false, UI_ICON_NONE },
        { "2", "Double the BPM of this section", 36, grid && !locked, false, UI_ICON_CROSS },
        { "2", "Halve the BPM of this section", 36, grid && !locked, false, UI_ICON_DIVIDE },
        { "LOCK", "Lock the grid: analysis will not replace it", 54, has, locked, UI_ICON_NONE },
        { "RE-ANALYSE", "Analyse this track again (a locked grid is kept)", 96, has, false,
         UI_ICON_NONE },
    };
    int clicked = -1;
    for (int i = 0; i < (int)(sizeof(tools) / sizeof(tools[0])); i++) {
        float w = PX(tools[i].w);
        if (x + w > b.x - PX(6))
            break;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        UiButton gb = {};
        gb.label = tools[i].label;
        gb.icon = tools[i].icon;
        gb.disabled = !tools[i].enabled;
        gb.lit = tools[i].lit;
        gb.lit_col = i == 9 ? UI_RED : 0;
        gb.font = i == 7 || i == 8 ? 12.0f : 0;
        gb.rounding = -1;
        char id[16];
        snprintf(id, sizeof(id), "gt%d", i);
        if (ui_button(ui, id, ImVec2(w, bh), gb))
            clicked = i;
        ui_tooltip(tools[i].tip);
        x += w + PX(4);
        if (i == 1 || i == 3 || i == 6 || i == 8)
            x += PX(8);
    }

    bool fine = ImGui::GetIO().KeyShift;
    switch (clicked) {
    case 0: tap(ui, deck, pos); break;
    case 1: tap_set_downbeat(ui, deck); break;
    case 2: ui_shift_grid(ui, deck, fine ? -0.001 : -0.005); break;
    case 3: ui_shift_grid(ui, deck, fine ? 0.001 : 0.005); break;
    case 4:
        rx_grid_set_downbeat(&t->grid, pos);
        ui_grid_edited(ui, deck);
        break;
    case 5:
        if (rx_grid_split_at(&t->grid, pos) >= 0)
            ui_grid_edited(ui, deck);
        break;
    case 6:
        if (rx_grid_merge_at(&t->grid, pos) >= 0)
            ui_grid_edited(ui, deck);
        break;
    case 7: apply_bpm(ui, deck, rx_grid_bpm_at(&t->grid, pos) * 2, pos); break;
    case 8: apply_bpm(ui, deck, rx_grid_bpm_at(&t->grid, pos) / 2, pos); break;
    case 9:
        t->grid_locked = !t->grid_locked;
        rx_app_save_track(app, t);
        break;
    case 10:
        rx_app_analyse_track(app, t, RX_ANALYSE_ALL, true);
        rx_app_set_status(app, "Re-analysing %s", ui_title(t));
        break;
    default:
        break;
    }

    // What the analyser said, at the right end.
    if (grid && x < b.x - PX(160)) {
        char info[96];
        int ns = t->grid.nb_sections;
        snprintf(info, sizeof(info), "%d section%s \xC2\xB7 confidence %.2f%s", ns, ns == 1 ? "" : "s",
                 t->grid_confidence, dv->nb_taps > 0 ? "" : "");
        if (dv->nb_taps > 0 && rx_time_us() - ud->tap_last_us < 10000000)
            snprintf(info, sizeof(info), "%d tap%s", dv->nb_taps, dv->nb_taps == 1 ? "" : "s");
        ui_text_right(ui, dl, nullptr, 11.5f, ImVec2(b.x - PX(8), y + (bh - PX(11.5f)) * 0.5f - PX(1)),
                      UI_DIM, info);
    }
}

/* -------------------------------------------------------------------- panel */

void ui_deck_panel(UiState *ui, int deck, ImVec2 size)
{
    RXApp *app = ui->app;
    RXDeckView *dv = &app->decks[deck];
    RXDeckState st;
    ui_deck_position(ui, deck, &st);
    st.loaded = st.loaded && dv->track;

    ImGui::PushID(deck);
    ImGui::BeginChild("##player", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetWindowPos();
    ImVec2 b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, UI_PANEL, PX(4));

    float pad = PX(6), gap = PX(4);
    float x0 = a.x + pad, x1 = b.x - pad;
    float y = a.y + pad;
    float title_h = PX(UI_TITLE_ROW_H), over_h = PX(UI_OVERVIEW_H), ctrl_h = PX(UI_CONTROL_ROW_H);
    float strip_h = dv->grid_edit ? PX(UI_GRID_STRIP_H) : 0;
    float detail_h = (b.y - pad) - y - title_h - over_h - ctrl_h - gap * 3;
    if (strip_h > 0)
        detail_h -= strip_h + gap;
    if (detail_h < PX(40))
        detail_h = PX(40);

    title_row(ui, deck, ImVec2(x0, y), ImVec2(x1, y + title_h), st);
    y += title_h + gap;
    ImVec2 wa(x0, y);
    ui_overview(ui, deck, ImVec2(x0, y), ImVec2(x1, y + over_h));
    y += over_h + gap;
    ui_detail(ui, deck, ImVec2(x0, y), ImVec2(x1, y + detail_h));
    y += detail_h + gap;
    ImVec2 wb(x1, y - gap);

    // While the decode job runs the waveforms are dimmed under "LOADING".
    if (dv->loading_request) {
        dl->AddRectFilled(wa, wb, IM_COL32(0, 0, 0, 0xA0));
        ui_text_centre(ui, dl, ui->font_bold, 16.0f, wa, wb, UI_ACCENT, "LOADING");
    }

    // Control row: transport | HOT CUE | BEAT LOOP | MEMORY | cues | tempo.
    control_row(ui, deck, ImVec2(x0, y), ImVec2(x1, y + ctrl_h), st);
    y += ctrl_h + gap;

    if (strip_h > 0)
        grid_strip(ui, deck, ImVec2(x0, y), ImVec2(x1, y + strip_h), st);

    // The frame glows orange while playing.
    if (st.playing)
        dl->AddRect(ImVec2(a.x + PX(1), a.y + PX(1)), ImVec2(b.x - PX(1), b.y - PX(1)), UI_ACCENT,
                    PX(4), PX(2));
    else
        dl->AddRect(a, b, IM_COL32(0x2A, 0x2A, 0x2A, 0xFF), PX(4));

    // Dropping tracks from the table on the panel loads the first.
    ImGui::SetCursorScreenPos(a);
    ImGui::Dummy(ImVec2(0, 0));
    ImRect r(a, b);
    if (ImGui::BeginDragDropTargetCustom(r, ImGui::GetID("##drop"))) {
        std::vector<uint64_t> ids;
        if (ui_accept_tracks(ui, ids) && !ids.empty())
            ui_load(ui, rx_library_find_id(&app->lib, ids[0]));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void ui_deck_shutdown(UiState *ui)
{
    for (int i = 0; i < RX_DECKS; i++) {
        free(ui->decks[i].beats);
        ui->decks[i].beats = nullptr;
        ui->decks[i].nb_beats = 0;
    }
}
