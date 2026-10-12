// rxbox's Dear ImGui configuration. The upstream imconfig.h is all
// commented-out options; these are the ones rxbox sets.
#pragma once
#define IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
// ImTextureID stays the default ImU64, which rxbox/gpu.h hands around as uint64_t.
