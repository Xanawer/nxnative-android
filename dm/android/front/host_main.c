// The host's main.c for libdmhost.so: its main becomes dm_main, which front_android.c runs on the host thread (the iOS app compiles main.c with -Dmain=dm_main the same way, dm/ios/project.yml)
#define main dm_main
#include "../../host/main.c"
