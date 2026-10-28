// Engine entry point shim.
//
// The Lead's engine patch provides a strong ps2x_android_game_main(); the weak
// definition below keeps the APK linkable (and the host debuggable) until that
// patch lands.  Defined in its own translation unit so the weak symbol never
// competes with the real one inside the same object file.
#include "ps2x_host.h"

extern "C" int ps2x_android_game_main(void) __attribute__((weak));

extern "C" int ps2x_android_game_main(void)
{
    ps2x::host::Log("E/host", "ps2x_android_game_main() is the weak fallback: the engine entry patch is missing");
    return 0;
}
