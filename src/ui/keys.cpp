/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The keyboard: rekordbox's Export preset for player 1 (rbxport
// src/lib/keymap.ts, transcribed from rekordbox's own preset file), plus
// the browser's keys. There is one player, so the preset's shifted
// second-player set is not bound. Keys are ignored while a text field has
// the keyboard and, for the player, while a modal dialog is open.
//
// CUE and the hot cue keys hold like the buttons: press on key down,
// release on key up, so holding C at the cue point previews as on a CDJ.

#include "ui_internal.h"

const UiKeyRow ui_key_rows[] = {
    { "Space", "Play / pause" },
    { "C", "Cue (hold at the cue point to preview)" },
    { "Q", "Quantize" },
    { "T", "Time mode (elapsed / remaining)" },
    { "M", "Store memory cue" },
    { "B / N", "Call previous / next memory cue" },
    { "X", "Delete memory cue" },
    { "A S D F G H J K L ;", "Call memory cue 1 to 10" },
    { "I / O / R", "Loop in / out / exit-reloop" },
    { "4 5 6 7 8 9", "1 2 4 8 16 32 beat loop" },
    { "/  and  \\", "Loop half / double" },
    { "1 2 3", "Hot cue A B C (set or call)" },
    { "Ctrl+1 2 3", "Clear hot cue A B C" },
    { "Left / Right", "Beat jump back / forward" },
    { "Ctrl+Left / Ctrl+Right", "Shift the beat grid 5 ms earlier / later" },
    { "Ctrl+G", "Grid tools" },
    { "Up / Down", "Move the focus in the track list" },
    { "Enter", "Load the focused track" },
    { "Delete", "Remove the selection from the playlist" },
    { "Ctrl+A", "Select every track in the list" },
    { "Ctrl+F", "Search" },
    { "Esc", "Clear the search / close a dialog" },
    { "Ctrl+Q", "Quit" },
};
const int ui_nb_key_rows = (int)(sizeof(ui_key_rows) / sizeof(ui_key_rows[0]));

static bool pressed(ImGuiKey k)
{
    return ImGui::IsKeyPressed(k, false);
}

void ui_keys(UiState *ui)
{
    RXApp *app = ui->app;
    ImGuiIO &io = ImGui::GetIO();
    RXDeckView *dv = &app->decks[0];
    UiDeck *ud = &ui->decks[0];
    RXDeckCtl *c = &dv->ctl;

    // A held key is released even if the focus moved meanwhile. Up, not
    // "released": when the window loses the keyboard ImGui clears every key
    // and IsKeyReleased() never fires, which would leave CUE previewing.
    if (ud->key_cue_down && !ImGui::IsKeyDown(ImGuiKey_C)) {
        ud->key_cue_down = false;
        rx_ctl_cue_release(c);
    }
    static const ImGuiKey hot_keys[3] = { ImGuiKey_1, ImGuiKey_2, ImGuiKey_3 };
    for (int i = 0; i < 3; i++) {
        if ((ud->keys_hot_down & (1u << i)) && !ImGui::IsKeyDown(hot_keys[i])) {
            ud->keys_hot_down &= ~(1u << i);
            rx_ctl_hot_cue_release(c, i);
        }
    }

    if (io.WantTextInput)
        return;
    bool ctrl = io.KeyCtrl, alt = io.KeyAlt, shift = io.KeyShift;

    if (ctrl && pressed(ImGuiKey_Q)) {
        app->quit = true;
        if (app->window)
            rx_window_request_close(app->window);
        return;
    }
    if (ctrl && pressed(ImGuiKey_F)) {
        ui->focus_search = true;
        return;
    }
    if (ctrl && pressed(ImGuiKey_G)) {
        dv->grid_edit = !dv->grid_edit;
        ImGui::MarkIniSettingsDirty();
        return;
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return;

    // The browser.
    if (!ctrl && !alt) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            ui_move_focus(ui, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            ui_move_focus(ui, 1);
        if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
            ui_load(ui, ui_focus_track(ui));
        }
        if (pressed(ImGuiKey_Delete))
            ui_remove_from_playlist(ui);
    }

    // The player.
    bool has = c->track != nullptr;
    if (!has || alt)
        return;
    if (ctrl) {
        // Ctrl+1..3 clear hot cues A..C; Ctrl+arrows shift the grid.
        for (int i = 0; i < 3; i++) {
            if (pressed(hot_keys[i]))
                rx_ctl_hot_cue_delete(c, i);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            ui_shift_grid(ui, 0, -0.005);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            ui_shift_grid(ui, 0, 0.005);
        return;
    }
    if (shift)
        return;

    if (pressed(ImGuiKey_Space))
        rx_ctl_play_pause(c);
    if (pressed(ImGuiKey_C)) {
        rx_ctl_cue_press(c);
        ud->key_cue_down = true;
    }
    if (pressed(ImGuiKey_Q))
        rx_ctl_set_quantize(c, !c->quantize);
    if (pressed(ImGuiKey_T))
        ud->time_remaining = !ud->time_remaining;
    if (pressed(ImGuiKey_M))
        rx_ctl_memory_set(c);
    if (pressed(ImGuiKey_B))
        rx_ctl_memory_prev(c);
    if (pressed(ImGuiKey_N))
        rx_ctl_memory_next(c);
    if (pressed(ImGuiKey_X))
        rx_ctl_memory_delete(c);
    if (pressed(ImGuiKey_I))
        rx_ctl_loop_in(c);
    if (pressed(ImGuiKey_O))
        rx_ctl_loop_out(c);
    if (pressed(ImGuiKey_R))
        rx_ctl_exit_reloop(c);
    if (pressed(ImGuiKey_Slash))
        rx_ctl_loop_half(c);
    if (pressed(ImGuiKey_Backslash))
        rx_ctl_loop_double(c);
    if (pressed(ImGuiKey_LeftArrow))
        ui_jump(ui, 0, -1);
    if (pressed(ImGuiKey_RightArrow))
        ui_jump(ui, 0, 1);

    // 4..9: 1 2 4 8 16 32 beat loops.
    static const ImGuiKey loop_keys[6] = { ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8,
                                          ImGuiKey_9 };
    for (int i = 0; i < 6; i++) {
        if (pressed(loop_keys[i]))
            rx_ctl_auto_loop(c, (double)(1 << i));
    }
    for (int i = 0; i < 3; i++) {
        if (pressed(hot_keys[i])) {
            rx_ctl_hot_cue_press(c, i);
            ud->keys_hot_down |= 1u << i;
        }
    }

    // A S D F G H J K L ;: call memory cue 1..10 (from the start of the track).
    static const ImGuiKey mem_keys[10] = {
        ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_F, ImGuiKey_G,
        ImGuiKey_H, ImGuiKey_J, ImGuiKey_K, ImGuiKey_L, ImGuiKey_Semicolon,
    };
    for (int i = 0; i < 10; i++) {
        if (!pressed(mem_keys[i]))
            continue;
        int idx = rx_track_memory_cue_nth(c->track, i + 1);
        if (idx >= 0)
            rx_ctl_memory_call(c, idx);
    }
}
