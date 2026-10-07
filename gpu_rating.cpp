/*
 * NvrGpuRating.dll -- tells Echo VR what GPU it runs on where Proton can't, so it picks the
 * graphics preset Windows would.
 *
 * On its first start (no settings file yet) Echo rates the GPU and saves a preset: NVIDIA by
 * cores x base clock x 2 (NVAPI's GetGpuCoreCount and GetAllClockFrequencies, base clock),
 * AMD by AGS's teraFlops; >= 8.0 (NVIDIA) or >= 10.0 (AMD) is High, >= 4.5 or >= 6.0 Medium,
 * anything less Low. Under Proton, dxvk-nvapi answers 0 cores and "base clock not supported",
 * and the game's own AGS can't reach the AMD driver: every Linux PC got Low.
 *
 * This plugin hooks nvapi64!nvapi_QueryInterface (the game asks it for both functions by id)
 * and amd_ags_x64!agsInit. Where the real answer is 0 or unsupported it fills in the GPU's
 * numbers, measured without a list of GPUs, so newer generations work as they come:
 *  - NVIDIA: SMs from Vulkan (VK_NV_shader_sm_builtins) x cores per SM (from the CUDA
 *    generation NVML reports; 128 for every generation since Ampere), and a base clock from
 *    NVML's maximum clock (NVML has no base clock).
 *  - AMD: compute units from Vulkan (VK_AMD_shader_core_properties[2]) x 64 lanes x 2 x the
 *    top engine clock (the Linux host's sysfs, through Wine's Z: drive).
 * Where the real driver answers (Windows), nothing changes.
 */

#include <windows.h>
#include <dxgi.h>
#include <MinHook.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "nevr_plugin_interface.h"

namespace {

// ---- what the game asks ----

constexpr unsigned kGetGpuCoreCount = 0xC7026A87;
constexpr unsigned kGetAllClockFrequencies = 0xDCB616C3;
constexpr unsigned kClockTypeBase = 1;
/* NV_GPU_CLOCK_FREQUENCIES v2/v3: version, ClockType (bits 0-3), then 32 domains of
 * {bIsPresent bit 0, frequency in kHz}; domain 0 is the graphics clock. */
constexpr unsigned kClocksSize = 0x108;

/* AGS 5 as Echo reads it: AGSGPUInfo {.., driverVersion +0x10, radeonSoftwareVersion +0x18,
 * numDevices +0x20, devices +0x28}, each AGSDeviceInfo 0x68 bytes {adapterString +0x08,
 * vendorId +0x10, deviceId +0x14, numCUs +0x1C, coreClock +0x20, teraFlops +0x28}. */
constexpr size_t kGpuInfoSize = 0x30;
constexpr size_t kDeviceSize = 0x68;

/* NVML has no base clock; base is about this much of the maximum clock NVML reports
 * (RTX 2080 Ti 1350/2190, RTX 3060 1320/2100). */
constexpr double kBaseOfMaxClock = 0.62;

/* A DLL's export as a function pointer of type T. */
template <typename T>
T Proc(HMODULE module, const char* name) {
    return module ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
}

// ---- the log ----

FILE* g_log = nullptr;
std::mutex g_logLock;

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logLock);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* plugin_logs/NvrGpuRating/NvrGpuRating.log beside the game's exe, where the other plugins
 * log. */
void OpenLog() {
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\plugin_logs", dir);
    CreateDirectoryW(path, nullptr);
    _snwprintf(path, MAX_PATH, L"%ls\\plugin_logs\\NvrGpuRating", dir);
    CreateDirectoryW(path, nullptr);
    _snwprintf(path, MAX_PATH, L"%ls\\plugin_logs\\NvrGpuRating\\NvrGpuRating.log", dir);
    g_log = _wfopen(path, L"w");
}

// ---- Vulkan, declared by hand (only what is read) ----

struct VkAppInfo {
    int sType;  // 0
    const void* pNext;
    const char* appName;
    uint32_t appVersion;
    const char* engineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};
