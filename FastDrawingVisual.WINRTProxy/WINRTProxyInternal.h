#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.h>
#include <windows.ui.composition.desktop.h>
#include <winrt/base.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>

struct WinRTProxyState
{
    HWND hostWindow = nullptr;
    int width = 0;
    int height = 0;
    HRESULT lastErrorHr = S_OK;
    bool apartmentInitialized = false;
    PDISPATCHERQUEUECONTROLLER dispatcherQueueController = nullptr;
    winrt::Windows::UI::Composition::Compositor compositor{ nullptr };
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget desktopTarget{ nullptr };
    winrt::Windows::UI::Composition::ContainerVisual rootVisual{ nullptr };
    winrt::Windows::UI::Composition::SpriteVisual spriteVisual{ nullptr };
    winrt::Windows::UI::Composition::ICompositionSurface compositionSurface{ nullptr };
    winrt::Windows::UI::Composition::CompositionSurfaceBrush surfaceBrush{ nullptr };
};
