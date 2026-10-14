/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_UI_INTERNAL_H
#define RXBOX_UI_INTERNAL_H

// The interface's private state and the helpers its screens share. One
// UiState hangs off RXApp.ui; every screen function takes it. Nothing here
// is global: the ImGui context is the only process-wide state and it is
// the window module's.
//
// Sizes are written in logical pixels at 1x and multiplied by `ui->s`
// (PX()), which rx_ui_frame derives each frame from the ImGui display size
// against rx_window_size(), so the layout holds whether the window module
// runs ImGui in logical or in device pixels.

#include <stdint.h>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

extern "C" {
#include "rxbox/common.h"
#include "rxbox/model.h"
#include "rxbox/library.h"
#include "rxbox/analysis.h"
#include "rxbox/player.h"
#include "rxbox/jobs.h"
#include "rxbox/gpu.h"
#include "rxbox/app.h"
#include "rxbox/colors.h"
#include "rxbox/notation.h"
#include "rxbox/export.h"
#include "rxbox/formats.h"
}

#define PX(v) (ui->s * (float)(v))

/* ------------------------------------------------------------------ colours */

// docs/UI.md's palette, rekordbox's dark theme; cue and waveform colours
// are rbxport's measurements (src/styles/theme.ts).
static const ImU32 UI_BG          = IM_COL32(0x12, 0x12, 0x12, 0xFF);
static const ImU32 UI_PANEL       = IM_COL32(0x1E, 0x1E, 0x1E, 0xFF);
static const ImU32 UI_BUTTON      = IM_COL32(0x2B, 0x2B, 0x2B, 0xFF);
static const ImU32 UI_BUTTON_HOT  = IM_COL32(0x3A, 0x3A, 0x3A, 0xFF);
static const ImU32 UI_BUTTON_DOWN = IM_COL32(0x4A, 0x4A, 0x4A, 0xFF);
static const ImU32 UI_TEXT        = IM_COL32(0xE6, 0xE6, 0xE6, 0xFF);
static const ImU32 UI_DIM         = IM_COL32(0x8A, 0x8A, 0x8A, 0xFF);
static const ImU32 UI_FAINT       = IM_COL32(0x55, 0x55, 0x55, 0xFF);
static const ImU32 UI_ACCENT      = IM_COL32(0xFF, 0x8C, 0x00, 0xFF);
static const ImU32 UI_QUANTIZE    = IM_COL32(0x3C, 0xEB, 0x50, 0xFF);
static const ImU32 UI_PLAY        = IM_COL32(0x3C, 0xEB, 0x50, 0xFF);
static const ImU32 UI_RED         = IM_COL32(0xE8, 0x3A, 0x30, 0xFF);
static const ImU32 UI_BLACK       = IM_COL32(0x00, 0x00, 0x00, 0xFF);
static const ImU32 UI_WAVE_BG     = IM_COL32(0x05, 0x05, 0x05, 0xFF);
static const ImU32 UI_CUE_HEAD    = IM_COL32(0xEA, 0x33, 0x23, 0xFF);
static const ImU32 UI_BEAT        = IM_COL32(0x4C, 0x4C, 0x4C, 0xFF);
static const ImU32 UI_DOWNBEAT    = IM_COL32(0x9A, 0x9A, 0x9A, 0xFF);
static const ImU32 UI_DECK_A      = IM_COL32(0x1F, 0x3B, 0x73, 0xFF);
static const ImU32 UI_DECK_B      = IM_COL32(0x73, 0x23, 0x1F, 0xFF);

static inline ImU32 ui_rgb(uint32_t rgb, float alpha = 1.0f)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, (int)(alpha * 255.0f + 0.5f));
}

