/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The browser's right side: the search field, the track table and the
// INFO panel. The table is one ImGui table over app->rows (the view's
// library indices, filtered and sorted by app.c), virtualised with the
// clipper so a few thousand tracks cost only the visible rows. Columns
// follow rekordbox's (rbxport src/lib/columns.ts: widths, the fixed `#`),
// and their order, widths and visibility persist with the rest of the
// layout. Sorting writes app->sort_column and lets app.c rebuild the rows;
// `#` is the view's own order (sort_column -1, descending reverses it).
//
// Selection is rekordbox's: click selects one, Ctrl toggles, Shift selects
// the range from the last click; the focused row (the keyboard's, Enter
// loads it) has a 1 px outline. A double-click loads the track into the
// player, never replacing a playing one without asking. Rows drag as a
// list of track ids onto the player or a playlist.

#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

enum UiCol {
    COL_IDX = 0, COL_PREVIEW, COL_ART, COL_TITLE, COL_KEY, COL_BPM, COL_TIME, COL_RATING,
    COL_ARTIST, COL_COLOR, COL_CUES, COL_GENRE, COL_YEAR, COL_ADDED, COL_ALBUM, COL_PATH,
    COL_PLAYS, COL_LAST_PLAYED, COL_N,
};

#define NO_SORT (-2)

struct UiColSpec {
    const char *name;
    float width;            // fixed width, or the stretch weight
    int   sort;             // RxColumn, -1 = the view's order, NO_SORT
    ImGuiTableColumnFlags flags;
    bool  right;            // right-aligned (numbers)
};

static const ImGuiTableColumnFlags FIXED = ImGuiTableColumnFlags_WidthFixed;
static const ImGuiTableColumnFlags STRETCH = ImGuiTableColumnFlags_WidthStretch;
static const ImGuiTableColumnFlags HIDDEN = ImGuiTableColumnFlags_DefaultHide;
static const ImGuiTableColumnFlags UNSORTED = ImGuiTableColumnFlags_NoSort;
static const ImGuiTableColumnFlags ALWAYS = ImGuiTableColumnFlags_NoHide;

// rekordbox's widths where rbxport measured them (Preview 128, Key 73,
// BPM 80, Time 80, Rating 101); Track Title and Artist share the rest.
// `#` is the view's own order and cannot be hidden, nor can the title.
static const UiColSpec COLS[COL_N] = {
    { "#",            40, -1,            FIXED | ALWAYS | ImGuiTableColumnFlags_DefaultSort, true },
    { "Preview",     128, NO_SORT,       FIXED | UNSORTED, false },
    { "Artwork",      40, NO_SORT,       FIXED | UNSORTED, false },
    { "Track Title",   3, RX_COL_TITLE,  STRETCH | ALWAYS, false },
    { "Key",          52, RX_COL_KEY,    FIXED, false },
    { "BPM",          62, RX_COL_BPM,    FIXED, true },
    { "Time",         52, RX_COL_TIME,   FIXED, true },
    { "Rating",       74, RX_COL_RATING, FIXED, false },
    { "Artist",        2, RX_COL_ARTIST, STRETCH, false },
    { "Color",        44, RX_COL_COLOR,  FIXED, false },
    { "Hot Cue",      84, RX_COL_CUES,   FIXED, false },
    { "Genre",       110, RX_COL_GENRE,  FIXED, false },
    { "Year",         44, RX_COL_YEAR,   FIXED, true },
    { "Date Added",   84, RX_COL_ADDED,  FIXED, true },
    { "Album",       160, RX_COL_ALBUM,  FIXED | HIDDEN, false },
    { "Location",    260, RX_COL_PATH,   FIXED | HIDDEN, false },
    { "Plays",        46, RX_COL_PLAYS,  FIXED | HIDDEN, true },
    { "Last Played",  84, RX_COL_LAST_PLAYED, FIXED | HIDDEN, true },
};

static const char DASH[] = "\xE2\x80\x94";

/* ---------------------------------------------------------------- selection */

bool ui_is_selected(const UiState *ui, uint64_t id)
{
    const RxApp *app = ui->app;
    for (int i = 0; i < app->nb_selection; i++) {
        if (app->selection[i] == id)
            return true;
    }
    return false;
}

