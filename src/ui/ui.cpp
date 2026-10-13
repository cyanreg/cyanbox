/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The interface's entry points (rx_ui_init, rx_ui_frame, rx_ui_shutdown),
// the window layout (rekordbox's "1 PLAYER" Export layout: the player across
// the top, the browser under it, a status bar at the bottom), the menu bar,
// the style, and the small drawing helpers every screen shares. There is
// one player: rxbox prepares a stick, it does not mix.
//
// Preferences live in the library: the detail zoom, the Traffic Light and
// the two splits in RXConfig (ui.* settings); the rest of the layout (the
// table's columns, the detail's style, the grid tools) is ImGui's ini text,
// stored whole in the "ui.layout" setting instead of a file. What the
// players' My Settings also say (the waveform colour, the overview type,
// the key notation, the playhead's place, hot cue colours, and the deck's
// state at startup: quantize, beat jump, tempo range, master tempo, time
// mode) comes from them alone (follow_settings), so rxbox shows and
// starts as the players will.
//
// The headless test hooks are environment variables read here:
// RXBOX_UI_AUTOLOAD=<id> loads that track on the first frame and starts it
// once it is loaded (RXBOX_UI_AUTOLOAD_AT=<seconds>: from there);
// RXBOX_UI_SHOW=<list> opens panels and dialogs on the first frame (grid,
// half, export, sync, inspect, analysis, settings, add, keys, about), and
// `tags` asks for the autoloaded track's tags from the model;
// RXBOX_UI_SCREENSHOT=<png> saves frame 100 (RXBOX_UI_SCREENSHOT_FRAME=<n>:
// frame n) through rx_window_screenshot_next(), which writes it once it is
// presented, and the run goes on; a run that ends before that frame
// (--frames below it) saves its last frame at shutdown instead. The state
// the tests look at (the loaded track, playing, a popup open, how the
// overview and the row previews were drawn, the deck's KEY readout) is
// logged with it. RXBOX_UI_EVENTS=<frame>:<event>,... gives ImGui input at those
// frames: down:<key> and up:<key> (ImGui's key names: C, Enter,
// DownArrow...), focus-out and focus-in, click:<x>:<y> (the pointer moved
// there, in logical pixels, and the left button pressed and let go two
// frames later), text:<characters> (typed, '+' for a space).

#include "ui_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include <algorithm>
#include <new>

#include <fontconfig/fontconfig.h>

#include "config.h"

/* ------------------------------------------------------------------ helpers */

const char *ui_fmt_time(double seconds, char *buf, size_t size, bool tenths)
{
    bool neg = seconds < 0;
    if (neg)
        seconds = -seconds;
    if (!isfinite(seconds))
        seconds = 0;
    if (tenths) {
        long ds = (long)floor(seconds * 10.0 + 1e-6);
        snprintf(buf, size, "%s%ld:%02ld.%ld", neg ? "-" : "", ds / 600, (ds / 10) % 60, ds % 10);
    } else {
        long s = (long)floor(seconds + 0.5);
        snprintf(buf, size, "%s%ld:%02ld", neg ? "-" : "", s / 60, s % 60);
    }
    return buf;
}

// The players' key display (device.key_display): classic names, or for
// "alphanumeric" the Camelot codes (8A).
const char *ui_key_text(const UiState *ui, const char *classic, char *buf, size_t size)
{
    if (!classic || !classic[0])
        return "";
    const char *k = rx_key_display(classic, ui->key_camelot ? "camelot" : "classic", buf, size);
    return k ? k : classic;
}

const char *ui_title(const RXTrack *t)
{
    if (t->meta.title && t->meta.title[0])
        return t->meta.title;
    return t->path ? rx_path_base(t->path) : "";
}

const char *ui_artist(const RXTrack *t)
{
    return t->meta.artist ? t->meta.artist : "";
}

// A hot cue's pad colour: with the players' HOT CUE COLOR on, its stored
// colour through rekordbox's 64-entry palette; with it off, or with no
// colour, the slot's default pad colour (rxbox/colors.h). The pads, the
// badges on the waveforms and in the cue list and the row previews all
// draw through here.
uint32_t ui_hot_cue_rgb(const UiState *ui, const RXCue *c)
{
    int code = 0;
    uint32_t rgb = 0;
    if (ui->hot_cue_colour && c->color[0] && rx_hot_cue_color(c->color, &code, &rgb) == 0 && rgb)
        return rgb;
    return rx_hot_cue_default_rgb(c->hot >= 0 ? c->hot : 0);
}

// Memory cues are rekordbox's red unless given one of the track colours.
uint32_t ui_memory_cue_rgb(const RXCue *c)
{
    if (c->color[0]) {
        int id = rx_memory_cue_color_id(c->color);
        if (id > 0)
            return rx_memory_cue_color_rgb(id);
        uint32_t rgb;
        if (rx_parse_hex_rgb(c->color, &rgb) == 0)
            return rgb;
    }
    return 0xEA3323;
}

// The track's hue at full saturation and the value the track colours are
// drawn at; 0 for none.
uint32_t ui_track_rgb(const RXTrack *t)
{
    return rx_track_hue_rgb(t->meta.hue);
}

// Black or white text for a filled colour, by its luma.
ImU32 ui_text_on(uint32_t rgb)
{
    float r = ((rgb >> 16) & 0xFF) / 255.0f;
    float g = ((rgb >> 8) & 0xFF) / 255.0f;
    float b = (rgb & 0xFF) / 255.0f;
    return 0.299f * r + 0.587f * g + 0.114f * b > 0.55f ? UI_BLACK : UI_TEXT;
}

float ui_text_width(UiState *ui, ImFont *font, float size, const char *text)
{
    if (!font)
        font = ImGui::GetFont();
    return font->CalcTextSizeA(PX(size), FLT_MAX, 0.0f, text).x;
}

void ui_text(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col,
             const char *text, float max_w)
{
    if (!text || !text[0])
        return;
    if (!font)
        font = ImGui::GetFont();
    float px = PX(size);
    const char *end = text + strlen(text);
    if (max_w <= 0 || font->CalcTextSizeA(px, FLT_MAX, 0.0f, text, end).x <= max_w) {
        dl->AddText(font, px, pos, col, text, end);
        return;
    }
    // Longest UTF-8 prefix that fits with "..." after it.
    float dots = font->CalcTextSizeA(px, FLT_MAX, 0.0f, "...").x;
    const char *lo = text, *hi = end;
    while (lo < hi) {
        const char *mid = lo + (hi - lo + 1) / 2;
        while (mid > text && mid < end && ((unsigned char)*mid & 0xC0) == 0x80)
            mid--;
        if (mid <= lo)
            break;
        if (font->CalcTextSizeA(px, FLT_MAX, 0.0f, text, mid).x + dots <= max_w)
            lo = mid;
        else
            hi = mid - 1;
    }
    while (lo > text && lo < end && ((unsigned char)*lo & 0xC0) == 0x80)
        lo--;
    dl->AddText(font, px, pos, col, text, lo);
    float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text, lo).x;
    dl->AddText(font, px, ImVec2(pos.x + w, pos.y), col, "...");
}

void ui_text_right(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 right_top,
                   ImU32 col, const char *text)
{
    float w = ui_text_width(ui, font, size, text);
    ui_text(ui, dl, font, size, ImVec2(right_top.x - w, right_top.y), col, text);
}

void ui_text_centre(UiState *ui, ImDrawList *dl, ImFont *font, float size, ImVec2 a, ImVec2 b,
                    ImU32 col, const char *text)
{
    if (!font)
        font = ImGui::GetFont();
    ImVec2 ts = font->CalcTextSizeA(PX(size), FLT_MAX, 0.0f, text);
    ImVec2 p(floorf((a.x + b.x - ts.x) * 0.5f), floorf((a.y + b.y - ts.y) * 0.5f));
    dl->AddText(font, PX(size), p, col, text);
}

