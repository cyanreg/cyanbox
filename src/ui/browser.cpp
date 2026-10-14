/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The browser's right side: the search field, the track table and the
// INFO panel, always shown beside the table. The table is one ImGui table
// over app->rows (the view's library indices, filtered and sorted by
// app.c), virtualised with the clipper so a few thousand tracks cost only
// the visible rows. Columns
// follow rekordbox's (rbxport src/lib/columns.ts: widths, the fixed `#`),
// and their order, widths and visibility persist with the rest of the
// layout. Sorting writes app->sort_column and lets app.c rebuild the rows;
// `#` is the view's own order (sort_column -1, descending reverses it),
// and each row's number its place in that order (app->row_numbers): a
// track keeps its number whatever the sort and the search.
//
// Selection is rekordbox's: click selects one, Ctrl toggles, Shift selects
// the range from the last click; the focused row (the keyboard's, Enter
// loads it) has a 1 px outline, the player's track a 1.5 px orange one,
// wherever the focus is (and only that one when the focus is on it), and
// dark amber for the selection's blue. A double-click loads the track into the
// player (a playing one is paused and replaced). Rows drag as a list of
// track ids onto the player or a playlist.

#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

enum UiCol {
    COL_IDX = 0, COL_PREVIEW, COL_ART, COL_TITLE, COL_KEY, COL_BPM, COL_TIME, COL_RATING,
    COL_ARTIST, COL_YEAR, COL_ALBUM, COL_PATH, COL_PLAYS, COL_LAST_PLAYED, COL_N,
};

#define NO_SORT (-2)

struct UiColSpec {
    const char *name;
    float width;            // fixed width, or the stretch weight
    int   sort;             // RXColumn, -1 = the view's order, NO_SORT
    ImGuiTableColumnFlags flags;
    bool  right;            // right-aligned (numbers)
};

static const ImGuiTableColumnFlags FIXED = ImGuiTableColumnFlags_WidthFixed;
static const ImGuiTableColumnFlags STRETCH = ImGuiTableColumnFlags_WidthStretch;
static const ImGuiTableColumnFlags HIDDEN = ImGuiTableColumnFlags_DefaultHide;
static const ImGuiTableColumnFlags UNSORTED = ImGuiTableColumnFlags_NoSort;
static const ImGuiTableColumnFlags ALWAYS = ImGuiTableColumnFlags_NoHide;

// Widths are the cells' content, 5 px of cell padding either side. Track
// Title and Artist are fixed (260 and 170, resizable): stretched they ran
// far wider than their text. The row preview stretches over what the
// others leave instead, at least about 100 px (table()), so a wider window
// shows a wider waveform. The artwork cell is rbxport's 80 (70 and the
// padding: src/lib/columns.ts). `#` is the view's own order and cannot be
// hidden, nor can the title. There is no colour, genre or hot cue column:
// the info panel beside the table shows and edits colour and genre, the
// player's cue list the cues. Nor is there a Date Added column (the
// library still keeps the date).
static const UiColSpec COLS[COL_N] = {
    { "#",            40, -1,            FIXED | ALWAYS | ImGuiTableColumnFlags_DefaultSort, true },
    { "Preview",       1, NO_SORT,       STRETCH | UNSORTED, false },
    { "Artwork",      70, NO_SORT,       FIXED | UNSORTED, false },
    { "Track Title", 330, RX_COL_TITLE,  FIXED | ALWAYS, false },
    { "Key",          52, RX_COL_KEY,    FIXED, false },
    { "BPM",          62, RX_COL_BPM,    FIXED, true },
    { "Time",         52, RX_COL_TIME,   FIXED, true },
    { "Rating",       74, RX_COL_RATING, FIXED, false },
    { "Artist",      170, RX_COL_ARTIST, FIXED, false },
    { "Year",         56, RX_COL_YEAR,   FIXED, false },
    { "Album",       160, RX_COL_ALBUM,  FIXED | HIDDEN, false },
    { "Location",    260, RX_COL_PATH,   FIXED | HIDDEN, false },
    { "Plays",        46, RX_COL_PLAYS,  FIXED | HIDDEN, true },
    { "Last Played",  84, RX_COL_LAST_PLAYED, FIXED | HIDDEN, true },
};

static const char DASH[] = "\xE2\x80\x94";

/* ---------------------------------------------------------------- selection */

bool ui_is_selected(const UiState *ui, uint64_t id)
{
    const RXApp *app = ui->app;
    for (int i = 0; i < app->nb_selection; i++) {
        if (app->selection[i] == id)
            return true;
    }
    return false;
}

static bool selection_reserve(RXApp *app, int n)
{
    uint64_t *s = (uint64_t *)realloc(app->selection, sizeof(uint64_t) * (size_t)ImMax(n, 1));
    if (!s)
        return false;
    app->selection = s;
    return true;
}

void ui_select_only(UiState *ui, uint64_t id)
{
    RXApp *app = ui->app;
    if (!selection_reserve(app, 1))
        return;
    app->selection[0] = id;
    app->nb_selection = 1;
}

static void select_toggle(UiState *ui, uint64_t id)
{
    RXApp *app = ui->app;
    for (int i = 0; i < app->nb_selection; i++) {
        if (app->selection[i] == id) {
            memmove(app->selection + i, app->selection + i + 1,
                    sizeof(uint64_t) * (app->nb_selection - i - 1));
            app->nb_selection--;
            return;
        }
    }
    if (!selection_reserve(app, app->nb_selection + 1))
        return;
    app->selection[app->nb_selection++] = id;
}

