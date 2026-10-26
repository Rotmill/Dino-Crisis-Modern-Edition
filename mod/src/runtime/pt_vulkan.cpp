// pt_vulkan.cpp - the shared Vulkan core of the GPU raster (gpu_vk_raster.cpp): device bring-up, command and
// buffer helpers, the 1x VRAM image and its upload, the GPU lock. Ported from KF2_PT; until 18.10.2026 the file
// also held the hardware path tracer (F5), removed together with it. API is extern "C" (ptvk_core.h).
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <vector>
/* 10.10.2026: math.h явно - для Clang (libc++ не тянет его сам, как заголовки MSVC) */
#include <math.h>

// ---------------- Module state ----------------
static bool             s_vkReady = false;
static bool             s_vkTried = false;
static VkInstance       s_instance = VK_NULL_HANDLE;
extern "C" { int g_vk_conservative = 0; }
extern "C" { int g_vk_nonuniform_tex = 0; }   /* 05.10: массив текстур с номером из вершины (подмена текстур из mods) */
extern "C" { int g_vk_d3d11_share = 0; }      /* 09.10: общая с Direct3D 11 текстура (VK_KHR_external_memory_win32 + keyed mutex) */
static VkPhysicalDevice s_phys     = VK_NULL_HANDLE;
static VkDevice         s_device   = VK_NULL_HANDLE;
static VkQueue          s_queue    = VK_NULL_HANDLE;
static uint32_t         s_queueFamily = 0;

struct VkBuf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr; };

/* 10.10: почему устройство не поднялось и на какой видеокарте - для окна игроку (main.cpp dc_renderer_fail_notice): в
 * релизе журнала нет, и у друга игрока игра молча показала родную картинку PS1 */
static char s_vkErr[200] = "";
static char s_vkDev[128] = "";
extern "C" const char* ptvk_last_error(void) { return s_vkErr; }
extern "C" const char* ptvk_device_name(void) { return s_vkDev; }
/* 24.10.2026: выбранная видеокарта - встроенная (main.cpp gfx_autolow_tick: настройки ниже для слабой видеокарты) */
static int s_vkIntegrated = 0;
extern "C" int ptvk_device_integrated(void) { return s_vkIntegrated; }
/* 24.10.2026: производитель выбранной видеокарты (PCI: 0x8086 Intel, 0x10DE NVIDIA, 0x1002 AMD) - main.cpp dsh_prepare */
static uint32_t s_vkVendor = 0;
extern "C" unsigned ptvk_device_vendor(void) { return s_vkVendor; }
/* 24.10.2026: видеопамять (VK_EXT_memory_budget, если есть): сколько занято игрой и сколько ей даёт система в кучах своей памяти
 * видеокарты (DEVICE_LOCAL), и их размер. У встроенной своей памяти бывает 512 МБ: сверх неё система гоняет образы туда-обратно,
 * и всё встаёт (игрок: со сглаживанием на встроенной Radeon игра на 20 % скорости). 1 - занято и бюджет известны, 0 - только размер. */
static int s_vkBudget = 0;
extern "C" int ptvk_vram_usage(double* used_mb, double* budget_mb, double* heap_mb) {
    *used_mb = *budget_mb = *heap_mb = 0.0;
    if (!s_phys) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    VkPhysicalDeviceMemoryProperties2 mp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
    if (s_vkBudget) mp.pNext = &b;
    vkGetPhysicalDeviceMemoryProperties2(s_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++) {
        if (!(mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        *heap_mb += (double)mp.memoryProperties.memoryHeaps[i].size / 1048576.0;
        if (s_vkBudget) { *used_mb += (double)b.heapUsage[i] / 1048576.0; *budget_mb += (double)b.heapBudget[i] / 1048576.0; }
    }
    return s_vkBudget;
}
/* 10.10: LUID видеокарты Vulkan - main.cpp сверяет его с видеокартой Direct3D 11 перед общей текстурой кадра: на ноутбуке
 * с двумя видеокартами Vulkan берёт дискретную, а вывод мог встать на встроенную */
static unsigned char s_vkLuid[8];
static bool s_vkLuidValid = false;
extern "C" int ptvk_device_luid(unsigned char* out8) {
    if (!s_vkLuidValid) return 0;
    memcpy(out8, s_vkLuid, 8);
    return 1;
}

extern "C" int dc_diag_log(void);   /* dc_boot.c: DC_DIAG_LOG, off in a release */
static FILE* s_log = nullptr;
static void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (s_log) { vfprintf(s_log, fmt, ap); fflush(s_log); }
    va_end(ap);
}

static VkDebugUtilsMessengerEXT s_dbgMsgr = VK_NULL_HANDLE;
static VKAPI_ATTR VkBool32 VKAPI_CALL dbgCb(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) vlog("[VK-DBG] %s\n", d->pMessage);
    return VK_FALSE;
}