void ui_icon(ImDrawList *dl, UiIcon icon, ImVec2 a, ImVec2 b, ImU32 col)
{
    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    float r = ImMin(b.x - a.x, b.y - a.y) * 0.5f;
    switch (icon) {
    case UI_ICON_LEFT:
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.7f, c.y), ImVec2(c.x + r * 0.5f, c.y - r * 0.7f),
                              ImVec2(c.x + r * 0.5f, c.y + r * 0.7f), col);
        break;
    case UI_ICON_RIGHT:
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.7f, c.y), ImVec2(c.x - r * 0.5f, c.y + r * 0.7f),
                              ImVec2(c.x - r * 0.5f, c.y - r * 0.7f), col);
        break;
    case UI_ICON_PLAY:
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.45f, c.y - r * 0.6f), ImVec2(c.x + r * 0.65f, c.y),
                              ImVec2(c.x - r * 0.45f, c.y + r * 0.6f), col);
        break;
    case UI_ICON_PAUSE:
        dl->AddRectFilled(ImVec2(c.x - r * 0.5f, c.y - r * 0.55f), ImVec2(c.x - r * 0.15f, c.y + r * 0.55f), col);
        dl->AddRectFilled(ImVec2(c.x + r * 0.15f, c.y - r * 0.55f), ImVec2(c.x + r * 0.5f, c.y + r * 0.55f), col);
        break;
    case UI_ICON_PLAYPAUSE: {
        // The CDJ's PLAY/PAUSE legend: a triangle and two bars side by side.
        float h = r * 0.42f;
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.62f, c.y - h), ImVec2(c.x - r * 0.02f, c.y),
                              ImVec2(c.x - r * 0.62f, c.y + h), col);
        dl->AddRectFilled(ImVec2(c.x + r * 0.14f, c.y - h), ImVec2(c.x + r * 0.30f, c.y + h), col);
        dl->AddRectFilled(ImVec2(c.x + r * 0.44f, c.y - h), ImVec2(c.x + r * 0.60f, c.y + h), col);
        break;
    }
    case UI_ICON_LOOP: {
        // Two arcs with arrow heads: the loop legend on the pads.
        float rr = r * 0.62f;
        dl->PathArcTo(c, rr, IM_PI * 0.15f, IM_PI * 0.95f, 10);
        dl->PathStroke(col, ImMax(1.0f, r * 0.16f));
        dl->PathArcTo(c, rr, IM_PI * 1.15f, IM_PI * 1.95f, 10);
        dl->PathStroke(col, ImMax(1.0f, r * 0.16f));
        ImVec2 p1(c.x - rr, c.y), p2(c.x + rr, c.y);
        float h = r * 0.3f;
        dl->AddTriangleFilled(ImVec2(p1.x - h, p1.y - h * 0.2f), ImVec2(p1.x + h, p1.y - h * 0.2f),
                              ImVec2(p1.x, p1.y + h), col);
        dl->AddTriangleFilled(ImVec2(p2.x - h, p2.y + h * 0.2f), ImVec2(p2.x + h, p2.y + h * 0.2f),
                              ImVec2(p2.x, p2.y - h), col);
        break;
    }
    case UI_ICON_CROSS: {
        float k = r * 0.5f, t = ImMax(1.0f, r * 0.2f);
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, t);
        dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, t);
        break;
    }
    case UI_ICON_DIVIDE: {
        // Drawn rather than set: the font's obelus at button sizes reads
        // as a plus.
        float k = r * 0.55f, t = ImMax(1.0f, r * 0.18f), d = ImMax(1.0f, r * 0.15f);
        dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x + k, c.y), col, t);
        dl->AddCircleFilled(ImVec2(c.x, c.y - k * 0.8f), d, col, 8);
        dl->AddCircleFilled(ImVec2(c.x, c.y + k * 0.8f), d, col, 8);
        break;
    }
    case UI_ICON_PLUS:
    case UI_ICON_MINUS: {
        float k = r * 0.55f, t = ImMax(1.0f, r * 0.18f);
        dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x + k, c.y), col, t);
        if (icon == UI_ICON_PLUS)
            dl->AddLine(ImVec2(c.x, c.y - k), ImVec2(c.x, c.y + k), col, t);
        break;
    }
    case UI_ICON_NONE:
        break;
    }
}

// The placeholder sleeve: a record.
void ui_record_icon(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    float r = ImMin(b.x - a.x, b.y - a.y) * 0.42f;
    dl->AddRectFilled(a, b, IM_COL32(0x18, 0x18, 0x18, 0xFF), 2.0f);
    dl->AddCircleFilled(c, r, IM_COL32(0x0A, 0x0A, 0x0A, 0xFF), 32);
    dl->AddCircle(c, r * 0.8f, IM_COL32(0x26, 0x26, 0x26, 0xFF), 32);
    dl->AddCircle(c, r * 0.6f, IM_COL32(0x26, 0x26, 0x26, 0xFF), 32);
    dl->AddCircleFilled(c, r * 0.32f, IM_COL32(0x5A, 0x5A, 0x5A, 0xFF), 24);
    dl->AddCircleFilled(c, r * 0.06f, IM_COL32(0x0A, 0x0A, 0x0A, 0xFF), 8);
}

void ui_spinner(ImDrawList *dl, ImVec2 c, float r, ImU32 col)
{
    float t = (float)ImGui::GetTime() * 6.0f;
    dl->PathArcTo(c, r, t, t + IM_PI * 1.4f, 16);
    dl->PathStroke(col, ImMax(1.0f, r * 0.3f));
}

void ui_star(ImDrawList *dl, ImVec2 c, float r, ImU32 col)
{
    ImVec2 pts[10];
    for (int i = 0; i < 10; i++) {
        float a = -IM_PI * 0.5f + i * IM_PI / 5.0f;
        float rr = i & 1 ? r * 0.45f : r;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    // A concave star: a fan of triangles from the centre.
    for (int i = 0; i < 10; i++)
        dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
}

void ui_tooltip(const char *text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", text);
}

bool ui_swatch(UiState *ui, const char *id, uint32_t rgb, bool selected, float size)
{
    ImVec2 sz(PX(size), PX(size));
    bool pressed = ImGui::InvisibleButton(id, sz);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (rgb)
        dl->AddRectFilled(a, b, ui_rgb(rgb), PX(2));
    else
        dl->AddLine(ImVec2(a.x + PX(2), b.y - PX(2)), ImVec2(b.x - PX(2), a.y + PX(2)), UI_RED,
                    PX(1.5f));
    if (selected)
        dl->AddRect(ImVec2(a.x - PX(1), a.y - PX(1)), ImVec2(b.x + PX(1), b.y + PX(1)), UI_TEXT, PX(2),
                    PX(1.5f));
    else if (ImGui::IsItemHovered())
        dl->AddRect(a, b, UI_DIM, PX(2));
    if (!rgb)
        dl->AddRect(a, b, UI_FAINT, PX(2));
    return pressed;
}

bool ui_hot_cue_colour_menu(UiState *ui, const char *current, char out[RX_COLOR_MAX])
{
    bool picked = false;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(PX(4), PX(4)));
    for (int i = 0; i < RX_HOT_CUE_COLORS; i++) {
        const char *name = rx_hot_cue_color_names[i];
        int code = 0;
        uint32_t rgb = 0;
        rx_hot_cue_color(name, &code, &rgb);
        char id[32];
        snprintf(id, sizeof(id), "##hc%d", i);
        if (i % 8)
            ImGui::SameLine();
        if (ui_swatch(ui, id, rgb, current && !strcmp(current, name), 18)) {
            snprintf(out, RX_COLOR_MAX, "%s", name);
            picked = true;
        }
        ui_tooltip(name);
    }
    if (ui_swatch(ui, "##hcnone", 0, !current || !current[0], 18)) {
        out[0] = 0;
        picked = true;
    }
    ui_tooltip("Default pad colour");
    ImGui::PopStyleVar();
    return picked;
}

bool ui_track_colour_menu(UiState *ui, const char *current, char out[RX_COLOR_MAX])
{
    bool picked = false;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(PX(4), PX(4)));
    if (ui_swatch(ui, "##tcnone", 0, !current || !current[0], 18)) {
        out[0] = 0;
        picked = true;
    }
    ui_tooltip("No colour");
    for (int i = 0; i < RX_TRACK_COLORS; i++) {
        const char *name = rx_track_color_names[i];
        char id[32];
        snprintf(id, sizeof(id), "##tc%d", i);
        ImGui::SameLine();
        if (ui_swatch(ui, id, rx_track_color_rgb(i + 1), current && !strcmp(current, name), 18)) {
            snprintf(out, RX_COLOR_MAX, "%s", name);
            picked = true;
        }
        ui_tooltip(name);
    }
    ImGui::PopStyleVar();
    return picked;
}

