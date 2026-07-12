#include <vndk/window.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <android/log.h>
#include <mutex>
#include <atomic>

#define LOG_TAG "NativeWindowWrapper"
#define ALOGE(...) do { printf("E/%s: ", LOG_TAG); printf(__VA_ARGS__); printf("\n"); } while(0)
#define ALOGW(...) do { printf("W/%s: ", LOG_TAG); printf(__VA_ARGS__); printf("\n"); } while(0)
#define ALOGI(...) do { printf("I/%s: ", LOG_TAG); printf(__VA_ARGS__); printf("\n"); } while(0)

// ================== 声明 libpojavexec.so 中的函数 ==================
typedef bool (*ns_load_t)(const char* lib_search_path);
typedef void* (*ns_dlopen_t)(const char* name, int flag);

static ns_load_t linker_ns_load = nullptr;
static ns_dlopen_t linker_ns_dlopen = nullptr;

// 尝试加载 libpojavexec.so 并获取函数指针
static bool initNamespaceBypass() {
    void* handle = dlopen("libpojavexec.so", RTLD_LAZY);
    if (!handle) {
        ALOGW("Failed to dlopen libpojavexec.so: %s", dlerror());
        return false;
    }
    linker_ns_load = (ns_load_t)dlsym(handle, "linker_ns_load");
    linker_ns_dlopen = (ns_dlopen_t)dlsym(handle, "linker_ns_dlopen");
    if (!linker_ns_load || !linker_ns_dlopen) {
        ALOGW("Failed to get symbols from libpojavexec.so");
        dlclose(handle);
        return false;
    }
    // 注意：这里不关闭 handle，因为后续还要调用里面的函数
    return true;
}

// 函数指针类型定义
typedef AHardwareBuffer* (*ANativeWindowBuffer_getHardwareBuffer_t)(ANativeWindowBuffer*);
typedef void (*AHardwareBuffer_acquire_t)(AHardwareBuffer*);
typedef void (*AHardwareBuffer_release_t)(AHardwareBuffer*);
typedef void (*AHardwareBuffer_describe_t)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
typedef int (*AHardwareBuffer_allocate_t)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
typedef const native_handle_t* (*AHardwareBuffer_getNativeHandle_t)(const AHardwareBuffer*);
typedef int (*AHardwareBuffer_isSupported_t)(const AHardwareBuffer_Desc*);
typedef void (*ANativeWindow_acquire_t)(ANativeWindow*);
typedef void (*ANativeWindow_release_t)(ANativeWindow*);
typedef int32_t (*ANativeWindow_getFormat_t)(ANativeWindow*);
typedef int (*ANativeWindow_setSwapInterval_t)(ANativeWindow*, int);
typedef int (*ANativeWindow_query_t)(const ANativeWindow*, ANativeWindowQuery, int*);
typedef int (*ANativeWindow_dequeueBuffer_t)(ANativeWindow*, ANativeWindowBuffer**, int*);
typedef int (*ANativeWindow_queueBuffer_t)(ANativeWindow*, ANativeWindowBuffer*, int);
typedef int (*ANativeWindow_cancelBuffer_t)(ANativeWindow*, ANativeWindowBuffer*, int);
typedef int (*ANativeWindow_setUsage_t)(ANativeWindow*, uint64_t);
typedef int (*ANativeWindow_setSharedBufferMode_t)(ANativeWindow*, bool);
typedef int32_t (*ANativeWindow_getWidth_t)(ANativeWindow*);
typedef int32_t (*ANativeWindow_getHeight_t)(ANativeWindow*);