static RXTrack *row_track(const RXApp *app, int row)
{
    if (row < 0 || row >= app->nb_rows)
        return nullptr;
    uint32_t idx = app->rows[row];
    return idx < (uint32_t)app->lib.nb_tracks ? app->lib.tracks[idx] : nullptr;
}

static void select_range(UiState *ui, int from, int to)
{
    RXApp *app = ui->app;
    if (from > to)
        ImSwap(from, to);
    from = ImMax(from, 0);
    to = ImMin(to, app->nb_rows - 1);
    if (!selection_reserve(app, to - from + 1))
        return;
    app->nb_selection = 0;
    for (int r = from; r <= to; r++) {
        RXTrack *t = row_track(app, r);
        if (t)
            app->selection[app->nb_selection++] = t->id;
    }
}

static void select_all(UiState *ui)
{
    if (ui->app->nb_rows > 0)
        select_range(ui, 0, ui->app->nb_rows - 1);
}

static int focus_row(const RXApp *app)
{
    for (int r = 0; r < app->nb_rows; r++) {
        RXTrack *t = row_track(app, r);
        if (t && t->id == app->focus_track)
            return r;
    }
    return -1;
}

// Up/down move the focus and the selection with it, as rekordbox's list does.
void ui_move_focus(UiState *ui, int delta)
{
    RXApp *app = ui->app;
    if (app->nb_rows <= 0)
        return;
    int r = focus_row(app);
    r = r < 0 ? 0 : ImClamp(r + delta, 0, app->nb_rows - 1);
    RXTrack *t = row_track(app, r);
    if (!t)
        return;
    app->focus_track = t->id;
    ui_select_only(ui, t->id);
    ui->anchor_row = r;
    ui->scroll_to_focus = true;
}

RXTrack *ui_focus_track(UiState *ui)
{
    return ui->app->focus_track ? rx_library_find_id(&ui->app->lib, ui->app->focus_track) : nullptr;
}

// Delete: the selected tracks leave the playlist shown (never the library).
void ui_remove_from_playlist(UiState *ui)
{
    RXApp *app = ui->app;
    RXPlaylist *p = app->current_playlist;
    if (!p || app->nb_selection == 0)
        return;
    int removed = 0;
    for (int s = 0; s < app->nb_selection; s++) {
        for (int i = p->nb - 1; i >= 0; i--) {
            if (p->track_ids[i] != app->selection[s])
                continue;
            if (rx_library_playlist_remove_at(&app->lib, p, i) >= 0) {
                removed++;
                break;
            }
        }
    }
    app->nb_selection = 0;
    rx_app_rebuild_rows(app);
    rx_app_set_status(app, "Removed %d track%s from %s", removed, removed == 1 ? "" : "s", p->name);
}

bool ui_accept_tracks(UiState *ui, std::vector<uint64_t> &ids)
{
    const ImGuiPayload *pl = ImGui::AcceptDragDropPayload(UI_DND_TRACKS);
    if (!pl || !pl->Data || pl->DataSize < (int)sizeof(uint64_t))
        return false;
    int n = pl->DataSize / (int)sizeof(uint64_t);
    ids.resize((size_t)n);
    memcpy(ids.data(), pl->Data, sizeof(uint64_t) * (size_t)n);
    return true;
}

/* ------------------------------------------------------------- context menu */

static void analyse_selection(UiState *ui, unsigned what, bool force)
{
    RXApp *app = ui->app;
    if (rx_app_analyse_tracks(app, app->selection, app->nb_selection, what, force) <= 0)
        rx_app_set_status(app, "Nothing to analyse");
}

// The playlists under `f` as menu items, folders as submenus; the one
// picked, or NULL.
static RXPlaylist *playlist_menu(RXPlaylistFolder *f)
{
    RXPlaylist *picked = nullptr;
    for (int i = 0; i < f->nb_folders; i++) {
        ImGui::PushID(i);
        if (ImGui::BeginMenu(f->folders[i]->name)) {
            RXPlaylist *p = playlist_menu(f->folders[i]);
            if (p)
                picked = p;
            ImGui::EndMenu();
        }
        ImGui::PopID();
    }
    for (int i = 0; i < f->nb_playlists; i++) {
        ImGui::PushID(f->nb_folders + i);
        if (ImGui::MenuItem(f->playlists[i]->name))
            picked = f->playlists[i];
        ImGui::PopID();
    }
    if (!f->nb_folders && !f->nb_playlists)
        ImGui::TextDisabled("No playlists");
    return picked;
}

// The tracks a menu item acts on, as Add to playlist takes them: the
// selection when the clicked track is in it, else that track alone.
static std::vector<uint64_t> menu_tracks(UiState *ui, const RXTrack *t)
{
    RXApp *app = ui->app;
    std::vector<uint64_t> ids;
    if (ui_is_selected(ui, t->id))
        ids.assign(app->selection, app->selection + app->nb_selection);
    else
        ids.push_back(t->id);
    return ids;
}