// The eight swatches in rekordbox's order (the legend's), after "none".
// A swatch is as wide as a ninth of the line allows, 12 to 20 px, so the
// row fits the info panel's field column and the menu alike.
bool ui_colour_picker(UiState *ui, const char *id, double *hue, float width)
{
    const RXConfig *cfg = &ui->app->lib.cfg;
    bool picked = false;
    ImGui::PushID(id);
    float gap = PX(4);
    float w = width > 0 ? PX(width) : ImGui::GetContentRegionAvail().x;
    float size = ImClamp((w - 8 * gap) / 9 / ui->s, 12.0f, 20.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    int current = rx_track_color_of_hue(*hue);
    if (ui_swatch(ui, "##none", 0, !rx_hue_valid(*hue), size)) {
        *hue = RX_HUE_NONE;
        picked = true;
    }
    ui_tooltip("No colour");
    for (int i = 1; i <= RX_TRACK_COLORS; i++) {
        char sid[16], tip[192];
        snprintf(sid, sizeof(sid), "##c%d", i);
        ImGui::SameLine();
        if (ui_swatch(ui, sid, rx_track_color_rgb(i), i == current, size)) {
            *hue = rx_track_color_hue(i);
            picked = true;
        }
        snprintf(tip, sizeof(tip), "%s: %s", rx_track_color_name(i), rx_config_vibe(cfg, i));
        ui_tooltip(tip);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return picked;
}

const char *ui_colour_text(UiState *ui, double hue, char *buf, size_t size)
{
    int id = rx_track_color_of_hue(hue);
    if (id) {
        snprintf(buf, size, "%s \xC2\xB7 %s", rx_track_color_name(id),
                 rx_config_vibe(&ui->app->lib.cfg, id));
        return buf;
    }
    int near = rx_track_color_nearest_hue(hue);
    if (!near)
        return "none";
    snprintf(buf, size, "%.0f\xC2\xB0, between the eight (%s on the stick)", fmod(hue, 360.0),
             rx_track_color_name(near));
    return buf;
}

bool ui_button(UiState *ui, const char *id, ImVec2 size, const UiButton &b)
{
    ImGui::PushID(id);
    if (b.disabled)
        ImGui::BeginDisabled();
    bool pressed = ImGui::InvisibleButton("##b", size);
    if (b.disabled)
        ImGui::EndDisabled();
    ImGui::PopID();
    if (b.repeat) {
        // Held: once on the press (the duration is 0 on that frame), then
        // at the beat jump keys' rate; letting go does nothing more.
        pressed = false;
        if (ImGui::IsItemActive() && !b.disabled) {
            ImGuiIO &io = ImGui::GetIO();
            float t = io.MouseDownDuration[ImGuiMouseButton_Left];
            pressed = t >= 0 && ImGui::CalcTypematicRepeatAmount(t - io.DeltaTime, t, UI_REPEAT_DELAY,
                                                                 UI_REPEAT_RATE) > 0;
        }
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    bool hot = ImGui::IsItemHovered() && !b.disabled, down = ImGui::IsItemActive();
    float rounding = b.rounding < 0 ? PX(3) : PX(b.rounding);
    ImU32 fill = b.fill ? b.fill : UI_BUTTON;
    if (down)
        fill = UI_BUTTON_DOWN;
    else if (hot)
        fill = b.fill ? b.fill : UI_BUTTON_HOT;
    ImU32 text = b.text ? b.text : UI_TEXT;
    if (b.lit) {
        ImU32 lit = b.lit_col ? b.lit_col : UI_ACCENT;
        fill = down ? ui_alpha(lit, 0.75f) : lit;
        text = UI_BLACK;
    }
    if (b.disabled)
        text = UI_FAINT;
    dl->AddRectFilled(p0, p1, fill, rounding);
    if (b.border)
        dl->AddRect(p0, p1, b.border, rounding);
    if (hot && !b.lit)
        dl->AddRect(p0, p1, IM_COL32(0x60, 0x60, 0x60, 0xFF), rounding);

    float fs = b.font > 0 ? b.font : 11.0f;
    if (b.label) {
        // A label that would spill over the edges is set smaller instead.
        float room = p1.x - p0.x - PX(6) - (b.icon != UI_ICON_NONE ? PX(fs + 3) : 0);
        float tw = ui_text_width(ui, ui->font_bold, fs, b.label);
        if (tw > room && room > 0)
            fs = ImMax(fs * room / tw, 7.5f);
    }
    if (b.icon != UI_ICON_NONE && b.label) {
        float isz = PX(fs);
        float tw = ui_text_width(ui, ui->font_bold, fs, b.label);
        float x = (p0.x + p1.x - isz - PX(3) - tw) * 0.5f;
        float cy = (p0.y + p1.y) * 0.5f;
        float ix = b.icon_after ? x + tw + PX(3) : x;
        float tx = b.icon_after ? x : x + isz + PX(3);
        ui_icon(dl, b.icon, ImVec2(ix, cy - isz * 0.5f), ImVec2(ix + isz, cy + isz * 0.5f), text);
        ui_text(ui, dl, ui->font_bold, fs, ImVec2(tx, cy - PX(fs) * 0.5f - PX(1)), text, b.label);
    } else if (b.icon != UI_ICON_NONE) {
        float m = ImMin(p1.x - p0.x, p1.y - p0.y) * 0.18f;
        ui_icon(dl, b.icon, ImVec2(p0.x + m, p0.y + m), ImVec2(p1.x - m, p1.y - m), text);
    } else if (b.label) {
        ui_text_centre(ui, dl, ui->font_bold, fs, p0, p1, text, b.label);
    }
    return pressed && !b.disabled;
}

/* -------------------------------------------------------------- My Settings */

const char *const ui_setting_titles[UI_NB_SETTING_SECTIONS] = { "Player", "Mixer", "Device" };
const char *const ui_setting_prefixes[UI_NB_SETTING_SECTIONS] = { "player", "mixer", "device" };

// The section `rxbox settings` prints a file's fields under (cli.c,
// settings_section): the background colour, which the databases carry, is
// a device setting like DEVSETTING.DAT's.
static int setting_section(const char *file)
{
    if (!rx_strcasecmp(file, "MYSETTING.DAT") || !rx_strcasecmp(file, "MYSETTING2.DAT"))
        return UI_SETTINGS_PLAYER;
    if (!rx_strcasecmp(file, "DJMMYSETTING.DAT"))
        return UI_SETTINGS_MIXER;
    return UI_SETTINGS_DEVICE;
}

static void setting_listed(void *opaque, const char *file, const char *name,
                           const char *const *values, int nb_values, const char *deflt)
{
    std::vector<UiSetting> *list = (std::vector<UiSetting> *)opaque;
    UiSetting f;
    f.file = file;
    f.name = name;
    f.section = setting_section(file);
    f.deflt = 0;
    for (int i = 0; i < nb_values; i++) {
        f.values.push_back(values[i]);
        if (deflt && !strcmp(values[i], deflt))
            f.deflt = i;
    }
    list->push_back(std::move(f));
}

// Every field settings.c has, in its order, so one added there shows up in
// the dialog by itself.
const std::vector<UiSetting> &ui_settings(UiState *ui)
{
    if (ui->settings.empty())
        rx_settings_list(setting_listed, &ui->settings);
    return ui->settings;
}

int ui_setting_key(const char *key, const char **name)
{
    for (int s = 0; s < UI_NB_SETTING_SECTIONS; s++) {
        size_t n = strlen(ui_setting_prefixes[s]);
        if (!strncmp(key, ui_setting_prefixes[s], n) && key[n] == '.') {
            *name = key + n + 1;
            return s;
        }
    }
    *name = key;
    return -1;
}

// The words of the name, each capitalised, and the abbreviations the
// players print in capitals.
const char *ui_setting_label(const char *name, char *buf, size_t size)
{
    static const struct { const char *word, *text; } words[] = {
        { "lcd", "LCD" }, { "midi", "MIDI" }, { "eq", "EQ" }, { "fx", "FX" }, { "cdj", "CDJ" },
    };
    std::string out;
    for (const char *p = name; *p; ) {
        size_t n = strcspn(p, "_");
        std::string word(p, n);
        const char *text = nullptr;
        for (const auto &w : words) {
            if (word == w.word)
                text = w.text;
        }
        if (!out.empty())
            out += ' ';
        if (text) {
            out += text;
        } else if (!word.empty()) {
            word[0] = (char)toupper((unsigned char)word[0]);
            out += word;
        }
        p += n;
        if (*p)
            p++;
    }
    snprintf(buf, size, "%s", out.c_str());
    return buf;
}

// `value` is as `rxbox settings` lists it (rx_settings_value gives that
// form for what the library holds). The switches are On and Off; numbers
// stay as they are, with the unit the listing leaves out; words are
// written out with the first capitalised ("steep_top" is "Steep top"),
// but for the players' own spellings.
const char *ui_setting_value(const char *name, const char *value, char *buf, size_t size)
{
    if (!value)
        value = "";
    if (!strcmp(value, "true") || !strcmp(value, "false")) {
        snprintf(buf, size, "%s", value[0] == 't' ? "On" : "Off");
        return buf;
    }
    if (value[0] && strspn(value, "-0123456789/") == strlen(value)) {
        if (!strcmp(name, "auto_cue_level") || !strcmp(name, "talk_over_level"))
            snprintf(buf, size, "%s dB", value);
        else if (!strcmp(name, "tempo_range"))
            snprintf(buf, size, "\xC2\xB1%s %%", value);
        else if (!strcmp(name, "phase_meter"))
            snprintf(buf, size, "Type %s", value);
        else if (!strcmp(name, "quantize_beat_value") || !strcmp(name, "beat_jump_beats"))
            snprintf(buf, size, "%s beat%s", value, strcmp(value, "1") && !strchr(value, '/') ? "s" : "");
        else
            snprintf(buf, size, "%s", value);
        return buf;
    }
    // The players' words (docs/settings.md, from the CDJ-3000 manual).
    static const struct { const char *name, *value, *text; } words[] = {
        { nullptr,              "touch_release",       "Touch & Release" },
        { nullptr,              "cdj",                 "CDJ" },
        { nullptr,              "rgb",                 "RGB" },
        { nullptr,              "3band",               "3Band" },
        { nullptr,              "chinese_simplified",  "Chinese (simplified)" },
        { nullptr,              "chinese_traditional", "Chinese (traditional)" },
        { "tempo_range",        "wide",                "WIDE" },
        { "hot_cue_autoload",   "rekordbox",           "rekordbox SETTING" },
        { "waveform_divisions", "time",                "Time scale" },
    };
    for (const auto &w : words) {
        if ((!w.name || !strcmp(w.name, name)) && !strcmp(w.value, value)) {
            snprintf(buf, size, "%s", w.text);
            return buf;
        }
    }
    snprintf(buf, size, "%s", value);
    for (char *p = buf; *p; p++) {
        if (*p == '_')
            *p = ' ';
    }
    buf[0] = (char)toupper((unsigned char)buf[0]);
    return buf;
}

const char *ui_my_setting(UiState *ui, const char *key)
{
    const char *v = rx_settings_get(&ui->app->lib.cfg, key);
    return v ? v : "";
}

void ui_want_frames(UiState *ui, int n)
{
    if (ui->frames_wanted < n)
        ui->frames_wanted = n;
}

void ui_save_config(UiState *ui)
{
    int ret = rx_library_config_save(&ui->app->lib);
    if (ret < 0)
        rx_app_set_status(ui->app, "Could not save the settings: %s", strerror(-ret));
}

/* -------------------------------------------------------------- deck loading */

static void load_now(UiState *ui, int deck, RXTrack *t)
{
    char name[256];
    int ret = rx_app_load_deck(ui->app, deck, t);
    if (ret < 0)
        rx_app_set_status(ui->app, "Could not load %s: %s", rx_track_display(t, name, sizeof(name)),
                          strerror(-ret));
    ui_want_frames(ui, 3);
}

static bool deck_playing(UiState *ui, int deck)
{
    if (!ui->app->player)
        return false;
    RXDeckState st;
    rx_deck_state(ui->app->player, deck, &st);
    return st.loaded && st.playing;
}

// A load replaces whatever the player holds, playing or not: the app
// refuses to swap a playing deck (rx_app_load_deck says EBUSY), so the
// player pauses first and the new track waits at its auto-cue.
void ui_load(UiState *ui, RXTrack *t)
{
    if (!t)
        return;
    if (t->missing) {
        rx_app_set_status(ui->app, "%s is missing: relocate it first", ui_title(t));
        return;
    }
    // A track loaded over a playing one plays on: the deck pauses for the
    // swap and starts the new track from its cue once it is in.
    bool playing = deck_playing(ui, 0);
    if (playing)
        rx_deck_pause(ui->app->player, 0);
    // The cue list's selection was the old track's: Delete is the table's
    // again (the list also lets go when it sees another track loaded).
    ui_cue_list_release(ui, 0);
    load_now(ui, 0, t);
    ui->decks[0].autoplay = playing;
}

/* ------------------------------------------------------------------- style */

static ImVec4 c4(ImU32 c)
{
    return ImGui::ColorConvertU32ToFloat4(c);
}

static void setup_style(void)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding    = 4.0f;
    st.ChildRounding     = 3.0f;
    st.FrameRounding     = 3.0f;
    st.PopupRounding     = 4.0f;
    st.GrabRounding      = 3.0f;
    st.TabRounding       = 3.0f;
    st.ScrollbarRounding = 4.0f;
    st.ScrollbarSize     = 10.0f;
    st.WindowPadding     = ImVec2(8, 8);
    st.FramePadding      = ImVec2(6, 3);
    st.ItemSpacing       = ImVec2(6, 4);
    st.ItemInnerSpacing  = ImVec2(4, 4);
    st.CellPadding       = ImVec2(4, 2);
    st.IndentSpacing     = 14.0f;
    st.WindowBorderSize  = 1.0f;
    st.ChildBorderSize   = 0.0f;
    st.PopupBorderSize   = 1.0f;
    st.FrameBorderSize   = 0.0f;
    st.WindowTitleAlign  = ImVec2(0.0f, 0.5f);

    ImVec4 *c = st.Colors;
    c[ImGuiCol_Text]                  = c4(UI_TEXT);
    c[ImGuiCol_TextDisabled]          = c4(UI_DIM);
    c[ImGuiCol_WindowBg]              = c4(UI_BG);
    c[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]               = c4(IM_COL32(0x1E, 0x1E, 0x1E, 0xF8));
    c[ImGuiCol_Border]                = c4(IM_COL32(0x33, 0x33, 0x33, 0xFF));
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = c4(UI_BUTTON);
    c[ImGuiCol_FrameBgHovered]        = c4(UI_BUTTON_HOT);
    c[ImGuiCol_FrameBgActive]         = c4(UI_BUTTON_DOWN);
    c[ImGuiCol_TitleBg]               = c4(UI_PANEL);
    c[ImGuiCol_TitleBgActive]         = c4(IM_COL32(0x2A, 0x2A, 0x2A, 0xFF));
    c[ImGuiCol_TitleBgCollapsed]      = c4(UI_PANEL);
    c[ImGuiCol_MenuBarBg]             = c4(IM_COL32(0x1A, 0x1A, 0x1A, 0xFF));
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = c4(IM_COL32(0x3A, 0x3A, 0x3A, 0xFF));
    c[ImGuiCol_ScrollbarGrabHovered]  = c4(IM_COL32(0x4A, 0x4A, 0x4A, 0xFF));
    c[ImGuiCol_ScrollbarGrabActive]   = c4(IM_COL32(0x5A, 0x5A, 0x5A, 0xFF));
    c[ImGuiCol_CheckMark]             = c4(UI_ACCENT);
    c[ImGuiCol_SliderGrab]            = c4(UI_ACCENT);
    c[ImGuiCol_SliderGrabActive]      = c4(IM_COL32(0xFF, 0xA6, 0x33, 0xFF));
    c[ImGuiCol_Button]                = c4(UI_BUTTON);
    c[ImGuiCol_ButtonHovered]         = c4(UI_BUTTON_HOT);
    c[ImGuiCol_ButtonActive]          = c4(UI_BUTTON_DOWN);
    // Selection is rekordbox's muted blue; hover a lighter grey.
    c[ImGuiCol_Header]                = c4(IM_COL32(0x23, 0x3E, 0x66, 0xFF));
    c[ImGuiCol_HeaderHovered]         = c4(IM_COL32(0x2C, 0x2C, 0x2C, 0xFF));
    c[ImGuiCol_HeaderActive]          = c4(IM_COL32(0x2B, 0x4B, 0x7A, 0xFF));
    c[ImGuiCol_Separator]             = c4(IM_COL32(0x2E, 0x2E, 0x2E, 0xFF));
    c[ImGuiCol_SeparatorHovered]      = c4(UI_ACCENT);
    c[ImGuiCol_SeparatorActive]       = c4(UI_ACCENT);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]     = c4(ui_alpha(UI_ACCENT, 0.6f));
    c[ImGuiCol_ResizeGripActive]      = c4(UI_ACCENT);
    c[ImGuiCol_Tab]                   = c4(IM_COL32(0x16, 0x16, 0x16, 0xFF));
    c[ImGuiCol_TabHovered]            = c4(UI_BUTTON_HOT);
    c[ImGuiCol_TabSelected]           = c4(UI_PANEL);
    c[ImGuiCol_TabSelectedOverline]   = c4(UI_ACCENT);
    c[ImGuiCol_TableHeaderBg]         = c4(IM_COL32(0x1A, 0x1A, 0x1A, 0xFF));
    c[ImGuiCol_TableBorderStrong]     = c4(IM_COL32(0x2E, 0x2E, 0x2E, 0xFF));
    c[ImGuiCol_TableBorderLight]      = c4(IM_COL32(0x26, 0x26, 0x26, 0xFF));
    c[ImGuiCol_TableRowBg]            = c4(IM_COL32(0x15, 0x15, 0x15, 0xFF));
    c[ImGuiCol_TableRowBgAlt]         = c4(IM_COL32(0x1A, 0x1A, 0x1A, 0xFF));
    c[ImGuiCol_TextSelectedBg]        = c4(ui_alpha(UI_ACCENT, 0.35f));
    c[ImGuiCol_DragDropTarget]        = c4(UI_ACCENT);
    c[ImGuiCol_NavCursor]             = c4(UI_ACCENT);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_PlotHistogram]         = c4(UI_ACCENT);
}

