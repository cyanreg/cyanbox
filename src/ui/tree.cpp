/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The playlist tree on the browser's left: Collection (every track), the
// Albums (every album tag, alphabetically, each a view of its tracks in
// disc and track order: nothing is added to, renamed or exported there),
// then the library's folders and playlists. A click shows a playlist; the
// context menu creates, renames (in place, as rekordbox does) and deletes
// (after a confirmation). Tracks dragged from the table onto a playlist
// are appended to it; playlists and folders dragged within the tree move
// under another folder (onto a folder: into it, last; onto a playlist:
// beside it), which the library keeps as positions.

#include "ui_internal.h"

#include <stdio.h>

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

/* -------------------------------------------------------------------- nodes */

// The label drawn over an empty-labelled tree node: icon, name, count.
static void node_label(UiState *ui, ImVec2 a, ImVec2 b, int icon, const char *name, int count,
                       bool selected)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float s = PX(12);
    float cy = (a.y + b.y) * 0.5f;
    ImU32 ic = selected ? UI_ACCENT : UI_DIM;
    ImVec2 ip(a.x, cy - s * 0.5f);
    if (icon == 0)
        collection_icon(dl, ip, s, ic);
    else if (icon == 1)
        folder_icon(dl, ip, s, ic);
    else if (icon == 3)
        album_icon(dl, ip, s, ic);
    else
        playlist_icon(dl, ip, s, ic);
    char num[16] = "";
    float nw = 0;
    if (count >= 0) {
        snprintf(num, sizeof(num), "%d", count);
        nw = ui_text_width(ui, nullptr, 11.0f, num) + PX(6);
        ui_text_right(ui, dl, nullptr, 11.0f, ImVec2(b.x - PX(4), cy - PX(6.5f)), UI_FAINT, num);
    }
    ui_text(ui, dl, selected ? ui->font_bold : nullptr, 13.0f, ImVec2(a.x + s + PX(6), cy - PX(7.5f)),
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(PX(4), PX(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(PX(4), PX(1)));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0x2E, 0x2E, 0x2E, 0xFF));

    // Collection: every track.
    bool sel = app->current_playlist == nullptr && app->current_album == nullptr;
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

    // Albums: every album of the library, alphabetically, each shown like
    // a playlist of its tracks in disc and track order. Nothing can be
    // dropped on one, nothing renames or deletes one, and the export does
    // not know them: they are the tags' view of the collection.
    const RXAlbum *albums = nullptr;
    int nb_albums = rx_app_albums(app, &albums);
    ImGuiTreeNodeFlags afl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                             ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    bool albums_open = ImGui::TreeNodeEx("##albums", afl);
    a = ImGui::GetItemRectMin();
    b = ImGui::GetItemRectMax();
    node_label(ui, ImVec2(a.x + ImGui::GetTreeNodeToLabelSpacing(), a.y), b, 3, "Albums",
               nb_albums, false);
    if (albums_open) {
        // Only the rows in view are drawn: a library has thousands.
        ImGuiListClipper clip;
        clip.Begin(nb_albums);
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
                if (ImGui::IsItemClicked() && !asel) {
                    app->nb_selection = 0;
                    rx_app_show_album(app, albums[i].name);
                    // The list may have been made again: this frame's names
                    // are gone with it.
                    nb_albums = rx_app_albums(app, &albums);
                }
                float llx = la.x + ImGui::GetTreeNodeToLabelSpacing() - PX(14);
                if (i < nb_albums)
                    node_label(ui, ImVec2(llx, la.y), lb, 3, albums[i].name, albums[i].nb, asel);
                ImGui::PopID();
            }
        }
        if (nb_albums == 0)
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
    ui->reveal = nullptr;
    if (ui->move_pending)
        reparent_now(ui);
    if (ui->create_pending)
        create_now(ui);
    ImGui::EndChild();
}
