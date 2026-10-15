/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The playlist tree on the browser's left: Collection (every track), the
// Albums (every album tag two tracks or more share, by artist and then
// name, each a view of its tracks in disc and track order, after None, the
// tracks of no album or of one no other track has: nothing is added to,
// renamed or exported there), the Album Artists (the artists of Albums,
// each a view of its albums' tracks, the albums as Albums lists them,
// after None), then the library's folders and playlists. A click shows a playlist; the
// context menu creates, renames (in place, as rekordbox does) and deletes
// (after a confirmation). Tracks dragged from the table onto a playlist
// are appended to it; playlists and folders dragged within the tree move
// under another folder (onto a folder: into it, last; onto a playlist:
// beside it), which the library keeps as positions.
//
// Above it, a finder (Ctrl+P): what is typed is matched fuzzily
// (rx_fuzzy_match, as the Add dialog's finder) against Collection, every
// album as "artist/album" and every playlist as "folder/playlist", and
// while it holds text the tree is the list of the matches, best first.
// Up and Down move along it, Enter or a click opens the one picked, as a
// click in the tree would, and the tree comes back with it shown (its
// folders or Albums opened, scrolled to it); Escape empties the field.

#include "ui_internal.h"

#include <stdio.h>

#include <algorithm>

#define UI_DND_NODE "RX_TREE_NODE"

struct TreeNodePayload {
    bool  folder;
    void *ptr;
};

/* ------------------------------------------------------------------ helpers */

static bool folder_contains(const RXPlaylistFolder *f, const void *node)
{
    if (f == node)
        return true;
    for (int i = 0; i < f->nb_playlists; i++) {
        if (f->playlists[i] == node)
            return true;
    }
    for (int i = 0; i < f->nb_folders; i++) {
        if (folder_contains(f->folders[i], node))
            return true;
    }
    return false;
}

static bool name_taken(const RXPlaylistFolder *in, const char *name)
{
    for (int i = 0; i < in->nb_playlists; i++) {
        if (!strcmp(in->playlists[i]->name, name))
            return true;
    }
    for (int i = 0; i < in->nb_folders; i++) {
        if (!strcmp(in->folders[i]->name, name))
            return true;
    }
    return false;
}

static void unique_name(const RXPlaylistFolder *in, const char *base, char *out, size_t size)
{
    snprintf(out, size, "%s", base);
    for (int n = 2; name_taken(in, out) && n < 1000; n++)
        snprintf(out, size, "%s %d", base, n);
}

static void start_rename(UiState *ui, void *target, bool folder, const char *name)
{
    ui->rename_target = target;
    ui->rename_folder = folder;
    ui->rename_focus = true;
    snprintf(ui->rename_buf, sizeof(ui->rename_buf), "%s", name);
}

// New playlists and folders are made after the tree is drawn (they change
// the arrays being walked), then renamed in place.
static void create(UiState *ui, RXPlaylistFolder *in, bool folder)
{
    ui->create_pending = true;
    ui->create_folder = folder;
    ui->create_in = in ? in : ui->app->lib.root_folder;
}

static void create_now(UiState *ui)
{
    RXApp *app = ui->app;
    RXPlaylistFolder *in = ui->create_in;
    ui->create_pending = false;
    if (!in)
        return;
    char name[128];
    unique_name(in, ui->create_folder ? "New Folder" : "New Playlist", name, sizeof(name));
    // The folder it goes in is opened, so the rename field is seen.
    ui->reveal = in;
    if (ui->create_folder) {
        RXPlaylistFolder *f = rx_library_folder_create(&app->lib, in, name);
        if (f)
            start_rename(ui, f, true, f->name);
        else
            rx_app_set_status(app, "Could not create the folder");
    } else {
        RXPlaylist *p = rx_library_playlist_create(&app->lib, in, name);
        if (p)
            start_rename(ui, p, false, p->name);
        else
            rx_app_set_status(app, "Could not create the playlist");
    }
}

static void commit_rename(UiState *ui)
{
    RXApp *app = ui->app;
    if (!ui->rename_target || !ui->rename_buf[0]) {
        ui->rename_target = nullptr;
        return;
    }
    int ret = ui->rename_folder
            ? rx_library_folder_rename(&app->lib, (RXPlaylistFolder *)ui->rename_target, ui->rename_buf)
            : rx_library_playlist_rename(&app->lib, (RXPlaylist *)ui->rename_target, ui->rename_buf);
    if (ret < 0)
        rx_app_set_status(app, "Could not rename: %s", strerror(-ret));
    ui->rename_target = nullptr;
}

