
#include <switch.h>

void setup_nxlink(void)
{
    socketInitializeDefault();
    nxlinkStdio();
    romfsInit();
}

void cleanup_nxlink(void)
{
    romfsExit();
    socketExit();
}
