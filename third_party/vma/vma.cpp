/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The one translation unit that compiles the AMD Vulkan Memory Allocator.
 * Build flags pass -DVMA_STATIC_VULKAN_FUNCTIONS=0
 * -DVMA_DYNAMIC_VULKAN_FUNCTIONS=1, so VMA imports every entry point through
 * the two loaders handed to vmaCreateAllocator(). The C side (src/gpu/vk.c)
 * calls the VMA C API directly: vk_mem_alloc.h is usable from C. */

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
