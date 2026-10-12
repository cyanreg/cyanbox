/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The dialogs: the confirmations (replace a playing track, remove from the
// library), Relocate, Analysis settings, Export, Sync from USB, Inspect
// USB, the Add browser (a plain directory lister: there is no native file
// dialog under a bare Wayland compositor), the keyboard map and About.
//
// Export follows the app's flow (rxbox/app.h export_state): Export starts
// a dry run; when it finds stick files to prune or player changes not yet
// synced back it stops with `confirm` set, both known by then, and the
// dialog asks once ("The stick has N changes you have not synced", the
// files to delete), each button an answer to both, before the real run.
// Sync from USB reads the stick on a worker
// (rx_app_read_stick: rx_stick_read/rx_stick_diff) and lists what the
// player wrote for review; the accepted rows are applied with
// rx_stick_apply on this thread, which owns the library. Inspect USB runs
// rx_inspect on a worker too (rx_app_inspect_stick). Both take their
// result from app->stick_result when its request is theirs and drop a
// result of their kind that is not (a read given up on); closing or
// reopening either gives up its request in flight (rx_app_cancel_stick),
// so a later read never shows an earlier one's result.

#include "ui_internal.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>

#include "config.h"

static void centre_next(UiState *ui, float w, float h)
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (w > 0)
        ImGui::SetNextWindowSize(ImVec2(PX(w), PX(h)), ImGuiCond_Appearing);
}

// A window that sizes itself is measured over its first frames, after the
// pivot was applied: centre it again until its size has settled (then it
// may be moved).
void ui_settle_centred(void)
{
    ImGuiStorage *st = ImGui::GetStateStorage();
    ImGuiID key = ImGui::GetID("##settle");
    int n = ImGui::IsWindowAppearing() ? 0 : st->GetInt(key, 99);
    if (n >= 3)
        return;
    ImVec2 c = ImGui::GetMainViewport()->GetCenter(), sz = ImGui::GetWindowSize();
    ImGui::SetWindowPos(ImVec2(floorf(c.x - sz.x * 0.5f), floorf(c.y - sz.y * 0.5f)));
    st->SetInt(key, n + 1);
}

/* ------------------------------------------------------------------ volumes */

// Removable volumes: what /proc/mounts lists under /run/media and /media.
void ui_scan_volumes(UiState *ui)
{
    ui->volumes.clear();
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char dev[512], mnt[1024], type[64];
        if (sscanf(line, "%511s %1023s %63s", dev, mnt, type) != 3)
            continue;
        // /proc/mounts escapes spaces and friends as \ooo.
        std::string m;
        for (const char *p = mnt; *p; p++) {
            if (p[0] == '\\' && p[1] >= '0' && p[1] <= '7' && p[2] && p[3]) {
                m += (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
                p += 3;
            } else {
                m += *p;
            }
        }
        if (rx_str_starts(m.c_str(), "/run/media/") || rx_str_starts(m.c_str(), "/media/"))
            ui->volumes.push_back(m);
    }
    fclose(f);
}

// The volume list with the path field above it; returns true when the
// path changed.
static bool volume_picker(UiState *ui, char *path, size_t size)
{
    bool changed = false;
    float w = PX(540);
    ImGui::SetNextItemWidth(w - PX(86));
    changed |= ImGui::InputTextWithHint("##dest", "/run/media/you/STICK", path, size);
    ImGui::SameLine();
    if (ImGui::Button("Rescan", ImVec2(PX(80), 0)))
        ui_scan_volumes(ui);
    ImGui::BeginChild("##volumes", ImVec2(w, PX(72)), ImGuiChildFlags_Borders);
    if (ui->volumes.empty())
        ImGui::TextDisabled("No removable volume is mounted under /run/media or /media.");
    for (const std::string &v : ui->volumes) {
        char label[64] = "";
        rx_stick_label(v.c_str(), label, sizeof(label));
        char row[1200];
        snprintf(row, sizeof(row), "%s   %s", label[0] ? label : rx_path_base(v.c_str()), v.c_str());
        if (ImGui::Selectable(row, !strcmp(path, v.c_str()))) {
            snprintf(path, size, "%s", v.c_str());
            changed = true;
        }
    }
    ImGui::EndChild();
    return changed;
}

/* ------------------------------------------------------------ confirmations */