// 函数指针变量
static ANativeWindowBuffer_getHardwareBuffer_t fp_ANativeWindowBuffer_getHardwareBuffer = nullptr;
static AHardwareBuffer_acquire_t fp_AHardwareBuffer_acquire = nullptr;
static AHardwareBuffer_release_t fp_AHardwareBuffer_release = nullptr;
static AHardwareBuffer_describe_t fp_AHardwareBuffer_describe = nullptr;
static AHardwareBuffer_allocate_t fp_AHardwareBuffer_allocate = nullptr;
static AHardwareBuffer_getNativeHandle_t fp_AHardwareBuffer_getNativeHandle = nullptr;
static AHardwareBuffer_isSupported_t fp_AHardwareBuffer_isSupported = nullptr;
static ANativeWindow_acquire_t fp_ANativeWindow_acquire = nullptr;
static ANativeWindow_release_t fp_ANativeWindow_release = nullptr;
static ANativeWindow_getFormat_t fp_ANativeWindow_getFormat = nullptr;
static ANativeWindow_setSwapInterval_t fp_ANativeWindow_setSwapInterval = nullptr;
static ANativeWindow_query_t fp_ANativeWindow_query = nullptr;
static ANativeWindow_dequeueBuffer_t fp_ANativeWindow_dequeueBuffer = nullptr;
static ANativeWindow_queueBuffer_t fp_ANativeWindow_queueBuffer = nullptr;
static ANativeWindow_cancelBuffer_t fp_ANativeWindow_cancelBuffer = nullptr;
static ANativeWindow_setUsage_t fp_ANativeWindow_setUsage = nullptr;
static ANativeWindow_setSharedBufferMode_t fp_ANativeWindow_setSharedBufferMode = nullptr;
static ANativeWindow_getWidth_t fp_ANativeWindow_getWidth = nullptr;
static ANativeWindow_getHeight_t fp_ANativeWindow_getHeight = nullptr;

// 线程安全初始化机制
static std::once_flag sInitFlag;
static std::mutex sLibraryMutex;
static void* sNativeWindowHandle = nullptr;
static std::atomic<bool> sLibraryLoaded{false};

