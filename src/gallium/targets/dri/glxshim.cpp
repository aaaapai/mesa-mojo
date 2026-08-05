#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdio.h>

extern "C" {
    typedef void (*__eglMustCastToProperFunctionPointerType)(void);
    typedef __eglMustCastToProperFunctionPointerType (*eglGetProcAddress_ptr_t)(const char *);
    typedef void* (*pojavGetEGLptr_t)(void);
}

struct context_t {
    context_t() {
        void* egl_handle = nullptr;
        eglGetProcAddress = nullptr;

        const char* egl_ptr_env = getenv("EGL_PTR");
        if (egl_ptr_env) {
            void* ptr = nullptr;
            if (sscanf(egl_ptr_env, "%p", &ptr) == 1) {
                egl_handle = ptr;
                printf("GLXShim: Retrieved EGL handle from EGL_PTR: %p\n", egl_handle);
                eglGetProcAddress = (eglGetProcAddress_ptr_t)dlsym(egl_handle, "eglGetProcAddress");
                if (eglGetProcAddress) {
                    printf("GLXShim: eglGetProcAddress found via EGL_PTR\n");
                } else {
                    printf("GLXShim: dlsym(eglGetProcAddress) failed on EGL_PTR handle: %s\n", dlerror());
                    egl_handle = nullptr;
                }
            } else {
                printf("GLXShim: Failed to parse EGL_PTR value: %s\n", egl_ptr_env);
            }
        }

        if (!egl_handle || !eglGetProcAddress) {
            void* pojavexec_handle = dlopen("libpojavexec.so", RTLD_NOLOAD);
            if (pojavexec_handle) {
                pojavGetEGLptr_t get_egl_ptr = (pojavGetEGLptr_t)dlsym(pojavexec_handle, "pojavGetEGLptr");
                if (get_egl_ptr) {
                    egl_handle = get_egl_ptr();
                    if (egl_handle) {
                        printf("GLXShim: Retrieved EGL handle from pojavexec: %p\n", egl_handle);
                        eglGetProcAddress = (eglGetProcAddress_ptr_t)dlsym(egl_handle, "eglGetProcAddress");
                        if (!eglGetProcAddress) {
                            printf("GLXShim: dlsym(eglGetProcAddress) failed on pojavexec handle: %s\n", dlerror());
                            egl_handle = nullptr;
                        }
                    } else {
                        printf("GLXShim: pojavGetEGLptr() returned NULL\n");
                    }
                } else {
                    printf("GLXShim: dlsym(pojavGetEGLptr) failed: %s\n", dlerror());
                }
            } else {
                printf("GLXShim: dlopen(libpojavexec.so, RTLD_NOLOAD) failed: %s\n", dlerror());
            }
        }

        if (!egl_handle || !eglGetProcAddress) {
            printf("GLXShim: Falling back to direct dlopen of libEGL_mesa.so\n");
            dl_handle = dlopen("libEGL_mesa.so", RTLD_NOLOAD);
            if (!dl_handle) {
                printf("GLXShim: libEGL_mesa.so not loaded, trying normal dlopen...\n");
                dl_handle = dlopen("libEGL_mesa.so", RTLD_LOCAL | RTLD_LAZY);
            }
            if (dl_handle) {
                eglGetProcAddress = (eglGetProcAddress_ptr_t)dlsym(dl_handle, "eglGetProcAddress");
                if (!eglGetProcAddress) {
                    printf("GLXShim: dlsym(eglGetProcAddress) on libEGL_mesa.so failed: %s\n", dlerror());
                    dlclose(dl_handle);
                    dl_handle = nullptr;
                } else {
                    printf("GLXShim: Successfully loaded eglGetProcAddress from libEGL_mesa.so\n");
                }
            } else {
                printf("GLXShim: dlopen(libEGL_mesa.so) failed: %s\n", dlerror());
            }
        } else {
            dl_handle = egl_handle;
        }

        if (!eglGetProcAddress) {
            printf("GLXShim: context init failed: could not obtain eglGetProcAddress\n");
        }
    }

    void* dl_handle = nullptr;
    eglGetProcAddress_ptr_t eglGetProcAddress = nullptr;
};

extern "C" {

__attribute__((visibility("default"))) void *glXGetProcAddress(const char *name) {
    static context_t ctx;
    if (!ctx.eglGetProcAddress) {
        printf("GLXShim: eglGetProcAddress not available, returning NULL\n");
        return nullptr;
    }
    void* pfunc = (void*)ctx.eglGetProcAddress(name);
    return pfunc;
}

__attribute__((visibility("default"))) void *glXGetProcAddressARB(const char *name) {
    return glXGetProcAddress(name);
}

} // extern "C"
