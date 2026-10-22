#ifndef PTVK_CORE_H
#define PTVK_CORE_H
/* Shared Vulkan core handles: created once by pt_vulkan.cpp (device bring-up,
 * PS1 VRAM image + persistent staging) and reused by the raster backend
 * (gpu_vk_raster.cpp). Both modules submit synchronously on the same queue
 * with their own command pools, so they never overlap in flight. */

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PtvkCore {
    VkDevice         device;
    VkPhysicalDevice phys;
    VkQueue          queue;
    uint32_t         qfam;
    VkImage          vramImg;      /* R16_UINT 1024x512, SHADER_READ layout */
    VkImageView      vramView;
    VkSampler        vramSamp;
    VkBuffer         vramStaging;  /* host-visible persistent staging */
    void*            vramStagingMap;
} PtvkCore;

/* Bring the device up (ptvk_init) + ensure the VRAM image exists.
 * Returns NULL on any failure. Idempotent. */
const PtvkCore* ptvk_core_acquire(void);

/* True when ptvk_upload_vram staged fresh VRAM content that has not been
 * copied to the image yet; the caller that records the copy clears it. */
int  ptvk_vram_dirty(void);
void ptvk_vram_clear_dirty(void);

/* Global GPU section lock. The PT trace now runs on its own thread while the
 * raster backend submits from the game thread; both share one VkQueue, the
 * VRAM image and its staging buffer, so every GPU-touching section (init,
 * record, submit, fence wait, readback) must hold this lock. NOT recursive. */
void ptvk_gpu_lock(void);
void ptvk_gpu_unlock(void);

/* 18.10.2026: wait for a submit to finish (1 = done). A busy GPU is waited for - Windows resets a hung one by itself
 * (TDR, about 2 s) and the wait then ends with VK_ERROR_DEVICE_LOST; that, or a minute without an answer, switches the
 * Vulkan path off until the game is restarted (ptvk_lost). A command buffer or fence of an unfinished submit is never
 * reused or destroyed. */
int  ptvk_wait_fence(VkFence f);
int  ptvk_lost(void);
void ptvk_mark_lost(int vkresult);

#ifdef __cplusplus
}
#endif
#endif /* PTVK_CORE_H */