void ui_add_to_playlist(UiState *ui, RXPlaylist *p, const std::vector<uint64_t> &ids)
{
    RXApp *app = ui->app;
    int added = 0;
    for (uint64_t id : ids) {
        RXTrack *t = rx_library_find_id(&app->lib, id);
        if (t && rx_library_playlist_add(&app->lib, p, t, -1) >= 0)
            added++;
    }
    if (app->current_playlist == p)
        rx_app_rebuild_rows(app);
    int dup = (int)ids.size() - added;
    if (dup > 0)
        rx_app_set_status(app, "Added %d track%s to %s (%d already in it)", added,
                          added == 1 ? "" : "s", p->name, dup);
    else
        rx_app_set_status(app, "Added %d track%s to %s", added, added == 1 ? "" : "s", p->name);
}

// Moves the dragged node under `to` at `position` (-1: last), once the
// tree has been drawn.
static void reparent(UiState *ui, const TreeNodePayload &n, RXPlaylistFolder *to, int position)
{
    if (n.folder && folder_contains((RXPlaylistFolder *)n.ptr, to)) {
        rx_app_set_status(ui->app, "A folder cannot go inside itself");
        return;
    }
    ui->move_pending = true;
    ui->move_folder = n.folder;
    ui->move_ptr = n.ptr;
    ui->move_to = to;
    ui->move_pos = position;
}

static void reparent_now(UiState *ui)
{
    RXApp *app = ui->app;
    ui->move_pending = false;
    int ret = ui->move_folder
            ? rx_library_folder_reparent(&app->lib, (RXPlaylistFolder *)ui->move_ptr, ui->move_to,
                                         ui->move_pos)
            : rx_library_playlist_reparent(&app->lib, (RXPlaylist *)ui->move_ptr, ui->move_to,
                                           ui->move_pos);
    if (ret < 0)
        rx_app_set_status(app, "Could not move: %s", strerror(-ret));
}

/* -------------------------------------------------------------------- icons */

static void folder_icon(ImDrawList *dl, ImVec2 p, float s, ImU32 col)
{
    dl->AddRectFilled(ImVec2(p.x, p.y + s * 0.18f), ImVec2(p.x + s * 0.45f, p.y + s * 0.32f), col,
                      1.0f);
    dl->AddRectFilled(ImVec2(p.x, p.y + s * 0.28f), ImVec2(p.x + s, p.y + s * 0.88f), col, 1.5f);
}

static void playlist_icon(ImDrawList *dl, ImVec2 p, float s, ImU32 col)
{
    for (int i = 0; i < 3; i++) {
        float y = p.y + s * (0.25f + i * 0.25f);
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + s * 0.14f, y + s * 0.12f), col);
        dl->AddRectFilled(ImVec2(p.x + s * 0.26f, y), ImVec2(p.x + s, y + s * 0.12f), col);
    }
}

static void collection_icon(ImDrawList *dl, ImVec2 p, float s, ImU32 col)
{
    ImVec2 c(p.x + s * 0.5f, p.y + s * 0.55f);
    dl->AddCircle(c, s * 0.42f, col, 20, 1.5f);
    dl->AddCircleFilled(c, s * 0.12f, col, 10);
}

// A sleeve with the record showing at its edge.
static void album_icon(ImDrawList *dl, ImVec2 p, float s, ImU32 col)
{
    dl->AddRect(ImVec2(p.x, p.y + s * 0.15f), ImVec2(p.x + s * 0.8f, p.y + s * 0.95f), col, 1.0f,
                1.5f);
    ImVec2 c(p.x + s * 0.72f, p.y + s * 0.55f);
    dl->AddCircle(c, s * 0.28f, col, 16, 1.5f);
    dl->AddCircleFilled(c, s * 0.08f, col, 8);
}

// A head and shoulders.
static void artist_icon(ImDrawList *dl, ImVec2 p, float s, ImU32 col)
{
    dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.36f), s * 0.2f, col, 12);
    dl->AddRectFilled(ImVec2(p.x + s * 0.14f, p.y + s * 0.64f), ImVec2(p.x + s * 0.86f, p.y + s * 0.95f),
                      col, s * 0.3f, ImDrawFlags_RoundTop);
}

/* -------------------------------------------------------------------- nodes */

// The tree's names, counts and icons (logical pixels).
static const float NAME_PX = 16.0f, COUNT_PX = 12.5f, ICON_PX = 15.0f;