static void track_menu(UiState *ui)
{
    RXApp *app = ui->app;
    RXTrack *t = ui_focus_track(ui);
    if (!t)
        return;
    int n = app->nb_selection;
    if (ImGui::BeginMenu("Analyse", !t->missing)) {
        if (ImGui::MenuItem("BPM and grid"))
            analyse_selection(ui, RX_ANALYSE_BEATS | RX_ANALYSE_WAVES, false);
        if (ImGui::MenuItem("Key"))
            analyse_selection(ui, RX_ANALYSE_KEY, false);
        if (ImGui::MenuItem("Both"))
            analyse_selection(ui, RX_ANALYSE_ALL, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Force re-analysis"))
            analyse_selection(ui, RX_ANALYSE_ALL, true);
        ImGui::EndMenu();
    }
    // The model listens to each track for its genre, colour and comment:
    // seconds a track on the GPU, so only ever when asked for here.
    if (ImGui::MenuItem("Analyse tags", nullptr, false, !t->missing)) {
        std::vector<uint64_t> ids = menu_tracks(ui, t);
        rx_app_tag_tracks(app, ids.data(), (int)ids.size());
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Set colour")) {
        // The info panel's swatches; every selected track takes the one
        // clicked. The outline is the focused track's colour.
        double hue = t->meta.hue;
        if (ui_colour_picker(ui, "##menucolour", &hue, 220)) {
            for (int i = 0; i < app->nb_selection; i++) {
                RXTrack *s = rx_library_find_id(&app->lib, app->selection[i]);
                if (!s)
                    continue;
                s->meta.hue = hue;
                rx_app_save_track(app, s);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Set rating")) {
        for (int r = 0; r <= 5; r++) {
            char label[16] = "None";
            if (r)
                snprintf(label, sizeof(label), "%d star%s", r, r == 1 ? "" : "s");
            if (ImGui::MenuItem(label, nullptr, n == 1 && t->meta.rating == r)) {
                for (int i = 0; i < app->nb_selection; i++) {
                    RXTrack *s = rx_library_find_id(&app->lib, app->selection[i]);
                    if (!s)
                        continue;
                    s->meta.rating = r;
                    rx_app_save_track(app, s);
                }
            }
        }
        ImGui::EndMenu();
    }
    if (t->missing && ImGui::MenuItem("Relocate..."))
        ui_open_relocate(ui, t);
    ImGui::Separator();
    // Every playlist, folders as submenus, in the tree's order.
    if (ImGui::BeginMenu("Add to playlist", app->lib.root_folder != nullptr)) {
        std::vector<uint64_t> ids = menu_tracks(ui, t);
        if (RXPlaylist *p = playlist_menu(app->lib.root_folder))
            ui_add_to_playlist(ui, p, ids);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Remove from playlist", "Delete", false, app->current_playlist != nullptr))
        ui_remove_from_playlist(ui);
    if (ImGui::MenuItem("Remove from library...")) {
        ui->remove_ids.assign(app->selection, app->selection + app->nb_selection);
        ui->want_confirm_remove = true;
    }
}

/* -------------------------------------------------------------------- cells */

static void cell_text(UiState *ui, ImVec2 p, float w, float row_h, const char *text, ImU32 col,
                      bool right, ImFont *font = nullptr, float size = 18.0f)
{
    if (!text || !text[0])
        return;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float y = p.y + (row_h - PX(size)) * 0.5f - PX(1);
    if (right) {
        float tw = ui_text_width(ui, font, size, text);
        ui_text(ui, dl, font, size, ImVec2(p.x + ImMax(w - tw, 0.0f), y), col, text, w);
    } else {
        ui_text(ui, dl, font, size, ImVec2(p.x, y), col, text, w);
    }
}

static void fmt_date(int64_t epoch, char *buf, size_t size)
{
    if (epoch <= 0) {
        buf[0] = 0;
        return;
    }
    time_t tt = (time_t)epoch;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(buf, size, "%Y-%m-%d", &tm);
}

static void warning_glyph(ImDrawList *dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y + r * 0.8f),
                          ImVec2(c.x - r, c.y + r * 0.8f), col);
    dl->AddLine(ImVec2(c.x, c.y - r * 0.35f), ImVec2(c.x, c.y + r * 0.3f), UI_BLACK,
                ImMax(1.0f, r * 0.25f));
    dl->AddRectFilled(ImVec2(c.x - r * 0.12f, c.y + r * 0.45f),
                      ImVec2(c.x + r * 0.12f, c.y + r * 0.65f), UI_BLACK);
}

// Five stars; a click sets the rating, the same star again clears it.
static void rating_cell(UiState *ui, RXTrack *t, ImVec2 p, float w, float row_h, bool dim)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = PX(5.5f), step = PX(13.5f);
    float cy = p.y + row_h * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(p.x, cy - PX(8)));
    ImGui::InvisibleButton("##rating", ImVec2(ImMin(step * 5, w), PX(16)));
    bool hot = ImGui::IsItemHovered();
    int hover = hot ? ImClamp((int)((ImGui::GetIO().MousePos.x - p.x) / step) + 1, 1, 5) : 0;
    if (ImGui::IsItemClicked()) {
        t->meta.rating = t->meta.rating == hover ? 0 : hover;
        rx_app_save_track(ui->app, t);
    }
    int shown = hover ? hover : t->meta.rating;
    for (int i = 0; i < 5; i++) {
        ImVec2 c(p.x + step * i + r + PX(1), cy);
        ImU32 col = IM_COL32(0x3A, 0x3A, 0x3A, 0xFF);
        if (i < shown)
            col = hover ? UI_ACCENT : dim ? UI_DIM : UI_TEXT;
        ui_star(dl, c, r, col);
    }
}