static inline ImU32 ui_alpha(ImU32 col, float alpha)
{
    unsigned a = (unsigned)(((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha + 0.5f);
    return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

/* ------------------------------------------------------------------- tables */

// The zoom levels of the detail waveform in bars (rbxport src/lib/player.ts
// ZOOM_STEPS: rekordbox's closest view is half a bar, plus a quarter).
static const double UI_ZOOM_STEPS[] = { 0.25, 0.5, 1, 2, 4, 8, 12, 16, 32, 64 };
#define UI_NB_ZOOM ((int)(sizeof(UI_ZOOM_STEPS) / sizeof(UI_ZOOM_STEPS[0])))

// rekordbox's beat jump menu, in its order (rbxport JUMP_SIZES), which
// changes unit at 8 bars, with the 1 and 2 beats it skips and the players'
// BEAT JUMP BEAT VALUE offers (player.beat_jump_beats, the deck's size at
// start). Fine moves 10 ms; it also stands for the setting's 1/2 beat.
struct UiJumpSize { const char *label; double beats; };
static const UiJumpSize UI_JUMP_SIZES[] = {
    { "Fine", 0 }, { "1Beat", 1 }, { "2Beats", 2 }, { "4Beats", 4 }, { "8Beats", 8 },
    { "16Beats", 16 }, { "8Bars", 32 }, { "16Bars", 64 }, { "32Bars", 128 },
};
#define UI_NB_JUMP ((int)(sizeof(UI_JUMP_SIZES) / sizeof(UI_JUMP_SIZES[0])))
#define UI_FINE_JUMP_SECONDS 0.01

// The tempo slider ranges a CDJ cycles through (WIDE is +-100 %).
static const double UI_TEMPO_RANGES[] = { 6, 10, 16, 100 };
#define UI_NB_TEMPO_RANGES 4

// The players' settings that set the deck's state at startup (ui.cpp).
#define UI_NB_STARTUP 5

// Beat jump repeats while held, from the keys and the buttons alike: the
// first jump on the press, then one every 120 ms once held for 300 ms.
#define UI_REPEAT_DELAY 0.300f
#define UI_REPEAT_RATE  0.120f

// The player panel's rows (logical pixels; deck_view.cpp lays them out,
// ui.cpp sizes the panel from them). The control row is one row of pads:
// the 18 px caps labels, 36 px pads, 16 px, the 24 px strips and 2 px
// under them. The overview, looked at less than the detail, is 48 (it was
// 64). The detail waveform takes the rest; at the player's natural height
// it is 220, which keeps the panel as tall as it was with two rows of pads
// (a 140 px control row and a 160 px detail): what the pads and the
// overview gave up went to the detail.
#define UI_TITLE_ROW_H     64
#define UI_OVERVIEW_H      48
#define UI_CONTROL_ROW_H   96
#define UI_GRID_STRIP_H    32
#define UI_DETAIL_NATURAL  220

// Where the detail's playhead sits with the players' WAVEFORM CURRENT
// POSITION at LEFT (device.waveform_position), as a fraction of the width
// from the left edge: the music to come takes the rest.
#define UI_PLAYHEAD_LEFT   0.15f

/* -------------------------------------------------------------------- state */

struct UiDeck {
    // Beats of the loaded grid, rebuilt when the grid changes.
    uint64_t beats_sig;
    RXBeat  *beats;
    int      nb_beats;

    // The live state of the deck's buttons. Each starts as the players'
    // setting says (player.time_mode, tempo_range, beat_jump_beats,
    // master_tempo: ui.cpp, follow_settings) and follows a change of it;
    // the buttons and keys change only these, never the setting.
    bool     time_remaining;   // time readout shows remaining (T)
    int      tempo_range;      // index into UI_TEMPO_RANGES
    int      jump_size;        // index into UI_JUMP_SIZES
    bool     master_tempo;     // MASTER TEMPO as last asked for

    // Waveform drag state (overview and detail share one deck scrub).
    int      drag_area;        // 0 none, 1 overview, 2 detail
    bool     scrubbing;
    float    drag_x0;
    double   drag_t0;
    double   detail_sec_per_px;

    // Detail context menu.
    double   ctx_time;
    int      ctx_cue;          // cue index under the pointer, -1

    // Hot cue pad popup and name editing.
    char     name_buf[RX_CUE_NAME_MAX];
    // The cue whose name is edited, kept by what it is as the selection
    // is (a cue set above it mid-edit moves its row): deck_view.cpp,
    // cue_edit_index.
    bool     editing;
    uint64_t edit_track;
    int      edit_hot;         // its hot cue slot, -1 a memory cue
    double   edit_time, edit_end;
    bool     edit_focus;

    // The cue list's selection. While cue_focus is set the list has the
    // keyboard: Delete deletes the selected cue, Up/Down move the
    // selection. A click in the track table, Escape or another track takes
    // it back. The cue is kept by what it is (deck_view.cpp, cue_select).
    bool     cue_focus;
    bool     cue_selected;     // a row is selected (cue_focus outlives its deletion)
    bool     cue_scroll;       // scroll the list to the selected row
    uint64_t cue_track;        // the track the selection belongs to
    int      cue_hot;          // the selected hot cue's slot, -1 a memory cue
    double   cue_time;         // the selected memory cue's time and loop end
    double   cue_end;

    // Grid tools.
    char     bpm_buf[16];
    bool     bpm_active;
    double   tap_first_pos;    // deck position at the first tap
    int64_t  tap_last_us;

    bool     cue_held_mouse;
    unsigned pads_down;        // hot cue pads pressed with the mouse (bit per slot)
    unsigned keys_hot_down;    // hot cue keys 1..3 held
    bool     key_cue_down;     // C held
    bool     autoplay;         // start playing once loaded (a load over a playing track, the test autoload)
};

struct UiFileEntry {
    std::string name;
    bool dir;
    bool audio;
};

// One folder or audio file the Add dialog's finder found: its path under
// the folder searched.
struct UiFinderEntry {
    std::string rel;
    bool dir;
};
struct UiFinder;

// A My Settings field as rx_settings_list gives it, in the section `rxbox
// settings` prints it under: both MYSETTING files are the player's,
// DJMMYSETTING.DAT the mixer's, DEVSETTING.DAT and the background colour
// (which the databases carry) the device's. The library keeps its value as
// "<prefix>.<name>" (player.quantize).
enum { UI_SETTINGS_PLAYER, UI_SETTINGS_MIXER, UI_SETTINGS_DEVICE, UI_NB_SETTING_SECTIONS };
struct UiSetting {
    std::string file;          // MYSETTING.DAT ...
    std::string name;          // quantize
    int         section;       // UI_SETTINGS_*
    std::vector<std::string> values;   // as `rxbox settings` lists them
    int         deflt;         // the default, an index into values
};

// An input event the test hook gives ImGui at a frame (RXBOX_UI_EVENTS).
struct UiTestEvent {
    int      frame;
    int      type;             // 0 key down, 1 key up, 2 focus out, 3 focus in, 4 click, 5 text
    ImGuiKey key;
    float    x, y;             // a click's position, logical pixels
    std::string text;          // typed text ('+' for a space)
};

// A match of the tree's finder (tree.cpp): Collection, an album (None's
// name is ""), or a playlist; `text` what was matched and is shown, the
// context (the album's artist, the playlist's folders) up to its last '/'.
struct UiTreeHit {
    enum Kind { COLLECTION, ALBUM, PLAYLIST } kind;
    std::string text;
    std::string album;
    RXPlaylist *playlist;
    RXPlaylistFolder *parent;
    int count;
    int score;
};

struct UiState {
    RXApp *app;
    float  s;                  // layout scale (see top of file)
    ImFont *font_bold;         // NULL: the regular font
    float  font_base;          // style.FontSizeBase at init

    // Layout: the splits are RXConfig's (split_player, split_tree); the rest
// is ImGui's ini text kept in the library's "ui.layout" setting (ui.cpp).
    float  deck_split;         // fraction of the height for the player, < 0: the default
    float  tree_width;         // logical pixels
    bool   detail_half;        // rekordbox 2 PLAYER half-height detail instead of CDJ centred
    int    traffic_light;      // RXKeyReach of the table's key highlight

    // The players' My Settings the drawing follows, set from RXConfig at
    // the start of every frame (ui.cpp, follow_settings).
    bool   key_camelot;        // device.key_display alphanumeric: keys as Camelot codes
    bool   overview_full;      // device.overview_waveform full: the overview and the row
                               // previews centred, both halves, instead of the stacked half
    bool   playhead_left;      // device.waveform_position left: the detail's playhead at
                               // UI_PLAYHEAD_LEFT of its width instead of the centre
    bool   hot_cue_colour;     // player.hot_cue_color: hot cues in their own colours, else
                               // in the slots' default pad colours
    // What the last frame drew, for the test hooks' log: the overview's and
    // the row previews' RXWaveMode (-1 none drawn) and the deck's KEY.
    int    overview_mode_drawn, preview_mode_drawn;
    char   key_drawn[16];
    // The settings of the deck's startup state as last applied to it
    // (ui.cpp, follow_settings: player.quantize, beat_jump_beats,
    // tempo_range, master_tempo, time_mode); NULL before the first time.
    // The strings are rx_settings_get's, which are static.
    const char *startup_applied[UI_NB_STARTUP];
    bool   layout_dirty;

    UiDeck decks[RX_DECKS];

    // Browser.
    int       anchor_row;
    bool      focus_search;
    bool      scroll_to_focus;
    uint64_t  info_track;      // track whose fields are in the info buffers
    unsigned  info_generation;
    std::string info_buf[16];  // the fields as typed: any length (ui_input_string)
    std::string info_tags;
    bool      row_hovered;

    // Tree.
    void     *rename_target;
    bool      rename_folder;
    char      rename_buf[256];
    bool      rename_focus;
    void     *delete_target;
    bool      delete_folder;
    bool      want_delete_popup;
    // Edits that change the tree's arrays wait until it has been drawn.
    bool      create_pending, create_folder;
    RXPlaylistFolder *create_in;
    RXPlaylistFolder *reveal;  // opened, with every folder above it, at the next tree draw
    // The finder above the tree: what is typed, its matches best first (made
    // again when it or the library changes), the one Enter opens; after a
    // pick, the row the tree scrolls to (an album's opens Albums).
    char      tree_find[128];
    bool      tree_find_focus;     // Ctrl+P: the field takes the keyboard
    std::vector<UiTreeHit> tree_hits;
    std::string tree_hits_query;
    unsigned  tree_hits_generation;
    int       tree_hit;
    bool      tree_hit_scroll;
    bool      tree_reveal_album;
    std::string tree_reveal_album_name;
    RXPlaylist *tree_reveal_playlist;
    unsigned  bytes_generation;    // lib.generation the status bar's size is for, 0 none
    int64_t   bytes_total;
    bool      move_pending, move_folder;
    void     *move_ptr;
    RXPlaylistFolder *move_to;
    int       move_pos;

    // Remove from library confirmation.
    bool      want_confirm_remove;
    std::vector<uint64_t> remove_ids;

    // Relocate (a missing track's new path).
    bool      want_relocate;
    uint64_t  relocate_id;
    char      relocate_buf[1024];

    // Dialogs.
    bool      dlg_analysis, dlg_export, dlg_keys, dlg_about, dlg_add, dlg_inspect, dlg_sync;
    bool      dlg_settings, dlg_link;
    bool      settings_focus;  // bring Player settings to the front (Ctrl+, while open)
    std::vector<UiSetting> settings;   // every My Settings field (ui_settings)
    int       bpm_preset;
    char      vibe_buf[RX_TRACK_COLORS][128];   // the legend's fields in Analysis settings
    std::string fb_path;
    char      fb_edit[1024];
    std::vector<UiFileEntry> fb_entries;
    std::string fb_error;      // why fb_path could not be listed
    bool      fb_dirty;
    std::set<std::string> fb_picked;   // absolute paths ticked, in the listing or the finder
    char      fb_find[256];    // the finder's query; empty: the listing shows
    bool      fb_find_focus;   // focus the finder's field next frame
    bool      fb_focused;      // the Add dialog had the focus last frame (Ctrl+F is its)
    int       fb_cursor;       // the finder's highlighted row
    bool      fb_cursor_moved; // scroll it into view
    UiFinder *finder;          // finder.cpp; NULL until the first search of an opening
    std::vector<std::string> volumes;
    bool      export_seen_running;
    int       export_scope;    // 0 all playlists, 1 selected playlists, 2 entire library
    std::vector<RXPlaylist *> export_playlists;
    char      stick_path[512];
    std::string inspect_report;
    int       inspect_result;
    bool      inspect_done;
    uint64_t  inspect_request; // the inspection running on a worker, 0 none
    // Sync from USB: read + diff (on a worker), review, apply.
    int       sync_stage;      // 0 pick, 1 review, 2 applied
    uint64_t  sync_request;    // the read running on a worker, 0 none
    RXStickState sync_state;
    bool      sync_state_valid;
    RXSyncItem *sync_items;
    int       nb_sync_items;
    int       sync_applied;
    std::string sync_progress;
    std::string sync_error;
    int       sync_filter;     // -1 all, else an RXSyncKind

    // Frame pacing and the headless test hooks.
    int       frames_wanted;
    bool      previews_pending;    // a visible row's preview or artwork is still being made
    int       pending_frames;
    int       frame_no;
    std::string screenshot_path;
    int       screenshot_frame;
    uint64_t  autoload;
    double    autoload_at;     // seconds to start from, < 0: the auto-cue
    std::string autoshow;      // panels and dialogs to open (test hook)
    std::vector<UiTestEvent> test_events;   // input to give ImGui (test hook)
    bool      autoload_done;
    double    saved_zoom;
};

static inline UiState *ui_of(RXApp *app) { return (UiState *)app->ui; }

/* ------------------------------------------------------------------ ui.cpp */

// Texts and formatting.
const char *ui_fmt_time(double seconds, char *buf, size_t size, bool tenths);
const char *ui_key_text(const UiState *ui, const char *classic, char *buf, size_t size);
const char *ui_title(const RXTrack *t);
const char *ui_artist(const RXTrack *t);
uint32_t    ui_hot_cue_rgb(const UiState *ui, const RXCue *c);
uint32_t    ui_memory_cue_rgb(const RXCue *c);
uint32_t    ui_track_rgb(const RXTrack *t);
ImU32       ui_text_on(uint32_t rgb);

// Draw-list text: size in logical pixels; clipped to max_w with "..." when > 0.
void  ui_text(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col,
              const char *text, float max_w = 0.0f);
float ui_text_width(UiState *ui, ImFont *font, float size, const char *text);
// InputText(WithHint) on a std::string that grows with the text (ImGui's
// resize callback), so nothing stored is cut to a buffer's size.
bool  ui_input_string(const char *id, const char *hint, std::string *s,
                      ImGuiInputTextFlags flags = 0);
void  ui_text_right(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 right_top,
                    ImU32 col, const char *text);
void  ui_text_centre(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 a, ImVec2 b,
                     ImU32 col, const char *text);

// Buttons drawn on the draw list; an InvisibleButton underneath so
// IsItemActivated/IsItemDeactivated give press and release.
enum UiIcon { UI_ICON_NONE = 0, UI_ICON_LEFT, UI_ICON_RIGHT, UI_ICON_PLAY, UI_ICON_PAUSE,
             UI_ICON_PLAYPAUSE, UI_ICON_LOOP, UI_ICON_CROSS, UI_ICON_PLUS, UI_ICON_MINUS,
             UI_ICON_DIVIDE };
struct UiButton {
    const char *label;         // shown text (may be NULL)
    UiIcon icon;
    ImU32  fill;               // 0: UI_BUTTON
    ImU32  text;               // 0: UI_TEXT
    ImU32  border;             // 0: none
    bool   lit;                // drawn in `lit_col`
    ImU32  lit_col;            // 0: accent
    bool   disabled;
    bool   repeat;             // held: true on the press, then at UI_REPEAT_RATE
    bool   icon_after;         // the icon right of the label ("CUE >")
    float  font;               // 0: 11; a label too wide for the button shrinks
    float  rounding;           // -1: 3
};
bool ui_button(UiState *ui, const char *id, ImVec2 size, const UiButton &b);
void ui_icon(ImDrawList *dl, UiIcon icon, ImVec2 a, ImVec2 b, ImU32 col);
void ui_record_icon(ImDrawList *dl, ImVec2 a, ImVec2 b);
void ui_spinner(ImDrawList *dl, ImVec2 c, float r, ImU32 col);
void ui_star(ImDrawList *dl, ImVec2 c, float r, ImU32 col);
void ui_tooltip(const char *text);
// A colour square for the colour menus; true when clicked.
bool ui_swatch(UiState *ui, const char *id, uint32_t rgb, bool selected, float size);
// The colour menus: hot cue colours (rekordbox's sixteen) and the eight
// track colours, which memory cues take (a track's own colour is a hue:
// ui_colour_picker). Return true when one was picked into `out` ("" = none).
bool ui_hot_cue_colour_menu(UiState *ui, const char *current, char out[RX_COLOR_MAX]);
bool ui_track_colour_menu(UiState *ui, const char *current, char out[RX_COLOR_MAX]);
// A track's colour (rxbox/model.h: a hue, one of the eight's when picked
// here or by the model), in `width` logical pixels (0: what is left of the
// line): a "none" swatch and the eight track colours, each with its name
// and the legend's words as a tooltip. The one the hue is is outlined;
// none is when the hue is between them, as an older library may hold, and
// the track is drawn in that hue still. True when one is clicked (its own too, for a selection):
// *hue is then its hue, or RX_HUE_NONE; the caller saves.
bool ui_colour_picker(UiState *ui, const char *id, double *hue, float width);
// "red · intense, peak time" (the legend's words), "200°, between the
// eight (aqua on the stick)" or "none".
const char *ui_colour_text(UiState *ui, double hue, char *buf, size_t size);

// My Settings as people read them, for the Player settings dialog and the
// Sync dialog's rows alike: "lcd_brightness" is "LCD Brightness",
// "touch_release" "Touch & Release", tempo_range's "6" "±6 %".
extern const char *const ui_setting_titles[UI_NB_SETTING_SECTIONS];    // "Player"
extern const char *const ui_setting_prefixes[UI_NB_SETTING_SECTIONS];  // "player", the keys'
const std::vector<UiSetting> &ui_settings(UiState *ui);
int         ui_setting_key(const char *key, const char **name);       // "player.quantize": section, name
const char *ui_setting_label(const char *name, char *buf, size_t size);
const char *ui_setting_value(const char *name, const char *value, char *buf, size_t size);
// The value of a My Setting the interface follows ("device.key_display"),
// as an export would write it: the library's, else the default.
const char *ui_my_setting(UiState *ui, const char *key);
// Sets a My Setting from outside the Player settings dialog (the View
// menu), the way the dialog sets one: in RXConfig and the library at once,
// said in the status bar. `value` as rx_settings_list lists it.
void        ui_setting_pick(UiState *ui, const char *key, const char *value);

// Loads a track into the player, pausing it first when it plays.
void ui_load(UiState *ui, RXTrack *t);
void ui_want_frames(UiState *ui, int n);
void ui_save_config(UiState *ui);

/* ---------------------------------------------------------- deck_view.cpp */

void ui_deck_panel(UiState *ui, int deck, ImVec2 size);
void ui_deck_shutdown(UiState *ui);
double ui_deck_position(UiState *ui, int deck, RXDeckState *st);
void ui_grid_edited(UiState *ui, int deck);
void ui_jump(UiState *ui, int deck, int dir);
void ui_shift_grid(UiState *ui, int deck, double seconds);
// The cue list's keyboard (keys.cpp): whether the list has it, giving it
// back to the table, Delete on the selected cue, Up/Down.
bool ui_cue_list_has_keys(UiState *ui, int deck);
void ui_cue_list_release(UiState *ui, int deck);
void ui_cue_list_delete(UiState *ui, int deck);
void ui_cue_list_move(UiState *ui, int deck, int dir);

/* ------------------------------------------------------ waveform_view.cpp */

void ui_overview(UiState *ui, int deck, ImVec2 a, ImVec2 b);
void ui_detail(UiState *ui, int deck, ImVec2 a, ImVec2 b);
void ui_row_preview(UiState *ui, const RXTrack *t, ImVec2 a, ImVec2 b, bool analysing);
const RXBeat *ui_deck_beats(UiState *ui, int deck, int *nb);
double ui_zoom_step(double bars, int dir);

/* ------------------------------------------------------------ browser.cpp */

void ui_browser(UiState *ui, ImVec2 size);
void ui_info_panel(UiState *ui, ImVec2 size);
void ui_select_only(UiState *ui, uint64_t id);
bool ui_is_selected(const UiState *ui, uint64_t id);
void ui_remove_from_playlist(UiState *ui);
void ui_move_focus(UiState *ui, int delta);
RXTrack *ui_focus_track(UiState *ui);

// Drag and drop: a payload of track ids.
#define UI_DND_TRACKS "RX_TRACKS"
bool ui_accept_tracks(UiState *ui, std::vector<uint64_t> &ids);

/* --------------------------------------------------------------- tree.cpp */

void ui_tree(UiState *ui, ImVec2 size);
// Appends the tracks to the playlist (a track once per playlist), with the
// count in the status bar.
void ui_add_to_playlist(UiState *ui, RXPlaylist *p, const std::vector<uint64_t> &ids);

/* ------------------------------------------------------------ dialogs.cpp */

void ui_dialogs(UiState *ui);
void ui_open_add(UiState *ui);
void ui_open_export(UiState *ui);
void ui_open_inspect(UiState *ui);
void ui_open_sync(UiState *ui);
void ui_open_settings(UiState *ui);
void ui_open_relocate(UiState *ui, RXTrack *t);
void ui_dialogs_shutdown(UiState *ui);
// A dialog's first frame: centred, w x h logical pixels (0: its own size).
void ui_centre_next(UiState *ui, float w, float h);
// Esc closes the focused dialog unless a text field has the keyboard.
void ui_escape_closes(bool *open);

/* ----------------------------------------------------------- link_view.cpp */

// PRO DJ LINK (rxbox --pro-link; app->link_state): the window with the
// Ethernet interfaces served and the devices on them. None of it is drawn
// without --pro-link.
void ui_link_window(UiState *ui);
void ui_open_link(UiState *ui);
// The status bar's word on it ("Link: 2 players"), and its colour.
const char *ui_link_summary(UiState *ui, char *buf, size_t size, ImU32 *col);

/* ------------------------------------------------------------- finder.cpp */

// Searches everything under `root` for `query`: starts the walk of `root`
// (again, when the root changed) and matches what it has found so far.
// Cheap to call every frame; *f is made on the first call.
void ui_finder_update(UiFinder **f, const std::string &root, const char *query);
// Stops the walk and frees; *f becomes NULL.
void ui_finder_free(UiFinder **f);
// The best UI_FINDER_SHOWN matches, best first, as indices for
// ui_finder_entry; ui_finder_matches counts them all.
#define UI_FINDER_SHOWN 300
const std::vector<int> &ui_finder_results(const UiFinder *f);
size_t ui_finder_matches(const UiFinder *f);
const UiFinderEntry &ui_finder_entry(const UiFinder *f, int i);
size_t ui_finder_count(const UiFinder *f);      // folders and files found so far
bool   ui_finder_walking(const UiFinder *f);
bool   ui_finder_truncated(const UiFinder *f);  // the walk stopped at its limits
void ui_scan_volumes(UiState *ui);
// Keeps a self-sizing dialog centred over its first frames (call after Begin).
void ui_settle_centred(void);

/* --------------------------------------------------------------- keys.cpp */

void ui_keys(UiState *ui);
struct UiKeyRow { const char *key; const char *action; };
extern const UiKeyRow ui_key_rows[];
extern const int ui_nb_key_rows;

#endif /* RXBOX_UI_INTERNAL_H */