static void draw_icon(ImDrawList *dl, int icon, ImVec2 p, float s, ImU32 col)
{
    if (icon == 0)
        collection_icon(dl, p, s, col);
    else if (icon == 1)
        folder_icon(dl, p, s, col);
    else if (icon == 3)
        album_icon(dl, p, s, col);
    else if (icon == 4)
        artist_icon(dl, p, s, col);
    else
        playlist_icon(dl, p, s, col);
}

// The label drawn over an empty-labelled tree node: icon, name, count.
static void node_label(UiState *ui, ImVec2 a, ImVec2 b, int icon, const char *name, int count,
                       bool selected)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float s = PX(ICON_PX);
    float cy = (a.y + b.y) * 0.5f;
    ImU32 ic = selected ? UI_ACCENT : UI_DIM;
    draw_icon(dl, icon, ImVec2(a.x, cy - s * 0.5f), s, ic);
    char num[16] = "";
    float nw = 0;
    if (count >= 0) {
        snprintf(num, sizeof(num), "%d", count);
        nw = ui_text_width(ui, nullptr, COUNT_PX, num) + PX(6);
        ui_text_right(ui, dl, nullptr, COUNT_PX, ImVec2(b.x - PX(4), cy - PX(COUNT_PX * 0.5f + 1)),
                      UI_FAINT, num);
    }
    ui_text(ui, dl, selected ? ui->font_bold : nullptr, NAME_PX,
            ImVec2(a.x + s + PX(6), cy - PX(NAME_PX * 0.5f + 1)),
            selected ? UI_TEXT : IM_COL32(0xC8, 0xC8, 0xC8, 0xFF), name, b.x - a.x - s - PX(10) - nw);
}

static void node_menu(UiState *ui, RXPlaylistFolder *parent, RXPlaylistFolder *folder, RXPlaylist *pl)
{
    RXPlaylistFolder *in = folder ? folder : parent;
    if (ImGui::MenuItem("New playlist"))
        create(ui, in, false);
    if (ImGui::MenuItem("New folder"))
        create(ui, in, true);
    if (folder || pl) {
        ImGui::Separator();
        if (ImGui::MenuItem("Rename"))
            start_rename(ui, folder ? (void *)folder : (void *)pl, folder != nullptr,
                         folder ? folder->name : pl->name);
        if (ImGui::MenuItem("Delete...")) {
            ui->delete_target = folder ? (void *)folder : (void *)pl;
            ui->delete_folder = folder != nullptr;
            ui->want_delete_popup = true;
        }
    }
}