static void row_cells(UiState *ui, RXTrack *t, int row, float row_h, const uint64_t *active,
                      int nb_active)
{
    RXApp *app = ui->app;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool dim = t->missing;
    ImU32 text = dim ? UI_FAINT : UI_TEXT;
    ImU32 sub = dim ? UI_FAINT : UI_DIM;
    char buf[160];
    // The Traffic Light's reference: the loaded track's key.
    const RXTrack *loaded = app->decks[0].track;
    const char *ref = loaded && loaded->key[0] ? loaded->key : nullptr;

    for (int c = 0; c < COL_N; c++) {
        if (!ImGui::TableSetColumnIndex(c))
            continue;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        switch (c) {
        case COL_IDX:
            // The row's Selectable is submitted here by the caller. The
            // number is the track's place in the view's own order, so it
            // stays with the track whatever the sort and the search.
            snprintf(buf, sizeof(buf), "%u", app->row_numbers ? app->row_numbers[row] : (unsigned)row + 1);
            cell_text(ui, p, w, row_h, buf, sub, true, nullptr, 15.0f);
            break;
        case COL_PREVIEW: {
            bool analysing = false;
            for (int i = 0; i < nb_active; i++)
                analysing |= active[i] == t->id;
            ui_row_preview(ui, t, ImVec2(p.x, p.y + PX(2)), ImVec2(p.x + w, p.y + row_h - PX(2)),
                           analysing);
            break;
        }
        case COL_ART: {
            // Every visible row asks: the library may hold a thumbnail for a
            // track whose flag is off (a folder image found later); app.c
            // looks once per track and reads the file only when the flag
            // says there is art.
            //
            // The sleeve fills the whole cell, padding included, cropped as
            // rbxport crops it (src/views/browser/TrackTable.module.css,
            // .artworkImage: object-fit cover, object-position center top):
            // scaled to cover the cell, the full width kept and the bottom
            // cut off, so the top of the cover shows. The thumbnail is
            // square (app.c makes it so), which the UVs assume. Without
            // art, the record square of the column's width, anchored at the
            // top and cut by the row the same way (.record there).
            float pad = ImGui::GetStyle().CellPadding.x;
            ImVec2 a(p.x - pad, p.y), b(p.x + w + pad, p.y + row_h);
            float cw = b.x - a.x, ch = b.y - a.y;
            uint64_t tex = rx_app_artwork_texture(app, t);
            if (tex && cw > 0 && ch > 0) {
                ImVec2 uv0(0, 0), uv1(1, 1);
                if (cw >= ch) {
                    uv1.y = ch / cw;
                } else {
                    uv0.x = 0.5f - cw / ch * 0.5f;
                    uv1.x = 0.5f + cw / ch * 0.5f;
                }
                dl->AddImage(ImTextureRef((ImTextureID)tex), a, b, uv0, uv1,
                             dim ? IM_COL32(0xFF, 0xFF, 0xFF, 0x60) : IM_COL32_WHITE);
            } else if (cw > 0 && ch > 0) {
                dl->PushClipRect(a, b, true);
                ui_record_icon(dl, a, ImVec2(b.x, a.y + cw));
                dl->PopClipRect();
            }
            if (!tex && rx_app_artwork_pending(app, t))
                ui->previews_pending = true;
            break;
        }
        case COL_TITLE: {
            float x = p.x;
            if (t->missing) {
                warning_glyph(dl, ImVec2(x + PX(6), p.y + row_h * 0.5f), PX(5.5f), UI_ACCENT);
                x += PX(15);
                if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + PX(14), p.y + row_h)))
                    ImGui::SetTooltip("File missing: %s\nRelocate it from the context menu.", t->path);
            }
            cell_text(ui, ImVec2(x, p.y), w - (x - p.x), row_h, ui_title(t), text, false, nullptr,
                      19.0f);
            break;
        }
        case COL_KEY: {
            // rekordbox's Traffic Light: the cell of a key that mixes with
            // the loaded track's is filled green with black text, its own
            // row's too, selected or not (rbxport's colour and rule,
            // src/views/browser/TrackTable.module.css, .cell[data-lit]).
            // The fill covers the padding: the cell, not its text, is lit.
            const char *k = ui_key_text(ui, t->key, buf, sizeof(buf));
            ImU32 col = k[0] ? text : UI_FAINT;
            if (k[0] && ref && !dim && rx_key_traffic_light(t->key, ref, (RXKeyReach)ui->traffic_light)) {
                float pad = ImGui::GetStyle().CellPadding.x;
                dl->AddRectFilled(ImVec2(p.x - pad, p.y), ImVec2(p.x + w + pad, p.y + row_h), UI_QUANTIZE);
                col = UI_BLACK;
            }
            cell_text(ui, p, w, row_h, k[0] ? k : DASH, col, false);
            break;
        }
        case COL_BPM: {
            // A dynamic grid's typical tempo (the stick's), else the first section's.
            double bpm = rx_grid_dynamic(&t->grid) ? rx_grid_bpm_median(&t->grid, t->duration)
                                                   : rx_grid_bpm(&t->grid);
            if (bpm > 0)
                snprintf(buf, sizeof(buf), "%.2f", bpm);
            cell_text(ui, p, w, row_h, bpm > 0 ? buf : DASH, bpm > 0 ? text : UI_FAINT, true);
            break;
        }
        case COL_TIME:
            if (t->duration > 0)
                cell_text(ui, p, w, row_h, ui_fmt_time(t->duration, buf, sizeof(buf), false), text, true);
            break;
        case COL_RATING:
            rating_cell(ui, t, p, w, row_h, dim);
            break;
        case COL_ARTIST:
            cell_text(ui, p, w, row_h, ui_artist(t), text, false);
            break;
        case COL_YEAR:
            // Left-aligned under its header, with air to its right.
            if (t->meta.year > 0) {
                snprintf(buf, sizeof(buf), "%d", t->meta.year);
                cell_text(ui, p, w, row_h, buf, text, false);
            }
            break;
        case COL_ALBUM:
            cell_text(ui, p, w, row_h, t->meta.album, text, false);
            break;
        case COL_PATH:
            cell_text(ui, p, w, row_h, t->path, sub, false, nullptr, 15.0f);
            break;
        case COL_PLAYS:
            if (t->play_count > 0) {
                snprintf(buf, sizeof(buf), "%d", t->play_count);
                cell_text(ui, p, w, row_h, buf, text, true);
            }
            break;
        case COL_LAST_PLAYED:
            fmt_date(t->last_played, buf, sizeof(buf));
            cell_text(ui, p, w, row_h, buf, sub, true, nullptr, 15.0f);
            break;
        }
    }
}

