/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The PRO DJ LINK window (cyanbox --pro-link): the Ethernet interfaces
// served, each with where it stands, and the devices on them, each with
// what it has loaded, whether it plays and, for a track of ours, how far
// its play has been counted. The state is the app's (app->link_state,
// copied from the set of services every tick by src/app/link.c); Link
// serves on every Ethernet interface from the start, so this only shows
// it. Without --pro-link nothing here is drawn.

#include "ui_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void ui_open_link(UiState *ui)
{
    if (ui->app->link_state.enabled)
        ui->dlg_link = true;
}

const char *ui_link_summary(UiState *ui, char *buf, size_t size, ImU32 *col)
{
    const auto &ls = ui->app->link_state;
    int players = 0, up = 0, joined = 0, failed = 0;
    for (int i = 0; i < ls.nb_devices; i++)
        players += ls.devices[i].type == CB_LINK_DEVICE_PLAYER;
    for (int i = 0; i < ls.nb_interfaces; i++) {
        up += ls.interfaces[i].running;
        joined += ls.interfaces[i].running && ls.interfaces[i].state == CB_LINK_UP;
        failed += !ls.interfaces[i].running && ls.interfaces[i].err;
    }
    // Green once on the link on one network at least, yellow until then.
    ImU32 ready = joined ? UI_PLAY : UI_YELLOW;
    *col = UI_DIM;
    if (!ls.on) {
        snprintf(buf, size, "Link: off");
    } else if (players) {
        snprintf(buf, size, "Link: %d player%s", players, players == 1 ? "" : "s");
        *col = ready;
    } else if (up) {
        snprintf(buf, size, "Link: on %d network%s", up, up == 1 ? "" : "s");
        *col = ready;
    } else if (failed) {
        snprintf(buf, size, "Link: cannot serve");
        *col = UI_RED;
    } else if (ls.nb_interfaces) {
        snprintf(buf, size, "Link: starting");
        *col = UI_YELLOW;
    } else {
        snprintf(buf, size, "Link: no Ethernet");
    }
    return buf;
}

const char *ui_link_cue_summary(UiState *ui, char *buf, size_t size, ImU32 *col)
{
    const CBLinkCueState &c = ui->app->link_state.cue;
    if (!ui->app->link_state.cue_enabled || !c.mixer) {
        snprintf(buf, size, "LINK CUE: no mixer");
        *col = UI_DIM;
        return buf;
    }
    if (!c.on)
        snprintf(buf, size, "LINK CUE: off on %s", c.name);
    else if (c.ours && c.sending)
        snprintf(buf, size, "LINK CUE: playing on %s", c.name);
    else if (c.ours || !c.source)
        snprintf(buf, size, "LINK CUE: %s", c.name);
    else
        snprintf(buf, size, "LINK CUE: %s has %s %d", c.name, c.source <= 6 ? "player" : "device",
                 c.source);
    // Another source elected keeps it until a deck here starts (cue.c),
    // and is what the mixer plays once its LINK CUE is on: the mixer can,
    // but something else has it. Ours, or nobody's yet, it is cyanbox's.
    *col = c.source && !c.ours ? UI_YELLOW : UI_PLAY;
    return buf;
}

// Why a service would not start, in the user's terms.
static void error_text(const char *ifname, int err, char *buf, size_t size)
{
    switch (-err) {
    case EADDRINUSE:
        snprintf(buf, size, "The players' ports are taken: is rekordbox, or another cyanbox, "
                 "serving on this computer?");
        break;
    case ENODEV:
        snprintf(buf, size, "There is no interface %s (any more).", ifname);
        break;
    case EADDRNOTAVAIL:
        snprintf(buf, size, "%s has no IPv4 address yet.", ifname);
        break;
    case EPROTONOSUPPORT:
        snprintf(buf, size, "%s is not Ethernet: Link runs on Ethernet only.", ifname);
        break;
    case EACCES:
    case EPERM:
        snprintf(buf, size, "Not allowed to open the players' ports on %s: %s.", ifname,
                 strerror(-err));
        break;
    default:
        snprintf(buf, size, "Cannot start on %s: %s.", ifname, strerror(-err));
        break;
    }
}

static const char *slot_name(int slot)
{
    switch (slot) {
    case 1:  return "CD";
    case 2:  return "SD";
    case 3:  return "USB";
    case 4:  return "rekordbox";
    default: return "media";
    }
}

static const char *type_name(int type)
{
    switch (type) {
    case CB_LINK_DEVICE_PLAYER:   return "player";
    case CB_LINK_DEVICE_MIXER:    return "mixer";
    case CB_LINK_DEVICE_COMPUTER: return "computer";
    default:                      return "device";
    }
}

static void tag(const char *text, ImU32 col)
{
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", text);
}

