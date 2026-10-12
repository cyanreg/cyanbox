/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Everything of the gpu module that has to be C++: Dear ImGui's context,
 * style and fonts, its SDL3 and Vulkan backends, user textures, the
 * waveform draw callback, and the few ImGui calls the demo makes from C.
 *
 * The Vulkan backend renders with dynamic rendering into the swapchain's
 * UNORM format, and rotates its vertex buffers over RX_FRAMES_IN_FLIGHT,
 * the same count the frame fences cover. Textures (the font atlas
 * included) go through the 1.92+ texture protocol
 * (ImGuiBackendFlags_RendererHasTextures), so the atlas grows on demand
 * and no glyph ranges are needed: a CJK title rasterises its glyphs the
 * first time it is drawn, from the CJK font merged behind the UI font.
 *
 * Fonts come from fontconfig: "sans-serif", then a font that covers
 * Japanese ("Noto Sans CJK JP", "Noto Sans CJK", or whatever fontconfig
 * has for lang=ja) merged into it. options.font replaces the first. The
 * size is options.font_size in ImGui units; the backend sets the
 * rasterizer density from the framebuffer scale, so text is sharp on a
 * HiDPI output without scaling the size, and style.FontScaleDpi carries
 * any UI scale the platform reports on top. */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"

#include <SDL3/SDL.h>
#include <fontconfig/fontconfig.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "gpu_priv.h"
#include "gpu_demo.h"
}

/* The backend's descriptor pool: one sampled-image descriptor per live
 * texture. textures.c refuses new textures before it runs out. */
#define RX_IMGUI_POOL_SIZE 4096

static ImGuiContext *ctx_of(RxWindow *w)
{
    return static_cast<ImGuiContext *>(w->imgui);
}

static void check_vk(VkResult res)
{
    if (res < 0)
        rx_err("gpu: imgui vulkan backend: %s", rx_vk_strerror(res));
}

/* ------------------------------------------------------------------- fonts */

static bool font_file_ok(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") ||
                   !strcasecmp(dot, ".ttc") || !strcasecmp(dot, ".otc"));
}

/* fontconfig's best match for `pattern`, as a file stb_truetype can read
 * and, with `need`, only if it covers those characters. */
static bool fc_find(const char *pattern, const FcChar32 *need, int nb_need,
                    char *path, size_t path_size, int *index)
{
    FcPattern *pat = FcNameParse(reinterpret_cast<const FcChar8 *>(pattern));
    if (!pat)
        return false;
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult res = FcResultNoMatch;
    FcPattern *match = FcFontMatch(nullptr, pat, &res);
    FcPatternDestroy(pat);
    if (!match)
        return false;

    bool ok = false;
    FcChar8 *file = nullptr;
    int idx = 0;
    FcCharSet *cs = nullptr;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
        font_file_ok(reinterpret_cast<const char *>(file))) {
        ok = true;
        if (nb_need) {
            ok = FcPatternGetCharSet(match, FC_CHARSET, 0, &cs) == FcResultMatch;
            for (int i = 0; ok && i < nb_need; i++)
                ok = FcCharSetHasChar(cs, need[i]);
        }
        if (FcPatternGetInteger(match, FC_INDEX, 0, &idx) != FcResultMatch)
            idx = 0;
    }

    if (ok) {
        snprintf(path, path_size, "%s", reinterpret_cast<const char *>(file));
        *index = idx;
    }

    FcPatternDestroy(match);
    return ok;
}

static void load_fonts(RxWindow *w)
{
    ImGuiIO &io = ImGui::GetIO();
    const float size = w->opts.font_size;
    char main_path[PATH_MAX] = "", cjk_path[PATH_MAX] = "";
    int main_index = 0, cjk_index = 0;

    if (!FcInit())
        rx_warn("gpu: fontconfig did not initialise");

    if (w->opts.font)
        snprintf(main_path, sizeof(main_path), "%s", w->opts.font);
    else if (!fc_find("sans-serif", nullptr, 0, main_path, sizeof(main_path), &main_index))
        main_path[0] = '\0';

    ImFont *font = nullptr;
    if (main_path[0]) {
        ImFontConfig cfg;
        cfg.FontNo = (ImU32)main_index;
        font = io.Fonts->AddFontFromFileTTF(main_path, size, &cfg);
        if (!font)
            rx_warn("gpu: cannot load the font %s", main_path);
    }
    if (!font) {
        ImFontConfig cfg;
        cfg.SizePixels = size;
        font = io.Fonts->AddFontDefault(&cfg);
        main_path[0] = '\0';
    }

    /* Hiragana, katakana and a common kanji: a font that has these is a
     * Japanese-capable fallback, whatever its name. */
    static const FcChar32 kana[] = { 0x3042, 0x30A2, 0x6F22 };
    static const char *const cjk_patterns[] = {
        "Noto Sans CJK JP",
        "Noto Sans CJK",
        ":lang=ja",
    };
    bool have_cjk = false;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(cjk_patterns) && !have_cjk; i++)
        have_cjk = fc_find(cjk_patterns[i], kana, (int)RX_ARRAY_ELEMS(kana), cjk_path,
                           sizeof(cjk_path), &cjk_index);

    if (have_cjk && (strcmp(cjk_path, main_path) || cjk_index != main_index)) {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.FontNo = (ImU32)cjk_index;
        if (!io.Fonts->AddFontFromFileTTF(cjk_path, size, &cfg)) {
            rx_warn("gpu: cannot load the CJK font %s", cjk_path);
            have_cjk = false;
        }
    }

    rx_info("gpu: font %s%s%s, %.1f px", main_path[0] ? main_path : "(ImGui default)",
            have_cjk ? " + " : "", have_cjk ? cjk_path : "", size);
    if (!have_cjk)
        rx_warn("gpu: no font covers Japanese; such titles will show as boxes");

    ImGui::GetStyle().FontSizeBase = size;
}