/* -------------------------------------------------------------------- table */

static void table(UiState *ui, ImVec2 size)
{
    RXApp *app = ui->app;
    ImGuiIO &io = ImGui::GetIO();
    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                            ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    ImGuiID menu_id = ImGui::GetID("##trackmenu");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(PX(5), 0));
    // The preview stretches over what the fixed columns leave; below the
    // width where it would keep less than 100 px the table scrolls sideways
    // instead, as rekordbox's does. The default columns take about 930 px,
    // so at 1600 wide, info panel and all, nothing scrolls.
    float fixed_w = 100;
    for (int c = 0; c < COL_N; c++) {
        if (!(COLS[c].flags & (HIDDEN | STRETCH)))
            fixed_w += COLS[c].width;
        if (!(COLS[c].flags & HIDDEN))
            fixed_w += 5 * 2 + 1;
    }
    float inner = ImMax(size.x - ImGui::GetStyle().ScrollbarSize, PX(fixed_w));
    // A new id with the column set (no Date Added; Title and Artist fixed,
    // the preview stretched, the artwork wider): a layout saved for the old
    // one would have kept the 40 px artwork and the old widths.
    if (!ImGui::BeginTable("##tracklist4", COL_N, flags, size, inner)) {
        ImGui::PopStyleVar();
        return;
    }
    for (int c = 0; c < COL_N; c++) {
        // A stretch column's "width" is its weight.
        float w = COLS[c].flags & STRETCH ? COLS[c].width : PX(COLS[c].width);
        ImGui::TableSetupColumn(COLS[c].name, COLS[c].flags, w, (ImGuiID)c);
    }
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs()) {
        if (ss->SpecsDirty) {
            if (ss->SpecsCount > 0) {
                int uid = (int)ss->Specs[0].ColumnUserID;
                app->sort_column = uid >= 0 && uid < COL_N ? COLS[uid].sort : -1;
                app->sort_descending = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            } else {
                app->sort_column = -1;
                app->sort_descending = false;
            }
            rx_app_rebuild_rows(app);
            ss->SpecsDirty = false;
        }
    }

    // Rekordbox: 40 px rows when the artwork column is shown, 22 otherwise.
    bool art = ImGui::TableGetColumnFlags(COL_ART) & ImGuiTableColumnFlags_IsEnabled;
    float row_h = art ? PX(40) : PX(22);
    uint64_t active[32];
    int nb_active = app->jobs ? rx_jobs_active_tracks(app->jobs, active, 32) : 0;
    int frow = ui->scroll_to_focus ? focus_row(app) : -1;
    ImRect clip = ImGui::GetCurrentWindow()->InnerClipRect;
    const RXTrack *loaded = app->decks[0].track;
    float outline = ImMax(1.0f, PX(1.5f));

    ImGuiListClipper clipper;
    clipper.Begin(app->nb_rows, row_h);
    if (frow >= 0)
        clipper.IncludeItemByIndex(frow);
    int clicked_row = -1;
    bool double_clicked = false;
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
            RXTrack *t = row_track(app, r);
            if (!t)
                continue;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
            ImGui::PushID((void *)(uintptr_t)t->id);
            ImGui::TableSetColumnIndex(COL_IDX);
            bool sel = ui_is_selected(ui, t->id);
            bool in_player = loaded && t->id == loaded->id;
            ImVec2 rp = ImGui::GetCursorScreenPos();
            // Without the half item spacing a Selectable adds above and
            // below: rows tile with no gap anyway, and with it a row's fill
            // and outline reach 2 px into its neighbours (a selected row
            // under the player's track covered the bottom of its outline).
            ImGuiSelectableFlags sf = ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap |
                                      ImGuiSelectableFlags_AllowDoubleClick |
                                      ImGuiSelectableFlags_NoPadWithHalfSpacing;
            // The player's track selected: a dark amber under its orange
            // outline, not the selection's blue, which clashes with it.
            if (in_player) {
                ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0x4A, 0x33, 0x16, 0xFF));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0x5E, 0x40, 0x1A, 0xFF));
            }
            if (ImGui::Selectable("##row", sel, sf, ImVec2(0, row_h))) {
                clicked_row = r;
                double_clicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            }
            if (in_player)
                ImGui::PopStyleColor(2);
            ImRect row_rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                if (!sel)
                    ui_select_only(ui, t->id);
                app->focus_track = t->id;
                ui->anchor_row = r;
                ImGui::OpenPopup(menu_id);
            }
            if (ImGui::BeginDragDropSource()) {
                std::vector<uint64_t> ids;
                if (sel)
                    ids.assign(app->selection, app->selection + app->nb_selection);
                else
                    ids.push_back(t->id);
                ImGui::SetDragDropPayload(UI_DND_TRACKS, ids.data(), sizeof(uint64_t) * ids.size());
                if (ids.size() == 1) {
                    char name[200];
                    ImGui::TextUnformatted(rx_track_display(t, name, sizeof(name)));
                } else {
                    ImGui::Text("%d tracks", (int)ids.size());
                }
                ImGui::EndDragDropSource();
            }
            if (r == frow) {
                if (row_rect.Min.y < clip.Min.y + row_h || row_rect.Max.y > clip.Max.y)
                    ImGui::SetScrollHereY(0.5f);
                ui->scroll_to_focus = false;
            }
            ImGui::SetCursorScreenPos(rp);
            row_cells(ui, t, r, row_h, active, nb_active);
            // Over the visible width, so a table scrolled sideways still
            // shows them: the player's track 1.5 px orange (nested 1 px
            // rectangles, crisp where a thick line would blur, the half
            // pixel an inner one at half strength), else the focus 1 px
            // grey. The focus on the player's track is its outline alone:
            // a grey line inside the orange is a light seam between it and
            // the fill, and no other row has the focus's outline then.
            if (in_player || t->id == app->focus_track) {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(clip.Min, clip.Max, false);
                // AddRect strokes the pixels from a to b less one: on the
                // fill's edge, not a pixel inside it.
                ImVec2 a(clip.Min.x, row_rect.Min.y), b(clip.Max.x, row_rect.Max.y);
                for (int i = 0; in_player && i < outline; i++) {
                    dl->AddRect(a, b, ui_alpha(UI_ACCENT, ImMin(outline - i, 1.0f)));
                    a = ImVec2(a.x + 1, a.y + 1);
                    b = ImVec2(b.x - 1, b.y - 1);
                }
                if (!in_player)
                    dl->AddRect(a, b, ui_alpha(UI_TEXT, 0.55f));
                dl->PopClipRect();
            }
            ImGui::PopID();
        }
    }
    if (frow < 0)
        ui->scroll_to_focus = false;

    if (clicked_row >= 0) {
        RXTrack *t = row_track(app, clicked_row);
        if (t) {
            if (double_clicked) {
                ui_load(ui, t);
            } else if (io.KeyCtrl) {
                select_toggle(ui, t->id);
                ui->anchor_row = clicked_row;
            } else if (io.KeyShift && ui->anchor_row >= 0) {
                select_range(ui, ui->anchor_row, clicked_row);
            } else {
                ui_select_only(ui, t->id);
                ui->anchor_row = clicked_row;
            }
            app->focus_track = t->id;
        }
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))
        select_all(ui);
    // A click anywhere in the table, rows or header, takes the keyboard
    // back from the player's cue list: Delete is the playlist's again.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        ui_cue_list_release(ui, 0);

    ImGui::EndTable();
    ImGui::PopStyleVar();
    ImGuiWindowFlags pf = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                          ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupEx(menu_id, pf)) {
        track_menu(ui);
        ImGui::EndPopup();
    }
}

