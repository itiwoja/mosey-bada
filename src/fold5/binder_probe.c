/* Query the real service manager through platform libbinder_ndk APIs.
 * Does not register/start any service or invoke application transactions. */
#include <stdbool.h>
#include <dlfcn.h>
#include <stdio.h>
typedef struct AIBinder AIBinder;
int main(void) {
    const char *name="com.google.android.moseyservice.IMoseyService/default";
    void *lib=dlopen("libbinder_ndk.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib){fprintf(stderr,"%s\n",dlerror());return 2;}
    bool (*declared)(const char *)=dlsym(lib,"AServiceManager_isDeclared");
    AIBinder *(*check)(const char *)=dlsym(lib,"AServiceManager_checkService");
    void (*release)(AIBinder *)=dlsym(lib,"AIBinder_decStrong");
    if(!declared || !check || !release)return 2;
    printf("declared=%d\n",declared(name));
    AIBinder *b=check(name);
    printf("registered=%d\n",b!=NULL);
    if(b)release(b);
    dlclose(lib);return 0;
}