struct VkInstanceInfo {
    int sType;  // 1
    const void* pNext;
    uint32_t flags;
    const VkAppInfo* app;
    uint32_t layerCount;
    const char* const* layers;
    uint32_t extensionCount;
    const char* const* extensions;
};
struct VkProperties2 {
    int sType;  // 1000059001
    void* pNext;
    unsigned char props[824];  // VkPhysicalDeviceProperties: vendorID +8, deviceID +12, deviceName +20
};
struct VkSmBuiltinsNV {
    int sType;  // 1000154001
    void* pNext;
    uint32_t shaderSMCount;
    uint32_t shaderWarpsPerSM;
};
struct VkShaderCoreAMD {
    int sType;  // 1000185000
    void* pNext;
    uint32_t shaderEngineCount, shaderArraysPerEngineCount, computeUnitsPerShaderArray;
    uint32_t rest[11];
};
struct VkShaderCore2AMD {
    int sType;  // 1000227000
    void* pNext;
    uint32_t shaderCoreFeatures;
    uint32_t activeComputeUnitCount;
};
struct VkExtension {
    char name[256];
    uint32_t specVersion;
};
using PFN_GetInstanceProcAddr = void*(__stdcall*)(void*, const char*);
using PFN_CreateInstance = int(__stdcall*)(const VkInstanceInfo*, const void*, void**);
using PFN_DestroyInstance = void(__stdcall*)(void*, const void*);
using PFN_EnumDevices = int(__stdcall*)(void*, uint32_t*, void**);
using PFN_Properties2 = void(__stdcall*)(void*, VkProperties2*);
using PFN_EnumExtensions = int(__stdcall*)(void*, const char*, uint32_t*, VkExtension*);

struct VulkanGpu {
    uint32_t vendor = 0, device = 0;
    char name[256] = {};
    uint32_t sms = 0;  // NVIDIA
    uint32_t cus = 0;  // AMD
};

