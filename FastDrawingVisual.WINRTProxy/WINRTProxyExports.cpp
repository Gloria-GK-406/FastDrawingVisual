#include "WINRTProxyExports.h"
#include "WINRTProxyInternal.h"

#include <algorithm>
#include <new>

namespace
{
    template<typename TInterface>
    winrt::com_ptr<TInterface> QueryCompositorInterface(
        winrt::Windows::UI::Composition::Compositor const& compositor)
    {
        winrt::com_ptr<TInterface> result;
        auto* unknown = reinterpret_cast<IUnknown*>(winrt::get_abi(compositor));
        winrt::check_hresult(unknown->QueryInterface(__uuidof(TInterface), result.put_void()));
        return result;
    }

    void SetLastError(WinRTProxyState* state, HRESULT hr)
    {
        if (state != nullptr)
            state->lastErrorHr = hr;
    }

    void ReleaseDispatcherQueue(WinRTProxyState* state)
    {
        if (state == nullptr)
            return;

        if (state->dispatcherQueueController != nullptr)
        {
            state->dispatcherQueueController->Release();
            state->dispatcherQueueController = nullptr;
        }
    }

    void ResetCompositionTree(WinRTProxyState* state)
    {
        if (state == nullptr)
            return;

        if (state->desktopTarget)
            state->desktopTarget.Root(nullptr);

        state->surfaceBrush = nullptr;
        state->compositionSurface = nullptr;
        state->spriteVisual = nullptr;
        state->rootVisual = nullptr;
        state->desktopTarget = nullptr;
    }

    bool EnsureDispatcherQueueInternal(WinRTProxyState* state)
    {
        if (state == nullptr)
            return false;

        if (state->dispatcherQueueController != nullptr)
        {
            SetLastError(state, S_OK);
            return true;
        }

        DispatcherQueueOptions options = {};
        options.dwSize = sizeof(options);
        options.threadType = DQTYPE_THREAD_CURRENT;
        options.apartmentType = DQTAT_COM_STA;

        PDISPATCHERQUEUECONTROLLER controller = nullptr;
        const HRESULT hr = CreateDispatcherQueueController(options, &controller);
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        {
            SetLastError(state, S_OK);
            return true;
        }

        if (FAILED(hr) || controller == nullptr)
        {
            SetLastError(state, FAILED(hr) ? hr : E_FAIL);
            return false;
        }

        state->dispatcherQueueController = controller;
        SetLastError(state, S_OK);
        return true;
    }

    bool EnsureDesktopTargetInternal(WinRTProxyState* state, HWND hwnd, bool isTopmost)
    {
        if (state == nullptr)
            return false;

        if (hwnd == nullptr || !IsWindow(hwnd))
        {
            SetLastError(state, E_INVALIDARG);
            return false;
        }

        if (!EnsureDispatcherQueueInternal(state))
            return false;

        try
        {
            if (!state->compositor)
                state->compositor = winrt::Windows::UI::Composition::Compositor();

            const bool needsNewTarget = !state->desktopTarget || state->hostWindow != hwnd;
            state->hostWindow = hwnd;
            if (needsNewTarget)
            {
                ResetCompositionTree(state);

                auto desktopInterop = QueryCompositorInterface<
                    ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>(state->compositor);
                ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget* targetAbi = nullptr;
                winrt::check_hresult(desktopInterop->CreateDesktopWindowTarget(
                    hwnd,
                    isTopmost ? TRUE : FALSE,
                    &targetAbi));

                state->desktopTarget = { targetAbi, winrt::take_ownership_from_abi };
                state->rootVisual = state->compositor.CreateContainerVisual();
                state->spriteVisual = state->compositor.CreateSpriteVisual();
                state->rootVisual.Children().InsertAtTop(state->spriteVisual);
                state->desktopTarget.Root(state->rootVisual);
            }

            SetLastError(state, S_OK);
            return true;
        }
        catch (winrt::hresult_error const& ex)
        {
            SetLastError(state, static_cast<HRESULT>(ex.code()));
            return false;
        }
    }

    bool BindSwapChainInternal(WinRTProxyState* state, IUnknown* swapChain)
    {
        if (state == nullptr || swapChain == nullptr)
            return false;

        if (!state->compositor || !state->spriteVisual)
        {
            SetLastError(state, E_UNEXPECTED);
            return false;
        }

        try
        {
            auto compositorInterop =
                QueryCompositorInterface<ABI::Windows::UI::Composition::ICompositorInterop>(state->compositor);
            ABI::Windows::UI::Composition::ICompositionSurface* surfaceAbi = nullptr;
            winrt::check_hresult(compositorInterop->CreateCompositionSurfaceForSwapChain(
                swapChain,
                &surfaceAbi));

            state->compositionSurface = { surfaceAbi, winrt::take_ownership_from_abi };
            state->surfaceBrush = state->compositor.CreateSurfaceBrush(state->compositionSurface);
            state->spriteVisual.Brush(state->surfaceBrush);

            SetLastError(state, S_OK);
            return true;
        }
        catch (winrt::hresult_error const& ex)
        {
            SetLastError(state, static_cast<HRESULT>(ex.code()));
            return false;
        }
    }