void ui_browser(UiState *ui, ImVec2 size)
{
    RXApp *app = ui->app;
    ImGui::BeginChild("##browser", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetWindowPos();
    dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), UI_PANEL, PX(4));

    // Header: the view and its count, the search field.
    float hh = PX(30);
    ImGui::SetCursorScreenPos(ImVec2(a.x + PX(8), a.y));
    const char *view = app->current_playlist ? app->current_playlist->name
                     : !app->current_album   ? "Collection"
                     : app->current_album[0] ? app->current_album
                                             : "None";
    char count[64];
    snprintf(count, sizeof(count), "%d track%s", app->nb_rows, app->nb_rows == 1 ? "" : "s");
    float ty = a.y + (hh - PX(13.5f)) * 0.5f - PX(1);
    ui_text(ui, dl, ui->font_bold, 13.5f, ImVec2(a.x + PX(10), ty), UI_TEXT, view, size.x * 0.3f);
    float vw = ImMin(ui_text_width(ui, ui->font_bold, 13.5f, view), size.x * 0.3f);
    ui_text(ui, dl, nullptr, 12.0f, ImVec2(a.x + PX(18) + vw, ty + PX(1)), UI_DIM, count);

    float sw = ImClamp(size.x * 0.32f, PX(160), PX(360));
    float sx = a.x + size.x - PX(8) - sw;
    ImGui::SetCursorScreenPos(ImVec2(sx, a.y + PX(4)));
    ImGui::SetNextItemWidth(sw);
    if (ui->focus_search) {
        ImGui::SetKeyboardFocusHere();
        ui->focus_search = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(PX(8), PX(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, PX(11));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0x10, 0x10, 0x10, 0xFF));
    // Esc clears the search (and a second Esc leaves the field).
    if (ImGui::InputTextWithHint("##search", "Search  (Ctrl+F)", app->search, sizeof(app->search),
                                 ImGuiInputTextFlags_EscapeClearsAll))
        rx_app_rebuild_rows(app);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (app->search[0]) {
        ImVec2 ip = ImGui::GetItemRectMax();
        ImVec2 cp(ip.x - PX(20), a.y + PX(7));
        ImGui::SetCursorScreenPos(cp);
        UiButton cb = {};
        cb.icon = UI_ICON_CROSS;
        cb.fill = IM_COL32(0x10, 0x10, 0x10, 0xFF);
        if (ui_button(ui, "clearsearch", ImVec2(PX(16), PX(16)), cb)) {
            app->search[0] = 0;
            rx_app_rebuild_rows(app);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + hh));
    table(ui, ImVec2(size.x, size.y - hh));
    if (app->nb_rows == 0) {
        const char *msg = "The library is empty: File > Add files/folders...";
        if (app->search[0])
            msg = "No tracks match the search.";
        else if (app->current_playlist)
            msg = "This playlist is empty: drag tracks here from the Collection.";
        ui_text_centre(ui, dl, nullptr, 13.0f, ImVec2(a.x, a.y + hh + PX(30)),
                       ImVec2(a.x + size.x, a.y + hh + PX(90)), UI_FAINT, msg);
    }
    ImGui::EndChild();
}

/* --------------------------------------------------------------- info panel */

enum InfoField {
    F_TITLE, F_ARTIST, F_ALBUM, F_ALBUM_ARTIST, F_GENRE, F_LABEL, F_REMIXER, F_COMPOSER,
    F_ORIGINAL, F_COMMENT, F_YEAR, F_TRACK, F_DISC, F_NB,
};

