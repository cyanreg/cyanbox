/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_UI_INTERNAL_H
#define RXBOX_UI_INTERNAL_H

// The interface's private state and the helpers its screens share. One
// UiState hangs off RxApp.ui; every screen function takes it. Nothing here
// is global: the ImGui context is the only process-wide state and it is
// the window module's.
//
// Sizes are written in logical pixels at 1x and multiplied by `ui->s`
// (PX()), which rx_ui_frame derives each frame from the ImGui display size
// against rx_window_size(), so the layout holds whether the window module
// runs ImGui in logical or in device pixels.

#include <stdint.h>
#include <string>
#include <vector>

// imconfig.h defines IMGUI_DEFINE_MATH_OPERATORS and so does imgui_dep's
// command line; drop the command-line one so the header's is not a
// redefinition.
#undef IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"

// rxbox/thread.h declares RxCancel with C11 `_Atomic int`, which C++17 does
// not parse. The interface never touches an RxCancel; an int has the same
// size and alignment, so the declaration is read as a plain int here.
#define _Atomic
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
}
#undef _Atomic

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

// rekordbox's beat jump menu, in its order (rbxport JUMP_SIZES): it skips
// 1 and 2 beats and changes unit at 8 bars. Fine moves 10 ms.
struct UiJumpSize { const char *label; double beats; };
static const UiJumpSize UI_JUMP_SIZES[] = {
    { "Fine", 0 }, { "4Beats", 4 }, { "8Beats", 8 }, { "16Beats", 16 },
    { "8Bars", 32 }, { "16Bars", 64 }, { "32Bars", 128 },
};
#define UI_NB_JUMP ((int)(sizeof(UI_JUMP_SIZES) / sizeof(UI_JUMP_SIZES[0])))
#define UI_FINE_JUMP_SECONDS 0.01

// The tempo slider ranges a CDJ cycles through (WIDE is +-100 %).
static const double UI_TEMPO_RANGES[] = { 6, 10, 16, 100 };
#define UI_NB_TEMPO_RANGES 4

enum UiPadMode { UI_PADS_HOT_CUE = 0, UI_PADS_BEAT_LOOP, UI_PADS_BEAT_JUMP, UI_PADS_MEMORY };

/* -------------------------------------------------------------------- state */

struct UiDeck {
    // Beats of the loaded grid, rebuilt when the grid changes.
    uint64_t beats_sig;
    RxBeat  *beats;
    int      nb_beats;

    bool     time_remaining;   // time readout shows remaining (T)
    int      tempo_range;      // index into UI_TEMPO_RANGES
    int      jump_size;        // index into UI_JUMP_SIZES

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
    int      pad_popup_slot;
    char     name_buf[RX_CUE_NAME_MAX];
    int      edit_memory;      // memory list row whose name is edited, -1
    bool     edit_focus;

    // Grid tools.
    char     bpm_buf[16];
    bool     bpm_active;
    double   tap_first_pos;    // deck position at the first tap
    int64_t  tap_last_us;

    bool     cue_held_mouse;
    unsigned pads_down;        // hot cue pads pressed with the mouse (bit per slot)
    unsigned keys_hot_down;    // hot cue keys 1..3 held
    bool     key_cue_down;     // C held
    bool     autoplay;         // start playing once loaded (test autoload)
};

struct UiFileEntry {
    std::string name;
    bool dir;
    bool audio;
    bool selected;
};

struct UiState {
    RxApp *app;
    float  s;                  // layout scale (see top of file)
    ImFont *font_bold;         // NULL: the regular font
    float  font_base;          // style.FontSizeBase at init

    // Layout: the splits are RxConfig's (split_player, split_tree); the rest
// is ImGui's ini text kept in the library's "ui.layout" setting (ui.cpp).
    float  deck_split;         // fraction of the height for the player, < 0: natural height
    float  tree_width;         // logical pixels
    bool   show_info;
    bool   detail_half;        // rekordbox 2 PLAYER half-height detail instead of CDJ centred
    int    key_notation;       // 0 classic, 1 Camelot, 2 Open Key
    bool   layout_dirty;

    UiDeck decks[RX_DECKS];

    // Browser.
    int       anchor_row;
    bool      focus_search;
    bool      scroll_to_focus;
    uint64_t  info_track;      // track whose fields are in the info buffers
    unsigned  info_generation;
    char      info_buf[16][256];
    char      info_tags[512];
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
    RxPlaylistFolder *create_in;
    bool      move_pending, move_folder;
    void     *move_ptr;
    RxPlaylistFolder *move_to;
    int       move_pos;

    // Load confirmation (a playing track is never replaced unasked).
    bool      want_confirm_load;
    uint64_t  confirm_track;

    // Remove from library confirmation.
    bool      want_confirm_remove;
    std::vector<uint64_t> remove_ids;

    // Relocate (a missing track's new path).
    bool      want_relocate;
    uint64_t  relocate_id;
    char      relocate_buf[1024];

    // Dialogs.
    bool      dlg_analysis, dlg_export, dlg_keys, dlg_about, dlg_add, dlg_inspect, dlg_sync;
    unsigned  analyse_what;
    int       bpm_preset;
    bool      allow_sections;
    std::string fb_path;
    char      fb_edit[1024];
    std::vector<UiFileEntry> fb_entries;
    bool      fb_dirty;
    std::vector<std::string> volumes;
    bool      export_seen_running;
    int       export_scope;    // 0 all playlists, 1 selected playlists, 2 entire library
    std::vector<RxPlaylist *> export_playlists;
    char      stick_path[512];
    std::string inspect_report;
    int       inspect_result;
    bool      inspect_done;
    // Sync from USB: read + diff, review, apply.
    int       sync_stage;      // 0 pick, 1 review, 2 applied
    RxStickState sync_state;
    bool      sync_state_valid;
    RxSyncItem *sync_items;
    int       nb_sync_items;
    int       sync_applied;
    std::string sync_progress;
    std::string sync_error;
    int       sync_filter;     // -1 all, else an RxSyncKind

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
    bool      autoload_done;
    double    saved_zoom;
};