// The title and button legends are bold, as rekordbox's are. The window
// module loads the regular face; the bold one (plus its CJK fallback, for
// Japanese titles) is found through fontconfig and added beside it.
static bool font_file(const char *spec, int min_weight, std::string &path, int &index)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)spec);
    if (!pat)
        return false;
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *m = FcFontMatch(nullptr, pat, &res);
    FcPatternDestroy(pat);
    if (!m)
        return false;
    FcChar8 *file = nullptr;
    int weight = 0, idx = 0;
    bool ok = FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch &&
              FcPatternGetInteger(m, FC_WEIGHT, 0, &weight) == FcResultMatch && weight >= min_weight;
    if (ok) {
        path = (const char *)file;
        FcPatternGetInteger(m, FC_INDEX, 0, &idx);
        index = idx;
    }
    FcPatternDestroy(m);
    return ok;
}

static void load_bold_font(UiState *ui)
{
    ImGuiIO &io = ImGui::GetIO();
    std::string path;
    int index = 0;
    if (!font_file("sans-serif:bold", FC_WEIGHT_BOLD, path, index))
        return;
    ImFontConfig cfg;
    cfg.FontNo = (ImU32)index;
    snprintf(cfg.Name, sizeof(cfg.Name), "rxbox bold");
    ImFont *f = io.Fonts->AddFontFromFileTTF(path.c_str(), ui->font_base, &cfg);
    if (!f)
        return;
    if (font_file("Noto Sans CJK JP:bold", FC_WEIGHT_BOLD, path, index)) {
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.FontNo = (ImU32)index;
        io.Fonts->AddFontFromFileTTF(path.c_str(), ui->font_base, &merge);
    }
    ui->font_bold = f;
}

/* ---------------------------------------------------------- layout settings */

