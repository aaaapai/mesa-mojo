#include <hardware/hardware.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <errno.h>

// ================== Hardware 函数包装 ==================

static void* sHardwareHandle = NULL;
static atomic_int isinitHardwareWrapper = 0;

typedef int (*hw_get_module_t)(const char*, const struct hw_module_t**);
static hw_get_module_t fp_hw_get_module = NULL;

static void initHardwareWrapper() {
    const char* hardwareLibPaths[] = {
        "/system/lib64/libhardware.so",
        "/system/lib/libhardware.so",
        "libhardware.so",
        NULL
    };
    
    for (int i = 0; hardwareLibPaths[i] != NULL; i++) {
        sHardwareHandle = dlopen(hardwareLibPaths[i], RTLD_LAZY | RTLD_GLOBAL);
        if (sHardwareHandle != NULL) {
            break;
        }
    }
    
    if (sHardwareHandle == NULL) {
        return;
    }
    
    fp_hw_get_module = (hw_get_module_t)dlsym(sHardwareHandle, "hw_get_module");
}

extern "C" {

// 只在 extern "C" 块中定义一次
int hw_get_module(const char *id, const struct hw_module_t **module) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&isinitHardwareWrapper, &expected, 1)) {
        initHardwareWrapper();
    }
    
    if (fp_hw_get_module) {
        return fp_hw_get_module(id, module);
    }
    
    // Stub 实现
    if (module) {
        *module = NULL;
    }
    return -ENOENT;
}

} // extern "C"
