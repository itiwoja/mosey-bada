/* Keep the vendor runtime in a private test directory on Fold5.
 * Redirect only the upstream hardcoded daemon-library path.
 * Optional diagnostic mode uses local system Binder stability per AOSP.
 * Always return the real service-manager result; no proxy or fake success. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <android/log.h>
typedef struct AIBinder AIBinder;
static void copy_log(int priority, const char *tag, const char *message) {
    const char *enabled = getenv("FOLD5_MOSEY_STDERR_LOG");
    if (enabled && !strcmp(enabled, "1")) {
        fprintf(stderr, "native_log[%d] %s: %s\n", priority,
                tag ? tag : "", message ? message : "");
        fflush(stderr);
    }
}
int __android_log_write(int priority, const char *tag, const char *message) {
    void *lib = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
    int (*write_log)(int, const char *, const char *) = lib ? dlsym(lib, "__android_log_write") : NULL;
    copy_log(priority, tag, message);
    int result = write_log ? write_log(priority, tag, message) : -1;
    if (lib) dlclose(lib);
    return result;
}
int __android_log_buf_write(int buffer, int priority, const char *tag, const char *message) {
    void *lib = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
    int (*write_log)(int, int, const char *, const char *) = lib ? dlsym(lib, "__android_log_buf_write") : NULL;
    copy_log(priority, tag, message);
    int result = write_log ? write_log(buffer, priority, tag, message) : -1;
    if (lib) dlclose(lib);
    return result;
}
static void *binder_symbol(const char *name) {
    void *library = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    void *symbol = library ? dlsym(library, name) : NULL;
    if (!symbol)
        __android_log_print(ANDROID_LOG_ERROR, "Fold5Mosey", "Resolve %s failed: %s", name, dlerror());
    return symbol;
}

static int prepare_service(AIBinder *binder, const char *name) {
    /* Normal integration uses the genuine VINTF declaration. Local stability
     * is an explicit diagnostic mode, never an automatic failure fallback. */
    const char *local = getenv("FOLD5_MOSEY_LOCAL_STABILITY");
    if (!local || strcmp(local, "1")) return 0;
    if (!name || strcmp(name, "com.google.android.moseyservice.IMoseyService/default"))
        return 0;
    void (*downgrade)(AIBinder *) = binder_symbol(
        "AIBinder_forceDowngradeToSystemStability");
    if (!downgrade) {
        __android_log_print(ANDROID_LOG_ERROR, "Fold5Mosey", "System stability API unavailable");
        return -1;
    }
    downgrade(binder);
    __android_log_print(ANDROID_LOG_INFO, "Fold5Mosey", "Using local system stability for %s", name);
    return 0;
}

int32_t AServiceManager_addService(AIBinder *binder, const char *name) {
    int32_t (*real_add)(AIBinder *, const char *) = binder_symbol(
        "AServiceManager_addService");
    if (!real_add || prepare_service(binder, name)) return -1;
    int32_t status = real_add(binder, name);
    __android_log_print(ANDROID_LOG_INFO, "Fold5Mosey", "addService(%s) result=%d", name, status);
    return status;
}

int32_t AServiceManager_addServiceWithFlags(AIBinder *binder, const char *name, int flags) {
    int32_t (*real_add)(AIBinder *, const char *, int) = binder_symbol(
        "AServiceManager_addServiceWithFlags");
    if (!real_add || prepare_service(binder, name)) return -1;
    int32_t status = real_add(binder, name, flags);
    __android_log_print(ANDROID_LOG_INFO, "Fold5Mosey", "addServiceWithFlags(%s) result=%d", name, status);
    return status;
}
void *dlopen(const char *path, int flags) {
    void *(*real_dlopen)(const char *, int) = dlsym(RTLD_NEXT, "dlopen");
    if (!real_dlopen) return 0;
    if (path && !strcmp(path, "/odm/lib64/libmosey_daemon_ffi.so"))
        path = "/data/local/tmp/fold5-mosey-native/libmosey_daemon_ffi.so";
    return real_dlopen(path, flags);
}
