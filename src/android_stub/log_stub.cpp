#include <android/log.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdarg.h>

// ================== log 相关函数包装 ==================

static void* sLogHandle = NULL;
static atomic_int isinitLogWrapper = 0;

// 函数指针类型定义
typedef int (*android_log_write_t)(int, const char*, const char*);
typedef int (*android_log_print_t)(int, const char*, const char*, ...);
typedef int (*android_log_vprint_t)(int, const char*, const char*, va_list);

// 函数指针变量
static android_log_write_t fp_android_log_write = NULL;
static android_log_print_t fp_android_log_print = NULL;
static android_log_vprint_t fp_android_log_vprint = NULL;

static void initLogWrapper() {
    const char* logLibPaths[] = {
        "/system/lib64/liblog.so",
        "/system/lib/liblog.so",
        "liblog.so",
        NULL
    };
    
    for (int i = 0; logLibPaths[i] != NULL; i++) {
        // 先尝试获取已加载的库
        sLogHandle = dlopen("liblog.so", RTLD_NOLOAD);
        if (sLogHandle != NULL) {
            printf("Successfully loaded %s (log)", logLibPaths[i]);
            break;
        } else {
            sLogHandle = dlopen(logLibPaths[i], RTLD_LAZY | RTLD_LOCAL);
            if (sLogHandle != NULL) {
                printf("Successfully loaded %s (log)", logLibPaths[i]);
                break;
            } else {
                printf("Failed to load %s (log): %s", logLibPaths[i], dlerror());
            }
        }
    }
    
    if (sLogHandle == NULL) {
        printf("All log library paths failed, using stub implementations");
        return;
    }
    
    // 解析 log 函数符号
    fp_android_log_write = (android_log_write_t)dlsym(sLogHandle, "__android_log_write");
    fp_android_log_print = (android_log_print_t)dlsym(sLogHandle, "__android_log_print");
    fp_android_log_vprint = (android_log_vprint_t)dlsym(sLogHandle, "__android_log_vprint");
    
    // 检查是否有函数解析失败
    if (!fp_android_log_write || !fp_android_log_print || !fp_android_log_vprint) {
        printf("Some log functions failed to resolve, library may be incomplete");
    }
}

// 修改原有的 __android_log_write 函数实现
int __android_log_write(int prio, const char* tag, const char* text) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&isinitLogWrapper, &expected, 1)) {
        initLogWrapper();
    }
    
    if (fp_android_log_write) {
        return fp_android_log_write(prio, tag, text);
    }
    // Stub 实现
    return 0;
}

// 修改原有的 __android_log_print 函数实现
int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&isinitLogWrapper, &expected, 1)) {
        initLogWrapper();
    }
    
    if (fp_android_log_print) {
        va_list ap;
        va_start(ap, fmt);
        int result = fp_android_log_vprint(prio, tag, fmt, ap);
        va_end(ap);
        return result;
    }
    // Stub 实现
    return 0;
}

// 修改原有的 __android_log_vprint 函数实现
int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&isinitLogWrapper, &expected, 1)) {
        initLogWrapper();
    }
    
    if (fp_android_log_vprint) {
        return fp_android_log_vprint(prio, tag, fmt, ap);
    }
    // Stub 实现
    return 0;
}