/* The first GPU of `vendor` Vulkan sees, with its SM (NVIDIA) or compute unit (AMD) count. */
bool VulkanFind(uint32_t vendor, VulkanGpu& out) {
    HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
    if (!vk) {
        Log("Vulkan: no vulkan-1.dll");
        return false;
    }
    auto gipa = Proc<PFN_GetInstanceProcAddr>(vk, "vkGetInstanceProcAddr");
    auto create = gipa ? reinterpret_cast<PFN_CreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    if (!create) return false;
    VkAppInfo app = {0, nullptr, "NvrGpuRating", 1, "NvrGpuRating", 1, (1u << 22) | (1u << 12)};  // 1.1
    VkInstanceInfo info = {1, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    void* instance = nullptr;
    if (create(&info, nullptr, &instance) != 0 || !instance) {
        Log("Vulkan: vkCreateInstance failed");
        return false;
    }
    auto destroy = reinterpret_cast<PFN_DestroyInstance>(gipa(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_EnumDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto props2 = reinterpret_cast<PFN_Properties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
    auto extensions = reinterpret_cast<PFN_EnumExtensions>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    bool found = false;
    void* devices[16];
    uint32_t count = 16;
    if (enumerate && props2 && extensions && enumerate(instance, &count, devices) >= 0) {
        for (uint32_t i = 0; i < count && !found; i++) {
            VkProperties2 p = {};
            p.sType = 1000059001;
            props2(devices[i], &p);
            uint32_t v, d;
            memcpy(&v, p.props + 8, 4);
            memcpy(&d, p.props + 12, 4);
            if (v != vendor) continue;
            // Only extensions the device has go into the chain.
            uint32_t n = 0;
            extensions(devices[i], nullptr, &n, nullptr);
            VkExtension* list = new VkExtension[n ? n : 1];
            extensions(devices[i], nullptr, &n, list);
            auto has = [&](const char* name) {
                for (uint32_t k = 0; k < n; k++)
                    if (!strcmp(list[k].name, name)) return true;
                return false;
            };
            VkSmBuiltinsNV sm = {1000154001, nullptr, 0, 0};
            VkShaderCoreAMD core = {};
            core.sType = 1000185000;
            VkShaderCore2AMD core2 = {1000227000, nullptr, 0, 0};
            void* chain = nullptr;
            if (has("VK_NV_shader_sm_builtins")) sm.pNext = chain, chain = &sm;
            if (has("VK_AMD_shader_core_properties")) core.pNext = chain, chain = &core;
            if (has("VK_AMD_shader_core_properties2")) core2.pNext = chain, chain = &core2;
            delete[] list;
            p.pNext = chain;
            props2(devices[i], &p);
            out.vendor = v;
            out.device = d;
            memcpy(out.name, p.props + 20, 255);
            out.sms = sm.shaderSMCount;
            out.cus = core2.activeComputeUnitCount
                          ? core2.activeComputeUnitCount
                          : core.shaderEngineCount * core.shaderArraysPerEngineCount * core.computeUnitsPerShaderArray;
            found = true;
        }
    }
    if (destroy) destroy(instance, nullptr);
    return found;
}

// ---- NVML (Wine's bridge to the host driver under Proton) ----

struct Nvml {
    int ccMajor = 0, ccMinor = 0;
    unsigned maxMHz = 0;
};

bool NvmlRead(Nvml& out) {
    HMODULE h = LoadLibraryW(L"nvml.dll");
    if (!h) return false;
    auto init = Proc<int (*)()>(h, "nvmlInit_v2");
    auto handle = Proc<int (*)(unsigned, void**)>(h, "nvmlDeviceGetHandleByIndex_v2");
    auto cc = Proc<int (*)(void*, int*, int*)>(h, "nvmlDeviceGetCudaComputeCapability");
    auto maxClock = Proc<int (*)(void*, int, unsigned*)>(h, "nvmlDeviceGetMaxClockInfo");
    void* dev = nullptr;
    if (!init || init() != 0 || !handle || handle(0, &dev) != 0) return false;
    if (cc) cc(dev, &out.ccMajor, &out.ccMinor);
    if (maxClock) maxClock(dev, 0 /* NVML_CLOCK_GRAPHICS */, &out.maxMHz);
    return true;
}

/* FP32 cores per SM by CUDA generation (major.minor); 128 for anything newer. */
unsigned CoresPerSm(int major, int minor) {
    if (major == 6 && minor == 0) return 64;  // Pascal GP100
    if (major == 7) return 64;                // Volta, Turing
    if (major == 8 && minor == 0) return 64;  // Ampere GA100
    return 128;                               // Maxwell, Pascal, Ampere, Ada, Hopper, Blackwell, ...
}

// ---- the AMD engine clock from the Linux host (Wine maps / to Z:) ----

unsigned AmdHostMaxMHz(uint32_t device) {
    for (int card = 0; card < 16; card++) {
        char path[96];
        snprintf(path, sizeof path, "Z:\\sys\\class\\drm\\card%d\\device\\device", card);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        unsigned id = 0;
        int got = fscanf(f, "%x", &id);
        fclose(f);
        if (got != 1 || id != device) continue;
        snprintf(path, sizeof path, "Z:\\sys\\class\\drm\\card%d\\device\\pp_dpm_sclk", card);
        f = fopen(path, "r");
        if (!f) continue;
        unsigned best = 0, level, mhz;
        char line[128];
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%u: %uMhz", &level, &mhz) == 2 && mhz > best) best = mhz;
        fclose(f);
        if (best) return best;
    }
    return 0;
}

/* The DXGI adapter name of the first adapter of `vendor` (what Echo matches AGS' devices by). */
bool DxgiName(uint32_t vendor, char* name, size_t size) {
    HMODULE h = LoadLibraryW(L"dxgi.dll");
    auto create = Proc<HRESULT(WINAPI*)(REFIID, void**)>(h, "CreateDXGIFactory1");
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory)))) return false;
    bool found = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !found && factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == vendor) {
            WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, name, (int)size, nullptr, nullptr);
            found = true;
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

// ---- measured once, on the first question that needs it ----

std::once_flag g_nvidiaOnce, g_amdOnce;
unsigned g_nvCores = 0, g_nvBaseKHz = 0;
float g_amdTflops = 0;
unsigned g_amdCus = 0, g_amdMHz = 0;
char g_amdName[128] = {};