static void *ini_open(ImGuiContext *, ImGuiSettingsHandler *, const char *name)
{
    return strcmp(name, "ui") == 0 ? (void *)1 : nullptr;
}

static void ini_line(ImGuiContext *, ImGuiSettingsHandler *h, void *, const char *line)
{
    UiState *ui = (UiState *)h->UserData;
    RXDeckView *dv = &ui->app->decks[0];
    int i;
    if (sscanf(line, "DetailHalf=%d", &i) == 1)
        ui->detail_half = i != 0;
    else if (sscanf(line, "GridTools=%d", &i) == 1)
        dv->grid_edit = i != 0;
    // Older layouts' ShowInfo= (the info panel is always shown now), Pads=
    // and CueList= (the pad mode and cue list tabs, gone since every group
    // is shown at once) fall through, ignored. So do TempoRange=, Jump= and
    // MasterTempo=: the deck starts with the players' tempo range, beat
    // jump size and MASTER TEMPO (follow_settings), and the buttons change
    // only the running deck.
}

static void ini_write(ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out)
{
    UiState *ui = (UiState *)h->UserData;
    RXDeckView *dv = &ui->app->decks[0];
    out->appendf("[rxbox][ui]\n");
    out->appendf("DetailHalf=%d\n", ui->detail_half);
    out->appendf("GridTools=%d\n\n", dv->grid_edit);
}

static void layout_changed(UiState *ui)
{
    ImGui::MarkIniSettingsDirty();
}

// ImGui asks for a save a moment after a layout change (io.WantSaveIniSettings).
static void save_layout(UiState *ui)
{
    size_t size = 0;
    const char *text = ImGui::SaveIniSettingsToMemory(&size);
    if (text && rx_library_setting_set(&ui->app->lib, "ui.layout", text) < 0)
        rx_warn("ui: could not save the layout");
    ImGui::GetIO().WantSaveIniSettings = false;
}

/* ---------------------------------------------------------------- the menus */

