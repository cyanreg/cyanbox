/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The PRO DJ LINK window (rxbox --pro-link): the switch, the network
// interface it serves on, and the devices on the link, each with what it
// has loaded, whether it plays and, for a track of ours, how far its play
// has been counted. The state is the app's (app->link_state, copied from
// the service every tick by src/app/link.c); this draws it and asks for
// changes. Without --pro-link nothing here is drawn and the switch does
// nothing (rx_app_link_start refuses).
//
// The interface is the user's choice, never a guess: while Link is on,
// anyone on that network can read the library's audio (docs/LINK.md,
// "Security"), so the window says so and the switch waits until an
// interface is chosen.

#include "ui_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

// The interface list is read again when the window opens and every few
// seconds while it is open (a cable plugged in).
#define IFS_REFRESH_US 3000000

static void read_interfaces(UiState *ui)
{
    RXLinkInterface ifs[32];
    int nb = rx_link_interfaces(ifs, 32);
    ui->link_ifs.assign(ifs, ifs + (nb > 0 ? (nb < 32 ? nb : 32) : 0));
    ui->link_ifs_time = rx_time_us();
}

static const RXLinkInterface *interface_named(UiState *ui, const char *name)
{
    for (const RXLinkInterface &i : ui->link_ifs) {
        if (name && !strcmp(i.name, name))
            return &i;
    }
    return nullptr;
}

static const char *interface_text(const RXLinkInterface &i, char *buf, size_t size)
{
    char a[16];
    snprintf(a, sizeof(a), "%u.%u.%u.%u", i.addr >> 24, i.addr >> 16 & 0xFF, i.addr >> 8 & 0xFF,
             i.addr & 0xFF);
    snprintf(buf, size, "%s  %s/%d%s", i.name, a, i.prefix, i.running ? "" : "  (no link)");
    return buf;
}

void ui_open_link(UiState *ui)
{
    if (!ui->app->link_state.enabled)
        return;
    ui->dlg_link = true;
    read_interfaces(ui);
}

void ui_link_switch(UiState *ui, bool on)
{
    RXApp *app = ui->app;
    if (!app->link_state.enabled)
        return;
    if (!on) {
        rx_app_link_stop(app);
        return;
    }
    const char *name = app->lib.cfg.link_interface;
    if (!name || !*name) {
        ui_open_link(ui);
        rx_app_set_status(app, "Choose the network interface the players are on");
        return;
    }
    RXLinkOptions o;
    rx_link_options_default(&o);
    o.interface = name;
    int ret = rx_app_link_start(app, &o);
    if (ret < 0 && ret != RX_ERROR(EBUSY) && ret != RX_ERROR(EALREADY))
        rx_app_set_status(app, "PRO DJ LINK cannot start on %s: %s", name, strerror(-ret));
}

const char *ui_link_summary(UiState *ui, char *buf, size_t size, ImU32 *col)
{
    const auto &ls = ui->app->link_state;
    *col = UI_DIM;
    if (ls.busy) {
        snprintf(buf, size, "Link: %s", ls.on ? "stopping" : "starting");
    } else if (!ls.on) {
        snprintf(buf, size, "Link: off");
        if (ls.err)
            *col = UI_RED;
    } else {
        int players = 0;
        for (int i = 0; i < ls.nb_devices; i++)
            players += ls.devices[i].type == RX_LINK_DEVICE_PLAYER;
        if (ls.state != RX_LINK_UP && !players)
            snprintf(buf, size, "Link: waiting on %s", ls.interface);
        else
            snprintf(buf, size, "Link: %d player%s", players, players == 1 ? "" : "s");
        *col = UI_ACCENT;
    }
    return buf;
}

