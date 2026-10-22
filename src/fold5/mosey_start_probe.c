/* One-shot genuine Binder client using upstream parcel definitions.
 * No TCP listener, no feature-flag changes and no forged success responses. */
#define main upstream_bridge_entry
#define BRIDGE_LOG_PATH "/data/local/tmp/fold5-mosey-native/start-probe.log"
#define BRIDGE_LOG_OLD_PATH "/data/local/tmp/fold5-mosey-native/start-probe.log.1"
#include "../bridge/mosey_bridge.c"
#undef main
int main(int argc, char **argv) {
    if(argc!=2 || (strcmp(argv[1],"--version") && strcmp(argv[1],"--start-channel-6"))) {
        fprintf(stderr,"Usage: %s --version|--start-channel-6\n",argv[0]);return 2;
    }
    setenv("MOSEY_BACKEND","binder",1);
    if(!init_backend()){puts("binder_backend=unavailable");return 3;}
    printf("binder_backend=connected\ninterface_version=%d\n",backend_call(CMD_GET_VERSION,NULL,0));
    if(!strcmp(argv[1],"--version"))return 0;
    uint8_t channels[5]={1,6,0,0,0};
    int started=backend_call(CMD_START,channels,sizeof(channels));
    printf("start_result=%d\n",started);
    int stopped=backend_call(CMD_STOP,NULL,0);
    printf("stop_result=%d\n",stopped);
    return started==0 && stopped==0?0:4;
}