static void menu_bar(UiState *ui)
{
    RXApp *app = ui->app;
    RXConfig *cfg = &app->lib.cfg;
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Add files/folders..."))
            ui_open_add(ui);
        ImGui::Separator();
        if (ImGui::MenuItem("Export to USB..."))
            ui_open_export(ui);
        if (ImGui::MenuItem("Sync from USB..."))
            ui_open_sync(ui);
        if (ImGui::MenuItem("Inspect USB..."))
            ui_open_inspect(ui);
        if (ImGui::MenuItem("Player settings...", "Ctrl+,"))
            ui_open_settings(ui);
        ImGui::Separator();
        if (ImGui::MenuItem("Rescan missing files")) {
            int n = rx_app_rescan(app);
            if (n > 0)
                rx_app_set_status(app, "%d track%s changed state", n, n == 1 ? "" : "s");
            else
                rx_app_set_status(app, "No change in missing files");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
            app->quit = true;
            if (app->window)
                rx_window_request_close(app->window);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        // rekordbox: View > Color > Waveform color, which is the players'
        // WAVEFORM COLOR: one setting, device.waveform_color, for the deck,
        // the table's previews and the stick, as the Player settings
        // dialog's Device tab shows it.
        static const char *const palettes[] = { "3Band", "Blue", "RGB" };
        static const char *const palette_cfg[] = { "3band", "blue", "rgb" };
        if (ImGui::BeginMenu("Waveform colour")) {
            const char *now = ui_my_setting(ui, "device.waveform_color");
            for (int i = 0; i < 3; i++) {
                bool on = !strcmp(now, palette_cfg[i]);
                if (ImGui::MenuItem(palettes[i], nullptr, on) && !on)
                    ui_setting_pick(ui, "device.waveform_color", palette_cfg[i]);
            }
            ImGui::EndMenu();
        }
        // The players' key display format (device.key_display), for the
        // table, the deck and the info panel. Open Key stays the command
        // line's (export.key_notation names keys on the stick).
        static const char *const notations[] = { "Classic (Am)", "Alphanumeric (8A)" };
        static const char *const notation_cfg[] = { "classic", "alphanumeric" };
        if (ImGui::BeginMenu("Key notation")) {
            const char *now = ui_my_setting(ui, "device.key_display");
            for (int i = 0; i < 2; i++) {
                bool on = !strcmp(now, notation_cfg[i]);
                if (ImGui::MenuItem(notations[i], nullptr, on) && !on)
                    ui_setting_pick(ui, "device.key_display", notation_cfg[i]);
            }
            ImGui::EndMenu();
        }
        // rekordbox's Traffic Light: the keys in the table that mix with
        // the loaded track's light green, within this reach.
        static const char *const reaches[] = { "Off", "Same key", "Related 1", "Related 2",
                                                "Related 3" };
        if (ImGui::BeginMenu("Traffic Light")) {
            for (int i = 0; i < 5; i++) {
                bool on = ui->traffic_light == i;
                if (ImGui::MenuItem(reaches[i], nullptr, on) && !on) {
                    ui->traffic_light = i;
                    snprintf(cfg->traffic_light, sizeof(cfg->traffic_light), "%s",
                             rx_key_reach_name((RXKeyReach)i));
                    ui_save_config(ui);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Detail waveform")) {
            if (ImGui::MenuItem("Centred (CDJ-3000)", nullptr, !ui->detail_half)) {
                ui->detail_half = false;
                layout_changed(ui);
            }
            if (ImGui::MenuItem("Half (rekordbox 2 PLAYER)", nullptr, ui->detail_half)) {
                ui->detail_half = true;
                layout_changed(ui);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Grid tools", "Ctrl+G", app->decks[0].grid_edit)) {
            app->decks[0].grid_edit = !app->decks[0].grid_edit;
            layout_changed(ui);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Analysis")) {
        bool any = app->nb_selection > 0;
        if (ImGui::MenuItem("Analyse selection", nullptr, false, any)) {
            for (int i = 0; i < app->nb_selection; i++) {
                RXTrack *t = rx_library_find_id(&app->lib, app->selection[i]);
                if (t)
                    rx_app_analyse_track(app, t, app->lib.cfg.analyse_what, false);
            }
        }
        if (ImGui::MenuItem("Analyse all unanalysed"))
            rx_app_analyse_all(app, false);
        if (ImGui::MenuItem("Re-analyse selection", nullptr, false, any)) {
            for (int i = 0; i < app->nb_selection; i++) {
                RXTrack *t = rx_library_find_id(&app->lib, app->selection[i]);
                if (t)
                    rx_app_analyse_track(app, t, app->lib.cfg.analyse_what, true);
            }
        }
        // The model's genre, colour and comment: asked for here or in the
        // track menu, never by "Analyse all" (seconds a track on the GPU).
        ImGui::Separator();
        if (ImGui::MenuItem("Analyse tags of selection", nullptr, false, any))
            rx_app_tag_tracks(app, app->selection, app->nb_selection);
        if (ImGui::MenuItem("Stop tagging", nullptr, false,
                            rx_app_tag_progress(app, nullptr, nullptr, nullptr)))
            rx_app_cancel_tags(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Analysis settings..."))
            ui->dlg_analysis = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard map"))
            ui->dlg_keys = true;
        if (ImGui::MenuItem("About rxbox"))
            ui->dlg_about = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

/* --------------------------------------------------------------- status bar */

static void status_bar(UiState *ui, ImVec2 a, ImVec2 b)
{
    RXApp *app = ui->app;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(0x1A, 0x1A, 0x1A, 0xFF));
    dl->AddLine(a, ImVec2(b.x, a.y), IM_COL32(0x2E, 0x2E, 0x2E, 0xFF));
    float fs = 12.0f;
    float ty = (a.y + b.y - PX(fs)) * 0.5f - PX(1);
    float w = b.x - a.x;

    // Right, inwards from the edge: the GPU, the library size, the audio
    // state (the stream's error in red while it is not running).
    float x = b.x - PX(8);
    const char *gpu = rx_window_gpu_name(app->window);
    if (gpu && gpu[0]) {
        x -= ui_text_width(ui, nullptr, fs, gpu);
        ui_text(ui, dl, nullptr, fs, ImVec2(x, ty), UI_DIM, gpu);
        x -= PX(16);
    }
    // The count and the files' size: what a stick with every track would
    // hold (an export of playlists copies fewer; transcodes change sizes).
    int64_t bytes = 0;
    for (int i = 0; i < app->lib.nb_tracks; i++)
        bytes += app->lib.tracks[i]->file_size;
    char tracks[64];
    if (bytes >= (int64_t)10 << 30)
        snprintf(tracks, sizeof(tracks), "%d tracks \xC2\xB7 %.0f GB", app->lib.nb_tracks,
                 bytes / 1073741824.0);
    else if (bytes >= (int64_t)1 << 30)
        snprintf(tracks, sizeof(tracks), "%d tracks \xC2\xB7 %.1f GB", app->lib.nb_tracks,
                 bytes / 1073741824.0);
    else
        snprintf(tracks, sizeof(tracks), "%d tracks \xC2\xB7 %.0f MB", app->lib.nb_tracks,
                 bytes / 1048576.0);
    x -= ui_text_width(ui, nullptr, fs, tracks);
    ui_text(ui, dl, nullptr, fs, ImVec2(x, ty), UI_DIM, tracks);
    char audio[160];
    const char *err = nullptr;
    if (!app->player || rx_player_backend(app->player) == RX_AUDIO_NULL) {
        snprintf(audio, sizeof(audio), "Audio: none");
    } else if ((err = rx_player_error(app->player))) {
        snprintf(audio, sizeof(audio), "%s", err);
    } else {
        const char *sink = rx_player_sink(app->player);
        snprintf(audio, sizeof(audio), "PipeWire %g kHz \xC2\xB7 %s",
                 rx_player_rate(app->player) / 1000.0, sink ? sink : "default");
    }
    x -= PX(16) + ui_text_width(ui, nullptr, fs, audio);
    ui_text(ui, dl, nullptr, fs, ImVec2(x, ty), err ? UI_RED : UI_DIM, audio);
    float right_edge = x - PX(16);

    // Middle: analysis and export progress, each with a thin bar.
    float mid_x = a.x + w * 0.38f;
    float mid_w = ImMin(right_edge - mid_x, PX(380));
    char mid[640] = "";
    float frac = -1;
    int tag_done = 0, tag_total = 0;
    uint64_t tag_current = 0;
    if (app->export_state.running) {
        const auto &es = app->export_state;
        bool copying = !strcmp(es.stage, "copy") && es.bytes_total > 0;
        char rate[64];
        rx_export_rate_text(copying ? es.rate : 0, es.seconds_left, rate, sizeof(rate));
        snprintf(mid, sizeof(mid), "Export %s %d/%d%s%s  %s", es.stage, es.done, es.total, rate[0] ? "  " : "",
                 rate, es.item);
        frac = copying ? (float)((double)es.bytes_done / (double)es.bytes_total)
             : es.total > 0 ? (float)es.done / es.total : 0;
    } else if (app->analysing_total > 0 && app->analysing_done < app->analysing_total) {
        char name[200] = "";
        uint64_t ids[16];
        int n = app->jobs ? rx_jobs_active_tracks(app->jobs, ids, 16) : 0;
        for (int i = 0; i < n; i++) {
            RXTrack *t = ids[i] ? rx_library_find_id(&app->lib, ids[i]) : nullptr;
            if (t) {
                rx_track_display(t, name, sizeof(name));
                break;
            }
        }
        snprintf(mid, sizeof(mid), "Analysing %d/%d%s%s", app->analysing_done + 1, app->analysing_total,
                 name[0] ? ": " : "", name);
        frac = (float)app->analysing_done / app->analysing_total;
    } else if (rx_app_tag_progress(app, &tag_done, &tag_total, &tag_current)) {
        // The track being tagged; none while the model loads, or unloads
        // after the last.
        char name[200];
        snprintf(name, sizeof(name), "%s the model", tag_done < tag_total ? "loading" : "unloading");
        RXTrack *t = tag_current ? rx_library_find_id(&app->lib, tag_current) : nullptr;
        if (t)
            rx_track_display(t, name, sizeof(name));
        snprintf(mid, sizeof(mid), "Tagging %d/%d: %s", ImMin(tag_done + 1, tag_total), tag_total,
                 name);
        frac = tag_total > 0 ? (float)tag_done / tag_total : 0;
    }
    if (mid[0] && mid_w > PX(60)) {
        ui_spinner(dl, ImVec2(mid_x + PX(6), (a.y + b.y) * 0.5f), PX(4.5f), UI_ACCENT);
        ui_text(ui, dl, nullptr, fs, ImVec2(mid_x + PX(16), ty - PX(2)), UI_TEXT, mid, mid_w - PX(16));
        float by = b.y - PX(4);
        dl->AddRectFilled(ImVec2(mid_x + PX(16), by), ImVec2(mid_x + mid_w, by + PX(2)),
                          IM_COL32(0x33, 0x33, 0x33, 0xFF));
        if (frac >= 0)
            dl->AddRectFilled(ImVec2(mid_x + PX(16), by),
                              ImVec2(mid_x + PX(16) + (mid_w - PX(16)) * ImClamp(frac, 0.0f, 1.0f),
                                     by + PX(2)), UI_ACCENT);
    } else {
        mid_x = right_edge;
    }

    // Left: the last message, dimmed after a while.
    if (app->status[0]) {
        int64_t age = rx_time_us() - app->status_time;
        bool fresh = app->status_time == 0 || age < 8000000;
        ui_text(ui, dl, nullptr, fs, ImVec2(a.x + PX(8), ty), fresh ? UI_TEXT : UI_DIM, app->status,
                mid_x - a.x - PX(24));
    }
}

/* ------------------------------------------------------------------ splitter */

// A draggable bar between two areas; returns the drag in pixels and sets
// `released` when the drag ends.
static float splitter(UiState *ui, const char *id, ImVec2 pos, ImVec2 size, bool vertical,
                      bool *released)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, size);
    *released = ImGui::IsItemDeactivated();
    bool hot = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (hot || active)
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (hot || active) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
        ImU32 col = active ? UI_ACCENT : IM_COL32(0x50, 0x50, 0x50, 0xFF);
        if (vertical)
            dl->AddLine(ImVec2(c.x, pos.y), ImVec2(c.x, pos.y + size.y), col, PX(2));
        else
            dl->AddLine(ImVec2(pos.x, c.y), ImVec2(pos.x + size.x, c.y), col, PX(2));
    }
    if (!active)
        return 0;
    ImVec2 d = ImGui::GetIO().MouseDelta;
    return vertical ? d.x : d.y;
}

/* ------------------------------------------------------------ test hooks */

// RXBOX_UI_EVENTS (see the top of the file).
static void parse_test_events(UiState *ui, const char *spec)
{
    std::string all = spec;
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find(',', at);
        std::string one = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? all.size() : end + 1;

        UiTestEvent ev = {};
        char what[32] = "", key[32] = "";
        if (sscanf(one.c_str(), "%d:%31[^:]:%31s", &ev.frame, what, key) < 2)
            continue;
        if (!strcmp(what, "focus-out") || !strcmp(what, "focus-in")) {
            ev.type = what[6] == 'o' ? 2 : 3;
        } else if (!strcmp(what, "text")) {
            ev.type = 5;
            ev.text = key;
            std::replace(ev.text.begin(), ev.text.end(), '+', ' ');
        } else if (!strcmp(what, "click")) {
            ev.type = 4;
            if (sscanf(key, "%f:%f", &ev.x, &ev.y) != 2) {
                rx_warn("RXBOX_UI_EVENTS: click wants x:y, not %s", key);
                continue;
            }
        } else if (!strcmp(what, "down") || !strcmp(what, "up")) {
            ev.type = what[0] == 'd' ? 0 : 1;
            ev.key = ImGuiKey_None;
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                if (!strcmp(ImGui::GetKeyName((ImGuiKey)k), key))
                    ev.key = (ImGuiKey)k;
            }
            if (ev.key == ImGuiKey_None) {
                rx_warn("RXBOX_UI_EVENTS: no key %s", key);
                continue;
            }
        } else {
            rx_warn("RXBOX_UI_EVENTS: no event %s", what);
            continue;
        }
        ui->test_events.push_back(ev);
    }
}

// Queued now, seen by the next frame's NewFrame(). A click moves the
// pointer at its frame and presses two frames later, as a hand does: an
// item that allows overlap (the cue list's rows) is only hovered once the
// pointer has been over it for a frame. The press and the release land on
// successive frames: ImGui trickles queued input.
static void test_events(UiState *ui)
{
    ImGuiIO &io = ImGui::GetIO();
    for (const UiTestEvent &ev : ui->test_events) {
        if (ev.type == 4 && ev.frame + 2 == ui->frame_no) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        }
        if (ev.frame != ui->frame_no)
            continue;
        if (ev.type <= 1)
            io.AddKeyEvent(ev.key, ev.type == 0);
        else if (ev.type == 5)
            io.AddInputCharactersUTF8(ev.text.c_str());
        else if (ev.type == 4)
            io.AddMousePosEvent(PX(ev.x), PX(ev.y));
        else
            io.AddFocusEvent(ev.type == 3);
    }
}

// How the last frame drew a waveform, for the log.
static const char *mode_drawn(int mode)
{
    switch (mode) {
    case RX_WAVE_CENTRED:       return "centred";
    case RX_WAVE_HALF_STACKED:  return "half";
    case RX_WAVE_HALF_OVERLAID: return "overlaid";
    default:                    return "none";
    }
}

static void log_test_state(UiState *ui)
{
    RXApp *app = ui->app;
    RXDeckState st;
    ui_deck_position(ui, 0, &st);
    bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const RXTrack *t = app->decks[0].track;
    rx_info("ui test: frame %d track %llu playing %d position %.2f popup %d cues %d cue-keys %d "
            "overview %s previews %s key %s",
            ui->frame_no, (unsigned long long)(t ? t->id : 0), st.playing, st.position, popup,
            t ? t->nb_cues : 0, ui_cue_list_has_keys(ui, 0), mode_drawn(ui->overview_mode_drawn),
            mode_drawn(ui->preview_mode_drawn), ui->key_drawn[0] ? ui->key_drawn : "-");
    // The Add dialog's finder: the query, its matches, the best one, the
    // row the cursor is on and what is ticked.
    if (ui->finder && ui->fb_find[0]) {
        const std::vector<int> &res = ui_finder_results(ui->finder);
        int c = ui->fb_cursor;
        rx_info("ui test: find \"%s\" matches %zu of %zu best \"%s\" cursor \"%s\" picked %zu",
                ui->fb_find, ui_finder_matches(ui->finder), ui_finder_count(ui->finder),
                res.empty() ? "" : ui_finder_entry(ui->finder, res[0]).rel.c_str(),
                c >= 0 && (size_t)c < res.size() ? ui_finder_entry(ui->finder, res[(size_t)c]).rel.c_str()
                                                 : "",
                ui->fb_picked.size());
    }
}