static inline UiState *ui_of(RxApp *app) { return (UiState *)app->ui; }

/* ------------------------------------------------------------------ ui.cpp */

// Texts and formatting.
const char *ui_fmt_time(double seconds, char *buf, size_t size, bool tenths);
const char *ui_key_text(const UiState *ui, const char *classic, char *buf, size_t size);
const char *ui_title(const RxTrack *t);
const char *ui_artist(const RxTrack *t);
uint32_t    ui_hot_cue_rgb(const RxCue *c);
uint32_t    ui_memory_cue_rgb(const RxCue *c);
uint32_t    ui_track_rgb(const RxTrack *t);
ImU32       ui_text_on(uint32_t rgb);

// Draw-list text: size in logical pixels; clipped to max_w with "..." when > 0.
void  ui_text(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col,
              const char *text, float max_w = 0.0f);
float ui_text_width(UiState *ui, ImFont *font, float size, const char *text);
void  ui_text_right(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 right_top,
                    ImU32 col, const char *text);
void  ui_text_centre(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 a, ImVec2 b,
                     ImU32 col, const char *text);

// Buttons drawn on the draw list; an InvisibleButton underneath so
// IsItemActivated/IsItemDeactivated give press and release.
enum UiIcon { UI_ICON_NONE = 0, UI_ICON_LEFT, UI_ICON_RIGHT, UI_ICON_PLAY, UI_ICON_PAUSE,
             UI_ICON_PLAYPAUSE, UI_ICON_LOOP, UI_ICON_CROSS, UI_ICON_PLUS, UI_ICON_MINUS };
struct UiButton {
    const char *label;         // shown text (may be NULL)
    UiIcon icon;
    ImU32  fill;               // 0: UI_BUTTON
    ImU32  text;               // 0: UI_TEXT
    ImU32  border;             // 0: none
    bool   lit;                // drawn in `lit_col`
    ImU32  lit_col;            // 0: accent
    bool   disabled;
    float  font;               // 0: 11
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
// track colours (also memory cue colours). Return true when one was picked
// into `out` ("" = none).
bool ui_hot_cue_colour_menu(UiState *ui, const char *current, char out[RX_COLOR_MAX]);
bool ui_track_colour_menu(UiState *ui, const char *current, char out[RX_COLOR_MAX]);

// Track loading with the rekordbox rule: a playing track is never replaced
// without asking.
void ui_load(UiState *ui, RxTrack *t);
void ui_load_confirmed(UiState *ui);
void ui_want_frames(UiState *ui, int n);
void ui_save_config(UiState *ui);

/* ---------------------------------------------------------- deck_view.cpp */

void ui_deck_panel(UiState *ui, int deck, ImVec2 size);
void ui_deck_shutdown(UiState *ui);
double ui_deck_position(UiState *ui, int deck, RxDeckState *st);
void ui_grid_edited(UiState *ui, int deck);
void ui_jump(UiState *ui, int deck, int dir);
void ui_shift_grid(UiState *ui, int deck, double seconds);

/* ------------------------------------------------------ waveform_view.cpp */

void ui_overview(UiState *ui, int deck, ImVec2 a, ImVec2 b);
void ui_detail(UiState *ui, int deck, ImVec2 a, ImVec2 b);
void ui_row_preview(UiState *ui, const RxTrack *t, ImVec2 a, ImVec2 b, bool analysing);
const RxBeat *ui_deck_beats(UiState *ui, int deck, int *nb);
double ui_zoom_step(double bars, int dir);

/* ------------------------------------------------------------ browser.cpp */

void ui_browser(UiState *ui, ImVec2 size);
void ui_info_panel(UiState *ui, ImVec2 size);
void ui_select_only(UiState *ui, uint64_t id);
bool ui_is_selected(const UiState *ui, uint64_t id);
void ui_remove_from_playlist(UiState *ui);
void ui_move_focus(UiState *ui, int delta);
RxTrack *ui_focus_track(UiState *ui);

// Drag and drop: a payload of track ids.
#define UI_DND_TRACKS "RX_TRACKS"
bool ui_accept_tracks(UiState *ui, std::vector<uint64_t> &ids);

/* --------------------------------------------------------------- tree.cpp */

void ui_tree(UiState *ui, ImVec2 size);

/* ------------------------------------------------------------ dialogs.cpp */

void ui_dialogs(UiState *ui);
void ui_open_add(UiState *ui);
void ui_open_export(UiState *ui);
void ui_open_inspect(UiState *ui);
void ui_open_sync(UiState *ui);
void ui_open_relocate(UiState *ui, RxTrack *t);
void ui_dialogs_shutdown(UiState *ui);
void ui_scan_volumes(UiState *ui);
// Keeps a self-sizing dialog centred over its first frames (call after Begin).
void ui_settle_centred(void);

/* --------------------------------------------------------------- keys.cpp */

void ui_keys(UiState *ui);
struct UiKeyRow { const char *key; const char *action; };
extern const UiKeyRow ui_key_rows[];
extern const int ui_nb_key_rows;

#endif /* RXBOX_UI_INTERNAL_H */