// 内部初始化函数
static void initNativeWindowWrapperImpl() {
    std::lock_guard<std::mutex> lock(sLibraryMutex);
    if (sLibraryLoaded) return;

    // 1️⃣ 优先使用命名空间绕过方式加载 libnativewindow.so
    bool loaded_bypass = false;
    if (initNamespaceBypass()) {
        // 根据架构选择搜索路径
        const char* searchPath = nullptr;
#if defined(__aarch64__)
        searchPath = "/system/lib64";
#else
        searchPath = "/system/lib";
#endif
        if (linker_ns_load(searchPath)) {
            ALOGI("Namespace created, loading libnativewindow.so via bypass");
            sNativeWindowHandle = linker_ns_dlopen("libnativewindow.so", RTLD_LAZY | RTLD_LOCAL);
            if (sNativeWindowHandle) {
                ALOGI("Successfully loaded libnativewindow.so via namespace bypass");
                loaded_bypass = true;
            } else {
                ALOGW("linker_ns_dlopen failed for libnativewindow.so");
            }
        } else {
            ALOGW("linker_ns_load failed");
        }
    }

    // 2️⃣ 回退方案：普通 dlopen
    if (!loaded_bypass) {
        const char* libPaths[] = {
            "/system/lib64/libnativewindow.so",
            "/system/lib/libnativewindow.so",
            "libnativewindow.so",
            nullptr
        };
        for (int i = 0; libPaths[i] != nullptr; i++) {
            void* handle = dlopen(libPaths[i], RTLD_NOLOAD | RTLD_LOCAL);
            if (!handle) {
                handle = dlopen(libPaths[i], RTLD_LAZY | RTLD_LOCAL);
            }
            if (handle) {
                sNativeWindowHandle = handle;
                ALOGI("Successfully loaded %s (fallback)", libPaths[i]);
                loaded_bypass = true;
                break;
            }
            ALOGW("Failed to load %s: %s", libPaths[i], dlerror());
        }
    }

    if (!loaded_bypass || sNativeWindowHandle == nullptr) {
        ALOGE("All attempts to load libnativewindow.so failed, using stub implementations");
        sLibraryLoaded = true;
        return;
    }

    // 解析函数符号（与原来相同）
    fp_ANativeWindowBuffer_getHardwareBuffer = reinterpret_cast<ANativeWindowBuffer_getHardwareBuffer_t>(
        dlsym(sNativeWindowHandle, "ANativeWindowBuffer_getHardwareBuffer"));
    fp_AHardwareBuffer_acquire = reinterpret_cast<AHardwareBuffer_acquire_t>(
        dlsym(sNativeWindowHandle, "AHardwareBuffer_acquire"));
    fp_AHardwareBuffer_release = reinterpret_cast<AHardwareBuffer_release_t>(
        dlsym(sNativeWindowHandle, "AHardwareBuffer_release"));
    fp_AHardwareBuffer_describe = reinterpret_cast<AHardwareBuffer_describe_t>(
        dlsym(sNativeWindowHandle, "AHardwareBuffer_describe"));
    fp_AHardwareBuffer_allocate = reinterpret_cast<AHardwareBuffer_allocate_t>(
        dlsym(sNativeWindowHandle, "AHardwareBuffer_allocate"));
    fp_AHardwareBuffer_getNativeHandle = reinterpret_cast<AHardwareBuffer_getNativeHandle_t>(
        dlsym(sNativeWindowHandle, "AHardwareBuffer_getNativeHandle"));
    fp_AHardwareBuffer_isSupported = reinterpret_cast<AHardwareBuffer_isSupported_t>(
        dlsym(sNativeWindowHandle, "AHardwareBuffer_isSupported"));
    fp_ANativeWindow_acquire = reinterpret_cast<ANativeWindow_acquire_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_acquire"));
    fp_ANativeWindow_release = reinterpret_cast<ANativeWindow_release_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_release"));
    fp_ANativeWindow_getFormat = reinterpret_cast<ANativeWindow_getFormat_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_getFormat"));
    fp_ANativeWindow_setSwapInterval = reinterpret_cast<ANativeWindow_setSwapInterval_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_setSwapInterval"));
    fp_ANativeWindow_query = reinterpret_cast<ANativeWindow_query_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_query"));
    fp_ANativeWindow_dequeueBuffer = reinterpret_cast<ANativeWindow_dequeueBuffer_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_dequeueBuffer"));
    fp_ANativeWindow_queueBuffer = reinterpret_cast<ANativeWindow_queueBuffer_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_queueBuffer"));
    fp_ANativeWindow_cancelBuffer = reinterpret_cast<ANativeWindow_cancelBuffer_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_cancelBuffer"));
    fp_ANativeWindow_setUsage = reinterpret_cast<ANativeWindow_setUsage_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_setUsage"));
    fp_ANativeWindow_setSharedBufferMode = reinterpret_cast<ANativeWindow_setSharedBufferMode_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_setSharedBufferMode"));
    fp_ANativeWindow_getWidth = reinterpret_cast<ANativeWindow_getWidth_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_getWidth"));
    fp_ANativeWindow_getHeight = reinterpret_cast<ANativeWindow_getHeight_t>(
        dlsym(sNativeWindowHandle, "ANativeWindow_getHeight"));

    if (!fp_ANativeWindowBuffer_getHardwareBuffer || !fp_AHardwareBuffer_acquire ||
        !fp_AHardwareBuffer_release || !fp_ANativeWindow_acquire || !fp_ANativeWindow_release) {
        ALOGW("Some critical functions failed to resolve, library may be incomplete");
    }

    sLibraryLoaded = true;
}

// 确保初始化的辅助函数
static inline void ensureInitialized() {
    std::call_once(sInitFlag, initNativeWindowWrapperImpl);
}

// 内部函数：安全地获取函数指针（模板，用于调试）
template<typename T>
static T getFunctionPointer(T* funcPtr, const char* funcName) {
    ensureInitialized();
    if (funcPtr == nullptr) {
        ALOGW("Function %s not available, using stub", funcName);
    }
    return funcPtr;
}