/* -------------------------------------------------------------- My Settings */

// The players' My Settings drive the interface the way they drive the
// players (docs/UI.md, Player settings): one setting for both, so rxbox
// shows what the stick will. They are read from RXConfig at every frame;
// the Player settings dialog, the View menu and Sync from USB write it,
// and the next frame shows the change. The waveform palette, auto cue,
// the quantize beat value and play mode are app.c's, which owns what they
// act on (the uploads, the loads, the deck controls).
//
// The deck's state at startup comes from the settings a CDJ starts with
// (and a LOAD of My Settings sets): QUANTIZE, the beat jump size, the
// tempo range, MASTER TEMPO and the time mode. Each is applied to the live
// state at the first frame and again whenever the setting changes; the
// deck's buttons and keys change the live state only, never the setting,
// as a CDJ's buttons leave its stored preferences alone.
static const char *const STARTUP_KEYS[UI_NB_STARTUP] = {
    "player.quantize", "player.beat_jump_beats", "player.tempo_range", "player.master_tempo",
    "player.time_mode",
};

// The beat jump size for the setting's beats: 1/2 is Fine (there is no
// half beat jump in the menu), the others the size of as many beats.
static int jump_index(const char *beats)
{
    double b = strchr(beats, '/') ? 0.0 : atof(beats);
    for (int i = 0; i < UI_NB_JUMP; i++) {
        if (UI_JUMP_SIZES[i].beats == b)
            return i;
    }
    return 0;
}

// Applies one startup setting to every deck; false when it cannot be yet
// (MASTER TEMPO needs the player), to be tried again on the next frame.
static bool apply_startup(UiState *ui, int which, const char *v)
{
    RXApp *app = ui->app;
    for (int d = 0; d < RX_DECKS; d++) {
        UiDeck *ud = &ui->decks[d];
        RXDeckCtl *c = &app->decks[d].ctl;
        switch (which) {
        case 0:
            rx_ctl_set_quantize(c, !strcmp(v, "true"));
            break;
        case 1:
            ud->jump_size = jump_index(v);
            c->jump_beats = UI_JUMP_SIZES[ud->jump_size].beats;
            break;
        case 2: {
            int r = !strcmp(v, "wide") ? 3 : !strcmp(v, "16") ? 2 : !strcmp(v, "10") ? 1 : 0;
            ud->tempo_range = r;
            // A tempo past the new range is brought to its edge, as the
            // range button does.
            RXDeckState st;
            ui_deck_position(ui, d, &st);
            double pct = (st.tempo - 1.0) * 100.0;
            if (app->player && fabs(pct) > UI_TEMPO_RANGES[r])
                rx_deck_set_tempo(app->player, d, 1.0 + copysign(UI_TEMPO_RANGES[r], pct) / 100.0);
            break;
        }
        case 3: {
            // A player that cannot keep the pitch refuses, and the deck
            // plays without it.
            if (!app->player)
                return false;
            bool on = !strcmp(v, "true");
            bool ok = rx_deck_set_master_tempo(app->player, d, on);
            ud->master_tempo = on && ok;
            if (on && !ok)
                rx_info("ui: master tempo is not available, the deck plays without it");
            break;
        }
        default:
            ud->time_remaining = !strcmp(v, "remain");
            break;
        }
    }
    return true;
}

static void follow_settings(UiState *ui)
{
    ui->key_camelot = !strcmp(ui_my_setting(ui, "device.key_display"), "alphanumeric");
    ui->overview_full = !strcmp(ui_my_setting(ui, "device.overview_waveform"), "full");
    ui->playhead_left = !strcmp(ui_my_setting(ui, "device.waveform_position"), "left");
    ui->hot_cue_colour = !strcmp(ui_my_setting(ui, "player.hot_cue_color"), "true");

    for (int k = 0; k < UI_NB_STARTUP; k++) {
        const char *v = ui_my_setting(ui, STARTUP_KEYS[k]);
        if (ui->startup_applied[k] && !strcmp(ui->startup_applied[k], v))
            continue;
        if (apply_startup(ui, k, v))
            ui->startup_applied[k] = v;
    }
}

/* ------------------------------------------------------------- entry points */

int rx_ui_init(RXApp *app)
{
    UiState *ui = new (std::nothrow) UiState();
    if (!ui)
        return RX_ERROR(ENOMEM);
    ui->app = app;
    ui->s = 1.0f;
    ui->deck_split = -1.0f;
    ui->tree_width = 220.0f;
    ui->anchor_row = -1;
    ui->overview_mode_drawn = ui->preview_mode_drawn = -1;
    // The jump size, tempo range, time mode and MASTER TEMPO come from the
    // players' settings at the first frame (follow_settings).
    for (int i = 0; i < RX_DECKS; i++) {
        UiDeck *ud = &ui->decks[i];
        ud->ctx_cue = -1;
        ud->edit_cue = -1;
    }
    app->ui = ui;

    // Preferences from the library's settings (RXConfig ui.*).
    RXConfig *cfg = &app->lib.cfg;
    if (cfg->split_player > 0)
        ui->deck_split = ImClamp(cfg->split_player, 0.2f, 0.8f);
    if (cfg->split_tree > 0)
        ui->tree_width = ImClamp(cfg->split_tree, 120.0f, 600.0f);
    int reach = rx_key_reach_parse(cfg->traffic_light);
    ui->traffic_light = reach >= 0 ? reach : RX_KEY_REACH_RELATED3;
    double bars = cfg->detail_bars > 0 ? cfg->detail_bars : 12;
    for (int i = 0; i < RX_DECKS; i++)
        app->decks[i].zoom_bars = bars;
    ui->saved_zoom = bars;

    if (!app->window)
        return 0;

    ImGuiIO &io = ImGui::GetIO();
    // The keyboard is rekordbox's map (keys.cpp): ImGui's keyboard
    // navigation would take Space and the arrows.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    // A paused interface draws nothing until input arrives, so a blinking
    // caret would freeze half the time: keep it steady.
    io.ConfigInputTextCursorBlink = false;
    setup_style();
    ui->font_base = ImGui::GetStyle().FontSizeBase > 0 ? ImGui::GetStyle().FontSizeBase : 15.0f;
    load_bold_font(ui);

    // The rest of the layout (table columns, jump size...) is ImGui's ini
    // text, kept in the library rather than in a file next to the binary.
    ImGuiSettingsHandler h;
    h.TypeName = "rxbox";
    h.TypeHash = ImHashStr("rxbox");
    h.ReadOpenFn = ini_open;
    h.ReadLineFn = ini_line;
    h.WriteAllFn = ini_write;
    h.UserData = ui;
    ImGui::AddSettingsHandler(&h);
    io.IniFilename = nullptr;
    char *layout = nullptr;
    if (rx_library_setting_get(&app->lib, "ui.layout", &layout) == 0 && layout) {
        ImGui::LoadIniSettingsFromMemory(layout, strlen(layout));
        free(layout);
    }

    // Headless test hooks.
    const char *autoload = getenv("RXBOX_UI_AUTOLOAD");
    if (autoload && autoload[0])
        ui->autoload = strtoull(autoload, nullptr, 10);
    const char *at = getenv("RXBOX_UI_AUTOLOAD_AT");
    ui->autoload_at = at && at[0] ? atof(at) : -1.0;
    const char *show = getenv("RXBOX_UI_SHOW");
    if (show)
        ui->autoshow = show;
    const char *shot = getenv("RXBOX_UI_SCREENSHOT");
    if (shot && shot[0])
        ui->screenshot_path = shot;
    const char *shot_frame = getenv("RXBOX_UI_SCREENSHOT_FRAME");
    ui->screenshot_frame = shot_frame && atoi(shot_frame) > 0 ? atoi(shot_frame) : 100;
    const char *events = getenv("RXBOX_UI_EVENTS");
    if (events && events[0])
        parse_test_events(ui, events);
    return 0;
}

void rx_ui_shutdown(RXApp *app)
{
    UiState *ui = ui_of(app);
    if (!ui)
        return;
    // The screenshot hook, when the run ended before the frame asked for
    // (--frames N below it): the last frame.
    if (app->window && !ui->screenshot_path.empty()) {
        int ret = rx_window_screenshot(app->window, ui->screenshot_path.c_str());
        if (ret < 0)
            rx_err("ui: screenshot to %s failed: %s", ui->screenshot_path.c_str(), strerror(-ret));
    }
    if (app->window && ImGui::GetCurrentContext()) {
        save_layout(ui);
        ImGui::RemoveSettingsHandler("rxbox");
    }
    if (app->decks[0].zoom_bars != ui->saved_zoom && app->decks[0].zoom_bars >= 1) {
        app->lib.cfg.detail_bars = (int)app->decks[0].zoom_bars;
        rx_library_config_save(&app->lib);
    }
    ui_dialogs_shutdown(ui);
    ui_deck_shutdown(ui);
    free(app->selection);
    app->selection = nullptr;
    app->nb_selection = 0;
    delete ui;
    app->ui = nullptr;
}