void MeasureNvidia() {
    VulkanGpu gpu;
    Nvml nvml;
    bool vk = VulkanFind(0x10DE, gpu);
    bool ml = NvmlRead(nvml);
    unsigned perSm = CoresPerSm(nvml.ccMajor, nvml.ccMinor);
    g_nvCores = gpu.sms * perSm;
    g_nvBaseKHz = nvml.maxMHz ? (unsigned)(nvml.maxMHz * kBaseOfMaxClock) * 1000 : 0;
    Log("NVIDIA: %s, %u SMs (Vulkan %s) x %u cores (CUDA %d.%d, NVML %s), max %u MHz -> %u cores, base %u MHz",
        gpu.name, gpu.sms, vk ? "ok" : "failed", perSm, nvml.ccMajor, nvml.ccMinor, ml ? "ok" : "failed",
        nvml.maxMHz, g_nvCores, g_nvBaseKHz / 1000);
}

void MeasureAmd() {
    VulkanGpu gpu;
    if (!VulkanFind(0x1002, gpu) || !DxgiName(0x1002, g_amdName, sizeof g_amdName)) {
        Log("AMD: no AMD GPU found (Vulkan or DXGI)");
        return;
    }
    g_amdCus = gpu.cus;
    g_amdMHz = AmdHostMaxMHz(gpu.device);
    g_amdTflops = (float)(g_amdCus * 64.0 * 2.0 * g_amdMHz / 1e6);
    Log("AMD: %s (\"%s\"), %u CUs, top clock %u MHz -> %.2f TFLOPS", gpu.name, g_amdName, g_amdCus, g_amdMHz,
        g_amdTflops);
}

// ---- the hooks ----

using QueryInterfaceFn = void*(__cdecl*)(unsigned);
using CoreCountFn = int(__cdecl*)(void*, unsigned*);
using ClocksFn = int(__cdecl*)(void*, uint32_t*);
using AgsInitFn = int(__cdecl*)(void**, const void*, unsigned char*);

QueryInterfaceFn g_trueQueryInterface = nullptr;
CoreCountFn g_trueCoreCount = nullptr;
ClocksFn g_trueClocks = nullptr;
AgsInitFn g_trueAgsInit = nullptr;
void* g_targets[2] = {};

int __cdecl CoreCountHook(void* gpu, unsigned* count) {
    int r = g_trueCoreCount(gpu, count);
    if (count && (r != 0 || *count == 0)) {
        std::call_once(g_nvidiaOnce, MeasureNvidia);
        if (g_nvCores) {
            Log("GetGpuCoreCount answered %d with %u cores: %u instead", r, r == 0 ? *count : 0, g_nvCores);
            *count = g_nvCores;
            return 0;
        }
    }
    return r;
}

int __cdecl ClocksHook(void* gpu, uint32_t* clocks) {
    int r = g_trueClocks(gpu, clocks);
    if (!clocks || (clocks[0] & 0xFFFF) != kClocksSize || (clocks[1] & 0xF) != kClockTypeBase) return r;
    if (r == 0 && (clocks[2] & 1) && clocks[3]) return r;
    std::call_once(g_nvidiaOnce, MeasureNvidia);
    if (!g_nvBaseKHz) return r;
    Log("GetAllClockFrequencies (base) answered %d: %u MHz instead", r, g_nvBaseKHz / 1000);
    clocks[2] |= 1;
    clocks[3] = g_nvBaseKHz;
    return 0;
}

void* __cdecl QueryInterfaceHook(unsigned id) {
    void* f = g_trueQueryInterface(id);
    if (!f) return f;
    if (id == kGetGpuCoreCount) {
        g_trueCoreCount = reinterpret_cast<CoreCountFn>(f);
        return reinterpret_cast<void*>(CoreCountHook);
    }
    if (id == kGetAllClockFrequencies) {
        g_trueClocks = reinterpret_cast<ClocksFn>(f);
        return reinterpret_cast<void*>(ClocksHook);
    }
    return f;
}

unsigned char g_amdDevice[kDeviceSize];
const char kEmpty[] = "";