/* -------------------------------------------------------------------- init */

extern "C" void rx_imgui_set_scale(RxWindow *w)
{
    ImGui::SetCurrentContext(ctx_of(w));
    ImGuiStyle &style = ImGui::GetStyle();

    /* Sizes are scaled by the ratio to what was applied before, so that
     * whatever the interface changed in the style survives. */
    float s = w->content_scale > 0.0f ? w->content_scale : 1.0f;
    if (w->imgui_scale > 0.0f && s != w->imgui_scale)
        style.ScaleAllSizes(s / w->imgui_scale);
    else if (w->imgui_scale <= 0.0f && s != 1.0f)
        style.ScaleAllSizes(s);
    style.FontScaleDpi = s;
    w->imgui_scale = s;
}

extern "C" int rx_imgui_init(RxWindow *w)
{
    IMGUI_CHECKVERSION();
    ImGuiContext *ctx = ImGui::CreateContext();
    if (!ctx)
        return RX_ERROR(ENOMEM);
    w->imgui = ctx;
    ImGui::SetCurrentContext(ctx);

    ImGuiIO &io = ImGui::GetIO();
    /* The interface keeps its own layout; no imgui.ini in whatever the
     * working directory happens to be. */
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    w->imgui_scale = 0.0f;
    rx_imgui_set_scale(w);
    load_fonts(w);

    if (!ImGui_ImplSDL3_InitForVulkan(w->win)) {
        rx_err("gpu: ImGui's SDL3 backend did not start");
        return RX_ERROR(ENODEV);
    }
    w->imgui_sdl = true;

    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion         = VK_API_VERSION_1_3;
    info.Instance           = w->inst;
    info.PhysicalDevice     = w->phys;
    info.Device             = w->dev;
    info.QueueFamily        = w->qfam;
    info.Queue              = w->queue;
    info.DescriptorPoolSize = RX_IMGUI_POOL_SIZE;
    info.MinImageCount      = 2;
    info.ImageCount         = RX_FRAMES_IN_FLIGHT;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &w->format.format;
    info.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&info)) {
        rx_err("gpu: ImGui's Vulkan backend did not start");
        return RX_ERROR(ENODEV);
    }
    w->imgui_vk = true;

    return 0;
}

extern "C" void rx_imgui_shutdown(RxWindow *w)
{
    if (!w->imgui)
        return;

    ImGui::SetCurrentContext(ctx_of(w));
    if (w->imgui_vk)
        ImGui_ImplVulkan_Shutdown();
    if (w->imgui_sdl)
        ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx_of(w));
    w->imgui = nullptr;
    w->imgui_vk = false;
    w->imgui_sdl = false;
}

/* ------------------------------------------------------------------ frames */

extern "C" void rx_imgui_process_event(RxWindow *w, const union SDL_Event *ev)
{
    ImGui::SetCurrentContext(ctx_of(w));
    ImGui_ImplSDL3_ProcessEvent(ev);
}

extern "C" void rx_imgui_new_frame(RxWindow *w)
{
    ImGui::SetCurrentContext(ctx_of(w));
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

extern "C" void *rx_imgui_render(RxWindow *w)
{
    ImGui::SetCurrentContext(ctx_of(w));
    ImGui::Render();
    return ImGui::GetDrawData();
}

extern "C" void *rx_imgui_last_draw_data(RxWindow *w)
{
    if (!w->imgui || w->in_frame)
        return nullptr;
    ImGui::SetCurrentContext(ctx_of(w));
    return ImGui::GetDrawData();
}

extern "C" void rx_imgui_record(RxWindow *w, void *draw_data, VkCommandBuffer cmd)
{
    ImGui::SetCurrentContext(ctx_of(w));
    ImGui_ImplVulkan_RenderDrawData(static_cast<ImDrawData *>(draw_data), cmd);
}

/* ---------------------------------------------------------------- textures */

extern "C" uint64_t rx_imgui_add_texture(RxWindow *w, VkImageView view)
{
    ImGui::SetCurrentContext(ctx_of(w));
    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(view,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return (uint64_t)(uintptr_t)set;
}

extern "C" void rx_imgui_remove_texture(RxWindow *w, uint64_t id)
{
    if (!w->imgui_vk || !id)
        return;
    ImGui::SetCurrentContext(ctx_of(w));
    ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)(uintptr_t)id);
}

