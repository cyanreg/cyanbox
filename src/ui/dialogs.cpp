/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The dialogs: the confirmation of a removal from the library, Relocate,
// Analysis settings, Player settings, Export, Sync from USB, Inspect USB,
// the Add browser (a plain directory lister, there being no native file
// dialog under a bare Wayland compositor, with a fuzzy finder over
// everything under the folder: finder.cpp), the keyboard map and About.
//
// Player settings is rekordbox's My Settings, built from cb_settings_list:
// a field settings.c gains shows up with no change here. Its values are
// CBConfig's player/mixer/device groups, set and saved at once with
// cb_library_config_save, the way Sync from USB's apply sets them. The
// interface follows several of them from the next frame on (ui.cpp,
// follow_settings), and the View menu's waveform colour and key notation
// set theirs through here (ui_setting_pick).
//
// Export follows the app's flow (cyanbox/app.h export_state): Export starts
// a dry run; when it finds stick files to prune or player changes not yet
// synced back it stops with `confirm` set, both known by then, and the
// dialog asks once ("The stick has N changes you have not synced", the
// files to delete), each button an answer to both, before the real run.
// Sync from USB reads the stick on a worker
// (cb_app_read_stick: cb_stick_read/cb_stick_diff) and lists what the
// player wrote for review; the accepted rows are applied with
// cb_stick_apply on this thread, which owns the library. Inspect USB runs
// cb_inspect on a worker too (cb_app_inspect_stick). Both take their
// result from app->stick_result when its request is theirs and drop a
// result of their kind that is not (a read given up on); closing or
// reopening either gives up its request in flight (cb_app_cancel_stick),
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

// Esc closes the dialog that has the focus, as its close button does,
// unless a text field has the keyboard: Esc is the field's then (the Add
// dialog's finder empties first, and a second Esc closes).
void ui_escape_closes(bool *open)
{
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        *open = false;
}

void ui_centre_next(UiState *ui, float w, float h)
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
        if (cb_str_starts(m.c_str(), "/run/media/") || cb_str_starts(m.c_str(), "/media/"))
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
        cb_stick_label(v.c_str(), label, sizeof(label));
        char row[1200];
        snprintf(row, sizeof(row), "%s   %s", label[0] ? label : cb_path_base(v.c_str()), v.c_str());
        if (ImGui::Selectable(row, !strcmp(path, v.c_str()))) {
            snprintf(path, size, "%s", v.c_str());
            changed = true;
        }
    }
    ImGui::EndChild();
    return changed;
}

/* ------------------------------------------------------------ confirmations */

// The first folder of `path` that is not there ("" when its own folder
// is): a drive not plugged in, a folder moved or renamed.
static std::string first_gone(const char *path)
{
    std::string p = path;
    size_t end = p.rfind('/');
    for (size_t at = p.find('/', 1); at != std::string::npos && at <= end; at = p.find('/', at + 1)) {
        std::string dir = p.substr(0, at);
        struct stat st;
        if (stat(dir.c_str(), &st) < 0 || !S_ISDIR(st.st_mode))
            return dir;
    }
    return "";
}

void ui_open_remove_missing(UiState *ui)
{
    CBApp *app = ui->app;
    if (cb_app_rescan(app) < 0)
        return;
    ui->remove_ids.clear();
    ui->remove_gone.clear();
    for (int i = 0; i < app->lib.nb_tracks; i++) {
        const CBTrack *t = app->lib.tracks[i];
        if (!t->missing)
            continue;
        ui->remove_ids.push_back(t->id);
        std::string gone = first_gone(t->path);
        if (gone.empty())
            continue;
        auto g = std::find_if(ui->remove_gone.begin(), ui->remove_gone.end(),
                              [&](const auto &x) { return x.first == gone; });
        if (g != ui->remove_gone.end())
            g->second++;
        else
            ui->remove_gone.emplace_back(gone, 1);
    }
    if (ui->remove_ids.empty()) {
        cb_app_set_status(app, "No track is missing its file");
        return;
    }
    std::sort(ui->remove_gone.begin(), ui->remove_gone.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    ui->remove_missing = true;
    ui->want_confirm_remove = true;
}

// The missing tracks the question is about, and the folders gone with
// them, which say the files may well be fine on a drive not plugged in.
static void missing_list(UiState *ui)
{
    CBApp *app = ui->app;
    int n = (int)ui->remove_ids.size();
    ImGui::Text("Remove %d track%s whose file%s cannot be found?", n, n == 1 ? "" : "s",
                n == 1 ? "" : "s");
    float row = ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##missing", ImVec2(PX(620), row * (float)std::min(n, 8) + PX(10)),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clip;
    clip.Begin(n);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const CBTrack *t = cb_library_find_id(&app->lib, ui->remove_ids[(size_t)i]);
            ImGui::TextUnformatted(t ? t->path : "?");
        }
    }
    ImGui::EndChild();
    if (!ui->remove_gone.empty()) {
        int k = 0;
        for (const auto &g : ui->remove_gone)
            k += g.second;
        ImGui::Spacing();
        bool one = ui->remove_gone.size() == 1;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT), "%d of them %s in %s gone too:",
                           k, k == 1 ? "is" : "are", one ? "a folder that is" : "folders that are");
        size_t shown = std::min(ui->remove_gone.size(), (size_t)4);
        for (size_t i = 0; i < shown; i++)
            ImGui::BulletText("%s (%d)", ui->remove_gone[i].first.c_str(), ui->remove_gone[i].second);
        if (ui->remove_gone.size() > shown)
            ImGui::BulletText("and %zu more", ui->remove_gone.size() - shown);
        ImGui::TextDisabled("A drive not plugged in, or a folder moved? Plug it in and rescan,\n"
                            "or relocate the tracks, to keep their cues and grids.");
    }
    ImGui::TextDisabled("Cues, grids and playlist entries go with them.");
}