static void confirm_load(UiState *ui)
{
    if (ui->want_confirm_load) {
        ImGui::OpenPopup("Replace the playing track?");
        ui->want_confirm_load = false;
    }
    centre_next(ui, 0, 0);
    if (!ImGui::BeginPopupModal("Replace the playing track?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ui_settle_centred();
    RxTrack *t = rx_library_find_id(&ui->app->lib, ui->confirm_track);
    char name[256] = "";
    if (t)
        rx_track_display(t, name, sizeof(name));
    ImGui::Text("The player is playing. Load \"%s\" anyway?", name);
    ImGui::Spacing();
    // Enter on a row asks in the frame it was pressed: that press is not
    // the answer, nor is a key held down and repeating.
    bool keys = !ImGui::IsWindowAppearing();
    if (ImGui::Button("Load", ImVec2(PX(110), 0)) ||
        (keys && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
        ui_load_confirmed(ui);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(PX(110), 0)) ||
        (keys && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void confirm_remove(UiState *ui)
{
    RxApp *app = ui->app;
    if (ui->want_confirm_remove) {
        ImGui::OpenPopup("Remove from library?");
        ui->want_confirm_remove = false;
    }
    centre_next(ui, 0, 0);
    if (!ImGui::BeginPopupModal("Remove from library?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ui_settle_centred();
    int n = (int)ui->remove_ids.size();
    ImGui::Text("Remove %d track%s from the library?", n, n == 1 ? "" : "s");
    ImGui::TextDisabled("Cues, grids and playlist entries go with them. The audio files stay.");
    ImGui::Spacing();
    if (ImGui::Button("Remove", ImVec2(PX(110), 0))) {
        int removed = 0;
        for (uint64_t id : ui->remove_ids) {
            RxTrack *t = rx_library_find_id(&app->lib, id);
            if (!t)
                continue;
            for (int d = 0; d < RX_DECKS; d++) {
                if (app->decks[d].track == t || app->decks[d].loading_track_id == id)
                    rx_app_unload_deck(app, d);
            }
            if (rx_library_remove(&app->lib, t) >= 0)
                removed++;
        }
        ui->remove_ids.clear();
        app->nb_selection = 0;
        app->focus_track = 0;
        rx_app_rebuild_rows(app);
        rx_app_set_status(app, "Removed %d track%s from the library", removed, removed == 1 ? "" : "s");
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(PX(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui->remove_ids.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* ----------------------------------------------------------------- relocate */

void ui_open_relocate(UiState *ui, RxTrack *t)
{
    ui->relocate_id = t->id;
    snprintf(ui->relocate_buf, sizeof(ui->relocate_buf), "%s", t->path);
    ui->want_relocate = true;
}

static void relocate(UiState *ui)
{
    RxApp *app = ui->app;
    if (ui->want_relocate) {
        ImGui::OpenPopup("Relocate");
        ui->want_relocate = false;
    }
    centre_next(ui, 560, 0);
    if (!ImGui::BeginPopupModal("Relocate", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ui_settle_centred();
    RxTrack *t = rx_library_find_id(&app->lib, ui->relocate_id);
    if (!t) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextDisabled("The file was not found at");
    ImGui::TextWrapped("%s", t->path);
    ImGui::Spacing();
    ImGui::TextUnformatted("Its new path:");
    ImGui::SetNextItemWidth(PX(540));
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##newpath", ui->relocate_buf, sizeof(ui->relocate_buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    char *expanded = rx_path_expand(ui->relocate_buf);
    bool exists = expanded && rx_file_exists(expanded);
    if (!exists)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT), "No file there.");
    else
        ImGui::TextDisabled("Found.");
    ImGui::BeginDisabled(!exists);
    if (ImGui::Button("Relocate", ImVec2(PX(110), 0)) || (enter && exists)) {
        char *abs = rx_path_abs(expanded);
        int ret = abs ? rx_library_relocate(&app->lib, t, abs) : RX_ERROR(ENOENT);
        if (ret < 0)
            rx_app_set_status(app, "Could not relocate: %s", strerror(-ret));
        else
            rx_app_set_status(app, "Relocated %s", ui_title(t));
        free(abs);
        rx_app_rebuild_rows(app);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    free(expanded);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(PX(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

/* -------------------------------------------------------- analysis settings */

struct BpmPreset { const char *label; double lo, hi; };
static const BpmPreset BPM_PRESETS[] = {
    { "Auto (by evidence)", 0, 0 },
    { "70 - 180", 70, 180 },
    { "98 - 195", 98, 195 },
    { "118 - 236", 118, 236 },
    { "58 - 115", 58, 115 },
};

static void analysis_settings(UiState *ui)
{
    if (!ui->dlg_analysis)
        return;
    RxConfig *cfg = &ui->app->lib.cfg;
    centre_next(ui, 380, 0);
    if (!ImGui::Begin("Analysis settings", &ui->dlg_analysis, ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_settle_centred();
    bool changed = false;
    ImGui::SeparatorText("Analyse");
    bool beats = cfg->analyse_what & RX_ANALYSE_BEATS, key = cfg->analyse_what & RX_ANALYSE_KEY;
    if (ImGui::Checkbox("BPM and beat grid", &beats) | ImGui::Checkbox("Key", &key)) {
        cfg->analyse_what = RX_ANALYSE_WAVES | (beats ? RX_ANALYSE_BEATS : 0) |
                            (key ? RX_ANALYSE_KEY : 0);
        changed = true;
    }
    ImGui::TextDisabled("Waveforms are always made.");

    ImGui::SeparatorText("Tempo");
    int preset = 0;
    for (int i = 0; i < (int)(sizeof(BPM_PRESETS) / sizeof(BPM_PRESETS[0])); i++) {
        if (fabs(cfg->bpm_min - BPM_PRESETS[i].lo) < 0.5 && fabs(cfg->bpm_max -
                                                                 BPM_PRESETS[i].hi) < 0.5)
            preset = i;
    }
    ImGui::SetNextItemWidth(PX(200));
    if (ImGui::BeginCombo("BPM range", BPM_PRESETS[preset].label)) {
        for (int i = 0; i < (int)(sizeof(BPM_PRESETS) / sizeof(BPM_PRESETS[0])); i++) {
            if (ImGui::Selectable(BPM_PRESETS[i].label, i == preset)) {
                cfg->bpm_min = BPM_PRESETS[i].lo;
                cfg->bpm_max = BPM_PRESETS[i].hi;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed |= ImGui::Checkbox("Snap to whole BPM when the track allows", &cfg->snap_bpm);
    changed |= ImGui::Checkbox("Allow tempo sections", &cfg->allow_sections);
    ImGui::SetItemTooltip("Split the grid where the tempo or phase really changes.");
    ImGui::SeparatorText("Cues");
    changed |= ImGui::Checkbox("Memory cue at the first downbeat", &cfg->auto_cues);
    if (changed)
        ui_save_config(ui);
    ImGui::Spacing();
    ImGui::TextDisabled("Applies to the next analysis.");
    ImGui::End();
}

/* ------------------------------------------------------------------- export */

void ui_open_export(UiState *ui)
{
    RxApp *app = ui->app;
    ui->dlg_export = true;
    app->export_state.open = true;
    // Closed on its summary with the title bar's x: the choices again.
    if (!app->export_state.running)
        ui->export_seen_running = false;
    ui_scan_volumes(ui);
    if (!app->export_state.dest[0] && ui->volumes.size() == 1)
        snprintf(app->export_state.dest, sizeof(app->export_state.dest), "%s", ui->volumes[0].c_str());
}

static bool playlist_in(const RxPlaylistFolder *f, const RxPlaylist *p)
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

// The chosen playlists that still exist: the tree can delete one while the
// dialog is open, and the pointer must not reach rx_app_start_export().
static void playlists_prune(UiState *ui)
{
    const RxPlaylistFolder *root = ui->app->lib.root_folder;
    auto &sel = ui->export_playlists;
    sel.erase(std::remove_if(sel.begin(), sel.end(), [root](RxPlaylist *p) {
        return !root || !playlist_in(root, p);
    }), sel.end());
}

static void playlist_checks(UiState *ui, RxPlaylistFolder *f)
{
    for (int i = 0; i < f->nb_folders; i++) {
        ImGui::PushID(f->folders[i]);
        if (ImGui::TreeNodeEx("##f", ImGuiTreeNodeFlags_DefaultOpen, "%s", f->folders[i]->name)) {
            playlist_checks(ui, f->folders[i]);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    for (int i = 0; i < f->nb_playlists; i++) {
        RxPlaylist *p = f->playlists[i];
        auto &sel = ui->export_playlists;
        auto it = std::find(sel.begin(), sel.end(), p);
        bool on = it != sel.end();
        char label[256];
        snprintf(label, sizeof(label), "%s (%d)##%p", p->name, p->nb, (void *)p);
        if (ImGui::Checkbox(label, &on)) {
            if (on)
                sel.push_back(p);
            else
                sel.erase(it);
        }
    }
}

static void export_start(UiState *ui)
{
    RxApp *app = ui->app;
    auto &es = app->export_state;
    playlists_prune(ui);
    if (ui->export_scope == 1 && ui->export_playlists.empty())
        return;
    es.cancel = false;
    es.ignore_unsynced = false;
    es.prune = false;
    es.log[0] = 0;
    es.result = 0;
    es.done = es.total = 0;
    es.whole_library = ui->export_scope == 2;
    if (ui->export_scope == 1 && !ui->export_playlists.empty()) {
        es.playlists = ui->export_playlists.data();
        es.nb_playlists = (int)ui->export_playlists.size();
    } else {
        es.playlists = nullptr;
        es.nb_playlists = 0;
    }
    int ret = rx_app_start_export(app);
    if (ret < 0) {
        rx_app_set_status(app, "Export could not start: %s", strerror(-ret));
        return;
    }
    ui->export_seen_running = true;
}

static void export_dialog(UiState *ui)
{
    RxApp *app = ui->app;
    auto &es = app->export_state;
    if (!ui->dlg_export)
        return;
    // The choices size themselves; the questions and the progress log get room.
    bool choosing = !es.running && !ui->export_seen_running;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!choosing)
        ImGui::SetNextWindowSize(ImVec2(PX(600), PX(440)), ImGuiCond_Once);
    bool open = true;
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (choosing)
        wf |= ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("Export to USB", &open, wf)) {
        ImGui::End();
        return;
    }
    ui_settle_centred();
    if (!open && !es.running) {
        ui->dlg_export = false;
        es.open = false;
    }

    if (!es.running && !ui->export_seen_running) {
        // The choices.
        playlists_prune(ui);
        ImGui::SeparatorText("Stick");
        volume_picker(ui, es.dest, sizeof(es.dest));
        ImGui::SeparatorText("What");
        ImGui::RadioButton("All playlists and their tracks", &ui->export_scope, 0);
        ImGui::RadioButton("Selected playlists", &ui->export_scope, 1);
        ImGui::RadioButton("Entire library", &ui->export_scope, 2);
        if (ui->export_scope == 1 && app->lib.root_folder) {
            ImGui::BeginChild("##pls", ImVec2(0, PX(120)), ImGuiChildFlags_Borders);
            playlist_checks(ui, app->lib.root_folder);
            ImGui::EndChild();
        }
        ImGui::SeparatorText("Options");
        ImGui::Checkbox("Compatibility transcode (AAC and hi-res to 16-bit/44.1 kHz FLAC)", &es.compat);
        ImGui::TextDisabled("Files already on the stick are skipped. Tracks no longer exported\n"
                            "are deleted from the stick only after you confirm.");
        ImGui::Spacing();
        bool ok = es.dest[0] && (ui->export_scope != 1 || !ui->export_playlists.empty());
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Export", ImVec2(PX(120), PX(28))))
            export_start(ui);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(PX(120), PX(28)))) {
            ui->dlg_export = false;
            es.open = false;
        }
    } else if (es.running && es.confirm) {
        // The dry run's questions, on one page and answered once: every
        // export button also says yes to overwriting the unsynced changes
        // (the text above says so; the four buttons fit the 600 px window).
        bool unsynced = es.unsynced > 0 && !es.ignore_unsynced;
        if (unsynced) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT),
                               "The stick has %d change%s you have not synced.", es.unsynced,
                               es.unsynced == 1 ? "" : "s");
            ImGui::TextWrapped("A player saved cues, plays or settings on it since the last export. "
                               "Exporting now overwrites them; Sync from USB first to keep them.");
            ImGui::Spacing();
        }
        if (es.nb_prune > 0) {
            ImGui::Text("Delete %d file%s from the stick?", es.nb_prune, es.nb_prune == 1 ? "" : "s");
            ImGui::TextDisabled("Tracks that are no longer in what you export:");
            ImGui::BeginChild("##prune", ImVec2(0, -PX(40)), ImGuiChildFlags_Borders);
            ImGuiListClipper clip;
            clip.Begin(es.nb_prune);
            while (clip.Step()) {
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++)
                    ImGui::TextUnformatted(es.prune_list[i] ? es.prune_list[i] : "");
            }
            ImGui::EndChild();
        }
        auto answer = [&](bool prune) {
            if (unsynced)
                es.ignore_unsynced = true;
            es.prune = prune;
            if (rx_app_start_export(app) < 0)
                rx_app_set_status(app, "Export could not continue");
        };
        float bw = PX(unsynced ? 160 : 140);
        if (unsynced) {
            if (ImGui::Button("Sync first", ImVec2(PX(120), PX(28)))) {
                es.cancel = true;
                snprintf(ui->stick_path, sizeof(ui->stick_path), "%s", es.dest);
                ui_open_sync(ui);
            }
            ImGui::SameLine();
        }
        if (es.nb_prune > 0) {
            if (ImGui::Button(unsynced ? "Export, delete them" : "Delete them", ImVec2(bw, PX(28))))
                answer(true);
            ImGui::SameLine();
            if (ImGui::Button(unsynced ? "Export, keep them" : "Keep them", ImVec2(bw, PX(28))))
                answer(false);
        } else if (unsynced) {
            if (ImGui::Button("Export anyway", ImVec2(PX(140), PX(28))))
                answer(false);
        } else {
            ImGui::TextDisabled("Continuing...");
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(PX(100), PX(28))))
            es.cancel = true;
    } else {
        // Progress, then the summary.
        float frac = es.total > 0 ? (float)es.done / es.total : 0.0f;
        if (es.running) {
            ImGui::Text("%s", es.stage[0] ? es.stage : "starting");
            ImGui::SameLine();
            ImGui::TextDisabled("%d / %d", es.done, es.total);
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, PX(6)), "");
            ImGui::TextDisabled("%s", es.item);
        } else if (es.result == RX_ERROR(ECANCELED)) {
            ImGui::TextUnformatted("Export cancelled.");
        } else if (es.result < 0) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_RED), "Export failed: %s",
                               strerror(-es.result));
        } else {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_PLAY), "Export finished.");
            ImGui::SameLine();
            ImGui::TextDisabled("%d copied, %d already on the stick, %d rewritten, %d deleted",
                                es.copied, es.skipped, es.rewritten, es.pruned);
        }
        ImGui::BeginChild("##log", ImVec2(0, -PX(40)), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(es.log);
        if (es.running && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        if (es.running) {
            if (ImGui::Button("Cancel", ImVec2(PX(120), PX(28))))
                es.cancel = true;
        } else {
            if (ImGui::Button("Done", ImVec2(PX(120), PX(28)))) {
                ui->export_seen_running = false;
                ui->dlg_export = false;
                es.open = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Export again", ImVec2(PX(120), PX(28))))
                ui->export_seen_running = false;
        }
    }
    ImGui::End();
}

/* --------------------------------------------------------------------- sync */

static void sync_free(UiState *ui)
{
    if (ui->sync_state_valid)
        rx_stick_state_free(&ui->sync_state);
    ui->sync_state_valid = false;
    free(ui->sync_items);
    ui->sync_items = nullptr;
    ui->nb_sync_items = 0;
}

// Gives up the read in flight, if any: the dialog closes or starts afresh.
static void sync_cancel(UiState *ui)
{
    rx_app_cancel_stick(ui->app, ui->sync_request);
    ui->sync_request = 0;
}

void ui_open_sync(UiState *ui)
{
    sync_cancel(ui);
    ui->dlg_sync = true;
    ui->sync_stage = 0;
    ui->sync_error.clear();
    ui->sync_filter = -1;
    sync_free(ui);
    ui_scan_volumes(ui);
    if (!ui->stick_path[0] && ui->volumes.size() == 1)
        snprintf(ui->stick_path, sizeof(ui->stick_path), "%s", ui->volumes[0].c_str());
}

static const char *sync_kind_name(int k)
{
    switch (k) {
    case RX_SYNC_HOT_CUE_SET:       return "Hot cue set";
    case RX_SYNC_HOT_CUE_CLEARED:   return "Hot cue cleared";
    case RX_SYNC_MEMORY_ADDED:      return "Memory cue added";
    case RX_SYNC_MEMORY_REMOVED:    return "Memory cue removed";
    case RX_SYNC_PLAY:              return "Play";
    case RX_SYNC_SETTING:           return "Setting";
    default:                        return "?";
    }
}

static void sync_detail(const RxSyncItem *it, char *buf, size_t size)
{
    char t[32], len[32] = "";
    switch (it->kind) {
    case RX_SYNC_HOT_CUE_SET:
    case RX_SYNC_HOT_CUE_CLEARED:
    case RX_SYNC_MEMORY_ADDED:
    case RX_SYNC_MEMORY_REMOVED:
        ui_fmt_time(it->cue.time, t, sizeof(t), true);
        if (rx_cue_is_loop(&it->cue))
            snprintf(len, sizeof(len), ", loop to %s", ui_fmt_time(it->cue.loop_end, t + 16, 16, true));
        if (it->kind == RX_SYNC_HOT_CUE_SET || it->kind == RX_SYNC_HOT_CUE_CLEARED)
            snprintf(buf, size, "%c at %.15s%s%s%s", 'A' + ImClamp(it->slot, 0, 7), t, len,
                     it->cue.name[0] ? " - " : "", it->cue.name);
        else
            snprintf(buf, size, "at %.15s%s%s%s", t, len, it->cue.name[0] ? " - " : "", it->cue.name);
        break;
    case RX_SYNC_PLAY:
        if (it->played_at > 0) {
            time_t tt = (time_t)it->played_at;
            struct tm tm;
            localtime_r(&tt, &tm);
            strftime(buf, size, "played %Y-%m-%d %H:%M", &tm);
        } else {
            snprintf(buf, size, "played (history entry %d, no time)", it->play_index + 1);
        }
        break;
    case RX_SYNC_SETTING:
        snprintf(buf, size, "%s: %s -> %s", it->setting_key,
                 it->setting_old[0] ? it->setting_old : "(default)",
                 it->setting_value);
        break;
    default:
        buf[0] = 0;
        break;
    }
}

static bool sync_progress(void *opaque, const char *stage, int done, int total, const char *item)
{
    UiState *ui = (UiState *)opaque;
    char line[320];
    snprintf(line, sizeof(line), "%s %d/%d %s", stage ? stage : "", done, total, item ? item : "");
    ui->sync_progress = line;
    return true;
}

static void sync_read(UiState *ui)
{
    RxApp *app = ui->app;
    sync_free(ui);
    ui->sync_error.clear();
    int ret = rx_app_read_stick(app, ui->stick_path);
    if (ret < 0) {
        ui->sync_error = std::string("Could not read the stick: ") + strerror(-ret);
        return;
    }
    ui->sync_request = app->stick_request;
    rx_app_set_status(app, "Reading %s...", ui->stick_path);
}

// The read's result, once the worker has posted it. A read's result for
// another request is one given up on: dropped, never shown as this one's.
static void sync_take(UiState *ui)
{
    RxApp *app = ui->app;
    RxJobResult *r = app->stick_result;
    if (!r || r->kind != RX_JOB_STICK_READ)
        return;
    app->stick_result = nullptr;
    if (!ui->sync_request || r->request != ui->sync_request) {
        rx_job_result_free(r);
        return;
    }
    ui->sync_request = 0;
    if (r->stick) {
        ui->sync_state = *r->stick;
        ui->sync_state_valid = true;
        free(r->stick);
        r->stick = nullptr;
    }
    if (r->err < 0) {
        ui->sync_error = std::string(ui->sync_state_valid ?
                                     "Could not compare the stick with the library: " :
                                     "Could not read the stick: ") + strerror(-r->err);
        rx_job_result_free(r);
        return;
    }
    ui->sync_items = r->sync_items;
    ui->nb_sync_items = r->nb_sync_items;
    r->sync_items = nullptr;
    rx_job_result_free(r);
    for (int i = 0; i < ui->nb_sync_items; i++)
        ui->sync_items[i].accepted = true;
    ui->sync_stage = 1;
    rx_app_set_status(app, "%d change%s on %s", ui->nb_sync_items, ui->nb_sync_items == 1 ? "" : "s",
                      ui->stick_path);
}

static void sync_dialog(UiState *ui)
{
    RxApp *app = ui->app;
    if (!ui->dlg_sync)
        return;
    sync_take(ui);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ui->sync_stage == 1)
        ImGui::SetNextWindowSize(ImVec2(PX(780), PX(500)), ImGuiCond_Once);
    else
        wf |= ImGuiWindowFlags_AlwaysAutoResize;
    bool open = true;
    if (!ImGui::Begin("Sync from USB", &open, wf)) {
        ImGui::End();
        return;
    }
    ui_settle_centred();
    if (!open) {
        ui->dlg_sync = false;
        sync_cancel(ui);
        sync_free(ui);
        ImGui::End();
        return;
    }

    if (ui->sync_stage == 0) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + PX(540));
        ImGui::TextUnformatted("After a gig: read what the player saved on the stick (cues, loops, "
                               "the play history, My Settings) and choose what to take into the "
                               "library.");
        ImGui::PopTextWrapPos();
        ImGui::SeparatorText("Stick");
        volume_picker(ui, ui->stick_path, sizeof(ui->stick_path));
        if (!ui->sync_error.empty())
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_RED), "%s", ui->sync_error.c_str());
        bool reading = ui->sync_request != 0;
        ImGui::BeginDisabled(!ui->stick_path[0] || app->stick_request);
        if (ImGui::Button("Read the stick", ImVec2(PX(140), PX(28))))
            sync_read(ui);
        ImGui::EndDisabled();
        if (reading) {
            ImGui::SameLine();
            ImGui::TextDisabled("Reading the stick...");
        }
    } else if (ui->sync_stage == 1) {
        RxStickState &s = ui->sync_state;
        ImGui::Text("%d track%s on the stick", s.nb_tracks, s.nb_tracks == 1 ? "" : "s");
        if (s.nb_tracks_unknown > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%d not in the library)", s.nb_tracks_unknown);
        }
        int accepted = 0;
        for (int i = 0; i < ui->nb_sync_items; i++)
            accepted += ui->sync_items[i].accepted;
        if (ui->nb_sync_items == 0) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Nothing to sync: the stick matches the library.");
        } else {
            if (ImGui::Button("Accept all"))
                for (int i = 0; i < ui->nb_sync_items; i++)
                    ui->sync_items[i].accepted = true;
            ImGui::SameLine();
            if (ImGui::Button("Accept none"))
                for (int i = 0; i < ui->nb_sync_items; i++)
                    ui->sync_items[i].accepted = false;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(PX(170));
            const char *fname = ui->sync_filter < 0 ? "Every kind" : sync_kind_name(ui->sync_filter);
            if (ImGui::BeginCombo("##kind", fname)) {
                if (ImGui::Selectable("Every kind", ui->sync_filter < 0))
                    ui->sync_filter = -1;
                for (int k = RX_SYNC_HOT_CUE_SET; k <= RX_SYNC_SETTING; k++) {
                    if (ImGui::Selectable(sync_kind_name(k), ui->sync_filter == k))
                        ui->sync_filter = k;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%d of %d accepted", accepted, ui->nb_sync_items);

            ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_BordersInnerV |
                                 ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("##sync", 4, tf, ImVec2(0, -PX(44)))) {
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, PX(24));
                ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, PX(130));
                ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthStretch, 2);
                ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 2);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (int i = 0; i < ui->nb_sync_items; i++) {
                    RxSyncItem *it = &ui->sync_items[i];
                    if (ui->sync_filter >= 0 && (int)it->kind != ui->sync_filter)
                        continue;
                    ImGui::TableNextRow();
                    ImGui::PushID(i);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Checkbox("##acc", &it->accepted);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(sync_kind_name(it->kind));
                    ImGui::TableSetColumnIndex(2);
                    RxTrack *t = it->track_id ? rx_library_find_id(&app->lib, it->track_id) : nullptr;
                    char name[200] = "-";
                    if (t)
                        rx_track_display(t, name, sizeof(name));
                    else if (it->kind == RX_SYNC_SETTING)
                        snprintf(name, sizeof(name), "My Settings");
                    ImGui::TextUnformatted(name);
                    ImGui::TableSetColumnIndex(3);
                    char detail[256];
                    sync_detail(it, detail, sizeof(detail));
                    ImGui::TextUnformatted(detail);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::BeginDisabled(accepted == 0);
        if (ImGui::Button("Apply", ImVec2(PX(120), PX(28)))) {
            ui->sync_progress.clear();
            int ret = rx_stick_apply(&app->lib, ui->sync_items, ui->nb_sync_items, sync_progress, ui);
            ui->sync_applied = ret;
            if (ret < 0)
                ui->sync_error = std::string("Could not apply: ") + strerror(-ret);
            ui->sync_stage = 2;
            rx_app_rebuild_rows(app);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(PX(120), PX(28))))
            ui->sync_stage = 0;
    } else {
        if (ui->sync_applied >= 0) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_PLAY),
                               "Applied %d change%s to the library.",
                               ui->sync_applied, ui->sync_applied == 1 ? "" : "s");
            int counts[RX_SYNC_SETTING + 1] = {};
            for (int i = 0; i < ui->nb_sync_items; i++) {
                if (ui->sync_items[i].accepted && ui->sync_items[i].kind <= RX_SYNC_SETTING)
                    counts[ui->sync_items[i].kind]++;
            }
            for (int k = RX_SYNC_HOT_CUE_SET; k <= RX_SYNC_SETTING; k++) {
                if (counts[k])
                    ImGui::BulletText("%s: %d", sync_kind_name(k), counts[k]);
            }
        } else {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_RED), "%s", ui->sync_error.c_str());
        }
        if (!ui->sync_progress.empty())
            ImGui::TextDisabled("%s", ui->sync_progress.c_str());
        if (ImGui::Button("Done", ImVec2(PX(120), PX(28)))) {
            ui->dlg_sync = false;
            sync_cancel(ui);
            sync_free(ui);
        }
    }
    ImGui::End();
}