static bool selection_reserve(RxApp *app, int n)
{
    uint64_t *s = (uint64_t *)realloc(app->selection, sizeof(uint64_t) * (size_t)ImMax(n, 1));
    if (!s)
        return false;
    app->selection = s;
    return true;
}

void ui_select_only(UiState *ui, uint64_t id)
{
    RxApp *app = ui->app;
    if (!selection_reserve(app, 1))
        return;
    app->selection[0] = id;
    app->nb_selection = 1;
}

static void select_toggle(UiState *ui, uint64_t id)
{
    RxApp *app = ui->app;
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

static RxTrack *row_track(const RxApp *app, int row)
{
    if (row < 0 || row >= app->nb_rows)
        return nullptr;
    uint32_t idx = app->rows[row];
    return idx < (uint32_t)app->lib.nb_tracks ? app->lib.tracks[idx] : nullptr;
}

static void select_range(UiState *ui, int from, int to)
{
    RxApp *app = ui->app;
    if (from > to)
        ImSwap(from, to);
    from = ImMax(from, 0);
    to = ImMin(to, app->nb_rows - 1);
    if (!selection_reserve(app, to - from + 1))
        return;
    app->nb_selection = 0;
    for (int r = from; r <= to; r++) {
        RxTrack *t = row_track(app, r);
        if (t)
            app->selection[app->nb_selection++] = t->id;
    }
}

static void select_all(UiState *ui)
{
    if (ui->app->nb_rows > 0)
        select_range(ui, 0, ui->app->nb_rows - 1);
}

static int focus_row(const RxApp *app)
{
    for (int r = 0; r < app->nb_rows; r++) {
        RxTrack *t = row_track(app, r);
        if (t && t->id == app->focus_track)
            return r;
    }
    return -1;
}

// Up/down move the focus and the selection with it, as rekordbox's list does.
void ui_move_focus(UiState *ui, int delta)
{
    RxApp *app = ui->app;
    if (app->nb_rows <= 0)
        return;
    int r = focus_row(app);
    r = r < 0 ? 0 : ImClamp(r + delta, 0, app->nb_rows - 1);
    RxTrack *t = row_track(app, r);
    if (!t)
        return;
    app->focus_track = t->id;
    ui_select_only(ui, t->id);
    ui->anchor_row = r;
    ui->scroll_to_focus = true;
}

RxTrack *ui_focus_track(UiState *ui)
{
    return ui->app->focus_track ? rx_library_find_id(&ui->app->lib, ui->app->focus_track) : nullptr;
}

// Delete: the selected tracks leave the playlist shown (never the library).
void ui_remove_from_playlist(UiState *ui)
{
    RxApp *app = ui->app;
    RxPlaylist *p = app->current_playlist;
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
    RxApp *app = ui->app;
    int queued = 0;
    for (int i = 0; i < app->nb_selection; i++) {
        RxTrack *t = rx_library_find_id(&app->lib, app->selection[i]);
        if (t && !t->missing && rx_app_analyse_track(app, t, what, force) >= 0)
            queued++;
    }
    if (queued == 0)
        rx_app_set_status(app, "Nothing to analyse");
}

static void track_menu(UiState *ui)
{
    RxApp *app = ui->app;
    RxTrack *t = ui_focus_track(ui);
    if (!t)
        return;
    int n = app->nb_selection;
    char head[160];
    if (n > 1)
        snprintf(head, sizeof(head), "%d tracks", n);
    else
        rx_track_display(t, head, sizeof(head));
    ImGui::TextDisabled("%s", head);
    ImGui::Separator();

    if (ImGui::MenuItem("Load", "Enter", false, !t->missing))
        ui_load(ui, t);
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
    ImGui::Separator();
    if (ImGui::BeginMenu("Set colour")) {
        char col[RX_COLOR_MAX];
        if (ui_track_colour_menu(ui, t->meta.color, col)) {
            for (int i = 0; i < app->nb_selection; i++) {
                RxTrack *s = rx_library_find_id(&app->lib, app->selection[i]);
                if (!s)
                    continue;
                snprintf(s->meta.color, sizeof(s->meta.color), "%s", col);
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
                    RxTrack *s = rx_library_find_id(&app->lib, app->selection[i]);
                    if (!s)
                        continue;
                    s->meta.rating = r;
                    rx_app_save_track(app, s);
                }
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Edit info", nullptr, false, n == 1)) {
        ui->show_info = true;
        ImGui::MarkIniSettingsDirty();
    }
    if (ImGui::MenuItem("Show in folder")) {
        // No file manager is assumed: the path goes to the status bar and
        // the clipboard.
        ImGui::SetClipboardText(t->path);
        rx_app_set_status(app, "%s (copied)", t->path);
    }
    if (t->missing && ImGui::MenuItem("Relocate..."))
        ui_open_relocate(ui, t);
    ImGui::Separator();
    if (ImGui::MenuItem("Remove from playlist", "Delete", false, app->current_playlist != nullptr))
        ui_remove_from_playlist(ui);
    if (ImGui::MenuItem("Remove from library...")) {
        ui->remove_ids.assign(app->selection, app->selection + app->nb_selection);
        ui->want_confirm_remove = true;
    }
}

/* -------------------------------------------------------------------- cells */

static void cell_text(UiState *ui, ImVec2 p, float w, float row_h, const char *text, ImU32 col,
                      bool right, ImFont *font = nullptr, float size = 13.0f)
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
static void rating_cell(UiState *ui, RxTrack *t, ImVec2 p, float w, float row_h, bool dim)
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

static void row_cells(UiState *ui, RxTrack *t, int row, float row_h, const uint64_t *active,
                      int nb_active)
{
    RxApp *app = ui->app;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool dim = t->missing;
    ImU32 text = dim ? UI_FAINT : UI_TEXT;
    ImU32 sub = dim ? UI_FAINT : UI_DIM;
    char buf[160];

    for (int c = 0; c < COL_N; c++) {
        if (!ImGui::TableSetColumnIndex(c))
            continue;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        switch (c) {
        case COL_IDX:
            // The row's Selectable is submitted here by the caller.
            snprintf(buf, sizeof(buf), "%d", row + 1);
            cell_text(ui, p, w, row_h, buf, sub, true, nullptr, 12.0f);
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
            float s = ImMin(row_h - PX(4), w);
            ImVec2 a(p.x + (w - s) * 0.5f, p.y + (row_h - s) * 0.5f), b(a.x + s, a.y + s);
            uint64_t tex = t->has_artwork && !t->missing ? rx_app_artwork_texture(app, t) : 0;
            if (tex)
                dl->AddImage(ImTextureRef((ImTextureID)tex), a, b);
            else
                ui_record_icon(dl, a, b);
            if (!tex && t->has_artwork && !t->missing)
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
            cell_text(ui, ImVec2(x, p.y), w - (x - p.x), row_h, ui_title(t), text, false);
            break;
        }
        case COL_KEY: {
            const char *k = ui_key_text(ui, t->key, buf, sizeof(buf));
            cell_text(ui, p, w, row_h, k[0] ? k : DASH, k[0] ? text : UI_FAINT, false);
            break;
        }
        case COL_BPM: {
            double bpm = rx_grid_bpm(&t->grid);
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
        case COL_COLOR: {
            uint32_t rgb = ui_track_rgb(t);
            if (rgb)
                dl->AddCircleFilled(ImVec2(p.x + PX(10), p.y + row_h * 0.5f), PX(5),
                                    ui_rgb(rgb, dim ? 0.4f : 1.0f), 16);
            break;
        }
        case COL_CUES: {
            // The set hot cue letters in their pad colours.
            float x = p.x;
            for (int slot = 0; slot < RX_HOT_CUE_SLOTS; slot++) {
                int idx = rx_track_hot_cue(t, slot);
                if (idx < 0)
                    continue;
                char l[2] = { (char)('A' + slot), 0 };
                ImU32 col = dim ? UI_FAINT : ui_rgb(ui_hot_cue_rgb(&t->cues[idx]));
                if (x + PX(10) > p.x + w)
                    break;
                ui_text(ui, dl, ui->font_bold, 12.0f, ImVec2(x, p.y + (row_h - PX(12)) * 0.5f - PX(1)),
                        col, l);
                x += PX(10);
            }
            break;
        }
        case COL_GENRE:
            cell_text(ui, p, w, row_h, t->meta.genre, text, false);
            break;
        case COL_YEAR:
            if (t->meta.year > 0) {
                snprintf(buf, sizeof(buf), "%d", t->meta.year);
                cell_text(ui, p, w, row_h, buf, text, true);
            }
            break;
        case COL_ADDED:
            fmt_date(t->added, buf, sizeof(buf));
            cell_text(ui, p, w, row_h, buf, sub, true, nullptr, 12.0f);
            break;
        case COL_ALBUM:
            cell_text(ui, p, w, row_h, t->meta.album, text, false);
            break;
        case COL_PATH:
            cell_text(ui, p, w, row_h, t->path, sub, false, nullptr, 12.0f);
            break;
        case COL_PLAYS:
            if (t->play_count > 0) {
                snprintf(buf, sizeof(buf), "%d", t->play_count);
                cell_text(ui, p, w, row_h, buf, text, true);
            }
            break;
        case COL_LAST_PLAYED:
            fmt_date(t->last_played, buf, sizeof(buf));
            cell_text(ui, p, w, row_h, buf, sub, true, nullptr, 12.0f);
            break;
        }
    }
}

/* -------------------------------------------------------------------- table */

static void table(UiState *ui, ImVec2 size)
{
    RxApp *app = ui->app;
    ImGuiIO &io = ImGui::GetIO();
    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                            ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    ImGuiID menu_id = ImGui::GetID("##trackmenu");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(PX(5), 0));
    // Title and Artist stretch over what the fixed columns leave; below a
    // width where they would be squeezed unreadable the table scrolls
    // sideways instead, as rekordbox's does.
    float inner = ImMax(size.x - ImGui::GetStyle().ScrollbarSize, PX(1320));
    if (!ImGui::BeginTable("##tracks", COL_N, flags, size, inner)) {
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

    ImGuiListClipper clipper;
    clipper.Begin(app->nb_rows, row_h);
    if (frow >= 0)
        clipper.IncludeItemByIndex(frow);
    int clicked_row = -1;
    bool double_clicked = false;
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
            RxTrack *t = row_track(app, r);
            if (!t)
                continue;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
            ImGui::PushID((void *)(uintptr_t)t->id);
            ImGui::TableSetColumnIndex(COL_IDX);
            bool sel = ui_is_selected(ui, t->id);
            ImVec2 rp = ImGui::GetCursorScreenPos();
            ImGuiSelectableFlags sf = ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap |
                                      ImGuiSelectableFlags_AllowDoubleClick;
            if (ImGui::Selectable("##row", sel, sf, ImVec2(0, row_h))) {
                clicked_row = r;
                double_clicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            }
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
            if (t->id == app->focus_track) {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(clip.Min, clip.Max, false);
                dl->AddRect(ImVec2(clip.Min.x, row_rect.Min.y),
                            ImVec2(clip.Max.x - 1, row_rect.Max.y - 1), ui_alpha(UI_TEXT, 0.55f));
                dl->PopClipRect();
            }
            ImGui::PopID();
        }
    }
    if (frow < 0)
        ui->scroll_to_focus = false;

    if (clicked_row >= 0) {
        RxTrack *t = row_track(app, clicked_row);
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
    RxApp *app = ui->app;
    ImGui::BeginChild("##browser", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetWindowPos();
    dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), UI_PANEL, PX(4));

    // Header: the view and its count, the search field, INFO.
    float hh = PX(30);
    ImGui::SetCursorScreenPos(ImVec2(a.x + PX(8), a.y));
    const char *view = app->current_playlist ? app->current_playlist->name : "Collection";
    char count[64];
    snprintf(count, sizeof(count), "%d track%s", app->nb_rows, app->nb_rows == 1 ? "" : "s");
    float ty = a.y + (hh - PX(13.5f)) * 0.5f - PX(1);
    ui_text(ui, dl, ui->font_bold, 13.5f, ImVec2(a.x + PX(10), ty), UI_TEXT, view, size.x * 0.3f);
    float vw = ImMin(ui_text_width(ui, ui->font_bold, 13.5f, view), size.x * 0.3f);
    ui_text(ui, dl, nullptr, 12.0f, ImVec2(a.x + PX(18) + vw, ty + PX(1)), UI_DIM, count);

    float info_w = PX(48);
    float sw = ImClamp(size.x * 0.32f, PX(160), PX(360));
    float sx = a.x + size.x - PX(8) - info_w - PX(6) - sw;
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
    ImGui::SetCursorScreenPos(ImVec2(a.x + size.x - PX(8) - info_w, a.y + PX(4)));
    UiButton ib = {};
    ib.label = "INFO";
    ib.lit = ui->show_info;
    ib.font = 10.0f;
    if (ui_button(ui, "info", ImVec2(info_w, hh - PX(8)), ib)) {
        ui->show_info = !ui->show_info;
        ImGui::MarkIniSettingsDirty();
    }
    ui_tooltip("Track information panel");

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

static char **meta_field(RxMeta *m, int f)
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

static int *meta_int(RxMeta *m, int f)
{
    switch (f) {
    case F_YEAR:            return &m->year;
    case F_TRACK:           return &m->track_number;
    case F_DISC:            return &m->disc_number;
    default:                return nullptr;
    }
}

static void info_load(UiState *ui, RxTrack *t)
{
    ui->info_track = t->id;
    ui->info_generation = ui->app->lib.generation;
    for (int f = 0; f < F_NB; f++) {
        char **s = meta_field(&t->meta, f);
        int *n = meta_int(&t->meta, f);
        if (s)
            snprintf(ui->info_buf[f], sizeof(ui->info_buf[f]), "%s", *s ? *s : "");
        else if (n && *n > 0)
            snprintf(ui->info_buf[f], sizeof(ui->info_buf[f]), "%d", *n);
        else
            ui->info_buf[f][0] = 0;
    }
    std::string tags;
    for (int i = 0; i < t->meta.nb_tags; i++) {
        if (i)
            tags += ", ";
        tags += t->meta.tags[i];
    }
    snprintf(ui->info_tags, sizeof(ui->info_tags), "%s", tags.c_str());
}

static void info_commit(UiState *ui, RxTrack *t, int f)
{
    char **s = meta_field(&t->meta, f);
    int *n = meta_int(&t->meta, f);
    if (s)
        rx_meta_set(s, ui->info_buf[f]);
    else if (n)
        *n = atoi(ui->info_buf[f]);
    rx_app_save_track(ui->app, t);
}

static void info_commit_tags(UiState *ui, RxTrack *t)
{
    rx_meta_clear_tags(&t->meta);
    const char *p = ui->info_tags;
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

static void fact(const char *name, const char *value)
{
    ImGui::TextDisabled("%s", name);
    ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
    ImGui::TextWrapped("%s", value);
}

void ui_info_panel(UiState *ui, ImVec2 size)
{
    RxApp *app = ui->app;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PX(10), PX(8)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, UI_PANEL);
    ImGui::BeginChild("##info", size, ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    // The fields edit the track they were loaded from; a new focus is
    // taken only when no field is being edited, so a commit can never land
    // on the wrong track.
    RxTrack *focus = ui_focus_track(ui);
    RxTrack *t = ui->info_track ? rx_library_find_id(&app->lib, ui->info_track) : nullptr;
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
        ImGui::InputText("##f", ui->info_buf[f], sizeof(ui->info_buf[f]), fl);
        if (ImGui::IsItemDeactivatedAfterEdit())
            info_commit(ui, t, f);
        ImGui::PopID();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("My Tags");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##tags", "comma separated", ui->info_tags, sizeof(ui->info_tags));
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
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Colour");
    ImGui::SameLine(label_w);
    char col[RX_COLOR_MAX];
    if (ui_track_colour_menu(ui, t->meta.color, col)) {
        snprintf(t->meta.color, sizeof(t->meta.color), "%s", col);
        rx_app_save_track(app, t);
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
    if (rx_track_analysed(t)) {
        snprintf(buf, sizeof(buf), "%.2f BPM, %d section%s, %.2f", rx_grid_bpm(&t->grid),
                 t->grid.nb_sections,
                 t->grid.nb_sections == 1 ? "" : "s", t->grid_confidence);
        fact("Grid", buf);
    }
    if (t->play_count > 0) {
        char date[32];
        fmt_date(t->last_played, date, sizeof(date));
        snprintf(buf, sizeof(buf), "%d (last %s)", t->play_count, date);
        fact("Plays", buf);
    }
    fact("Path", t->path);
    if (t->missing)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT), "The file is missing.");
    ImGui::EndChild();
}