// The in-place rename field over a node.
static bool rename_field(UiState *ui, ImVec2 a, float w)
{
    ImGui::SetCursorScreenPos(a);
    ImGui::SetNextItemWidth(w);
    if (ui->rename_focus) {
        ImGui::SetKeyboardFocusHere();
        ui->rename_focus = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(PX(4), PX(1)));
    bool enter = ImGui::InputText("##rename", ui->rename_buf, sizeof(ui->rename_buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleVar();
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui->rename_target = nullptr;
        return true;
    }
    if (enter || ImGui::IsItemDeactivated()) {
        commit_rename(ui);
        return true;
    }
    return false;
}

static void drag_source(bool folder, void *ptr, const char *name)
{
    if (!ImGui::BeginDragDropSource())
        return;
    TreeNodePayload n = { folder, ptr };
    ImGui::SetDragDropPayload(UI_DND_NODE, &n, sizeof(n));
    ImGui::TextUnformatted(name);
    ImGui::EndDragDropSource();
}

static bool accept_node(TreeNodePayload &n)
{
    const ImGuiPayload *p = ImGui::AcceptDragDropPayload(UI_DND_NODE);
    if (!p || p->DataSize != (int)sizeof(n))
        return false;
    memcpy(&n, p->Data, sizeof(n));
    return true;
}

static void draw_folder(UiState *ui, RXPlaylistFolder *f, RXPlaylistFolder *parent);

static void draw_playlist(UiState *ui, RXPlaylist *p, RXPlaylistFolder *parent, int index)
{
    RXApp *app = ui->app;
    ImGui::PushID(p);
    bool sel = app->current_playlist == p;
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    if (sel)
        fl |= ImGuiTreeNodeFlags_Selected;
    ImGui::TreeNodeEx("##pl", fl);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (ui->tree_reveal_playlist == p) {
        ImGui::SetScrollHereY(0.5f);
        ui->tree_reveal_playlist = nullptr;
    }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && ui->rename_target != p) {
        app->current_playlist = p;
        app->current_folder = parent;
        app->nb_selection = 0;
        rx_app_show_album(app, nullptr);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        start_rename(ui, p, false, p->name);
    if (ImGui::BeginPopupContextItem("##menu")) {
        node_menu(ui, parent, nullptr, p);
        ImGui::EndPopup();
    }
    drag_source(false, p, p->name);
    if (ImGui::BeginDragDropTarget()) {
        std::vector<uint64_t> ids;
        TreeNodePayload n;
        if (ui_accept_tracks(ui, ids))
            ui_add_to_playlist(ui, p, ids);
        else if (accept_node(n) && n.ptr != p)
            reparent(ui, n, parent, index);
        ImGui::EndDragDropTarget();
    }
    float lx = a.x + ImGui::GetTreeNodeToLabelSpacing() - PX(14);
    if (ui->rename_target == p)
        rename_field(ui, ImVec2(lx, a.y), b.x - lx - PX(4));
    else
        node_label(ui, ImVec2(lx, a.y), b, 2, p->name, p->nb, sel);
    ImGui::PopID();
}

static void draw_folder(UiState *ui, RXPlaylistFolder *f, RXPlaylistFolder *parent)
{
    ImGui::PushID(f);
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    if (ui->reveal && folder_contains(f, ui->reveal))
        ImGui::SetNextItemOpen(true);
    bool open = ImGui::TreeNodeEx("##folder", fl);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        ui->app->current_folder = f;
    if (ImGui::BeginPopupContextItem("##menu")) {
        node_menu(ui, parent, f, nullptr);
        ImGui::EndPopup();
    }
    drag_source(true, f, f->name);
    if (ImGui::BeginDragDropTarget()) {
        TreeNodePayload n;
        if (accept_node(n) && n.ptr != f)
            reparent(ui, n, f, -1);
        ImGui::EndDragDropTarget();
    }
    float lx = a.x + ImGui::GetTreeNodeToLabelSpacing();
    if (ui->rename_target == f)
        rename_field(ui, ImVec2(lx, a.y), b.x - lx - PX(4));
    else
        node_label(ui, ImVec2(lx, a.y), b, 1, f->name, -1, false);
    if (open) {
        for (int i = 0; i < f->nb_folders; i++)
            draw_folder(ui, f->folders[i], f);
        for (int i = 0; i < f->nb_playlists; i++)
            draw_playlist(ui, f->playlists[i], f, i);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

/* --------------------------------------------------------------------- tree */

static void delete_popup(UiState *ui)
{
    RXApp *app = ui->app;
    if (ui->want_delete_popup) {
        ImGui::OpenPopup("Delete?##tree");
        ui->want_delete_popup = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Delete?##tree", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ui_settle_centred();
    const char *name = "";
    if (ui->delete_target && ui->delete_folder)
        name = ((RXPlaylistFolder *)ui->delete_target)->name;
    else if (ui->delete_target)
        name = ((RXPlaylist *)ui->delete_target)->name;
    if (ui->delete_folder)
        ImGui::Text("Delete the folder \"%s\" and every playlist in it?", name);
    else
        ImGui::Text("Delete the playlist \"%s\"?", name);
    ImGui::TextDisabled("The tracks stay in the Collection.");
    ImGui::Spacing();
    if (ImGui::Button("Delete", ImVec2(PX(100), 0)) && ui->delete_target) {
        int ret;
        if (ui->delete_folder) {
            RXPlaylistFolder *f = (RXPlaylistFolder *)ui->delete_target;
            if (app->current_playlist && folder_contains(f, app->current_playlist))
                app->current_playlist = nullptr;
            if (app->current_folder && folder_contains(f, app->current_folder))
                app->current_folder = nullptr;
            ret = rx_library_folder_delete(&app->lib, f);
        } else {
            RXPlaylist *p = (RXPlaylist *)ui->delete_target;
            if (app->current_playlist == p)
                app->current_playlist = nullptr;
            ret = rx_library_playlist_delete(&app->lib, p);
        }
        if (ret < 0)
            rx_app_set_status(app, "Could not delete: %s", strerror(-ret));
        ui->delete_target = nullptr;
        rx_app_rebuild_rows(app);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(PX(100), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui->delete_target = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* ------------------------------------------------------------------- finder */

static void add_playlists(std::vector<UiTreeHit> &out, RXPlaylistFolder *f, const std::string &path)
{
    for (int i = 0; i < f->nb_playlists; i++) {
        RXPlaylist *p = f->playlists[i];
        out.push_back({ UiTreeHit::PLAYLIST, path + p->name, "", p, f, p->nb, 0 });
    }
    for (int i = 0; i < f->nb_folders; i++)
        add_playlists(out, f->folders[i], path + f->folders[i]->name + "/");
}

// The matches of what is typed, best first, made again when it or the
// library has changed; the pick goes back to the first when what is typed
// does.
static void find_update(UiState *ui)
{
    RXApp *app = ui->app;
    if (ui->tree_hits_query == ui->tree_find && ui->tree_hits_generation == app->lib.generation)
        return;
    if (ui->tree_hits_query != ui->tree_find)
        ui->tree_hit = 0;
    ui->tree_hits_query = ui->tree_find;
    ui->tree_hits_generation = app->lib.generation;

    std::vector<UiTreeHit> all;
    all.push_back({ UiTreeHit::COLLECTION, "Collection", "", nullptr, nullptr, app->lib.nb_tracks, 0 });
    const RXAlbum *albums = nullptr;
    int nb_albums = rx_app_albums(app, &albums);
    for (int i = 0; i < nb_albums; i++) {
        const RXAlbum &al = albums[i];
        std::string text = !al.name[0] ? std::string("None")
                         : al.artist ? std::string(al.artist) + "/" + al.name : std::string(al.name);
        all.push_back({ UiTreeHit::ALBUM, text, al.name, nullptr, nullptr, al.nb, 0 });
    }
    if (app->lib.root_folder)
        add_playlists(all, app->lib.root_folder, "");

    ui->tree_hits.clear();
    for (UiTreeHit &h : all) {
        if (rx_fuzzy_match(h.text.c_str(), ui->tree_find, &h.score, nullptr, 0, nullptr))
            ui->tree_hits.push_back(std::move(h));
    }
    // Best first; between equals Collection, the albums, then the
    // playlists, each kind in natural order.
    std::stable_sort(ui->tree_hits.begin(), ui->tree_hits.end(),
                     [](const UiTreeHit &x, const UiTreeHit &y) {
        if (x.score != y.score)
            return x.score > y.score;
        if (x.kind != y.kind)
            return x.kind < y.kind;
        return rx_strnatcasecmp(x.text.c_str(), y.text.c_str()) < 0;
    });
    if (ui->tree_hits.size() > 300)
        ui->tree_hits.resize(300);
    ui->tree_hit = ImClamp(ui->tree_hit, 0, ImMax((int)ui->tree_hits.size() - 1, 0));
}

static bool playlist_in(const RXPlaylistFolder *f, const RXPlaylist *p)
{
    for (int i = 0; i < f->nb_playlists; i++) {
        if (f->playlists[i] == p)
            return true;
    }
    for (int i = 0; i < f->nb_folders; i++) {
        if (playlist_in(f->folders[i], p))
            return true;
    }
    return false;
}

// Opens a match as a click in the tree does, empties the finder, and
// says which folder the tree opens down to (the picked playlist's).
static RXPlaylistFolder *find_pick(UiState *ui, const UiTreeHit &h)
{
    RXApp *app = ui->app;
    RXPlaylistFolder *reveal = nullptr;
    switch (h.kind) {
    case UiTreeHit::COLLECTION:
        app->current_playlist = nullptr;
        rx_app_show_album(app, nullptr);
        break;
    case UiTreeHit::ALBUM:
        rx_app_show_album(app, h.album.c_str());
        ui->tree_reveal_album = true;
        ui->tree_reveal_album_name = h.album;
        break;
    case UiTreeHit::PLAYLIST:
        // The tree may have lost it since the list was made.
        if (!app->lib.root_folder || !playlist_in(app->lib.root_folder, h.playlist))
            return nullptr;
        app->current_playlist = h.playlist;
        app->current_folder = h.parent;
        rx_app_show_album(app, nullptr);
        ui->tree_reveal_playlist = h.playlist;
        reveal = h.parent;
        break;
    }
    app->nb_selection = 0;
    ui->tree_find[0] = 0;
    return reveal;
}

// A match's text, the context (up to the last '/') dimmed, the letters
// matched lit, as the Add dialog's finder draws its paths.
static void lit_text(UiState *ui, ImVec2 pos, const char *text, float max_w)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float size = PX(NAME_PX);
    size_t len = strlen(text);
    int hits[64], nh = 0;
    rx_fuzzy_match(text, ui->tree_find, nullptr, hits, 64, &nh);
    std::vector<bool> lit(len, false);
    for (int i = 0; i < nh; i++) {
        if (hits[i] >= 0 && (size_t)hits[i] < len)
            lit[(size_t)hits[i]] = true;
    }
    const char *slash = strrchr(text, '/');
    size_t base = slash ? (size_t)(slash - text) + 1 : 0;
    auto colour = [&](size_t at) -> ImU32 {
        return lit[at] ? UI_PLAY : at < base ? UI_DIM : IM_COL32(0xC8, 0xC8, 0xC8, 0xFF);
    };
    float x = pos.x, right = pos.x + max_w;
    size_t i = 0;
    while (i < len && x < right) {
        ImU32 col = colour(i);
        size_t j = i;
        while (j < len && colour(j) == col) {
            j++;
            while (j < len && ((unsigned char)text[j] & 0xC0) == 0x80)
                j++;
        }
        dl->AddText(font, size, ImVec2(x, pos.y), col, text + i, text + j);
        x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, text + i, text + j).x;
        i = j;
    }
}

// The field, and while it holds text the matches in the tree's place; true
// then. *picked_out when one was opened, *reveal the folder the tree then
// opens down to.
static bool finder(UiState *ui, bool *picked_out, RXPlaylistFolder **reveal)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ui->tree_find_focus) {
        ImGui::SetKeyboardFocusHere();
        ui->tree_find_focus = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(PX(8), PX(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, PX(11));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0x10, 0x10, 0x10, 0xFF));
    // Escape empties it (and a second Escape leaves it); Enter opens the pick.
    bool enter = ImGui::InputTextWithHint("##treefind", "Find a playlist or album  (Ctrl+P)",
                                          ui->tree_find, sizeof(ui->tree_find),
                                          ImGuiInputTextFlags_EscapeClearsAll |
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    bool active = ImGui::IsItemActive();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Dummy(ImVec2(0, PX(2)));
    if (!ui->tree_find[0])
        return false;

    find_update(ui);
    int nb = (int)ui->tree_hits.size();
    if (active && nb) {
        int step = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1
                 : ImGui::IsKeyPressed(ImGuiKey_PageDown) ? 10
                 : ImGui::IsKeyPressed(ImGuiKey_PageUp) ? -10 : 0;
        if (step) {
            ui->tree_hit = ImClamp(ui->tree_hit + step, 0, nb - 1);
            ui->tree_hit_scroll = true;
        }
    }
    if (enter && nb) {
        *reveal = find_pick(ui, ui->tree_hits[(size_t)ui->tree_hit]);
        *picked_out = true;
        return false;
    }
    if (!nb) {
        ImGui::TextDisabled("  Nothing found.");
        return true;
    }

    float row_h = ImGui::GetFrameHeight();
    int picked = -1;
    ImGuiListClipper clip;
    clip.Begin(nb, row_h);
    if (ui->tree_hit_scroll)
        clip.IncludeItemByIndex(ui->tree_hit);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const UiTreeHit &h = ui->tree_hits[(size_t)i];
            ImGui::PushID(i);
            if (ImGui::Selectable("##hit", i == ui->tree_hit, 0, ImVec2(0, row_h)))
                picked = i;
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (i == ui->tree_hit && ui->tree_hit_scroll) {
                ImGui::SetScrollHereY(0.5f);
                ui->tree_hit_scroll = false;
            }
            ImDrawList *dl = ImGui::GetWindowDrawList();
            float s = PX(ICON_PX), cy = (a.y + b.y) * 0.5f;
            int icon = h.kind == UiTreeHit::COLLECTION ? 0 : h.kind == UiTreeHit::ALBUM ? 3 : 2;
            draw_icon(dl, icon, ImVec2(a.x + PX(4), cy - s * 0.5f), s,
                      i == ui->tree_hit ? UI_ACCENT : UI_DIM);
            char num[16];
            snprintf(num, sizeof(num), "%d", h.count);
            float nw = ui_text_width(ui, nullptr, COUNT_PX, num) + PX(6);
            ui_text_right(ui, dl, nullptr, COUNT_PX,
                          ImVec2(b.x - PX(4), cy - PX(COUNT_PX * 0.5f + 1)), UI_FAINT, num);
            float x = a.x + PX(4) + s + PX(6);
            lit_text(ui, ImVec2(x, cy - PX(NAME_PX * 0.5f + 1)), h.text.c_str(),
                     b.x - x - PX(4) - nw);
            ImGui::PopID();
        }
    }
    if (picked >= 0) {
        *reveal = find_pick(ui, ui->tree_hits[(size_t)picked]);
        *picked_out = true;
    }
    return true;
}

/* -------------------------------------------------------------------- tree */

// Collection, Albums and the playlists.
static void tree_nodes(UiState *ui)
{
    RXApp *app = ui->app;
    RXPlaylistFolder *root = app->lib.root_folder;
    // Collection: every track.
    bool sel = app->current_playlist == nullptr && app->current_album == nullptr &&
               app->current_album_artist == nullptr;
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    if (sel)
        fl |= ImGuiTreeNodeFlags_Selected;
    ImGui::TreeNodeEx("##collection", fl);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (ImGui::IsItemClicked() && !sel) {
        app->current_playlist = nullptr;
        app->nb_selection = 0;
        rx_app_show_album(app, nullptr);
    }
    if (ImGui::BeginPopupContextItem("##menu")) {
        node_menu(ui, root, nullptr, nullptr);
        ImGui::EndPopup();
    }
    if (ImGui::BeginDragDropTarget()) {
        TreeNodePayload n;
        if (root && accept_node(n))
            reparent(ui, n, root, -1);
        ImGui::EndDragDropTarget();
    }
    float lx = a.x + ImGui::GetTreeNodeToLabelSpacing() - PX(14);
    node_label(ui, ImVec2(lx, a.y), b, 0, "Collection", app->lib.nb_tracks, sel);

    // Albums: every album of the library, one flat list by artist and then
    // name (rx_app_albums), each shown like a playlist of its tracks in
    // disc and track order, and None first (the album "": the tracks
    // without an album or with one no other track has, by title); the
    // collection is the list's tracks one after the other. Nothing can be
    // dropped on one, nothing renames or deletes one, and the export does
    // not know them: they are the tags' view of the collection. The count
    // beside Albums is of albums, None aside.
    const RXAlbum *albums = nullptr;
    int nb_albums = rx_app_albums(app, &albums);
    int nb_named = nb_albums - (nb_albums > 0 && !albums[0].name[0]);
    ImGuiTreeNodeFlags afl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                             ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    if (ui->tree_reveal_album)
        ImGui::SetNextItemOpen(true);
    bool albums_open = ImGui::TreeNodeEx("##albums", afl);
    a = ImGui::GetItemRectMin();
    b = ImGui::GetItemRectMax();
    node_label(ui, ImVec2(a.x + ImGui::GetTreeNodeToLabelSpacing(), a.y), b, 3, "Albums",
               nb_named, false);
    if (albums_open) {
        // Only the rows in view are drawn: a library has thousands; the
        // one the finder picked is, and is scrolled to.
        int reveal = -1;
        for (int i = 0; ui->tree_reveal_album && i < nb_albums && reveal < 0; i++) {
            if (ui->tree_reveal_album_name == albums[i].name)
                reveal = i;
        }
        ImGuiListClipper clip;
        clip.Begin(nb_albums);
        if (reveal >= 0)
            clip.IncludeItemByIndex(reveal);
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd && i < nb_albums; i++) {
                ImGui::PushID(i);
                bool asel = app->current_album && !strcmp(app->current_album, albums[i].name);
                ImGuiTreeNodeFlags lf = ImGuiTreeNodeFlags_Leaf |
                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                        ImGuiTreeNodeFlags_SpanAvailWidth |
                                        ImGuiTreeNodeFlags_FramePadding;
                if (asel)
                    lf |= ImGuiTreeNodeFlags_Selected;
                ImGui::TreeNodeEx("##album", lf);
                ImVec2 la = ImGui::GetItemRectMin(), lb = ImGui::GetItemRectMax();
                if (i == reveal) {
                    ImGui::SetScrollHereY(0.5f);
                    ui->tree_reveal_album = false;
                }
                if (ImGui::IsItemClicked() && !asel) {
                    app->nb_selection = 0;
                    rx_app_show_album(app, albums[i].name);
                    // The list may have been made again: this frame's names
                    // are gone with it.
                    nb_albums = rx_app_albums(app, &albums);
                }
                float llx = la.x + ImGui::GetTreeNodeToLabelSpacing() - PX(14);
                if (i < nb_albums)
                    node_label(ui, ImVec2(llx, la.y), lb, 3,
                               albums[i].name[0] ? albums[i].name : "None", albums[i].nb, asel);
                ImGui::PopID();
            }
        }
        if (nb_albums == 0)
            ImGui::TextDisabled("  No album tags yet.");
        ImGui::TreePop();
    }

    // Album Artists: the artists of Albums in its order, None first (the
    // name "": None's tracks and the albums of no artist), each shown like
    // a playlist of its albums' tracks, the albums one after the other as
    // Albums lists them. The count beside it is of artists, None aside.
    const RXAlbumArtist *artists = nullptr;
    int nb_artists = rx_app_album_artists(app, &artists);
    int nb_named_artists = nb_artists - (nb_artists > 0 && !artists[0].name[0]);
    bool artists_open = ImGui::TreeNodeEx("##album_artists", afl);
    a = ImGui::GetItemRectMin();
    b = ImGui::GetItemRectMax();
    node_label(ui, ImVec2(a.x + ImGui::GetTreeNodeToLabelSpacing(), a.y), b, 4, "Album Artists",
               nb_named_artists, false);
    if (artists_open) {
        ImGuiListClipper clip;
        clip.Begin(nb_artists);
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd && i < nb_artists; i++) {
                ImGui::PushID(i);
                bool asel = app->current_album_artist &&
                            !strcmp(app->current_album_artist, artists[i].name);
                ImGuiTreeNodeFlags lf = ImGuiTreeNodeFlags_Leaf |
                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                        ImGuiTreeNodeFlags_SpanAvailWidth |
                                        ImGuiTreeNodeFlags_FramePadding;
                if (asel)
                    lf |= ImGuiTreeNodeFlags_Selected;
                ImGui::TreeNodeEx("##album_artist", lf);
                ImVec2 la = ImGui::GetItemRectMin(), lb = ImGui::GetItemRectMax();
                if (ImGui::IsItemClicked() && !asel) {
                    app->nb_selection = 0;
                    rx_app_show_album_artist(app, artists[i].name);
                    nb_artists = rx_app_album_artists(app, &artists);
                }
                float llx = la.x + ImGui::GetTreeNodeToLabelSpacing() - PX(14);
                if (i < nb_artists)
                    node_label(ui, ImVec2(llx, la.y), lb, 4,
                               artists[i].name[0] ? artists[i].name : "None", artists[i].nb, asel);
                ImGui::PopID();
            }
        }
        if (nb_artists == 0)
            ImGui::TextDisabled("  No album tags yet.");
        ImGui::TreePop();
    }

    ImGui::Dummy(ImVec2(0, PX(2)));
    ImGui::TextDisabled("  PLAYLISTS");
    if (root) {
        for (int i = 0; i < root->nb_folders; i++)
            draw_folder(ui, root->folders[i], root);
        for (int i = 0; i < root->nb_playlists; i++)
            draw_playlist(ui, root->playlists[i], root, i);
        if (root->nb_folders == 0 && root->nb_playlists == 0)
            ImGui::TextDisabled("  Right-click to create one.");
    }
}