// Why a start failed, in the user's terms.
static void error_text(const char *ifname, int err, char *buf, size_t size)
{
    switch (-err) {
    case EADDRINUSE:
        snprintf(buf, size, "The players' ports are taken: is rekordbox, or another rxbox, "
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
    case RX_LINK_DEVICE_PLAYER:   return "player";
    case RX_LINK_DEVICE_MIXER:    return "mixer";
    case RX_LINK_DEVICE_COMPUTER: return "computer";
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
    RXApp *app = ui->app;
    const auto &ls = app->link_state;
    if (!ls.nb_devices) {
        ImGui::TextDisabled("No player heard yet. A player shows up here once it is on the same");
        ImGui::TextDisabled("network and turned on; rxbox joins the link when it hears the first.");
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
        const RXLinkPlayer &d = ls.devices[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%d", d.number);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(d.name[0] ? d.name : type_name(d.type));
        char a[16];
        snprintf(a, sizeof(a), "%u.%u.%u.%u", d.addr >> 24, d.addr >> 16 & 0xFF,
                 d.addr >> 8 & 0xFF, d.addr & 0xFF);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s %d at %s", type_name(d.type), d.number, a);

        ImGui::TableSetColumnIndex(2);
        bool ours = ls.state == RX_LINK_UP && d.track_device == ls.number && d.track_id;
        if (d.type != RX_LINK_DEVICE_PLAYER) {
            ImGui::TextDisabled("%s", a);
        } else if (!d.has_status) {
            ImGui::TextDisabled("(no status yet)");
        } else if (!d.track_device || !d.track_id) {
            ImGui::TextDisabled("Nothing loaded");
        } else if (ours) {
            RXTrack *t = rx_library_find_id(&app->lib, d.track_id);
            char name[256];
            if (t)
                ImGui::TextUnformatted(rx_track_display(t, name, sizeof(name)));
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
        if (d.type == RX_LINK_DEVICE_PLAYER && d.has_status && d.track_id) {
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
    RXApp *app = ui->app;
    auto &ls = app->link_state;
    if (!ls.enabled || !ui->dlg_link)
        return;
    if (rx_time_us() - ui->link_ifs_time > IFS_REFRESH_US)
        read_interfaces(ui);
    ui_centre_next(ui, 720, 360);
    if (!ImGui::Begin("PRO DJ LINK", &ui->dlg_link,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ui_escape_closes(&ui->dlg_link);

    // The switch.
    const char *chosen = app->lib.cfg.link_interface;
    // Busy is a change under way: starting while off, stopping while on.
    bool on = ls.on != ls.busy;
    bool want = on;
    ImGui::BeginDisabled(ls.busy || (!ls.on && (!chosen || !*chosen)));
    if (ImGui::Checkbox("Serve the library to players", &want) && want != on)
        ui_link_switch(ui, want);
    ImGui::EndDisabled();

    // The interface, chosen while off.
    ImGui::SameLine(0, PX(24));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("on");
    ImGui::SameLine();
    const RXLinkInterface *cur = interface_named(ui, chosen);
    char preview[96];
    if (cur)
        interface_text(*cur, preview, sizeof(preview));
    else if (chosen && *chosen)
        snprintf(preview, sizeof(preview), "%s  (not an Ethernet interface that is up)", chosen);
    else
        snprintf(preview, sizeof(preview), "Choose the players' network...");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::BeginDisabled(ls.on || ls.busy);
    if (ImGui::BeginCombo("##link_if", preview)) {
        if (ImGui::IsWindowAppearing())
            read_interfaces(ui);
        if (ui->link_ifs.empty())
            ImGui::TextDisabled("No network interface is up.");
        for (const RXLinkInterface &i : ui->link_ifs) {
            char text[96];
            bool sel = chosen && !strcmp(chosen, i.name);
            if (ImGui::Selectable(interface_text(i, text, sizeof(text)), sel) && !sel) {
                rx_config_set(&app->lib.cfg, "link.interface", i.name);
                ui_save_config(ui);
                chosen = app->lib.cfg.link_interface;
                ls.err = 0;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if ((ls.on || ls.busy) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Switch Link off to change the interface.");

    // Where it stands.
    char line[256];
    ImU32 col = UI_DIM;
    if (ls.busy) {
        snprintf(line, sizeof(line), "%s...", ls.on ? "Stopping" : "Starting");
    } else if (!ls.on && ls.err) {
        error_text(ls.interface, ls.err, line, sizeof(line));
        col = UI_RED;
    } else if (!ls.on) {
        snprintf(line, sizeof(line), "Off. Players see nothing of this computer.");
    } else if (ls.state == RX_LINK_UP) {
        snprintf(line, sizeof(line), "On the link as rekordbox, device %d, on %s.", ls.number,
                 ls.interface);
        col = UI_PLAY;
    } else if (ls.state == RX_LINK_JOINING) {
        snprintf(line, sizeof(line), "Joining the link on %s...", ls.interface);
        col = UI_ACCENT;
    } else {
        snprintf(line, sizeof(line), "Waiting for a player on %s.", ls.interface);
        col = UI_ACCENT;
    }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", line);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("While on, anything on this network can read the library's audio, "
                        "artwork and analysis (never other files, never writes). Use a network "
                        "with only the players on it: a cable or a switch of their own.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    devices_table(ui);
    ImGui::End();
}
