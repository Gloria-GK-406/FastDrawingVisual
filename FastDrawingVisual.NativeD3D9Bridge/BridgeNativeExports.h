#pragma once

#if defined(_MANAGED)
#define FDV_NATIVE_REGION_BEGIN __pragma(managed(push, off))
#define FDV_NATIVE_REGION_END __pragma(managed(pop))
#else
#define FDV_NATIVE_REGION_BEGIN
#define FDV_NATIVE_REGION_END
#endif

extern "C" {
__declspec(dllexport) bool __cdecl FDV_IsBridgeReady();
__declspec(dllexport) void *__cdecl FDV_CreateRenderer(void *hwnd, int width,
                                                       int height);
__declspec(dllexport) void __cdecl FDV_DestroyRenderer(void *renderer);
__declspec(dllexport) bool __cdecl FDV_Resize(void *renderer, int width,
                                              int height);
__declspec(dllexport) bool __cdecl FDV_SubmitCommands(void *renderer,
                                                      const void *commands,
                                                      int commandBytes);
__declspec(dllexport) bool __cdecl FDV_TryAcquirePresentSurface(
    void *renderer, void **outSurface9);
__declspec(dllexport) void __cdecl FDV_OnSurfacePresented(void *renderer);
__declspec(dllexport) void __cdecl FDV_OnFrontBufferAvailable(void *renderer,
                                                              bool available);
}