extern "C" int ptvk_init(void)
{
    if (s_vkTried) return s_vkReady ? 1 : 0;
    s_vkTried = true;
    s_log = dc_diag_log() ? fopen("pt_vk.txt", "w") : nullptr;
    vlog("[VK] DC PT: device bring-up\n");

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "DinoCrisis PT";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    const char* wantLayer = "VK_LAYER_KHRONOS_validation";
    const char* dbgExt = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    bool haveValidation = false;
    if (getenv("PSX_VK_VALIDATION")) {
        uint32_t lc = 0; vkEnumerateInstanceLayerProperties(&lc, nullptr);
        std::vector<VkLayerProperties> layers(lc); vkEnumerateInstanceLayerProperties(&lc, layers.data());
        for (auto& l : layers) if (!strcmp(l.layerName, wantLayer)) haveValidation = true;
    }
    if (haveValidation) {
        ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &wantLayer;
        ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = &dbgExt;
        vlog("[VK] validation ON (PSX_VK_VALIDATION)\n");
    }
    if (vkCreateInstance(&ici, nullptr, &s_instance) != VK_SUCCESS) {
        snprintf(s_vkErr, sizeof s_vkErr, "no Vulkan driver (vkCreateInstance failed)");
        vlog("[VK] vkCreateInstance FAILED\n"); return 0;
    }
    if (haveValidation) {
        auto fp = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(s_instance, "vkCreateDebugUtilsMessengerEXT");
        if (fp) {
            VkDebugUtilsMessengerCreateInfoEXT ci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            ci.pfnUserCallback = dbgCb;
            fp(s_instance, &ci, nullptr, &s_dbgMsgr);
        }
    }

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(s_instance, &pdCount, nullptr);
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(s_instance, &pdCount, pds.data());

    std::vector<const char*> devExts;
    int bestScore = -1;
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qp(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp.data());
        bool gq = false;
        for (uint32_t i = 0; i < qn; i++)
            if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) gq = true;
        const int type = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                         props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 :
                         props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 1 : 0;
        const int score = gq ? type : -1;              /* 18.10.2026: discrete > integrated > virtual */
        vlog("[VK]  - %s  Vulkan %u.%u  graphics+compute=%s  score %d\n", props.deviceName,
             VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), gq ? "yes" : "no", score);
        {   /* 23.10.2026 (GitHub #2: a crash after the shader preparation on Intel UHD 630 with nothing to go by): every
             * device and its driver into the game's log - pt_vk.txt is off in a release (dc_boot.c) */
            VkPhysicalDeviceDriverProperties drv{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            if (props.apiVersion >= VK_API_VERSION_1_2) p2.pNext = &drv;
            vkGetPhysicalDeviceProperties2(pd, &p2);
            static const char* const kType[5] = { "other", "integrated", "discrete", "virtual", "CPU" };
            fprintf(stderr, "render: Vulkan device %s (%s), Vulkan %u.%u.%u, driver %s %s (0x%08X), id %04X:%04X, graphics+compute %s\n",
                    props.deviceName, props.deviceType <= VK_PHYSICAL_DEVICE_TYPE_CPU ? kType[props.deviceType] : "other",
                    VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
                    drv.driverName[0] ? drv.driverName : "?", drv.driverInfo, props.driverVersion, props.vendorID, props.deviceID,
                    gq ? "yes" : "no");
        }
        if (score > bestScore) { bestScore = score; s_phys = pd; s_vkIntegrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
                                 s_vkVendor = props.vendorID;
                                 snprintf(s_vkDev, sizeof s_vkDev, "%s", props.deviceName); }
        else if (!s_vkDev[0]) snprintf(s_vkDev, sizeof s_vkDev, "%s", props.deviceName);
    }
    if (s_phys == VK_NULL_HANDLE) {
        snprintf(s_vkErr, sizeof s_vkErr, pdCount ? "the graphics card has no Vulkan graphics queue" : "no Vulkan graphics card found");
        vlog("[VK] no Vulkan device with a graphics+compute queue\n"); return 0;
    }
    {   /* 10.10: LUID выбранной видеокарты (ptvk_device_luid) */
        VkPhysicalDeviceIDProperties idp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &idp;
        vkGetPhysicalDeviceProperties2(s_phys, &p2);
        s_vkLuidValid = idp.deviceLUIDValid == VK_TRUE;
        if (s_vkLuidValid) memcpy(s_vkLuid, idp.deviceLUID, 8);
        vlog("[VK] device LUID: %s%02X%02X%02X%02X-%02X%02X%02X%02X\n", s_vkLuidValid ? "" : "(not valid) ",
             idp.deviceLUID[7], idp.deviceLUID[6], idp.deviceLUID[5], idp.deviceLUID[4],
             idp.deviceLUID[3], idp.deviceLUID[2], idp.deviceLUID[1], idp.deviceLUID[0]);
    }

    /* Conservative rasterization (DC_CONSERVATIVE, default on if the GPU has
     * it): a triangle smaller than a pixel covers no sample point and is not
     * drawn at all — on the console it was sub-pixel anyway, but we render the
     * same polygon at 5x, so the body gets see-through gaps that grow with
     * distance. Overestimated rasterization fills any pixel the polygon
     * TOUCHES, which removes those gaps without moving a single vertex. */
    g_vk_conservative = 0;
    {
        uint32_t nx = 0;
        vkEnumerateDeviceExtensionProperties(s_phys, nullptr, &nx, nullptr);
        std::vector<VkExtensionProperties> xprops(nx);
        vkEnumerateDeviceExtensionProperties(s_phys, nullptr, &nx, xprops.data());
        for (uint32_t i = 0; i < nx; i++) {
            if (!strcmp(xprops[i].extensionName, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME)) {
                /* DEFAULT OFF: overestimated coverage paints a bright hairline
                 * along every polygon seam (walls, door frames) — worse than
                 * the gaps it was meant to close. DC_CONSERVATIVE=1 to try. */
                const char* ce = getenv("DC_CONSERVATIVE");
                if (ce && *ce == '1') {
                    devExts.push_back(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
                    g_vk_conservative = 1;
                    vlog("[VK] conservative rasterization: ENABLED%c", 10);
                }
                break;
            }
        }
        if (!g_vk_conservative) vlog("[VK] conservative rasterization: not available/off%c", 10);
    }
    /* 09.10.2026: кадр растра в Direct3D 11 без чтения в память процессора (gpu_vk_raster.cpp, DC_VK_SHARED): общая
     * текстура D3D11 (NT-дескриптор) и её мьютекс. Оба расширения - только если есть; нет - прежнее чтение кадра. */
    g_vk_d3d11_share = 0;
    {
        uint32_t nx = 0;
        vkEnumerateDeviceExtensionProperties(s_phys, nullptr, &nx, nullptr);
        std::vector<VkExtensionProperties> xprops(nx);
        vkEnumerateDeviceExtensionProperties(s_phys, nullptr, &nx, xprops.data());
        bool hm = false, hk = false;
        for (uint32_t i = 0; i < nx; i++) {
            if (!strcmp(xprops[i].extensionName, "VK_KHR_external_memory_win32")) hm = true;
            if (!strcmp(xprops[i].extensionName, "VK_KHR_win32_keyed_mutex")) hk = true;
        }
        if (hm && hk) {
            devExts.push_back("VK_KHR_external_memory_win32");
            devExts.push_back("VK_KHR_win32_keyed_mutex");
            g_vk_d3d11_share = 1;
        }
        vlog("[VK] D3D11 shared texture (external memory win32 %d, keyed mutex %d): %s%c", hm ? 1 : 0, hk ? 1 : 0,
             g_vk_d3d11_share ? "available" : "no", 10);
        /* 24.10.2026: сколько видеопамяти занято и сколько её даёт система (ptvk_vram_usage, строки vram: журнала) */
        s_vkBudget = 0;
        for (uint32_t i = 0; i < nx; i++)
            if (!strcmp(xprops[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
                devExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                s_vkBudget = 1;
                break;
            }
    }

    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &qCount, qprops.data());
    bool found = false;
    for (uint32_t i = 0; i < qCount; i++) {
        if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { s_queueFamily = i; found = true; break; }
    }
    if (!found) { vlog("[VK] no graphics+compute queue\n"); return 0; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = s_queueFamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;


    /* fillModeNonSolid: the wireframe overlay (DC_WIREFRAME) draws polygons as
     * lines. Asked for only when the device has it, so a device without it
     * still comes up - the overlay is a diagnostic, not a requirement. */
    VkPhysicalDeviceFeatures avail{};
    vkGetPhysicalDeviceFeatures(s_phys, &avail);
    VkPhysicalDeviceFeatures want{};
    want.fillModeNonSolid = avail.fillModeNonSolid;
    want.wideLines        = avail.wideLines;
    /* 25.09: MSAA «по выборкам» (dc_raster_ss.frag.spv, квалификатор sample) — только если устройство умеет */
    want.sampleRateShading = avail.sampleRateShading;

    /* 05.10.2026: текстуры PNG из mods (gpu_vk_raster.cpp TEXREP) — массив картинок в шейдере растра с номером из вершины
     * (nonuniformEXT). Есть у всех устройств с трассировкой лучей; если вдруг нет — устройство поднимается как раньше,
     * а подмена текстур выключается (g_vk_nonuniform_tex). */
    VkPhysicalDeviceDescriptorIndexingFeatures dif{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    {
        VkPhysicalDeviceDescriptorIndexingFeatures q{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
        VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        f2.pNext = &q;
        vkGetPhysicalDeviceFeatures2(s_phys, &f2);
        dif.shaderSampledImageArrayNonUniformIndexing = q.shaderSampledImageArrayNonUniformIndexing;
        g_vk_nonuniform_tex = q.shaderSampledImageArrayNonUniformIndexing ? 1 : 0;
        vlog("[VK] sampled image array non-uniform indexing: %s%c", g_vk_nonuniform_tex ? "yes" : "NO", 10);
    }

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &dif;
    dci.pEnabledFeatures = &want;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();
    if (vkCreateDevice(s_phys, &dci, nullptr, &s_device) != VK_SUCCESS) {
        snprintf(s_vkErr, sizeof s_vkErr, "the Vulkan device could not be created (vkCreateDevice failed)");
        vlog("[VK] vkCreateDevice FAILED\n"); return 0;
    }
    vkGetDeviceQueue(s_device, s_queueFamily, 0, &s_queue);


    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(s_phys, &props);
    vlog("[VK] device ready: %s\n", props.deviceName);
    fprintf(stderr, "render: Vulkan ready on %s; textures from mods %s, frame to Direct3D 11 without a copy %s\n",
            props.deviceName, g_vk_nonuniform_tex ? "yes" : "no (no non-uniform indexing)",
            g_vk_d3d11_share ? "possible" : "no (no external memory / keyed mutex)");
    s_vkReady = true;
    return 1;
}

// ---------------- Command / buffer helpers (KF2 verbatim) ----------------
static VkCommandPool   s_cmdPool = VK_NULL_HANDLE;
static VkCommandBuffer s_cmd     = VK_NULL_HANDLE;
static VkFence         s_fence   = VK_NULL_HANDLE;

static uint32_t findMemType(uint32_t typeBits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(s_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
static bool createBuf(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuf& out) {
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s_device, &bci, nullptr, &out.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_device, out.buf, &mr);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMemType(mr.memoryTypeBits, props);
    if (mai.memoryTypeIndex == ~0u || vkAllocateMemory(s_device, &mai, nullptr, &out.mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(s_device, out.buf, out.mem, 0);
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) vkMapMemory(s_device, out.mem, 0, mr.size, 0, &out.map);
    return true;
}
static bool ensureCmd() {
    if (s_cmdPool != VK_NULL_HANDLE) return true;
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s_queueFamily;
    if (vkCreateCommandPool(s_device, &pci, nullptr, &s_cmdPool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(s_device, &ai, &s_cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(s_device, &fci, nullptr, &s_fence);
    return true;
}
/* 18.10.2026 (разбор кода): КОНЕЦ РАБОТЫ ВИДЕОКАРТЫ (ptvk_core.h). Прежде ожидание сдавалось через 4 с, и следующий кадр
 * сбрасывал и переписывал буфер команд, который видеокарта ещё исполняла, а разовая заливка удаляла его забор - Vulkan
 * это запрещает, итог - зависание или вылет в драйвере. Теперь занятую видеокарту ждём дальше: зависшую Windows
 * сбрасывает сама (TDR, около 2 с), и ожидание кончается VK_ERROR_DEVICE_LOST. Потеря (или минута без ответа) выключает
 * путь Vulkan до перезапуска: растр отдаёт пустой кадр, игра идёт с родной картинкой, main.cpp говорит игроку. */
static volatile LONG s_vkLost = 0;
extern "C" int ptvk_lost(void) { return s_vkLost ? 1 : 0; }
extern "C" void ptvk_mark_lost(int vkresult) {
    if (InterlockedExchange(&s_vkLost, 1) == 0)
        fprintf(stderr, "render: Vulkan device lost (VkResult %d) - Vulkan graphics are off until the game restarts\n", vkresult);
}
extern "C" int ptvk_wait_fence(VkFence f) {
    if (s_vkLost) return 0;
    for (int sec = 0; ; sec++) {
        const VkResult r = vkWaitForFences(s_device, 1, &f, VK_TRUE, 1000000000ULL);
        if (r == VK_SUCCESS) return 1;
        if (r != VK_TIMEOUT || sec >= 60) { ptvk_mark_lost((int)r); return 0; }
        if (sec == 3) fprintf(stderr, "render: the graphics card has been busy for 4 s - still waiting\n");
    }
}
static void beginCmd() {
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_cmd, &bi);
}
// Returns false if the device was lost (ptvk_wait_fence) - the caller then bails out.
static bool endSubmitWait() {
    vkEndCommandBuffer(s_cmd);
    vkResetFences(s_device, 1, &s_fence);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd;
    const VkResult sr = vkQueueSubmit(s_queue, 1, &si, s_fence);
    if (sr != VK_SUCCESS) { if (sr == VK_ERROR_DEVICE_LOST) ptvk_mark_lost((int)sr); return false; }
    return ptvk_wait_fence(s_fence) != 0;
}
static void imgBarrier(VkImage img, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    bar.oldLayout = from; bar.newLayout = to; bar.image = img;
    bar.srcAccessMask = srcA; bar.dstAccessMask = dstA;
    bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(s_cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &bar);
}

// ---------------- PS1 VRAM image (R16_UINT 1024x512) ----------------
static const int      VRAM_W = 1024, VRAM_H = 512;
static VkImage        s_vramImg = VK_NULL_HANDLE;
static VkDeviceMemory s_vramMem = VK_NULL_HANDLE;
static VkImageView    s_vramView = VK_NULL_HANDLE;
static VkSampler      s_vramSamp = VK_NULL_HANDLE;
static bool           s_vramImgReady = false;
static VkBuf          s_vramStaging;

static bool ensureVramImage() {
    if (s_vramImgReady) return true;
    if (!s_vkReady) return false;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R16_UINT;
    ici.extent = { (uint32_t)VRAM_W, (uint32_t)VRAM_H, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_device, &ici, nullptr, &s_vramImg) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_device, s_vramImg, &mr);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(s_device, &mai, nullptr, &s_vramMem);
    vkBindImageMemory(s_device, s_vramImg, s_vramMem, 0);
    if (ensureCmd() && !s_vkLost) {
        beginCmd();
        imgBarrier(s_vramImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);   /* 24.10.2026: RT stage without RT on the device was invalid */
        endSubmitWait();
    }
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = s_vramImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R16_UINT;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(s_device, &vci, nullptr, &s_vramView);
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(s_device, &sci, nullptr, &s_vramSamp);
    // Persistent staging buffer (uploads happen every traced frame).
    createBuf((VkDeviceSize)VRAM_W * VRAM_H * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s_vramStaging);
    s_vramImgReady = true;
    return true;
}

// Stage VRAM content; the copy is recorded into the next render submit.
static bool s_vramDirty = false;
extern "C" void ptvk_upload_vram(const unsigned short* vram) {
    if (!s_vkReady || !vram) return;
    if (!ensureVramImage() || !s_vramStaging.map) return;
    memcpy(s_vramStaging.map, vram, (size_t)VRAM_W * VRAM_H * 2);
    s_vramDirty = true;
}

// ---------------- Shared core for the raster backend (ptvk_core.h) --------
#include "ptvk_core.h"
static PtvkCore s_core;
extern "C" const PtvkCore* ptvk_core_acquire(void) {
    if (!ptvk_init()) return nullptr;
    if (!ensureVramImage()) return nullptr;
    s_core.device = s_device; s_core.phys = s_phys;
    s_core.queue = s_queue;   s_core.qfam = s_queueFamily;
    s_core.vramImg = s_vramImg; s_core.vramView = s_vramView;
    s_core.vramSamp = s_vramSamp;
    s_core.vramStaging = s_vramStaging.buf;
    s_core.vramStagingMap = s_vramStaging.map;
    return &s_core;
}
extern "C" int  ptvk_vram_dirty(void)       { return s_vramDirty ? 1 : 0; }
extern "C" void ptvk_vram_clear_dirty(void) { s_vramDirty = false; }

// Global GPU section lock (see ptvk_core.h): serializes the PT render thread
// against the game-thread raster backend on the shared queue / VRAM image.
static SRWLOCK s_gpuLock = SRWLOCK_INIT;
extern "C" void ptvk_gpu_lock(void)   { AcquireSRWLockExclusive(&s_gpuLock); }
extern "C" void ptvk_gpu_unlock(void) { ReleaseSRWLockExclusive(&s_gpuLock); }