static const char *const FIELD_NAMES[F_NB] = {
    "Title", "Artist", "Album", "Album artist", "Genre", "Label", "Remixer", "Composer",
    "Original artist", "Comment", "Year", "Track", "Disc",
};

static char **meta_field(RXMeta *m, int f)
{
    switch (f) {
    case F_TITLE:           return &m->title;
    case F_ARTIST:          return &m->artist;
    case F_ALBUM:           return &m->album;
    case F_ALBUM_ARTIST:    return &m->album_artist;
    case F_GENRE:           return &m->genre;
    case F_LABEL:           return &m->label;
    case F_REMIXER:         return &m->remixer;
    case F_COMPOSER:        return &m->composer;
    case F_ORIGINAL:        return &m->original_artist;
    case F_COMMENT:         return &m->comment;
    default:                return nullptr;
    }
}

static int *meta_int(RXMeta *m, int f)
{
    switch (f) {
    case F_YEAR:            return &m->year;
    case F_TRACK:           return &m->track_number;
    case F_DISC:            return &m->disc_number;
    default:                return nullptr;
    }
}

static void info_load(UiState *ui, RXTrack *t)
{
    ui->info_track = t->id;
    ui->info_generation = ui->app->lib.generation;
    for (int f = 0; f < F_NB; f++) {
        char **s = meta_field(&t->meta, f);
        int *n = meta_int(&t->meta, f);
        if (s)
            ui->info_buf[f] = *s ? *s : "";
        else if (n && *n > 0)
            ui->info_buf[f] = std::to_string(*n);
        else
            ui->info_buf[f].clear();
    }
    std::string tags;
    for (int i = 0; i < t->meta.nb_tags; i++) {
        if (i)
            tags += ", ";
        tags += t->meta.tags[i];
    }
    ui->info_tags = tags;
}

static void info_commit(UiState *ui, RXTrack *t, int f)
{
    char **s = meta_field(&t->meta, f);
    int *n = meta_int(&t->meta, f);
    if (s)
        rx_meta_set(s, ui->info_buf[f].c_str());
    else if (n)
        *n = atoi(ui->info_buf[f].c_str());
    rx_app_save_track(ui->app, t);
}

static void info_commit_tags(UiState *ui, RXTrack *t)
{
    rx_meta_clear_tags(&t->meta);
    const char *p = ui->info_tags.c_str();
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        std::string tag(p, len);
        size_t b = tag.find_first_not_of(" \t"), z = tag.find_last_not_of(" \t");
        if (b != std::string::npos)
            rx_meta_add_tag(&t->meta, tag.substr(b, z - b + 1).c_str());
        if (!e)
            break;
        p = e + 1;
    }
    rx_app_save_track(ui->app, t);
}

// What the model said of the track beyond the genre, colour and comment
// it wrote (src/ml/tag.c): the tagging's version, the colour it picked
// with its probability, in the legend's words, and the runner-up, then
// its measures, 0 to 1. A colour picked with words since edited says so.
static void tags_line(UiState *ui, const RXTrack *t, float label_w)
{
    ImGui::TextDisabled("Tags");
    ImGui::SameLine(label_w);
    if (t->tags_version <= 0) {
        ImGui::TextDisabled("none (Analyse tags)");
        return;
    }
    char pick[256] = "";
    if (!isnan(t->ml_colors[0])) {
        int best = 1;
        for (int i = 2; i <= RX_TRACK_COLORS; i++) {
            if (t->ml_colors[i - 1] > t->ml_colors[best - 1])
                best = i;
        }
        int second = best == 1 ? 2 : 1;
        for (int i = 1; i <= RX_TRACK_COLORS; i++) {
            if (i != best && t->ml_colors[i - 1] > t->ml_colors[second - 1])
                second = i;
        }
        const char *legend[RX_TRACK_COLORS];
        rx_config_legend(&ui->app->lib.cfg, legend);
        bool same = t->ml_legend == rx_ml_legend_hash(legend);
        snprintf(pick, sizeof(pick), " \xC2\xB7 %s %.2f (%s), then %s %.2f",
                 rx_track_color_name(best), t->ml_colors[best - 1],
                 same ? legend[best - 1] : "asked with other words", rx_track_color_name(second),
                 t->ml_colors[second - 1]);
    }
    // A group, so the lines it wraps into start under its first.
    ImGui::BeginGroup();
    ImGui::TextWrapped("v%d%s%s \xC2\xB7 intensity %.2f \xC2\xB7 club %.2f \xC2\xB7 mood %.2f "
                       "\xC2\xB7 %s", t->tags_version,
                       t->tags_version < RX_ML_TAGS_VERSION ? " (older)" : "", pick,
                       t->ml_intensity, t->ml_club, t->ml_valence,
                       t->ml_vocals >= 0.5f ? "vocals" : "instrumental");
    ImGui::EndGroup();
}

static void fact(const char *name, const char *value)
{
    ImGui::TextDisabled("%s", name);
    ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
    ImGui::TextWrapped("%s", value);
}