void ui_tree(UiState *ui, ImVec2 size)
{
    RXApp *app = ui->app;
    RXPlaylistFolder *root = app->lib.root_folder;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PX(4), PX(6)));
    ImGui::BeginChild("##tree", size, ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    ImGui::PopStyleVar();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wa = ImGui::GetWindowPos();
    dl->AddRectFilled(wa, ImVec2(wa.x + size.x, wa.y + size.y), UI_PANEL, PX(4));
    RXPlaylistFolder *reveal = nullptr;
    bool picked = false;
    bool finding = finder(ui, &picked, &reveal);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(PX(4), PX(5.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(PX(4), PX(1)));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0x2E, 0x2E, 0x2E, 0xFF));
    if (!finding)
        tree_nodes(ui);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // The empty space below: a context menu for the root, and a drop
    // target that moves a node to the root.
    ImVec2 rest = ImGui::GetContentRegionAvail();
    if (rest.y > 4) {
        ImGui::InvisibleButton("##empty", ImVec2(ImMax(rest.x, 1.0f), rest.y));
        if (ImGui::BeginPopupContextItem("##rootmenu", ImGuiPopupFlags_MouseButtonRight)) {
            node_menu(ui, root, nullptr, nullptr);
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropTarget()) {
            TreeNodePayload n;
            if (root && accept_node(n))
                reparent(ui, n, root, -1);
            ImGui::EndDragDropTarget();
        }
    }
    delete_popup(ui);
    ui->reveal = reveal;
    // A pick's rows have had their draw (an album's this frame, a
    // playlist's the next, once its folders are open): a reveal not used
    // goes, so it does not hold Albums open.
    if (!picked) {
        ui->tree_reveal_album = false;
        ui->tree_reveal_playlist = nullptr;
    }
    if (ui->move_pending)
        reparent_now(ui);
    if (ui->create_pending)
        create_now(ui);
    ImGui::EndChild();
}