    bool UpdateSpriteRectInternal(WinRTProxyState* state, float offsetX, float offsetY, float width, float height)
    {
        if (state == nullptr)
            return false;

        if (!state->spriteVisual)
        {
            SetLastError(state, E_UNEXPECTED);
            return false;
        }

        if (width <= 0.0f || height <= 0.0f)
        {
            SetLastError(state, E_INVALIDARG);
            return false;
        }

        const float safeWidth = (std::max)(1.0f, width);
        const float safeHeight = (std::max)(1.0f, height);
        state->spriteVisual.Offset({ offsetX, offsetY, 0.0f });
        state->spriteVisual.Size({ safeWidth, safeHeight });
        state->width = static_cast<int>(safeWidth);
        state->height = static_cast<int>(safeHeight);
        SetLastError(state, S_OK);
        return true;
    }
}

extern "C"
{
    __declspec(dllexport) bool __cdecl FDV_WinRTProxy_IsReady()
    {
        return true;
    }

    __declspec(dllexport) void* __cdecl FDV_WinRTProxy_Create()
    {
        auto* state = new (std::nothrow) WinRTProxyState();
        if (state == nullptr)
            return nullptr;

        state->hostWindow = nullptr;
        state->width = 0;
        state->height = 0;
        state->lastErrorHr = S_OK;
        state->apartmentInitialized = false;
        state->dispatcherQueueController = nullptr;
        state->compositor = nullptr;
        state->desktopTarget = nullptr;
        state->rootVisual = nullptr;
        state->spriteVisual = nullptr;
        state->compositionSurface = nullptr;
        state->surfaceBrush = nullptr;

        try
        {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            state->apartmentInitialized = true;
            state->lastErrorHr = S_OK;
        }
        catch (winrt::hresult_error const& ex)
        {
            if (ex.code() == RPC_E_CHANGED_MODE)
                state->lastErrorHr = S_OK;
            else
                state->lastErrorHr = static_cast<HRESULT>(ex.code());
        }

        return state;
    }

    __declspec(dllexport) void __cdecl FDV_WinRTProxy_Destroy(void* proxy)
    {
        auto* state = static_cast<WinRTProxyState*>(proxy);
        if (state == nullptr)
            return;

        ResetCompositionTree(state);
        state->compositor = nullptr;
        ReleaseDispatcherQueue(state);

        if (state->apartmentInitialized)
        {
            winrt::uninit_apartment();
            state->apartmentInitialized = false;
        }

        delete state;
    }

    __declspec(dllexport) bool __cdecl FDV_WinRTProxy_SetHostWindow(void* proxy, void* hwnd)
    {
        auto* state = static_cast<WinRTProxyState*>(proxy);
        if (state == nullptr)
            return false;

        if (hwnd == nullptr)
        {
            SetLastError(state, E_POINTER);
            return false;
        }

        auto hostWindow = static_cast<HWND>(hwnd);
        if (!IsWindow(hostWindow))
        {
            SetLastError(state, E_INVALIDARG);
            return false;
        }

        state->hostWindow = hostWindow;
        SetLastError(state, S_OK);
        return true;
    }

    __declspec(dllexport) bool __cdecl FDV_WinRTProxy_Resize(void* proxy, int width, int height)
    {
        auto* state = static_cast<WinRTProxyState*>(proxy);
        if (state == nullptr)
            return false;

        if (width <= 0 || height <= 0)
        {
            SetLastError(state, E_INVALIDARG);
            return false;
        }

        state->width = width;
        state->height = height;
        SetLastError(state, S_OK);
        return true;
    }

    __declspec(dllexport) bool __cdecl FDV_WinRTProxy_EnsureDispatcherQueue(void* proxy)
    {
        auto* state = static_cast<WinRTProxyState*>(proxy);
        if (state == nullptr)
            return false;

        return EnsureDispatcherQueueInternal(state);
    }

    __declspec(dllexport) bool __cdecl FDV_WinRTProxy_EnsureDesktopTarget(
        void* proxy,
        void* hwnd,
        bool isTopmost)
    {
        auto* state = static_cast<WinRTProxyState*>(proxy);
        if (state == nullptr)
            return false;

        auto hostWindow = static_cast<HWND>(hwnd);
        return EnsureDesktopTargetInternal(state, hostWindow, isTopmost);
    }

    __declspec(dllexport) bool __cdecl FDV_WinRTProxy_BindSwapChain(
        void* proxy,
        void* swapChain)
    {
        auto* state = static_cast<WinRTProxyState*>(proxy);
        if (state == nullptr)
            return false;

        if (swapChain == nullptr)
        {
            SetLastError(state, E_POINTER);
            return false;
        }

        return BindSwapChainInternal(state, static_cast<IUnknown*>(swapChain));
    }

    __declspec(dllexport) bool __cdecl FDV_WinRTProxy_UpdateSpriteRect(
        void* proxy,
        float offsetX,
        float offsetY,
        float width,
        float height)
    {
        auto* state = static_cast<WinRTProxyState*>(proxy);
        if (state == nullptr)
            return false;

        return UpdateSpriteRectInternal(state, offsetX, offsetY, width, height);
    }

    __declspec(dllexport) int32_t __cdecl FDV_WinRTProxy_GetLastErrorHr(void* proxy)
    {
        auto* state = static_cast<WinRTProxyState*>(proxy);
        if (state == nullptr)
            return static_cast<int32_t>(E_POINTER);

        return static_cast<int32_t>(state->lastErrorHr);
    }
}