/* ------------------------------------------------------------------ inspect */

void ui_open_inspect(UiState *ui)
{
    rx_app_cancel_stick(ui->app, ui->inspect_request);
    ui->inspect_request = 0;
    ui->dlg_inspect = true;
    ui->inspect_done = false;
    ui->inspect_report.clear();
    ui_scan_volumes(ui);
    if (!ui->stick_path[0] && ui->volumes.size() == 1)
        snprintf(ui->stick_path, sizeof(ui->stick_path), "%s", ui->volumes[0].c_str());
}

static void inspect_dialog(UiState *ui)
{
    RxApp *app = ui->app;
    if (!ui->dlg_inspect)
        return;
    RxJobResult *r = app->stick_result;
    if (r && r->kind == RX_JOB_INSPECT) {
        app->stick_result = nullptr;
        if (ui->inspect_request && r->request == ui->inspect_request) {
            ui->inspect_request = 0;
            ui->inspect_report = r->report ? r->report : "";
            ui->inspect_result = r->err < 0 ? r->err : r->problems;
            ui->inspect_done = true;
        }
        rx_job_result_free(r);
    }
    centre_next(ui, 700, 500);
    if (!ImGui::Begin("Inspect USB", &ui->dlg_inspect,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_settle_centred();
    if (!ui->dlg_inspect) {
        // Closed: the inspection in flight is given up.
        rx_app_cancel_stick(app, ui->inspect_request);
        ui->inspect_request = 0;
    }
    volume_picker(ui, ui->stick_path, sizeof(ui->stick_path));
    ImGui::BeginDisabled(!ui->stick_path[0] || app->stick_request);
    if (ImGui::Button("Inspect", ImVec2(PX(120), 0))) {
        ui->inspect_report.clear();
        ui->inspect_done = false;
        int ret = rx_app_inspect_stick(app, ui->stick_path);
        if (ret < 0) {
            ui->inspect_result = ret;
            ui->inspect_done = true;
        } else {
            ui->inspect_request = app->stick_request;
        }
    }
    ImGui::EndDisabled();
    if (ui->inspect_request) {
        ImGui::SameLine();
        ImGui::TextDisabled("Inspecting...");
    } else if (ui->inspect_done) {
        ImGui::SameLine();
        if (ui->inspect_result < 0)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_RED), "Failed: %s",
                               strerror(-ui->inspect_result));
        else if (ui->inspect_result == 0)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_PLAY), "No problems found.");
        else
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT), "%d problem%s found.",
                               ui->inspect_result,
                               ui->inspect_result == 1 ? "" : "s");
    }
    ImGui::BeginChild("##report", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(ui->inspect_report.c_str());
    ImGui::EndChild();
    ImGui::End();
}