// A path breaks after any '/' (ImGui wraps only at spaces, so a long path
// ran off the panel), inside a word only when a name alone is too wide;
// the tooltip has it on one line.
static void path_fact(const char *name, const char *path)
{
    ImGui::TextDisabled("%s", name);
    ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
    float avail = ImMax(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 4.0f);
    ImGui::BeginGroup();
    const char *p = path, *end = path + strlen(path);
    while (p < end) {
        const char *cut = p, *slash = nullptr;
        while (cut < end) {
            const char *next = cut + 1;
            while (next < end && ((unsigned char)*next & 0xC0) == 0x80)
                next++;
            if (cut > p && ImGui::CalcTextSize(p, next).x > avail)
                break;
            if (*cut == '/')
                slash = next;
            cut = next;
        }
        if (cut < end && slash)
            cut = slash;
        ImGui::TextUnformatted(p, cut);
        p = cut;
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", path);
}

void ui_info_panel(UiState *ui, ImVec2 size)
{
    RXApp *app = ui->app;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PX(10), PX(8)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, UI_PANEL);
    ImGui::BeginChild("##info", size, ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    // The fields edit the track they were loaded from; a new focus is
    // taken only when no field is being edited, so a commit can never land
    // on the wrong track.
    RXTrack *focus = ui_focus_track(ui);
    RXTrack *t = ui->info_track ? rx_library_find_id(&app->lib, ui->info_track) : nullptr;
    bool editing = ImGui::IsAnyItemActive() && t;
    if (focus && !editing && (t != focus || ui->info_generation != app->lib.generation)) {
        info_load(ui, focus);
        t = focus;
    }
    ImGui::PushFont(ui->font_bold, 0.0f);
    ImGui::TextUnformatted("INFO");
    ImGui::PopFont();
    ImGui::Separator();
    if (!t) {
        ImGui::TextDisabled("Select a track.");
        ImGui::EndChild();
        return;
    }

    float label_w = PX(92);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(PX(5), PX(2)));
    for (int f = 0; f < F_NB; f++) {
        ImGui::PushID(f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", FIELD_NAMES[f]);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGuiInputTextFlags fl = meta_int(&t->meta, f) ? ImGuiInputTextFlags_CharsDecimal : 0;
        ui_input_string("##f", nullptr, &ui->info_buf[f], fl);
        if (ImGui::IsItemDeactivatedAfterEdit())
            info_commit(ui, t, f);
        ImGui::PopID();
        if (f == F_COMMENT)
            tags_line(ui, t, label_w);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("My Tags");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ui_input_string("##tags", "comma separated", &ui->info_tags);
    if (ImGui::IsItemDeactivatedAfterEdit())
        info_commit_tags(ui, t);
    ImGui::PopStyleVar();

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Rating");
    ImGui::SameLine(label_w);
    ImVec2 rp = ImGui::GetCursorScreenPos();
    ImGui::PushID("inforating");
    rating_cell(ui, t, rp, PX(80), ImGui::GetFrameHeight(), false);
    ImGui::PopID();
    // The colour: beside the label a square of it as the track is drawn,
    // its name and the legend's words for it; under them the swatches,
    // which set and save it.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Colour");
    ImGui::SameLine(label_w);
    {
        ImGui::BeginGroup();
        char text[192];
        uint32_t rgb = rx_track_hue_rgb(t->meta.hue);
        if (rgb) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float s = PX(12), fh = ImGui::GetFrameHeight();
            ImVec2 a(p.x, p.y + (fh - s) * 0.5f);
            ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + s, a.y + s), ui_rgb(rgb),
                                                      PX(2));
            ImGui::Dummy(ImVec2(s, fh));
            ImGui::SameLine();
        }
        ImGui::TextWrapped("%s", ui_colour_text(ui, t->meta.hue, text, sizeof(text)));
        double hue = t->meta.hue;
        if (ui_colour_picker(ui, "##infocolour", &hue, 0)) {
            t->meta.hue = hue;
            rx_app_save_track(app, t);
        }
        ImGui::EndGroup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    char buf[96];
    fact("Type", t->codec);
    if (t->file_size > 0) {
        snprintf(buf, sizeof(buf), "%.1f MB", t->file_size / 1048576.0);
        fact("Size", buf);
    }
    if (t->sample_rate > 0) {
        snprintf(buf, sizeof(buf), "%.1f kHz, %d ch", t->sample_rate / 1000.0, t->channels);
        fact("Rate", buf);
    }
    if (t->bit_depth > 0) {
        snprintf(buf, sizeof(buf), "%d bit", t->bit_depth);
        fact("Depth", buf);
    }
    if (t->bit_rate > 0) {
        snprintf(buf, sizeof(buf), "%d kbps", t->bit_rate / 1000);
        fact("Bit rate", buf);
    }
    fact("Duration", ui_fmt_time(t->duration, buf, sizeof(buf), true));
    if (rx_track_analysed(t) && rx_grid_dynamic(&t->grid)) {
        int nm = 0;
        for (int i = 0; i < t->grid.nb_sections; i++)
            nm += t->grid.sections[i].measured;
        snprintf(buf, sizeof(buf), "%.2f BPM, dynamic (%d beats), %.2f",
                 rx_grid_bpm_median(&t->grid, t->duration), nm, t->grid_confidence);
        fact("Grid", buf);
    } else if (rx_track_analysed(t)) {
        snprintf(buf, sizeof(buf), "%.2f BPM, %d section%s, %.2f", rx_grid_bpm(&t->grid),
                 t->grid.nb_sections,
                 t->grid.nb_sections == 1 ? "" : "s", t->grid_confidence);
        fact("Grid", buf);
    }
    // In the players' key display, as the table and the deck show it.
    if (t->key[0])
        fact("Key", ui_key_text(ui, t->key, buf, sizeof(buf)));
    if (t->play_count > 0) {
        char date[32];
        fmt_date(t->last_played, date, sizeof(date));
        snprintf(buf, sizeof(buf), "%d (last %s)", t->play_count, date);
        fact("Plays", buf);
    }
    path_fact("Path", t->path);
    if (t->missing)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT), "The file is missing.");
    ImGui::EndChild();
}