static bool shown(const UiState *ui, const char *what)
{
    const std::string &s = ui->autoshow;
    size_t n = strlen(what);
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) {
        bool start = at == 0 || s[at - 1] == ',';
        bool end = at + n == s.size() || s[at + n] == ',';
        if (start && end)
            return true;
    }
    return false;
}

static void autoshow(UiState *ui)
{
    RXApp *app = ui->app;
    if (ui->autoshow.empty())
        return;
    if (shown(ui, "grid"))
        app->decks[0].grid_edit = true;
    if (shown(ui, "half"))
        ui->detail_half = true;
    if (shown(ui, "export"))
        ui_open_export(ui);
    if (shown(ui, "sync"))
        ui_open_sync(ui);
    if (shown(ui, "inspect"))
        ui_open_inspect(ui);
    if (shown(ui, "analysis"))
        ui->dlg_analysis = true;
    if (shown(ui, "settings"))
        ui_open_settings(ui);
    if (shown(ui, "add"))
        ui_open_add(ui);
    if (shown(ui, "keys"))
        ui->dlg_keys = true;
    if (shown(ui, "about"))
        ui->dlg_about = true;
}

static void autoload_tick(UiState *ui)
{
    RXApp *app = ui->app;
    UiDeck *ud = &ui->decks[0];
    RXDeckView *dv = &app->decks[0];
    if (!ui->autoload_done) {
        ui->autoload_done = true;
        autoshow(ui);
        if (!ui->autoload)
            return;
        app->focus_track = ui->autoload;
        ui_select_only(ui, ui->autoload);
        RXTrack *t = rx_library_find_id(&app->lib, ui->autoload);
        if (!t) {
            rx_app_set_status(app, "RXBOX_UI_AUTOLOAD: no track %llu",
                              (unsigned long long)ui->autoload);
            return;
        }
        load_now(ui, 0, t);
        ud->autoplay = true;
        if (shown(ui, "tags"))
            rx_app_tag_tracks(app, &t->id, 1);
    }
    if (!ud->autoplay || dv->loading_request || !dv->ctl.track || !app->player)
        return;
    if (!rx_deck_loaded(app->player, 0))
        return;
    ud->autoplay = false;
    if (ui->autoload_at >= 0)
        rx_ctl_seek(&dv->ctl, ui->autoload_at);
    RXDeckState st;
    rx_deck_state(app->player, 0, &st);
    if (!st.playing)
        rx_ctl_play_pause(&dv->ctl);
}

bool rx_ui_frame(RXApp *app)
{
    UiState *ui = ui_of(app);
    if (!ui || !app->window)
        return false;
    ImGuiIO &io = ImGui::GetIO();
    ui->frame_no++;

    int lw = 0, lh = 0;
    rx_window_size(app->window, &lw, &lh);
    ui->s = lw > 0 && io.DisplaySize.x > 0 ? io.DisplaySize.x / (float)lw : 1.0f;
    if (ui->s < 0.5f || ui->s > 4.0f)
        ui->s = 1.0f;

    follow_settings(ui);
    autoload_tick(ui);
    if (!ui->test_events.empty())
        test_events(ui);
    // The screenshot hook: this is the frame to keep; the window saves it
    // once it is presented.
    if (!ui->screenshot_path.empty() &&
        rx_window_frames_drawn(app->window) + 1 >= ui->screenshot_frame) {
        rx_window_screenshot_next(app->window, ui->screenshot_path.c_str());
        ui->screenshot_path.clear();
        log_test_state(ui);
    }
    // The rows follow the library (app.c may have rebuilt them already).
    if (app->rows_dirty || app->rows_generation != app->lib.generation || !app->rows)
        rx_app_rebuild_rows(app);
    ui->overview_mode_drawn = ui->preview_mode_drawn = -1;
    ui->key_drawn[0] = '\0';

    ui->previews_pending = false;
    ui_keys(ui);

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoScrollbar;
    ImGui::Begin("##rxbox", nullptr, wf);
    ImGui::PopStyleVar(3);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    menu_bar(ui);
    ImGui::PopStyleVar();

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float gap = PX(4), split_h = PX(6);
    float status_h = PX(22);
    float body_h = avail.y - status_h;

    // The player keeps room for every row (title, overview, the control
    // row, the grid strip when shown) and a detail of at least 80 px. By
    // default it takes half the body, or its natural height with a
    // UI_DETAIL_NATURAL detail when that is more; whatever it has beyond
    // its fixed rows goes to the detail waveform (ui_deck_panel). The
    // browser keeps at least a few rows.
    float fixed = PX(UI_TITLE_ROW_H + UI_OVERVIEW_H + UI_CONTROL_ROW_H + 5 * 4 + 12) +
                  (app->decks[0].grid_edit ? PX(UI_GRID_STRIP_H + 4) : 0);
    float natural = fixed + PX(UI_DETAIL_NATURAL), smallest = fixed + PX(80);
    float max_deck = ImMax(body_h - PX(200), smallest);
    float want = ui->deck_split < 0 ? ImMax(natural, floorf(0.5f * body_h))
                                    : floorf(ui->deck_split * body_h);
    float deck_h = ImClamp(want, smallest, max_deck);

    ImGui::SetCursorScreenPos(ImVec2(origin.x + gap, origin.y + gap));
    ui_deck_panel(ui, 0, ImVec2(avail.x - gap * 2, deck_h - gap));

    bool released = false;
    float dy = splitter(ui, "##split_player", ImVec2(origin.x, origin.y + deck_h),
                        ImVec2(avail.x, split_h), false, &released);
    if (dy != 0 && body_h > 0)
        ui->deck_split = ImClamp((deck_h + dy) / body_h, 0.2f, 0.8f);
    if (released && ui->deck_split > 0) {
        app->lib.cfg.split_player = ui->deck_split;
        ui_save_config(ui);
    }

    float by = origin.y + deck_h + split_h;
    float bh = body_h - deck_h - split_h;
    float tree_w = ImClamp(PX(ui->tree_width), PX(120), avail.x * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + gap, by));
    ui_tree(ui, ImVec2(tree_w, bh - gap));
    float dx = splitter(ui, "##split_tree", ImVec2(origin.x + gap + tree_w, by),
                        ImVec2(split_h, bh - gap), true, &released);
    if (dx != 0)
        ui->tree_width = ImClamp(ui->tree_width + dx / ui->s, 120.0f, 600.0f);
    if (released) {
        app->lib.cfg.split_tree = ui->tree_width;
        ui_save_config(ui);
    }
    // Tree | table | info, the info panel always there.
    float tx = origin.x + gap + tree_w + split_h;
    float tw = origin.x + avail.x - gap - tx;
    float info_w = ImMin(PX(260), tw * 0.4f);
    ImGui::SetCursorScreenPos(ImVec2(tx, by));
    ui_browser(ui, ImVec2(tw - info_w - gap, bh - gap));
    ImGui::SetCursorScreenPos(ImVec2(tx + tw - info_w, by));
    ui_info_panel(ui, ImVec2(info_w, bh - gap));

    status_bar(ui, ImVec2(origin.x, origin.y + body_h), ImVec2(origin.x + avail.x, origin.y + avail.y));
    ImGui::End();

    ui_dialogs(ui);
    if (io.WantSaveIniSettings)
        save_layout(ui);

    // Another frame is wanted while anything moves.
    bool more = false;
    for (int i = 0; i < RX_DECKS && app->player; i++) {
        RXDeckState st;
        rx_deck_state(app->player, i, &st);
        if (st.playing || st.scrubbing || app->decks[i].loading_request || app->decks[i].ctl.cue_held)
            more = true;
        if (ui->decks[i].autoplay)
            more = true;
    }
    if (ImGui::IsAnyItemActive() || ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        more = true;
    // An export waiting for an answer is not moving.
    if (app->export_state.running && !app->export_state.confirm)
        more = true;
    // Previews and thumbnails are made a few per tick (rx_app_preview_wave);
    // keep drawing while visible rows still wait, but not forever.
    if (ui->previews_pending && ui->pending_frames < 120) {
        ui->pending_frames++;
        more = true;
    } else if (!ui->previews_pending) {
        ui->pending_frames = 0;
    }
    if (app->analysing_total > 0 && app->analysing_done < app->analysing_total)
        more = true;
    if (rx_app_tag_progress(app, nullptr, nullptr, nullptr))
        more = true;
    // The Add dialog's finder walking: its matches come in as it goes.
    if (ui->finder && ui_finder_walking(ui->finder))
        more = true;
    // Input this frame may have opened or resized something that settles
    // over the next two frames (ImGui sizes new windows on their second).
    if (GImGui->InputEventsTrail.Size > 0 || io.MouseDelta.x != 0 || io.MouseDelta.y != 0)
        ui_want_frames(ui, 2);
    if (ui->frames_wanted > 0) {
        ui->frames_wanted--;
        more = true;
    }
    return more;
}