int __cdecl AgsInitHook(void** context, const void* config, unsigned char* info) {
    int r = g_trueAgsInit(context, config, info);
    if (!info) return r;
    if (r != 0) memset(info, 0, kGpuInfoSize);  // a failed agsInit may leave it unset
    int n = 0;
    unsigned char* devices = nullptr;
    memcpy(&n, info + 0x20, 4);
    memcpy(&devices, info + 0x28, 8);
    for (int i = 0; devices && i < n; i++) {
        float tf;
        memcpy(&tf, devices + i * kDeviceSize + 0x28, 4);
        if (tf > 0) return r;  // AGS knows the GPU (Windows)
    }
    std::call_once(g_amdOnce, MeasureAmd);
    if (g_amdTflops <= 0) return r;
    memset(g_amdDevice, 0, sizeof g_amdDevice);
    const char* name = g_amdName;
    int vendor = 0x1002, cus = (int)g_amdCus, clock = (int)g_amdMHz;
    memcpy(g_amdDevice + 0x08, &name, 8);
    memcpy(g_amdDevice + 0x10, &vendor, 4);
    memcpy(g_amdDevice + 0x1C, &cus, 4);
    memcpy(g_amdDevice + 0x20, &clock, 4);
    memcpy(g_amdDevice + 0x28, &g_amdTflops, 4);
    const char* empty = kEmpty;
    void* list = g_amdDevice;
    int one = 1;
    if (r != 0) {
        memcpy(info + 0x10, &empty, 8);
        memcpy(info + 0x18, &empty, 8);
    }
    memcpy(info + 0x20, &one, 4);
    memcpy(info + 0x28, &list, 8);
    Log("agsInit answered %d without the GPU's TFLOPS: %s, %.2f TFLOPS instead", r, g_amdName, g_amdTflops);
    return r;
}

bool Hook(HMODULE module, const char* function, void* detour, void** original, void** target) {
    void* t = module ? reinterpret_cast<void*>(GetProcAddress(module, function)) : nullptr;
    if (!t) return false;
    MH_STATUS s = MH_CreateHook(t, detour, original);
    if (s == MH_OK) s = MH_EnableHook(t);
    if (s != MH_OK) {
        Log("ERROR: hooking %s: %s", function, MH_StatusToString(s));
        MH_RemoveHook(t);
        return false;
    }
    *target = t;
    Log("hooked %s", function);
    return true;
}

void Unhook() {
    for (void*& t : g_targets) {
        if (t) {
            MH_DisableHook(t);
            MH_RemoveHook(t);
            t = nullptr;
        }
    }
}

int Install() {
    if (g_targets[0] || g_targets[1]) return 0;  // installed through the other entry point
    OpenLog();
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("ERROR: MH_Initialize: %s", MH_StatusToString(s));
        return 1;
    }
    // NVAPI is a system DLL (the game loads it itself); AGS is the game's import.
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    HMODULE ags = GetModuleHandleW(L"amd_ags_x64.dll");
    Hook(nvapi, "nvapi_QueryInterface", reinterpret_cast<void*>(QueryInterfaceHook),
         reinterpret_cast<void**>(&g_trueQueryInterface), &g_targets[0]);
    Hook(ags, "agsInit", reinterpret_cast<void*>(AgsInitHook), reinterpret_cast<void**>(&g_trueAgsInit),
         &g_targets[1]);
    if (!g_targets[0] && !g_targets[1]) {
        Log("neither NVAPI nor AGS here: nothing to do");
        return 1;
    }
    return 0;
}

}  // namespace

extern "C" {

NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "gpu_rating";
    info.description = "Tells the game the GPU's cores and clock (or TFLOPS) where Proton can't, for the right graphics preset";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

uint32_t NvrPluginGetCapabilities(void) { return NEVR_PLUGIN_CAP_COSMETIC | NEVR_PLUGIN_CAP_HOOKS_ENGINE; }

int NvrPluginInitEx(const NvrGameContext*, const char*) { return Install(); }

int NvrPluginInit(const NvrGameContext*) { return Install(); }

void NvrPluginShutdown(void) {
    Unhook();
    std::lock_guard<std::mutex> lock(g_logLock);
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
