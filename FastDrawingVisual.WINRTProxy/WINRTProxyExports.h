#pragma once

#include <stdint.h>

extern "C" {
__declspec(dllexport) bool __cdecl FDV_WinRTProxy_IsReady();
__declspec(dllexport) void* __cdecl FDV_WinRTProxy_Create();
__declspec(dllexport) void __cdecl FDV_WinRTProxy_Destroy(void* proxy);
__declspec(dllexport) bool __cdecl FDV_WinRTProxy_SetHostWindow(void* proxy, void* hwnd);
__declspec(dllexport) bool __cdecl FDV_WinRTProxy_Resize(void* proxy, int width, int height);
__declspec(dllexport) bool __cdecl FDV_WinRTProxy_EnsureDispatcherQueue(void* proxy);
__declspec(dllexport) bool __cdecl FDV_WinRTProxy_EnsureDesktopTarget(
    void* proxy,
    void* hwnd,
    bool isTopmost);
__declspec(dllexport) bool __cdecl FDV_WinRTProxy_BindSwapChain(
    void* proxy,
    void* swapChain);
__declspec(dllexport) bool __cdecl FDV_WinRTProxy_UpdateSpriteRect(
    void* proxy,
    float offsetX,
    float offsetY,
    float width,
    float height);
__declspec(dllexport) int32_t __cdecl FDV_WinRTProxy_GetLastErrorHr(void* proxy);
}