// 简化调用的宏
#define SAFE_CALL(func, ...) \
    ensureInitialized(); \
    if (fp_##func) { \
        return fp_##func(__VA_ARGS__); \
    } \
    ALOGW("%s: no implementation available", #func);

#define SAFE_CALL_VOID(func, ...) \
    ensureInitialized(); \
    if (fp_##func) { \
        fp_##func(__VA_ARGS__); \
        return; \
    } \
    ALOGW("%s: no implementation available", #func);

#define SAFE_CALL_RET(func, default_val, ...) \
    ensureInitialized(); \
    if (fp_##func) { \
        return fp_##func(__VA_ARGS__); \
    } \
    ALOGW("%s: no implementation available, returning default", #func); \
    return default_val;

extern "C" {

AHardwareBuffer* ANativeWindowBuffer_getHardwareBuffer(ANativeWindowBuffer* anwb) {
    SAFE_CALL(ANativeWindowBuffer_getHardwareBuffer, anwb);
    return nullptr;
}

void AHardwareBuffer_acquire(AHardwareBuffer* buffer) {
    SAFE_CALL_VOID(AHardwareBuffer_acquire, buffer);
}

void AHardwareBuffer_release(AHardwareBuffer* buffer) {
    SAFE_CALL_VOID(AHardwareBuffer_release, buffer);
}

void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc) {
    SAFE_CALL_VOID(AHardwareBuffer_describe, buffer, outDesc);
}

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer) {
    SAFE_CALL(AHardwareBuffer_allocate, desc, outBuffer);
    return 0;
}

const native_handle_t* AHardwareBuffer_getNativeHandle(const AHardwareBuffer* buffer) {
    SAFE_CALL(AHardwareBuffer_getNativeHandle, buffer);
    return nullptr;
}

int AHardwareBuffer_isSupported(const AHardwareBuffer_Desc* desc) {
    ensureInitialized();
    if (fp_AHardwareBuffer_isSupported) {
        return fp_AHardwareBuffer_isSupported(desc);
    }
    ALOGW("AHardwareBuffer_isSupported: no implementation available");
    return -ENOENT;
}

void ANativeWindow_acquire(ANativeWindow* window) {
    SAFE_CALL_VOID(ANativeWindow_acquire, window);
}

void ANativeWindow_release(ANativeWindow* window) {
    SAFE_CALL_VOID(ANativeWindow_release, window);
}

int32_t ANativeWindow_getFormat(ANativeWindow* window) {
    SAFE_CALL(ANativeWindow_getFormat, window);
    return 0;
}

int ANativeWindow_setSwapInterval(ANativeWindow* window, int interval) {
    SAFE_CALL(ANativeWindow_setSwapInterval, window, interval);
    return 0;
}

int ANativeWindow_query(const ANativeWindow* window, ANativeWindowQuery query, int* value) {
    SAFE_CALL(ANativeWindow_query, window, query, value);
    return 0;
}

int ANativeWindow_dequeueBuffer(ANativeWindow* window, ANativeWindowBuffer** buffer, int* fenceFd) {
    SAFE_CALL(ANativeWindow_dequeueBuffer, window, buffer, fenceFd);
    return 0;
}

int ANativeWindow_queueBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer, int fenceFd) {
    SAFE_CALL(ANativeWindow_queueBuffer, window, buffer, fenceFd);
    return 0;
}

int ANativeWindow_cancelBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer, int fenceFd) {
    SAFE_CALL(ANativeWindow_cancelBuffer, window, buffer, fenceFd);
    return 0;
}

int ANativeWindow_setUsage(ANativeWindow* window, uint64_t usage) {
    SAFE_CALL(ANativeWindow_setUsage, window, usage);
    return 0;
}

int ANativeWindow_setSharedBufferMode(ANativeWindow* window, bool sharedBufferMode) {
    SAFE_CALL(ANativeWindow_setSharedBufferMode, window, sharedBufferMode);
    return 0;
}

int32_t ANativeWindow_getWidth(ANativeWindow* window) {
    SAFE_CALL(ANativeWindow_getWidth, window);
    return 0;
}

int32_t ANativeWindow_getHeight(ANativeWindow* window) {
    SAFE_CALL(ANativeWindow_getHeight, window);
    return 0;
}

} // extern "C"
