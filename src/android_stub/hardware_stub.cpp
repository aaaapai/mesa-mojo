#include <hardware/hardware.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <errno.h>
#include <android/log.h>

#define LOG_TAG "HardwareWrapper"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ================== Hardware 函数包装 ==================
static void* sHardwareHandle = nullptr;
static atomic_int isinitHardwareWrapper = 0;

typedef int (*hw_get_module_t)(const char*, const struct hw_module_t**);
static hw_get_module_t fp_hw_get_module = nullptr;

static void initHardwareWrapper() {
    // 仅使用普通 dlopen，尝试多个可能路径
    const char* hardwareLibPaths[] = {
        "/system/lib64/libhardware.so",
        "/system/lib/libhardware.so",
        "libhardware.so",
        nullptr
    };
    
    for (int i = 0; hardwareLibPaths[i] != nullptr; i++) {
        sHardwareHandle = dlopen(hardwareLibPaths[i], RTLD_LAZY | RTLD_GLOBAL);
        if (sHardwareHandle != nullptr) {
            LOGD("dlopen succeeded for %s", hardwareLibPaths[i]);
            break;
        }
    }
    
    if (sHardwareHandle == nullptr) {
        LOGE("All dlopen attempts for libhardware.so failed");
        return;
    }
    
    fp_hw_get_module = (hw_get_module_t)dlsym(sHardwareHandle, "hw_get_module");
    if (fp_hw_get_module) {
        LOGD("Successfully loaded hw_get_module");
    } else {
        LOGE("dlsym hw_get_module failed");
        dlclose(sHardwareHandle);
        sHardwareHandle = nullptr;
    }
}

extern "C" {

int hw_get_module(const char *id, const struct hw_module_t **module) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&isinitHardwareWrapper, &expected, 1)) {
        initHardwareWrapper();
    }
    
    if (fp_hw_get_module) {
        return fp_hw_get_module(id, module);
    }
    
    // 如果加载失败，返回默认错误，并将 module 置空
    if (module) {
        *module = nullptr;
    }
    return -ENOENT;
}

} // extern "C"