/* ---------------------------------------------------------------------- add */

static void fb_scan(UiState *ui)
{
    ui->fb_entries.clear();
    ui->fb_error.clear();
    ui->fb_dirty = false;
    DIR *d = opendir(ui->fb_path.c_str());
    if (!d) {
        ui->fb_error = std::string("Cannot read this folder: ") + strerror(errno);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        std::string full = ui->fb_path + "/" + e->d_name;
        struct stat stt;
        if (stat(full.c_str(), &stt) < 0) {
            if (ui->fb_error.empty())
                ui->fb_error = std::string("Cannot read ") + e->d_name + ": " + strerror(errno);
            continue;
        }
        UiFileEntry fe;
        fe.name = e->d_name;
        fe.dir = S_ISDIR(stt.st_mode);
        fe.audio = !fe.dir && rx_is_audio_path(full.c_str());
        fe.selected = false;
        if (fe.dir || fe.audio)
            ui->fb_entries.push_back(fe);
    }
    closedir(d);
    std::sort(ui->fb_entries.begin(), ui->fb_entries.end(),
              [](const UiFileEntry &a, const UiFileEntry &b) {
        if (a.dir != b.dir)
            return a.dir;
        return rx_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

static void fb_go(UiState *ui, const std::string &path)
{
    char *abs = rx_path_abs(path.c_str());
    if (!abs || !rx_dir_exists(abs)) {
        free(abs);
        return;
    }
    ui->fb_path = abs;
    free(abs);
    snprintf(ui->fb_edit, sizeof(ui->fb_edit), "%s", ui->fb_path.c_str());
    fb_scan(ui);
}

void ui_open_add(UiState *ui)
{
    ui->dlg_add = true;
    if (ui->fb_path.empty()) {
        const char *home = getenv("HOME");
        std::string music = std::string(home ? home : "/") + "/Music";
        fb_go(ui, rx_dir_exists(music.c_str()) ? music : std::string(home ? home : "/"));
    } else {
        fb_scan(ui);
    }
}

static void add_dialog(UiState *ui)
{
    RxApp *app = ui->app;
    if (!ui->dlg_add)
        return;
    centre_next(ui, 640, 520);
    if (!ImGui::Begin("Add files and folders", &ui->dlg_add,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_settle_centred();
    if (ImGui::Button("Up")) {
        std::string up = ui->fb_path;
        size_t slash = up.find_last_of('/');
        fb_go(ui, slash == 0 || slash == std::string::npos ? "/" : up.substr(0, slash));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##path", ui->fb_edit, sizeof(ui->fb_edit),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        char *e = rx_path_expand(ui->fb_edit);
        if (e)
            fb_go(ui, e);
        free(e);
    }
    int nsel = 0;
    for (const UiFileEntry &fe : ui->fb_entries)
        nsel += fe.selected;
    ImGui::BeginChild("##files", ImVec2(0, -PX(40)), ImGuiChildFlags_Borders);
    std::string go;
    for (size_t i = 0; i < ui->fb_entries.size(); i++) {
        UiFileEntry &fe = ui->fb_entries[i];
        ImGui::PushID((int)i);
        ImGui::Checkbox("##sel", &fe.selected);
        ImGui::SameLine();
        char label[600];
        snprintf(label, sizeof(label), "%s%s", fe.name.c_str(), fe.dir ? "/" : "");
        if (fe.dir)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(UI_ACCENT));
        if (ImGui::Selectable(label, fe.selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && fe.dir)
                go = ui->fb_path + "/" + fe.name;
            else
                fe.selected = !fe.selected;
        }
        if (fe.dir)
            ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (!ui->fb_error.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_RED), "%s", ui->fb_error.c_str());
    else if (ui->fb_entries.empty())
        ImGui::TextDisabled("No folders or audio files here.");
    ImGui::EndChild();
    if (!go.empty())
        fb_go(ui, go);

    // Adding probes the files and queues their analysis (app.c).
    if (nsel > 0) {
        char label[64];
        snprintf(label, sizeof(label), "Add %d selected", nsel);
        if (ImGui::Button(label, ImVec2(PX(160), PX(28)))) {
            // One item: its own status says what came of it. Several: a
            // count, the failures in it (each is in the log).
            int added = 0, failed = 0;
            for (const UiFileEntry &fe : ui->fb_entries) {
                if (!fe.selected)
                    continue;
                if (rx_app_add_path(app, (ui->fb_path + "/" + fe.name).c_str()) >= 0)
                    added++;
                else
                    failed++;
            }
            if (nsel > 1 && failed)
                rx_app_set_status(app, "Added %d of %d items; %d could not be added", added, nsel,
                                  failed);
            else if (nsel > 1)
                rx_app_set_status(app, "Added %d items", added);
            ui->dlg_add = false;
        }
    } else if (ImGui::Button("Add this folder", ImVec2(PX(160), PX(28)))) {
        // rx_app_add_path's status says what came of it.
        rx_app_add_path(app, ui->fb_path.c_str());
        ui->dlg_add = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(PX(120), PX(28))))
        ui->dlg_add = false;
    ImGui::End();
}

/* ------------------------------------------------------------ keys, about */

static void keys_dialog(UiState *ui)
{
    if (!ui->dlg_keys)
        return;
    centre_next(ui, 520, 560);
    if (!ImGui::Begin("Keyboard map", &ui->dlg_keys,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_settle_centred();
    ImGui::TextDisabled("rekordbox's Export preset, player 1.");
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##keys", 2, tf)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, PX(170));
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < ui_nb_key_rows; i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT), "%s", ui_key_rows[i].key);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(ui_key_rows[i].action);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void about_dialog(UiState *ui)
{
    if (!ui->dlg_about)
        return;
    centre_next(ui, 0, 0);
    if (!ImGui::Begin("About rxbox", &ui->dlg_about,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_settle_centred();
    ImGui::PushFont(ui->font_bold, ui->font_base * 1.6f);
    ImGui::TextUnformatted("rxbox");
    ImGui::PopFont();
    ImGui::TextDisabled("version %s", RXBOX_VERSION);
    ImGui::Spacing();
    ImGui::TextUnformatted("A DJ library manager and CDJ stick writer.");
    ImGui::TextUnformatted("Prepares cues, loops, beat grids and playlists, and writes sticks\n"
                           "that rekordbox-compatible players read (both libraries).");
    ImGui::Spacing();
    ImGui::TextDisabled("Library: %s", ui->app->lib.root ? ui->app->lib.root : "");
    ImGui::TextDisabled("Dear ImGui %s", IMGUI_VERSION);
    ImGui::TextDisabled("BSD-2-Clause. Copyright 2026, Lynne.");
    ImGui::End();
}

/* --------------------------------------------------------------------- all */

void ui_dialogs(UiState *ui)
{
    confirm_load(ui);
    confirm_remove(ui);
    relocate(ui);
    analysis_settings(ui);
    export_dialog(ui);
    sync_dialog(ui);
    inspect_dialog(ui);
    add_dialog(ui);
    keys_dialog(ui);
    about_dialog(ui);
}

void ui_dialogs_shutdown(UiState *ui)
{
    sync_free(ui);
}