static void devices_table(UiState *ui)
{
    CBApp *app = ui->app;
    const auto &ls = app->link_state;
    if (!ls.nb_devices) {
        ImGui::TextDisabled("No player heard yet: a player shows up here once it is switched on");
        ImGui::TextDisabled("on one of these networks.");
        return;
    }
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##link_devices", 5, tf, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, PX(28));
    ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthFixed, PX(150));
    ImGui::TableSetupColumn("Loaded", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Tempo", ImGuiTableColumnFlags_WidthFixed, PX(120));
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, PX(200));
    ImGui::TableHeadersRow();
    for (int i = 0; i < ls.nb_devices; i++) {
        const CBLinkPlayer &d = ls.devices[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%d", d.number);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(d.name[0] ? d.name : type_name(d.type));
        char a[16];
        snprintf(a, sizeof(a), "%u.%u.%u.%u", d.addr >> 24, d.addr >> 16 & 0xFF,
                 d.addr >> 8 & 0xFF, d.addr & 0xFF);
        const CBLinkServed *on = ls.device_if[i] < ls.nb_interfaces
                                 ? &ls.interfaces[ls.device_if[i]] : nullptr;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s %d at %s, on %s", type_name(d.type), d.number, a,
                              on ? on->interface : "?");

        ImGui::TableSetColumnIndex(2);
        // Ours: loaded from our number on the device's own network.
        bool ours = on && on->running && on->state == CB_LINK_UP && d.track_device == on->number &&
                    d.track_id;
        if (d.type != CB_LINK_DEVICE_PLAYER) {
            ImGui::TextDisabled("%s", a);
        } else if (!d.has_status) {
            ImGui::TextDisabled("(no status yet)");
        } else if (!d.track_device || !d.track_id) {
            ImGui::TextDisabled("Nothing loaded");
        } else if (ours) {
            CBTrack *t = cb_library_find_id(&app->lib, d.track_id);
            char name[256];
            if (t)
                ImGui::TextUnformatted(cb_track_display(t, name, sizeof(name)));
            else
                ImGui::TextDisabled("Track %u, gone from the library", d.track_id);
        } else if (d.track_device == d.number) {
            ImGui::TextDisabled("Its own %s, track %u", slot_name(d.track_slot), d.track_id);
        } else {
            ImGui::TextDisabled("Player %d's %s, track %u", d.track_device,
                                slot_name(d.track_slot), d.track_id);
        }

        ImGui::TableSetColumnIndex(3);
        if (d.has_status && d.bpm > 0) {
            ImGui::Text("%.2f", d.bpm * (1.0 + d.pitch / 100.0));
            if (d.pitch != 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%+.2f%%", d.pitch);
            }
        }

        ImGui::TableSetColumnIndex(4);
        if (d.type == CB_LINK_DEVICE_PLAYER && d.has_status && d.track_id) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(d.playing ? UI_PLAY : UI_DIM), "%s",
                               d.playing ? "Playing" : "Paused");
            if (d.on_air)
                tag("ON AIR", UI_RED);
            if (d.master)
                tag("MASTER", UI_ACCENT);
            if (d.synced)
                tag("SYNC", UI_DIM);
            // A track of ours: its play, counted at 30 s as the deck's.
            if (ours && ls.counted[i]) {
                tag("counted", UI_PLAY);
            } else if (ours && ls.played[i] >= 1.0) {
                char s[32];
                snprintf(s, sizeof(s), "%.0f s", ls.played[i]);
                tag(s, UI_DIM);
            }
        } else if (d.master) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT), "MASTER");
        }
    }
    ImGui::EndTable();
}

void ui_link_window(UiState *ui)
{
    CBApp *app = ui->app;
    const auto &ls = app->link_state;
    if (!ls.enabled || !ui->dlg_link)
        return;
    ui_centre_next(ui, 720, 360);
    if (!ImGui::Begin("PRO DJ LINK", &ui->dlg_link,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_escape_closes(&ui->dlg_link);

    // Each Ethernet interface: where it stands there.
    if (!ls.on)
        ImGui::TextDisabled("Off.");
    else if (!ls.nb_interfaces)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_ACCENT),
                           "No Ethernet interface is up with an address: cyanbox serves on one as "
                           "soon as it is.");
    for (int i = 0; i < ls.nb_interfaces; i++) {
        const CBLinkServed &s = ls.interfaces[i];
        char a[16], line[256];
        snprintf(a, sizeof(a), "%u.%u.%u.%u", s.addr >> 24, s.addr >> 16 & 0xFF,
                 s.addr >> 8 & 0xFF, s.addr & 0xFF);
        ImU32 col = UI_ACCENT;
        if (!s.running && s.err) {
            error_text(s.interface, s.err, line, sizeof(line));
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " Trying again.");
            col = UI_RED;
        } else if (!s.running) {
            snprintf(line, sizeof(line), "%s (%s): starting...", s.interface, a);
        } else if (s.state == CB_LINK_UP) {
            snprintf(line, sizeof(line), "%s (%s): on the link as rekordbox, device %d.",
                     s.interface, a, s.number);
            col = UI_PLAY;
        } else {
            snprintf(line, sizeof(line), "%s (%s): joining the link...", s.interface, a);
        }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", line);
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("cyanbox serves on every Ethernet interface while it runs with --pro-link. "
                        "Anything on those networks can read the library's audio, artwork and "
                        "analysis (never other files, never writes): the best network has only "
                        "the players on it, a cable or a switch of their own.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    devices_table(ui);
    ImGui::End();
}