/* --------------------------------------------------------------- waveforms */

/* Runs inside ImGui_ImplVulkan_RenderDrawData(), with the frame's command
 * buffer in the backend's render state. The callback's data is copied out
 * first: ImGui stores it in a byte buffer shared by every callback of the
 * draw list, with no promise of alignment. */
static void wave_callback(const ImDrawList *, const ImDrawCmd *pcmd)
{
    RxWaveCmd c;
    if (!pcmd->UserCallbackData || pcmd->UserCallbackDataSize != (int)sizeof(c))
        return;
    memcpy(&c, pcmd->UserCallbackData, sizeof(c));

    ImDrawData *dd = ImGui::GetDrawData();
    ImGui_ImplVulkan_RenderState *rs = ImGui_ImplVulkan_GetRenderState();
    if (!dd || !rs)
        return;

    /* The same projection the backend uses for its own clip rectangles. */
    RxWaveView v;
    v.off_x     = dd->DisplayPos.x;
    v.off_y     = dd->DisplayPos.y;
    v.scale_x   = dd->FramebufferScale.x;
    v.scale_y   = dd->FramebufferScale.y;
    v.fb_width  = (float)(int)(dd->DisplaySize.x * dd->FramebufferScale.x + 0.5f);
    v.fb_height = (float)(int)(dd->DisplaySize.y * dd->FramebufferScale.y + 0.5f);
    v.clip[0] = fmaxf((pcmd->ClipRect.x - v.off_x) * v.scale_x, 0.0f);
    v.clip[1] = fmaxf((pcmd->ClipRect.y - v.off_y) * v.scale_y, 0.0f);
    v.clip[2] = fminf((pcmd->ClipRect.z - v.off_x) * v.scale_x, v.fb_width);
    v.clip[3] = fminf((pcmd->ClipRect.w - v.off_y) * v.scale_y, v.fb_height);
    if (v.clip[2] <= v.clip[0] || v.clip[3] <= v.clip[1])
        return;

    rx_wave_record(&c, rs->CommandBuffer, &v);
}

extern "C" void rx_imgui_add_wave(void *draw_list, const RxWaveCmd *c)
{
    ImDrawList *dl = static_cast<ImDrawList *>(draw_list);
    ImDrawCallback reset = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;
    if (!reset)
        return;

    dl->AddCallback(wave_callback, const_cast<RxWaveCmd *>(c), sizeof(*c));
    dl->AddCallback(reset, nullptr);
}

/* -------------------------------------------------------------- demo hooks */

static ImU32 col32(uint32_t rgba)
{
    return IM_COL32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
}

extern "C" void rx_demo_show_demo_window(void)
{
    ImGui::ShowDemoWindow();
}

extern "C" bool rx_demo_begin(const char *title, float x, float y, float width, float height)
{
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    return ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings);
}

extern "C" void rx_demo_end(void)
{
    ImGui::End();
}

extern "C" void rx_demo_text(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

extern "C" void *rx_demo_window_draw_list(void)
{
    return ImGui::GetWindowDrawList();
}

extern "C" void *rx_demo_background_draw_list(void)
{
    return ImGui::GetBackgroundDrawList();
}

extern "C" void *rx_demo_foreground_draw_list(void)
{
    return ImGui::GetForegroundDrawList();
}

extern "C" void rx_demo_cursor(float *x, float *y)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    *x = p.x;
    *y = p.y;
}

extern "C" float rx_demo_avail_width(void)
{
    return ImGui::GetContentRegionAvail().x;
}

extern "C" void rx_demo_dummy(float width, float height)
{
    ImGui::Dummy(ImVec2(width, height));
}

extern "C" void rx_demo_rect(void *draw_list, float x0, float y0, float x1, float y1,
                             uint32_t rgba)
{
    static_cast<ImDrawList *>(draw_list)->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                                                        col32(rgba));
}

extern "C" void rx_demo_line(void *draw_list, float x0, float y0, float x1, float y1,
                             uint32_t rgba, float thickness)
{
    static_cast<ImDrawList *>(draw_list)->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col32(rgba),
                                                  thickness);
}

extern "C" void rx_demo_image(uint64_t tex, float width, float height)
{
    ImGui::Image(ImTextureRef((ImTextureID)tex), ImVec2(width, height));
}

extern "C" bool rx_demo_checkbox(const char *label, bool *v)
{
    return ImGui::Checkbox(label, v);
}

extern "C" bool rx_demo_slider(const char *label, float *v, float lo, float hi)
{
    return ImGui::SliderFloat(label, v, lo, hi);
}