static void confirm_remove(UiState *ui)
{
    CBApp *app = ui->app;
    if (ui->want_confirm_remove) {
        ImGui::OpenPopup("Remove from library?");
        ui->want_confirm_remove = false;
    }
    ui_centre_next(ui, 0, 0);
    if (!ImGui::BeginPopupModal("Remove from library?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ui_settle_centred();
    int n = (int)ui->remove_ids.size();
    if (ui->remove_missing) {
        missing_list(ui);
    } else {
        ImGui::Text("Remove %d track%s from the library?", n, n == 1 ? "" : "s");
        ImGui::TextDisabled("Cues, grids and playlist entries go with them. The audio files stay.");
    }
    ImGui::Spacing();
    bool remove = ImGui::Button("Remove", ImVec2(PX(110), 0));
    ui->remove_button = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
    if (remove) {
        int removed = 0;
        for (uint64_t id : ui->remove_ids) {
            CBTrack *t = cb_library_find_id(&app->lib, id);
            if (!t)
                continue;
            for (int d = 0; d < CB_DECKS; d++) {
                if (app->decks[d].track == t || app->decks[d].loading_track_id == id)
                    cb_app_unload_deck(app, d);
            }
            if (cb_library_remove(&app->lib, t) >= 0)
                removed++;
        }
        ui->remove_ids.clear();
        ui->remove_gone.clear();
        ui->remove_missing = false;
        app->nb_selection = 0;
        app->focus_track = 0;
        cb_app_rebuild_rows(app);
        cb_app_set_status(app, "Removed %d track%s from the library", removed, removed == 1 ? "" : "s");
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(PX(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui->remove_ids.clear();
        ui->remove_gone.clear();
        ui->remove_missing = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* ----------------------------------------------------------------- relocate */

void ui_open_relocate(UiState *ui, CBTrack *t)
{
    ui->relocate_id = t->id;
    snprintf(ui->relocate_buf, sizeof(ui->relocate_buf), "%s", t->path);
    ui->want_relocate = true;
}

static void relocate(UiState *ui)
{
    CBApp *app = ui->app;
    if (ui->want_relocate) {
        ImGui::OpenPopup("Relocate");
        ui->want_relocate = false;
    }
    ui_centre_next(ui, 560, 0);
    if (!ImGui::BeginPopupModal("Relocate", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ui_settle_centred();
    CBTrack *t = cb_library_find_id(&app->lib, ui->relocate_id);
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
    char *expanded = cb_path_expand(ui->relocate_buf);
    bool exists = expanded && cb_file_exists(expanded);
    if (!exists)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT), "No file there.");
    else
        ImGui::TextDisabled("Found.");
    ImGui::BeginDisabled(!exists);
    if (ImGui::Button("Relocate", ImVec2(PX(110), 0)) || (enter && exists)) {
        char *abs = cb_path_abs(expanded);
        int ret = abs ? cb_library_relocate(&app->lib, t, abs) : CB_ERROR(ENOENT);
        if (ret < 0)
            cb_app_set_status(app, "Could not relocate: %s", strerror(-ret));
        else
            cb_app_set_status(app, "Relocated %s", ui_title(t));
        free(abs);
        cb_app_rebuild_rows(app);
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

// The legend: a swatch, the colour's name and its words per row, edited in
// place and saved when a field is left. A field shows CBConfig's words
// unless it is being typed in.
static bool legend_section(UiState *ui, CBConfig *cfg)
{
    bool changed = false;
    ImGui::SeparatorText("Track colours");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + PX(360));
    ImGui::TextDisabled("Analyse tags gives a track the colour whose words fit it best: say "
                        "what can be heard (energy, brightness, darkness, groove).");
    ImGui::PopTextWrapPos();
    float name_w = ImGui::CalcTextSize("purple").x + PX(8);
    for (int id = 1; id <= CB_TRACK_COLORS; id++) {
        ImGui::PushID(id);
        char *buf = ui->vibe_buf[id - 1];
        ImGui::AlignTextToFramePadding();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float s = PX(14), fh = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + (fh - s) * 0.5f),
                                                  ImVec2(p.x + s, p.y + (fh + s) * 0.5f),
                                                  ui_rgb(cb_track_color_rgb(id)), PX(2));
        ImGui::Dummy(ImVec2(s, fh));
        ImGui::SameLine();
        float x = ImGui::GetCursorPosX();
        ImGui::TextUnformatted(cb_track_color_name(id));
        ImGui::SameLine(x + name_w);
        ImGui::SetNextItemWidth(PX(300));
        if (ImGui::GetActiveID() != ImGui::GetID("##vibe"))
            snprintf(buf, sizeof(ui->vibe_buf[0]), "%s", cb_config_vibe(cfg, id));
        ImGui::InputText("##vibe", buf, sizeof(ui->vibe_buf[0]));
        if (ImGui::IsItemDeactivatedAfterEdit() && cb_config_set_vibe(cfg, id, buf) >= 0)
            changed = true;
        char tip[192];
        snprintf(tip, sizeof(tip), "%s; by default \"%s\"", cb_config_vibe_key(id),
                 cb_config_vibe_default(id));
        ui_tooltip(tip);
        ImGui::PopID();
    }
    bool defaults = true;
    for (int id = 1; id <= CB_TRACK_COLORS; id++)
        defaults &= !cfg->vibes[id - 1];
    ImGui::BeginDisabled(defaults);
    if (ImGui::Button("Reset to defaults")) {
        for (int id = 1; id <= CB_TRACK_COLORS; id++)
            cb_config_set_vibe(cfg, id, NULL);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Tagged tracks keep their colour until tagged again.");
    return changed;
}

static void analysis_settings(UiState *ui)
{
    if (!ui->dlg_analysis)
        return;
    CBConfig *cfg = &ui->app->lib.cfg;
    ui_centre_next(ui, 380, 0);
    if (!ImGui::Begin("Analysis settings", &ui->dlg_analysis, ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_escape_closes(&ui->dlg_analysis);
    ui_settle_centred();
    bool changed = false;
    ImGui::SeparatorText("Analyse");
    bool beats = cfg->analyse_what & CB_ANALYSE_BEATS, key = cfg->analyse_what & CB_ANALYSE_KEY;
    if (ImGui::Checkbox("BPM and beat grid", &beats) | ImGui::Checkbox("Key", &key)) {
        cfg->analyse_what = CB_ANALYSE_WAVES | (beats ? CB_ANALYSE_BEATS : 0) |
                            (key ? CB_ANALYSE_KEY : 0);
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
    ImGui::Spacing();
    ImGui::TextDisabled("Applies to the next analysis.");
    changed |= legend_section(ui, cfg);
    if (changed)
        ui_save_config(ui);
    ImGui::End();
}

/* ---------------------------------------------------------- player settings */

void ui_open_settings(UiState *ui)
{
    ui->dlg_settings = true;
    ui->settings_focus = true;
}

static CBKv **setting_group(CBConfig *cfg, int section, int **nb)
{
    switch (section) {
    case UI_SETTINGS_PLAYER:
        *nb = &cfg->nb_player;
        return &cfg->player;
    case UI_SETTINGS_MIXER:
        *nb = &cfg->nb_mixer;
        return &cfg->mixer;
    default:
        *nb = &cfg->nb_device;
        return &cfg->device;
    }
}

// The library's value: CBConfig's copy of the "<prefix>.<name>" row, NULL
// when there is none (the field is at its default).
static const char *setting_stored(UiState *ui, const UiSetting &f)
{
    int *nb;
    CBKv **kv = setting_group(&ui->app->lib.cfg, f.section, &nb);
    for (int i = 0; i < *nb; i++) {
        if (!strcmp((*kv)[i].key, f.name.c_str()))
            return (*kv)[i].value ? (*kv)[i].value : "";
    }
    return nullptr;
}

// The value the export would write, an index into the field's values; -1
// for a value it would refuse (`cyanbox config` takes anything).
static int setting_current(UiState *ui, const UiSetting &f)
{
    const char *v = setting_stored(ui, f);
    if (!v)
        return f.deflt;
    const char *listed = cb_settings_value(f.file.c_str(), f.name.c_str(), v);
    for (int i = 0; listed && i < (int)f.values.size(); i++) {
        if (f.values[i] == listed)
            return i;
    }
    return -1;
}

// Sets one field in CBConfig; the caller saves it. The row lands in the
// setting table as `cyanbox config player.NAME VALUE` writes it, and CBConfig
// stays in step, as Sync from USB keeps it (cb_stick_apply): written with
// cb_library_setting_set alone, the row would be put back to CBConfig's old
// copy by the next cb_library_config_save (any preference changed).
static int setting_put(UiState *ui, const UiSetting &f, const std::string &value)
{
    int *nb;
    CBKv **kv = setting_group(&ui->app->lib.cfg, f.section, &nb);
    char *v = cb_strdup(value.c_str());
    if (!v)
        return CB_ERROR(ENOMEM);
    for (int i = 0; i < *nb; i++) {
        if (!strcmp((*kv)[i].key, f.name.c_str())) {
            free((*kv)[i].value);
            (*kv)[i].value = v;
            return 0;
        }
    }
    char *k = cb_strdup(f.name.c_str());
    CBKv *n = k ? (CBKv *)realloc(*kv, (size_t)(*nb + 1) * sizeof(*n)) : nullptr;
    if (!n) {
        free(k);
        free(v);
        return CB_ERROR(ENOMEM);
    }
    *kv = n;
    n[(*nb)++] = CBKv { k, v };
    return 0;
}

// A value picked: into the library at once, and said in the status bar.
static void setting_write(UiState *ui, const UiSetting &f, int value)
{
    CBApp *app = ui->app;
    int ret = setting_put(ui, f, f.values[value]);
    if (ret >= 0)
        ret = cb_library_config_save(&app->lib);
    if (ret < 0) {
        cb_app_set_status(app, "Could not save the setting: %s", strerror(-ret));
        return;
    }
    char label[64], text[64];
    ui_setting_label(f.name.c_str(), label, sizeof(label));
    ui_setting_value(f.name.c_str(), f.values[value].c_str(), text, sizeof(text));
    cb_app_set_status(app, "%s setting %s: %s", ui_setting_titles[f.section], label, text);
}

void ui_setting_pick(UiState *ui, const char *key, const char *value)
{
    const char *name;
    int section = ui_setting_key(key, &name);
    for (const UiSetting &f : ui_settings(ui)) {
        if (f.section != section || f.name != name)
            continue;
        for (int i = 0; i < (int)f.values.size(); i++) {
            if (f.values[i] == value) {
                setting_write(ui, f, i);
                return;
            }
        }
    }
    cb_warn("ui: %s = %s is not a My Setting", key, value);
}

// Every field of the section that is not at its default goes back to it.
// The rows stay, holding the defaults: a section the library has rows for
// is written to the stick, so the files a past export left there are
// replaced by defaults rather than left as they were.
static void settings_reset(UiState *ui, int section)
{
    CBApp *app = ui->app;
    int changed = 0, ret = 0;
    for (const UiSetting &f : ui_settings(ui)) {
        if (f.section != section || setting_current(ui, f) == f.deflt)
            continue;
        if ((ret = setting_put(ui, f, f.values[f.deflt])) < 0)
            break;
        changed++;
    }
    if (changed) {
        int err = cb_library_config_save(&app->lib);
        if (ret >= 0)
            ret = err;
    }
    if (ret < 0)
        cb_app_set_status(app, "Could not save the settings: %s", strerror(-ret));
    else
        cb_app_set_status(app, "%s settings back to the defaults (%d changed)",
                          ui_setting_titles[section], changed);
}

// One field: its label (in the accent colour when not at its default; the
// key and the default on hover) and a combo of its values.
static void setting_row(UiState *ui, const UiSetting &f, float combo_w)
{
    int cur = setting_current(ui, f);
    const char *name = f.name.c_str();
    char label[64], text[64];
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ui_setting_label(name, label, sizeof(label));
    if (cur != f.deflt)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT), "%s", label);
    else
        ImGui::TextUnformatted(label);
    ui_setting_value(name, f.values[f.deflt].c_str(), text, sizeof(text));
    ImGui::SetItemTooltip("%s.%s, by default %s", ui_setting_prefixes[f.section], name, text);

    ImGui::TableNextColumn();
    char preview[96];
    if (cur >= 0)
        ui_setting_value(name, f.values[cur].c_str(), preview, sizeof(preview));
    else
        snprintf(preview, sizeof(preview), "%s (not valid)", setting_stored(ui, f));
    ImGui::PushID(name);
    ImGui::SetNextItemWidth(combo_w);
    if (cur < 0)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(UI_RED));
    bool open = ImGui::BeginCombo("##value", preview, ImGuiComboFlags_HeightLarge);
    if (cur < 0) {
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Not a value the players know: the export refuses it until one is picked.");
    }
    if (open) {
        const float dw = ImGui::CalcTextSize("default").x;
        for (int i = 0; i < (int)f.values.size(); i++) {
            ImGui::PushID(i);
            ui_setting_value(name, f.values[i].c_str(), text, sizeof(text));
            if (ImGui::Selectable(text, i == cur) && i != cur)
                setting_write(ui, f, i);
            if (i == f.deflt) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddText(ImVec2(b.x - dw - ImGui::GetStyle().ItemSpacing.x,
                                                           a.y), UI_DIM, "default");
            }
            if (i == cur)
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

// A section's fields in two columns, the first half down the left: the
// player's 29 fit a 720 px high window without scrolling. Every tab is
// padded to `rows` so the dialog keeps its size from tab to tab.
static void settings_tab(UiState *ui, int section, int rows, float label_w, float combo_w)
{
    std::vector<const UiSetting *> list;
    std::string files;
    for (const UiSetting &f : ui_settings(ui)) {
        if (f.section != section)
            continue;
        list.push_back(&f);
        if (files.find(f.file) == std::string::npos)
            files += (files.empty() ? "" : ", ") + f.file;
    }
    ImGui::TextDisabled("On the stick in %s.%s", files.c_str(), section == UI_SETTINGS_DEVICE ?
                        " The player applies these as it reads the stick, without LOAD." : "");
    ImGui::Spacing();

    int n = (int)list.size(), half = (n + 1) / 2;
    float y0 = ImGui::GetCursorPosY();
    ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("##fields", 5, tf)) {
        ImGui::TableSetupColumn("##label1", ImGuiTableColumnFlags_WidthFixed, label_w);
        ImGui::TableSetupColumn("##value1", ImGuiTableColumnFlags_WidthFixed, combo_w);
        ImGui::TableSetupColumn("##gap", ImGuiTableColumnFlags_WidthFixed, PX(24));
        ImGui::TableSetupColumn("##label2", ImGuiTableColumnFlags_WidthFixed, label_w);
        ImGui::TableSetupColumn("##value2", ImGuiTableColumnFlags_WidthFixed, combo_w);
        for (int r = 0; r < half; r++) {
            ImGui::TableNextRow();
            setting_row(ui, *list[r], combo_w);
            ImGui::TableNextColumn();
            if (r + half < n)
                setting_row(ui, *list[r + half], combo_w);
        }
        ImGui::EndTable();
    }
    float want = rows * (ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().CellPadding.y);
    float used = ImGui::GetCursorPosY() - y0;
    if (want > used)
        ImGui::Dummy(ImVec2(0, want - used));

    ImGui::Spacing();
    bool any = false;
    for (const UiSetting *f : list)
        any |= setting_current(ui, *f) != f->deflt;
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Reset to defaults", ImVec2(PX(160), PX(28))))
        settings_reset(ui, section);
    ImGui::EndDisabled();
    if (!any) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Every %s setting is at its default.", ui_setting_prefixes[section]);
    }
}

// rekordbox's My Settings: what the export writes for the players to LOAD.
// Generated from cb_settings_list; the values are the library's (CBConfig's
// player/mixer/device groups), so the dialog keeps nothing of its own.
static void settings_dialog(UiState *ui)
{
    if (!ui->dlg_settings)
        return;
    ui_centre_next(ui, 0, 0);
    if (ui->settings_focus) {
        ImGui::SetNextWindowFocus();
        ui->settings_focus = false;
    }
    if (!ImGui::Begin("Player settings", &ui->dlg_settings, ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_escape_closes(&ui->dlg_settings);
    ui_settle_centred();

    // Columns as wide as the widest label and value of any section, room
    // for the dropdown's "default" beside the value.
    const ImGuiStyle &st = ImGui::GetStyle();
    float label_w = 0, value_w = 0;
    int count[UI_NB_SETTING_SECTIONS] = {}, rows = 0;
    for (const UiSetting &f : ui_settings(ui)) {
        char buf[96];
        label_w = ImMax(label_w, ImGui::CalcTextSize(ui_setting_label(f.name.c_str(), buf, sizeof(buf))).x);
        for (const std::string &v : f.values) {
            ui_setting_value(f.name.c_str(), v.c_str(), buf, sizeof(buf));
            value_w = ImMax(value_w, ImGui::CalcTextSize(buf).x);
        }
        count[f.section]++;
    }
    for (int s = 0; s < UI_NB_SETTING_SECTIONS; s++)
        rows = ImMax(rows, (count[s] + 1) / 2);
    label_w += PX(12);
    float combo_w = value_w + ImGui::CalcTextSize("default").x + st.ItemSpacing.x * 2 +
                    ImGui::GetFrameHeight() + st.FramePadding.x * 2;
    float table_w = 2 * (label_w + combo_w) + PX(24) + 10 * st.CellPadding.x;

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + table_w);
    ImGui::TextWrapped("Written to the USB stick on export as the players' My Settings; "
                       "LOAD them on the player under its settings.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::BeginTabBar("##sections")) {
        for (int s = 0; s < UI_NB_SETTING_SECTIONS; s++) {
            if (!ImGui::BeginTabItem(ui_setting_titles[s]))
                continue;
            settings_tab(ui, s, rows, label_w, combo_w);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

/* ------------------------------------------------------------------- export */

void ui_open_export(UiState *ui)
{
    CBApp *app = ui->app;
    ui->dlg_export = true;
    app->export_state.open = true;
    // Closed on its summary with the title bar's x: the choices again.
    if (!app->export_state.running)
        ui->export_seen_running = false;
    ui_scan_volumes(ui);
    if (!app->export_state.dest[0] && ui->volumes.size() == 1)
        snprintf(app->export_state.dest, sizeof(app->export_state.dest), "%s", ui->volumes[0].c_str());
}

static bool playlist_in(const CBPlaylistFolder *f, const CBPlaylist *p)
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
// dialog is open, and the pointer must not reach cb_app_start_export().
static void playlists_prune(UiState *ui)
{
    const CBPlaylistFolder *root = ui->app->lib.root_folder;
    auto &sel = ui->export_playlists;
    sel.erase(std::remove_if(sel.begin(), sel.end(), [root](CBPlaylist *p) {
        return !root || !playlist_in(root, p);
    }), sel.end());
}

static void playlist_checks(UiState *ui, CBPlaylistFolder *f)
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
        CBPlaylist *p = f->playlists[i];
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
    CBApp *app = ui->app;
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
    int ret = cb_app_start_export(app);
    if (ret < 0) {
        cb_app_set_status(app, "Export could not start: %s", strerror(-ret));
        return;
    }
    ui->export_seen_running = true;
}

// The players' background colour for the stick's library, which the
// databases carry (export.background_color): default or one of the eight
// track colours. Saved when picked, as the library's other settings are:
// the export reads the library's settings when it starts.
static void background_picker(UiState *ui)
{
    CBApp *app = ui->app;
    CBConfig *cfg = &app->lib.cfg;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Background colour on the players");
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(PX(4), PX(4)));
    int picked = -1;
    if (ui_swatch(ui, "##bgdefault", 0, cfg->background_color == 0, 18))
        picked = 0;
    ui_tooltip("default (the player's own)");
    for (int i = 1; i <= CB_TRACK_COLORS; i++) {
        char id[16];
        snprintf(id, sizeof(id), "##bg%d", i);
        ImGui::SameLine();
        if (ui_swatch(ui, id, cb_track_color_rgb(i), cfg->background_color == i, 18))
            picked = i;
        ui_tooltip(cb_track_color_name(i));
    }
    ImGui::PopStyleVar();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", cfg->background_color ? cb_track_color_name(cfg->background_color)
                                                    : "default");
    if (picked >= 0 && picked != cfg->background_color) {
        int old = cfg->background_color;
        cfg->background_color = picked;
        int ret = cb_library_config_save(&app->lib);
        if (ret < 0) {
            cfg->background_color = old;
            cb_app_set_status(app, "The background colour could not be saved: %s",
                              strerror(-ret));
        }
    }
}

static void export_dialog(UiState *ui)
{
    CBApp *app = ui->app;
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
    ui_escape_closes(&open);
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
        // What the stick's audio is (CBStickFormat): encoded from the
        // decoded samples as it is written, when not copied.
        static const char *const formats[] = {
            "Each file as it is",
            "FLAC (the lossless files)",
            "WAV, 16-bit (players before the CDJ-2000NXS2)",
            "AIFF, 16-bit (players before the CDJ-2000NXS2)",
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Audio");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(PX(360));
        ImGui::Combo("##format", &es.format, formats, IM_ARRAYSIZE(formats));
        ui_tooltip("WAV and AIFF are for the CDJ-900 and the other players that read no FLAC; "
                   "MP3s go as they are. What is not copied is encoded as it is written, "
                   "from the very audio the analysis was made of.");
        ImGui::Checkbox("Compatibility transcode (AAC and hi-res to 16-bit/44.1 kHz FLAC)", &es.compat);
        background_picker(ui);
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
            if (cb_app_start_export(app) < 0)
                cb_app_set_status(app, "Export could not continue");
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
        // The copy's bar is its bytes on the stick, the other stages' their steps.
        bool copying = !strcmp(es.stage, "copy") && es.bytes_total > 0;
        float frac = copying ? (float)((double)es.bytes_done / (double)es.bytes_total)
                   : es.total > 0 ? (float)es.done / es.total : 0.0f;
        if (es.running) {
            ImGui::Text("%s", es.stage[0] ? es.stage : "starting");
            ImGui::SameLine();
            ImGui::TextDisabled("%d / %d", es.done, es.total);
            char rate[64];
            cb_export_rate_text(copying ? es.rate : 0, es.seconds_left, rate, sizeof(rate));
            if (rate[0]) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", rate);
            }
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, PX(6)), "");
            ImGui::TextDisabled("%s", es.item);
        } else if (es.result == CB_ERROR(ECANCELED)) {
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
        cb_stick_state_free(&ui->sync_state);
    ui->sync_state_valid = false;
    free(ui->sync_items);
    ui->sync_items = nullptr;
    ui->nb_sync_items = 0;
}

// Gives up the read in flight, if any: the dialog closes or starts afresh.
static void sync_cancel(UiState *ui)
{
    cb_app_cancel_stick(ui->app, ui->sync_request);
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
    case CB_SYNC_HOT_CUE_SET:       return "Hot cue set";
    case CB_SYNC_HOT_CUE_CLEARED:   return "Hot cue cleared";
    case CB_SYNC_MEMORY_ADDED:      return "Memory cue added";
    case CB_SYNC_MEMORY_REMOVED:    return "Memory cue removed";
    case CB_SYNC_PLAY:              return "Play";
    case CB_SYNC_SETTING:           return "Setting";
    default:                        return "?";
    }
}

static void sync_detail(const CBSyncItem *it, char *buf, size_t size)
{
    char t[32], len[32] = "";
    switch (it->kind) {
    case CB_SYNC_HOT_CUE_SET:
    case CB_SYNC_HOT_CUE_CLEARED:
    case CB_SYNC_MEMORY_ADDED:
    case CB_SYNC_MEMORY_REMOVED:
        ui_fmt_time(it->cue.time, t, sizeof(t), true);
        if (cb_cue_is_loop(&it->cue))
            snprintf(len, sizeof(len), ", loop to %s", ui_fmt_time(it->cue.loop_end, t + 16, 16, true));
        if (it->kind == CB_SYNC_HOT_CUE_SET || it->kind == CB_SYNC_HOT_CUE_CLEARED)
            snprintf(buf, size, "%c at %.15s%s%s%s", 'A' + ImClamp(it->slot, 0, 7), t, len,
                     it->cue.name[0] ? " - " : "", it->cue.name);
        else
            snprintf(buf, size, "at %.15s%s%s%s", t, len, it->cue.name[0] ? " - " : "", it->cue.name);
        break;
    case CB_SYNC_PLAY:
        if (it->played_at > 0) {
            time_t tt = (time_t)it->played_at;
            struct tm tm;
            localtime_r(&tt, &tm);
            strftime(buf, size, "played %Y-%m-%d %H:%M", &tm);
        } else {
            snprintf(buf, size, "played (history entry %d, no time)", it->play_index + 1);
        }
        break;
    case CB_SYNC_SETTING: {
        // As the Player settings dialog shows them: both values are in
        // the listed form (cb_stick_diff decodes the stick's bytes and the
        // library's value alike).
        const char *name;
        ui_setting_key(it->setting_key, &name);
        char label[64], was[64] = "(default)", now[64];
        ui_setting_label(name, label, sizeof(label));
        if (it->setting_old[0])
            ui_setting_value(name, it->setting_old, was, sizeof(was));
        ui_setting_value(name, it->setting_value, now, sizeof(now));
        snprintf(buf, size, "%s: %s -> %s", label, was, now);
        break;
    }
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
    CBApp *app = ui->app;
    sync_free(ui);
    ui->sync_error.clear();
    int ret = cb_app_read_stick(app, ui->stick_path);
    if (ret < 0) {
        ui->sync_error = std::string("Could not read the stick: ") + strerror(-ret);
        return;
    }
    ui->sync_request = app->stick_request;
    cb_app_set_status(app, "Reading %s...", ui->stick_path);
}

// The read's result, once the worker has posted it. A read's result for
// another request is one given up on: dropped, never shown as this one's.
static void sync_take(UiState *ui)
{
    CBApp *app = ui->app;
    CBJobResult *r = app->stick_result;
    if (!r || r->kind != CB_JOB_STICK_READ)
        return;
    app->stick_result = nullptr;
    if (!ui->sync_request || r->request != ui->sync_request) {
        cb_job_result_free(r);
        return;
    }
    ui->sync_request = 0;
    if (r->stick) {
        ui->sync_state = *r->stick;
        ui->sync_state_valid = true;
        free(r->stick);
        r->stick = nullptr;
    }
    if (r->err == CB_ERROR(ENOENT) && !ui->sync_state_valid) {
        // cb_stick_read: no export.pdb, no exportLibrary.db, no My Settings.
        ui->sync_error = std::string(ui->stick_path) + " is not a stick: it holds no rekordbox "
                         "database or player settings.";
        cb_job_result_free(r);
        return;
    }
    if (r->err < 0) {
        ui->sync_error = std::string(ui->sync_state_valid ?
                                     "Could not compare the stick with the library: " :
                                     "Could not read the stick: ") + strerror(-r->err);
        cb_job_result_free(r);
        return;
    }
    ui->sync_items = r->sync_items;
    ui->nb_sync_items = r->nb_sync_items;
    r->sync_items = nullptr;
    cb_job_result_free(r);
    for (int i = 0; i < ui->nb_sync_items; i++)
        ui->sync_items[i].accepted = true;
    ui->sync_stage = 1;
    cb_app_set_status(app, "%d change%s on %s", ui->nb_sync_items, ui->nb_sync_items == 1 ? "" : "s",
                      ui->stick_path);
}

static void sync_dialog(UiState *ui)
{
    CBApp *app = ui->app;
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
    ui_escape_closes(&open);
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
        CBStickState &s = ui->sync_state;
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
                for (int k = CB_SYNC_HOT_CUE_SET; k <= CB_SYNC_SETTING; k++) {
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
                    CBSyncItem *it = &ui->sync_items[i];
                    if (ui->sync_filter >= 0 && (int)it->kind != ui->sync_filter)
                        continue;
                    ImGui::TableNextRow();
                    ImGui::PushID(i);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Checkbox("##acc", &it->accepted);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(sync_kind_name(it->kind));
                    ImGui::TableSetColumnIndex(2);
                    CBTrack *t = it->track_id ? cb_library_find_id(&app->lib, it->track_id) : nullptr;
                    char name[200] = "-";
                    const char *field;
                    int section = it->kind == CB_SYNC_SETTING ?
                                  ui_setting_key(it->setting_key, &field) : -1;
                    if (t)
                        cb_track_display(t, name, sizeof(name));
                    else if (section >= 0)
                        snprintf(name, sizeof(name), "%s settings", ui_setting_titles[section]);
                    else if (it->kind == CB_SYNC_SETTING)
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
            int ret = cb_stick_apply(&app->lib, ui->sync_items, ui->nb_sync_items, sync_progress, ui);
            ui->sync_applied = ret;
            if (ret < 0)
                ui->sync_error = std::string("Could not apply: ") + strerror(-ret);
            ui->sync_stage = 2;
            cb_app_rebuild_rows(app);
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
            int counts[CB_SYNC_SETTING + 1] = {};
            for (int i = 0; i < ui->nb_sync_items; i++) {
                if (ui->sync_items[i].accepted && ui->sync_items[i].kind <= CB_SYNC_SETTING)
                    counts[ui->sync_items[i].kind]++;
            }
            for (int k = CB_SYNC_HOT_CUE_SET; k <= CB_SYNC_SETTING; k++) {
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
    cb_app_cancel_stick(ui->app, ui->inspect_request);
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
    CBApp *app = ui->app;
    if (!ui->dlg_inspect)
        return;
    CBJobResult *r = app->stick_result;
    if (r && r->kind == CB_JOB_INSPECT) {
        app->stick_result = nullptr;
        if (ui->inspect_request && r->request == ui->inspect_request) {
            ui->inspect_request = 0;
            ui->inspect_report = r->report ? r->report : "";
            ui->inspect_result = r->err < 0 ? r->err : r->problems;
            ui->inspect_done = true;
        }
        cb_job_result_free(r);
    }
    ui_centre_next(ui, 700, 500);
    if (!ImGui::Begin("Inspect USB", &ui->dlg_inspect,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_escape_closes(&ui->dlg_inspect);
    ui_settle_centred();
    if (!ui->dlg_inspect) {
        // Closed: the inspection in flight is given up.
        cb_app_cancel_stick(app, ui->inspect_request);
        ui->inspect_request = 0;
    }
    volume_picker(ui, ui->stick_path, sizeof(ui->stick_path));
    ImGui::BeginDisabled(!ui->stick_path[0] || app->stick_request);
    if (ImGui::Button("Inspect", ImVec2(PX(120), 0))) {
        ui->inspect_report.clear();
        ui->inspect_done = false;
        int ret = cb_app_inspect_stick(app, ui->stick_path);
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

// What the Add dialog lists in one folder, and what its finder shows
// (finder.cpp) when something is typed in its field: every folder and
// audio file under the folder, best match first. Ticks are kept by
// absolute path, so they survive moving between folders and between the
// listing and the finder, and one Add adds them all.

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
        fe.audio = !fe.dir && cb_is_audio_path(full.c_str());
        if (fe.dir || fe.audio)
            ui->fb_entries.push_back(fe);
    }
    closedir(d);
    std::sort(ui->fb_entries.begin(), ui->fb_entries.end(),
              [](const UiFileEntry &a, const UiFileEntry &b) {
        if (a.dir != b.dir)
            return a.dir;
        return cb_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

static void fb_go(UiState *ui, const std::string &path)
{
    char *abs = cb_path_abs(path.c_str());
    if (!abs || !cb_dir_exists(abs)) {
        free(abs);
        return;
    }
    ui->fb_path = abs;
    free(abs);
    snprintf(ui->fb_edit, sizeof(ui->fb_edit), "%s", ui->fb_path.c_str());
    ui->fb_cursor = 0;
    fb_scan(ui);
}

// `rel` under the folder shown.
static std::string fb_abs(const UiState *ui, const std::string &rel)
{
    return ui->fb_path == "/" ? "/" + rel : ui->fb_path + "/" + rel;
}

static void fb_toggle(UiState *ui, const std::string &abs)
{
    if (!ui->fb_picked.erase(abs))
        ui->fb_picked.insert(abs);
}

// The home directory as ~, for the finder's status line.
static std::string fb_short(const std::string &path)
{
    const char *home = getenv("HOME");
    size_t n = home ? strlen(home) : 0;
    if (n > 1 && path.compare(0, n, home) == 0 && (path.size() == n || path[n] == '/'))
        return "~" + path.substr(n);
    return path;
}

void ui_open_add(UiState *ui)
{
    ui->dlg_add = true;
    ui->fb_picked.clear();
    ui->fb_find[0] = '\0';
    ui->fb_find_focus = true;
    ui->fb_cursor = 0;
    if (ui->fb_path.empty()) {
        const char *home = getenv("HOME");
        std::string music = std::string(home ? home : "/") + "/Music";
        fb_go(ui, cb_dir_exists(music.c_str()) ? music : std::string(home ? home : "/"));
    } else {
        fb_scan(ui);
    }
}

// A finder row's path, the folders it is in dimmed, its name in the
// listing's colours, the letters the query matched lit.
static void fb_path_text(UiState *ui, ImVec2 pos, const UiFinderEntry &fe, float max_w)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float size = ImGui::GetFontSize();
    std::string text = fe.rel + (fe.dir ? "/" : "");
    int hits[64], nh = 0;
    cb_fuzzy_match(fe.rel.c_str(), ui->fb_find, nullptr, hits, 64, &nh);
    std::vector<bool> lit(text.size(), false);
    for (int i = 0; i < nh; i++) {
        if (hits[i] >= 0 && (size_t)hits[i] < lit.size())
            lit[(size_t)hits[i]] = true;
    }
    size_t base = fe.rel.find_last_of('/');
    base = base == std::string::npos ? 0 : base + 1;

    const char *s = text.c_str();
    float x = pos.x, right = pos.x + max_w;
    size_t i = 0;
    while (i < text.size() && x < right) {
        // One run of characters of the same colour.
        auto colour = [&](size_t at) -> ImU32 {
            if (lit[at])
                return UI_PLAY;
            if (at < base)
                return UI_DIM;
            return fe.dir ? UI_ACCENT : UI_TEXT;
        };
        ImU32 col = colour(i);
        size_t j = i;
        while (j < text.size()) {
            size_t k = j + 1;
            while (k < text.size() && ((unsigned char)s[k] & 0xC0) == 0x80)
                k++;
            if (colour(j) != col)
                break;
            j = k;
        }
        dl->AddText(font, size, ImVec2(x, pos.y), col, s + i, s + j);
        x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, s + i, s + j).x;
        i = j;
    }
}

// Adds what is ticked, or `fallback` when nothing is, and closes.
static void fb_add(UiState *ui, const std::string &fallback)
{
    CBApp *app = ui->app;
    std::vector<std::string> items(ui->fb_picked.begin(), ui->fb_picked.end());
    if (items.empty() && !fallback.empty())
        items.push_back(fallback);
    if (items.empty())
        return;
    // One item: its own status says what came of it. Several: a count,
    // the failures in it (each is in the log).
    int added = 0, failed = 0;
    for (const std::string &p : items) {
        if (cb_app_add_path(app, p.c_str()) >= 0)
            added++;
        else
            failed++;
    }
    int n = (int)items.size();
    if (n > 1 && failed)
        cb_app_set_status(app, "Added %d of %d items; %d could not be added", added, n, failed);
    else if (n > 1)
        cb_app_set_status(app, "Added %d items", added);
    ui->fb_picked.clear();
    ui->dlg_add = false;
}

// The finder's matches in the list's place. Returns the folder to go to
// (a double click on one), or "".
static std::string fb_find_rows(UiState *ui)
{
    std::string go;
    const std::vector<int> &res = ui_finder_results(ui->finder);
    int shown = std::min((int)res.size(), UI_FINDER_SHOWN);
    ui->fb_cursor = shown ? std::clamp(ui->fb_cursor, 0, shown - 1) : 0;
    float h = ImGui::GetTextLineHeight();
    for (int i = 0; i < shown; i++) {
        const UiFinderEntry &fe = ui_finder_entry(ui->finder, res[(size_t)i]);
        std::string abs = fb_abs(ui, fe.rel);
        ImGui::PushID(i);
        bool picked = ui->fb_picked.count(abs) > 0;
        if (ImGui::Checkbox("##sel", &picked))
            fb_toggle(ui, abs);
        ImGui::SameLine();
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", i == ui->fb_cursor, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(0, h))) {
            ui->fb_cursor = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && fe.dir)
                go = abs;
            else
                fb_toggle(ui, abs);
        }
        if (i == ui->fb_cursor && ui->fb_cursor_moved) {
            ImGui::SetScrollHereY();
            ui->fb_cursor_moved = false;
        }
        fb_path_text(ui, p, fe, ImGui::GetItemRectMax().x - p.x);
        ImGui::PopID();
    }
    if (res.empty() && !ui_finder_walking(ui->finder))
        ImGui::TextDisabled("Nothing here matches.");
    return go;
}

static void add_dialog(UiState *ui)
{
    CBApp *app = ui->app;
    if (!ui->dlg_add) {
        // Closed: the walk stops, its list goes.
        ui_finder_free(&ui->finder);
        ui->fb_focused = false;
        return;
    }
    ui_centre_next(ui, 680, 560);
    if (!ImGui::Begin("Add files and folders", &ui->dlg_add,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_escape_closes(&ui->dlg_add);
    ui_settle_centred();
    ui->fb_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (ImGui::Button("Up")) {
        std::string up = ui->fb_path;
        size_t slash = up.find_last_of('/');
        fb_go(ui, slash == 0 || slash == std::string::npos ? "/" : up.substr(0, slash));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##path", ui->fb_edit, sizeof(ui->fb_edit),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        char *e = cb_path_expand(ui->fb_edit);
        if (e)
            fb_go(ui, e);
        free(e);
    }

    // The finder's field: what is typed is looked for under this folder.
    // Up and Down move through the matches, Enter ticks one, Ctrl+Enter
    // adds, Escape empties the field.
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ui->fb_find_focus) {
        ImGui::SetKeyboardFocusHere();
        ui->fb_find_focus = false;
    }
    char before[sizeof(ui->fb_find)];
    memcpy(before, ui->fb_find, sizeof(before));
    bool enter = ImGui::InputTextWithHint("##find", "Find under this folder (Ctrl+F): any part of a name",
                                          ui->fb_find, sizeof(ui->fb_find),
                                          ImGuiInputTextFlags_EnterReturnsTrue |
                                          ImGuiInputTextFlags_EscapeClearsAll);
    bool find_active = ImGui::IsItemActive();
    if (strcmp(before, ui->fb_find)) {
        ui->fb_cursor = 0;
        ui->fb_cursor_moved = true;
    }
    bool finding = ui->fb_find[0] != '\0';
    if (finding)
        ui_finder_update(&ui->finder, ui->fb_path, ui->fb_find);
    const std::vector<int> *res = finding ? &ui_finder_results(ui->finder) : nullptr;
    int shown = res ? std::min((int)res->size(), UI_FINDER_SHOWN) : 0;
    std::string cursor_abs;
    if (res && ui->fb_cursor >= 0 && ui->fb_cursor < shown)
        cursor_abs = fb_abs(ui, ui_finder_entry(ui->finder, (*res)[(size_t)ui->fb_cursor]).rel);
    if (finding && find_active) {
        int step = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1
                 : ImGui::IsKeyPressed(ImGuiKey_PageDown) ? 10
                 : ImGui::IsKeyPressed(ImGuiKey_PageUp) ? -10 : 0;
        if (step && shown) {
            ui->fb_cursor = std::clamp(ui->fb_cursor + step, 0, shown - 1);
            ui->fb_cursor_moved = true;
        }
    }
    if (enter && finding) {
        // Enter leaves the field (ImGui's way); it comes back next frame.
        ui->fb_find_focus = true;
        if (ImGui::GetIO().KeyCtrl)
            fb_add(ui, cursor_abs);
        else if (!cursor_abs.empty())
            fb_toggle(ui, cursor_abs);
    }
    if (!ui->dlg_add) {
        ImGui::End();
        return;
    }

    int nsel = (int)ui->fb_picked.size();
    ImGui::BeginChild("##files", ImVec2(0, -PX(finding ? 62 : 40)), ImGuiChildFlags_Borders);
    std::string go;
    if (finding) {
        go = fb_find_rows(ui);
    } else {
        for (size_t i = 0; i < ui->fb_entries.size(); i++) {
            const UiFileEntry &fe = ui->fb_entries[i];
            std::string abs = fb_abs(ui, fe.name);
            ImGui::PushID((int)i);
            bool picked = ui->fb_picked.count(abs) > 0;
            if (ImGui::Checkbox("##sel", &picked))
                fb_toggle(ui, abs);
            ImGui::SameLine();
            char label[600];
            snprintf(label, sizeof(label), "%s%s", fe.name.c_str(), fe.dir ? "/" : "");
            if (fe.dir)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(UI_ACCENT));
            if (ImGui::Selectable(label, picked, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && fe.dir)
                    go = abs;
                else
                    fb_toggle(ui, abs);
            }
            if (fe.dir)
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (!ui->fb_error.empty())
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_RED), "%s", ui->fb_error.c_str());
        else if (ui->fb_entries.empty())
            ImGui::TextDisabled("No folders or audio files here.");
    }
    ImGui::EndChild();
    if (!go.empty()) {
        // A folder found: the listing of it, the field emptied.
        ui->fb_find[0] = '\0';
        fb_go(ui, go);
    }

    if (finding && ui->finder) {
        size_t n = ui_finder_count(ui->finder), m = ui_finder_matches(ui->finder);
        std::string where = fb_short(ui->fb_path);
        char line[512];
        snprintf(line, sizeof(line), "%zu match%s among %zu folders and files under %s%s%s", m,
                 m == 1 ? "" : "es", n, where.c_str(),
                 ui_finder_walking(ui->finder) ? ", still looking..." : "",
                 ui_finder_truncated(ui->finder) ? " (the search stopped at its limit)" : "");
        ImGui::TextDisabled("%s", line);
        if (m > (size_t)UI_FINDER_SHOWN) {
            ImGui::SameLine();
            ImGui::TextDisabled("(the best %d shown)", UI_FINDER_SHOWN);
        }
    }

    // Adding probes the files and queues their analysis (app.c).
    if (nsel > 0) {
        char label[64];
        snprintf(label, sizeof(label), "Add %d selected", nsel);
        if (ImGui::Button(label, ImVec2(PX(160), PX(28))))
            fb_add(ui, "");
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(PX(80), PX(28))))
            ui->fb_picked.clear();
    } else if (ImGui::Button("Add this folder", ImVec2(PX(160), PX(28)))) {
        // cb_app_add_path's status says what came of it.
        cb_app_add_path(app, ui->fb_path.c_str());
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
    ui_centre_next(ui, 520, 560);
    if (!ImGui::Begin("Keyboard map", &ui->dlg_keys,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_escape_closes(&ui->dlg_keys);
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
    ui_centre_next(ui, 0, 0);
    if (!ImGui::Begin("About cyanbox", &ui->dlg_about,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_escape_closes(&ui->dlg_about);
    ui_settle_centred();
    ImGui::PushFont(ui->font_bold, ui->font_base * 1.6f);
    ImGui::TextUnformatted("cyanbox");
    ImGui::PopFont();
    ImGui::TextDisabled("version %s", CYANBOX_VERSION);
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
    confirm_remove(ui);
    relocate(ui);
    analysis_settings(ui);
    settings_dialog(ui);
    export_dialog(ui);
    sync_dialog(ui);
    inspect_dialog(ui);
    add_dialog(ui);
    keys_dialog(ui);
    about_dialog(ui);
    ui_link_window(ui);
}

void ui_dialogs_shutdown(UiState *ui)
{
    sync_free(ui);
    ui_finder_free(&ui->finder);
}
