#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE moduleHandle, DWORD reason, LPVOID reserved)
{
    (void)moduleHandle;
    (void)reason;
    (void)reserved;
    return TRUE;
}
