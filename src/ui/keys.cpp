/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The keyboard: rekordbox's Export preset for player 1 (rbxport
// src/lib/keymap.ts, transcribed from rekordbox's own preset file), plus
// the browser's keys. There is one player, so the preset's shifted
// second-player set is not bound. Keys are ignored while a text field has
// the keyboard and, for the player and the browser, while a modal dialog
// is open or a dialog window (Add, Export, the settings...) has the
// focus: Space would play behind it, Delete take tracks out of a
// playlist. Ctrl+Q, Ctrl+F, Ctrl+G and Ctrl+, act everywhere.
//
// CUE and the hot cue keys hold like the buttons: press on key down,
// release on key up, so holding C at the cue point previews as on a CDJ.
// The beat jump arrows repeat while held at their own rate (UI_REPEAT_*,
// the strip's buttons share it), not ImGui's text-editing one; the grid
// shift on Ctrl+arrows moves once per press.

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
    { "Left / Right", "Beat jump back / forward (hold to repeat)" },
    { "Ctrl+Left / Ctrl+Right", "Shift the beat grid 5 ms earlier / later, once per press" },
    { "Ctrl+G", "Grid tools" },
    { "Up / Down", "Move the focus in the track list (the selection in the cue list after a "
                   "click on a cue)" },
    { "Enter", "Load the focused track" },
    { "Delete", "Remove the selection from the playlist; after a click on a cue, delete that cue" },
    { "Ctrl+A", "Select every track in the list" },
    { "Ctrl+F", "Search" },
    { "Ctrl+,", "Player settings (the My Settings the export writes)" },
    { "Esc", "Clear the search / close a dialog / give Delete back to the track list" },
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
        // The Add dialog's finder while that dialog has the focus.
        if (ui->dlg_add && ui->fb_focused)
            ui->fb_find_focus = true;
        else
            ui->focus_search = true;
        return;
    }
    if (ctrl && pressed(ImGuiKey_G)) {
        dv->grid_edit = !dv->grid_edit;
        ImGui::MarkIniSettingsDirty();
        return;
    }
    // Ctrl+, opens the settings in most programs; here, the players'.
    if (ctrl && pressed(ImGuiKey_Comma)) {
        ui_open_settings(ui);
        return;
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return;
    // The window with the focus as of last frame (this runs before the
    // windows of this one): a dialog's, or the main window's.
    ImGuiWindow *focus = GImGui->NavWindow;
    if (focus && focus->RootWindow && strcmp(focus->RootWindow->Name, "##rxbox") != 0)
        return;

    // The browser, or the player's cue list while a click on one of its
    // rows has given it the keyboard: then Delete deletes the selected cue
    // and Up/Down move the selection, until Escape, a click in the table or
    // another track gives them back to the table.
    if (!ctrl && !alt) {
        bool cues = ui_cue_list_has_keys(ui, 0);
        if (cues && pressed(ImGuiKey_Escape)) {
            ui_cue_list_release(ui, 0);
            cues = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            if (cues)
                ui_cue_list_move(ui, 0, -1);
            else
                ui_move_focus(ui, -1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            if (cues)
                ui_cue_list_move(ui, 0, 1);
            else
                ui_move_focus(ui, 1);
        }
        if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
            ui_load(ui, ui_focus_track(ui));
        }
        if (pressed(ImGuiKey_Delete)) {
            if (cues)
                ui_cue_list_delete(ui, 0);
            else
                ui_remove_from_playlist(ui);
        }
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
        if (pressed(ImGuiKey_LeftArrow))
            ui_shift_grid(ui, 0, -0.005);
        if (pressed(ImGuiKey_RightArrow))
            ui_shift_grid(ui, 0, 0.005);
        return;
    }
    if (shift) {
        // Shift+arrows: one beat at a time whatever the jump size, repeating
        // while held as the plain arrows do.
        int back = ImGui::GetKeyPressedAmount(ImGuiKey_LeftArrow, UI_REPEAT_DELAY, UI_REPEAT_RATE);
        int fwd = ImGui::GetKeyPressedAmount(ImGuiKey_RightArrow, UI_REPEAT_DELAY, UI_REPEAT_RATE);
        for (int i = 0; i < back; i++)
            rx_ctl_beat_jump(c, -1);
        for (int i = 0; i < fwd; i++)
            rx_ctl_beat_jump(c, 1);
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow))
            ui_want_frames(ui, 2);
        return;
    }

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
    // Beat jump: once on the press, then again every UI_REPEAT_RATE once
    // held for UI_REPEAT_DELAY. A paused interface draws only on input, so
    // a held arrow keeps the frames coming.
    int back = ImGui::GetKeyPressedAmount(ImGuiKey_LeftArrow, UI_REPEAT_DELAY, UI_REPEAT_RATE);
    int fwd = ImGui::GetKeyPressedAmount(ImGuiKey_RightArrow, UI_REPEAT_DELAY, UI_REPEAT_RATE);
    for (int i = 0; i < back; i++)
        ui_jump(ui, 0, -1);
    for (int i = 0; i < fwd; i++)
        ui_jump(ui, 0, 1);
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow))
        ui_want_frames(ui, 2);

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
