// ==WindhawkMod==
// @id              taskbar-liquid-glass
// @name            Taskbar Liquid Glass
// @description     Real refracting liquid glass backdrop behind the Windows 11 taskbar.
// @version         0.4.0
// @author          AlexVishnevsky
// @github          https://github.com/AlexVishnevsky
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lwindowsapp -luuid -luser32 -lgdi32 -ldwmapi -ld3d11 -ld2d1 -ldcomp -limm32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Liquid Glass

Puts a real lens behind the Windows 11 taskbar: the screen content under the
taskbar is captured, blurred and bent along the edge of the bar with a
displacement map, the same way the Liquid Glass material of
[Island Media Controls](https://github.com/usho-lear/island-media-controls)
(MIT) renders its popup.

The glass is drawn in a separate click-through window placed right below the
taskbar, so the taskbar background itself has to be transparent. With
**Windows 11 Taskbar Styler** set the fill of
`Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill`
to `Transparent`.

## Notes

- The glass window is always hidden from screenshots and screen recording,
  otherwise it would capture itself.
- With **Exclude taskbar from capture** on (default) the taskbar is hidden
  from screenshots and screen sharing as well. Turning it off keeps the
  taskbar in screenshots, but its own icons then show up blurred inside the
  glass.
- Only the primary taskbar is handled.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- refraction: 45
  $name: Сила заломлення (px)
  $description: Наскільки зміщується зображення на самому краю скла. Край, за яким є екран, затягує в себе те, що поруч; край, що впирається в межу екрана, заломлює досередини. Від'ємне значення міняє напрямки місцями. 0 вимикає заломлення.
- bezelWidth: 18
  $name: Ширина лінзи по краю (px)
  $description: Глибина від краю, на якій діє заломлення.
- blur: 4
  $name: Розмиття (px)
- cornerRadius: 6
  $name: Радіус кутів (px)
- tintColor: "#1A090909"
  $name: Тонування (#AARRGGBB)
- rimHighlight: 0
  $name: Світлий обідок (%)
  $description: Додатковий відблиск по краю. 0, якщо обводку вже малює тема панелі завдань.
- marginLeft: 0
  $name: Відступ зліва (px)
- marginTop: 0
  $name: Відступ зверху (px)
- marginRight: 0
  $name: Відступ справа (px)
- marginBottom: 0
  $name: Відступ знизу (px)
- excludeTaskbarFromCapture: true
  $name: Ховати панель завдань від захоплення
  $description: Чисте скло без відбиття власних значків, але панелі завдань не буде на скриншотах і в демонстрації екрана.
- debugDump: false
  $name: Зберегти кадр скла у %TEMP%\taskbar-liquid-glass.bmp
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <imm.h>

#include <inspectable.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dcomp.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
namespace direct3d11 = winrt::Windows::Graphics::DirectX::Direct3D11;

// Windhawk's Clang/MinGW SDK lacks the WinRT interop headers and ignores
// __declspec(uuid), so the interfaces are declared here with explicit IIDs.
static constexpr GUID kIID_CaptureItemInterop{
    0x3628E81B, 0x3CAC, 0x4C60,
    {0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56}};
static constexpr GUID kIID_DxgiInterfaceAccess{
    0xA9B3D012, 0x3DF2, 0x4EE3,
    {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}};
static constexpr GUID kIID_GraphicsCaptureItem{
    0x79C3F95B, 0x31F7, 0x4EC2,
    {0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60}};

struct ICaptureItemInterop : ::IUnknown {
    virtual HRESULT __stdcall CreateForWindow(HWND window,
                                              REFIID riid,
                                              void** result) = 0;
    virtual HRESULT __stdcall CreateForMonitor(HMONITOR monitor,
                                               REFIID riid,
                                               void** result) = 0;
};

struct IDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID iid, void** p) = 0;
};

extern "C" HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(
    IDXGIDevice* dxgiDevice,
    IInspectable** graphicsDevice);

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static const wchar_t kWindowClass[] = L"WindhawkTaskbarLiquidGlass";
static constexpr UINT_PTR kTimerId = 1;
static constexpr UINT kTimerIntervalMs = 100;

struct Settings {
    int refraction = 45;
    int bezelWidth = 18;
    int blur = 4;
    int cornerRadius = 6;
    int marginLeft = 0;
    int marginTop = 0;
    int marginRight = 0;
    int marginBottom = 0;
    int rimHighlight = 0;
    UINT32 tintArgb = 0x1A090909;
    bool excludeTaskbarFromCapture = true;
};

// Everything the renderer needs to know about where and how to draw. All
// rectangles are in screen pixels.
struct Layout {
    RECT glass{};
    RECT monitor{};
    RECT region{};  // Part of the monitor that is copied out of each frame.
    int radius = 0;
    int bezel = 0;
    int refraction = 0;
    float blur = 0.0f;
    float rim = 0.0f;
    UINT32 tintArgb = 0;

    bool operator==(Layout const& other) const {
        return EqualRect(&glass, &other.glass) &&
               EqualRect(&monitor, &other.monitor) &&
               EqualRect(&region, &other.region) && radius == other.radius &&
               bezel == other.bezel && refraction == other.refraction &&
               blur == other.blur && rim == other.rim &&
               tintArgb == other.tintArgb;
    }
};

std::mutex g_settingsMutex;
Settings g_settings;

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_needRestart{false};
std::atomic<bool> g_dumpRequested{false};
std::atomic<int> g_callbacksInFlight{0};
HWND g_hwnd;
HWND g_affinityTaskbar;

// Guards all the rendering state below.
std::mutex g_mutex;
winrt::com_ptr<ID3D11Device> g_d3dDevice;
winrt::com_ptr<ID3D11DeviceContext> g_d3dContext;
winrt::com_ptr<IDXGIDevice> g_dxgiDevice;
winrt::com_ptr<ID2D1Factory1> g_d2dFactory;
winrt::com_ptr<ID2D1Device> g_d2dDevice;
winrt::com_ptr<ID2D1DeviceContext> g_d2dContext;
direct3d11::IDirect3DDevice g_captureDevice{nullptr};
winrt::com_ptr<IDCompositionDevice> g_dcompDevice;
winrt::com_ptr<IDCompositionTarget> g_dcompTarget;
winrt::com_ptr<IDCompositionVisual> g_dcompVisual;
winrt::com_ptr<IDXGISwapChain1> g_swapChain;
winrt::com_ptr<ID2D1Bitmap1> g_targetBitmap;
int g_targetWidth;
int g_targetHeight;
winrt::com_ptr<ID3D11Texture2D> g_regionTexture;
winrt::com_ptr<ID2D1Bitmap1> g_regionBitmap;
int g_regionWidth;
int g_regionHeight;
bool g_regionValid;
winrt::com_ptr<ID2D1Bitmap1> g_lensMap;
winrt::com_ptr<ID2D1RoundedRectangleGeometry> g_clipGeometry;
winrt::com_ptr<ID2D1LinearGradientBrush> g_rimBrush;
winrt::com_ptr<ID2D1SolidColorBrush> g_tintBrush;
bool g_shapeResourcesValid;
winrt::com_ptr<ID2D1Effect> g_borderEffect;
winrt::com_ptr<ID2D1Effect> g_blurEffect;
winrt::com_ptr<ID2D1Effect> g_displacementEffect;
capture::GraphicsCaptureItem g_captureItem{nullptr};
capture::Direct3D11CaptureFramePool g_framePool{nullptr};
capture::GraphicsCaptureSession g_captureSession{nullptr};
winrt::event_token g_frameArrivedToken;
bool g_capturing;
bool g_dirtyRegionsEnabled;
HMONITOR g_captureMonitor;
Layout g_layout;
bool g_layoutValid;

Settings GetSettings() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings;
}

UINT32 ParseColor(PCWSTR text, UINT32 fallback) {
    if (!text) {
        return fallback;
    }
    while (*text == L' ' || *text == L'#') {
        ++text;
    }
    size_t length = wcslen(text);
    wchar_t* end = nullptr;
    unsigned long value = wcstoul(text, &end, 16);
    if (end == text) {
        return fallback;
    }
    if (length <= 6) {
        value |= 0xFF000000;
    }
    return static_cast<UINT32>(value);
}

void LoadSettings() {
    Settings settings;
    settings.refraction = std::clamp(Wh_GetIntSetting(L"refraction"), -300, 300);
    settings.bezelWidth = std::clamp(Wh_GetIntSetting(L"bezelWidth"), 1, 200);
    settings.blur = std::clamp(Wh_GetIntSetting(L"blur"), 0, 60);
    settings.cornerRadius =
        std::clamp(Wh_GetIntSetting(L"cornerRadius"), 0, 200);
    settings.marginLeft = Wh_GetIntSetting(L"marginLeft");
    settings.marginTop = Wh_GetIntSetting(L"marginTop");
    settings.marginRight = Wh_GetIntSetting(L"marginRight");
    settings.marginBottom = Wh_GetIntSetting(L"marginBottom");
    settings.rimHighlight =
        std::clamp(Wh_GetIntSetting(L"rimHighlight"), 0, 100);
    settings.excludeTaskbarFromCapture =
        Wh_GetIntSetting(L"excludeTaskbarFromCapture") != 0;

    PCWSTR tint = Wh_GetStringSetting(L"tintColor");
    settings.tintArgb = ParseColor(tint, 0x1A090909);
    Wh_FreeStringSetting(tint);

    if (Wh_GetIntSetting(L"debugDump")) {
        g_dumpRequested = true;
    }

    std::lock_guard lock(g_settingsMutex);
    g_settings = settings;
}

D2D1_COLOR_F ColorFromArgb(UINT32 argb) {
    return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f,
                        ((argb >> 8) & 0xFF) / 255.0f,
                        (argb & 0xFF) / 255.0f,
                        ((argb >> 24) & 0xFF) / 255.0f);
}

HWND FindTaskbarInThisProcess() {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) {
        return nullptr;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(taskbar, &processId);
    return processId == GetCurrentProcessId() ? taskbar : nullptr;
}

void UpdateTaskbarAffinity(HWND taskbar, bool exclude) {
    HWND target = exclude ? taskbar : nullptr;
    if (target == g_affinityTaskbar) {
        return;
    }
    if (g_affinityTaskbar && IsWindow(g_affinityTaskbar)) {
        SetWindowDisplayAffinity(g_affinityTaskbar, WDA_NONE);
    }
    g_affinityTaskbar = nullptr;
    if (target) {
        if (SetWindowDisplayAffinity(target, WDA_EXCLUDEFROMCAPTURE)) {
            g_affinityTaskbar = target;
        } else {
            Wh_Log(L"taskbar SetWindowDisplayAffinity failed error=%lu",
                   GetLastError());
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering. Every *Locked function expects g_mutex to be held.

bool CreateDevicesLocked() {
    if (g_d3dDevice) {
        return true;
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   featureLevels, ARRAYSIZE(featureLevels),
                                   D3D11_SDK_VERSION, g_d3dDevice.put(),
                                   nullptr, g_d3dContext.put());
    if (FAILED(hr)) {
        Wh_Log(L"D3D11CreateDevice failed hr=0x%08X",
               static_cast<unsigned>(hr));
        return false;
    }

    try {
        g_dxgiDevice = g_d3dDevice.as<IDXGIDevice>();

        D2D1_FACTORY_OPTIONS factoryOptions{};
        winrt::check_hresult(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1),
            &factoryOptions,
            reinterpret_cast<void**>(g_d2dFactory.put())));
        winrt::check_hresult(
            g_d2dFactory->CreateDevice(g_dxgiDevice.get(), g_d2dDevice.put()));
        winrt::check_hresult(g_d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, g_d2dContext.put()));

        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            g_dxgiDevice.get(), inspectable.put()));
        g_captureDevice = inspectable.as<direct3d11::IDirect3DDevice>();

        winrt::check_hresult(DCompositionCreateDevice(
            g_dxgiDevice.get(), __uuidof(IDCompositionDevice),
            g_dcompDevice.put_void()));
        winrt::check_hresult(g_dcompDevice->CreateTargetForHwnd(
            g_hwnd, TRUE, g_dcompTarget.put()));
        winrt::check_hresult(g_dcompDevice->CreateVisual(g_dcompVisual.put()));
        winrt::check_hresult(g_dcompTarget->SetRoot(g_dcompVisual.get()));

        // Extends the copied region by repeating its edge pixels, so a strong
        // refraction that reaches past the screen edge still finds a colour
        // there instead of transparent black.
        winrt::check_hresult(
            g_d2dContext->CreateEffect(CLSID_D2D1Border, g_borderEffect.put()));
        g_borderEffect->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X,
                                 D2D1_BORDER_EDGE_MODE_CLAMP);
        g_borderEffect->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y,
                                 D2D1_BORDER_EDGE_MODE_CLAMP);
        winrt::check_hresult(g_d2dContext->CreateEffect(
            CLSID_D2D1GaussianBlur, g_blurEffect.put()));
        winrt::check_hresult(g_d2dContext->CreateEffect(
            CLSID_D2D1DisplacementMap, g_displacementEffect.put()));
        g_displacementEffect->SetValue(
            D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT,
            D2D1_CHANNEL_SELECTOR_R);
        g_displacementEffect->SetValue(
            D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT,
            D2D1_CHANNEL_SELECTOR_G);
    } catch (winrt::hresult_error const& error) {
        Wh_Log(L"device setup failed hr=0x%08X",
               static_cast<unsigned>(error.code().value));
        return false;
    }

    return true;
}

void ReleaseDevicesLocked() {
    g_layoutValid = false;
    g_shapeResourcesValid = false;
    g_regionValid = false;
    g_rimBrush = nullptr;
    g_tintBrush = nullptr;
    g_clipGeometry = nullptr;
    g_lensMap = nullptr;
    g_borderEffect = nullptr;
    g_blurEffect = nullptr;
    g_displacementEffect = nullptr;
    g_regionBitmap = nullptr;
    g_regionTexture = nullptr;
    g_regionWidth = g_regionHeight = 0;
    if (g_d2dContext) {
        g_d2dContext->SetTarget(nullptr);
    }
    g_targetBitmap = nullptr;
    g_swapChain = nullptr;
    g_targetWidth = g_targetHeight = 0;
    if (g_dcompTarget) {
        g_dcompTarget->SetRoot(nullptr);
    }
    if (g_dcompDevice) {
        g_dcompDevice->Commit();
    }
    g_dcompVisual = nullptr;
    g_dcompTarget = nullptr;
    g_dcompDevice = nullptr;
    g_captureDevice = nullptr;
    g_d2dContext = nullptr;
    g_d2dDevice = nullptr;
    g_d2dFactory = nullptr;
    g_dxgiDevice = nullptr;
    g_d3dContext = nullptr;
    g_d3dDevice = nullptr;
}

bool EnsureSwapChainLocked(int width, int height) {
    if (g_swapChain && g_targetBitmap && g_targetWidth == width &&
        g_targetHeight == height) {
        return true;
    }

    g_d2dContext->SetTarget(nullptr);
    g_targetBitmap = nullptr;

    try {
        if (g_swapChain) {
            winrt::check_hresult(g_swapChain->ResizeBuffers(
                2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
        } else {
            winrt::com_ptr<IDXGIAdapter> adapter;
            winrt::check_hresult(g_dxgiDevice->GetAdapter(adapter.put()));
            winrt::com_ptr<IDXGIFactory2> factory;
            winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2),
                                                    factory.put_void()));

            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = width;
            desc.Height = height;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            winrt::check_hresult(factory->CreateSwapChainForComposition(
                g_d3dDevice.get(), &desc, nullptr, g_swapChain.put()));
            winrt::check_hresult(
                g_dcompVisual->SetContent(g_swapChain.get()));
            winrt::check_hresult(g_dcompDevice->Commit());
        }

        winrt::com_ptr<IDXGISurface> backBuffer;
        winrt::check_hresult(g_swapChain->GetBuffer(
            0, __uuidof(IDXGISurface), backBuffer.put_void()));
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(g_d2dContext->CreateBitmapFromDxgiSurface(
            backBuffer.get(), &props, g_targetBitmap.put()));
    } catch (winrt::hresult_error const& error) {
        Wh_Log(L"swap chain %dx%d failed hr=0x%08X", width, height,
               static_cast<unsigned>(error.code().value));
        g_targetBitmap = nullptr;
        g_swapChain = nullptr;
        g_needRestart = true;
        return false;
    }

    g_targetWidth = width;
    g_targetHeight = height;
    return true;
}

bool EnsureRegionLocked(int width, int height) {
    if (g_regionTexture && g_regionBitmap && g_regionWidth == width &&
        g_regionHeight == height) {
        return true;
    }

    g_regionBitmap = nullptr;
    g_regionTexture = nullptr;
    g_regionValid = false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    try {
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(
            &desc, nullptr, g_regionTexture.put()));
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE));
        winrt::check_hresult(g_d2dContext->CreateBitmapFromDxgiSurface(
            g_regionTexture.as<IDXGISurface>().get(), &props,
            g_regionBitmap.put()));
    } catch (winrt::hresult_error const& error) {
        Wh_Log(L"region texture %dx%d failed hr=0x%08X", width, height,
               static_cast<unsigned>(error.code().value));
        g_regionBitmap = nullptr;
        g_regionTexture = nullptr;
        return false;
    }

    g_regionWidth = width;
    g_regionHeight = height;
    return true;
}

// Builds the RG displacement texture for the lens. It covers the whole copied
// region and is neutral (128) everywhere except a band along the inside of
// the glass edge, where it points along the edge normal with a quadratic
// profile: strongest right at the edge, fading to nothing at the bezel depth.
bool BuildLensMapLocked() {
    const Layout& layout = g_layout;
    int width = g_regionWidth;
    int height = g_regionHeight;
    double glassWidth = layout.glass.right - layout.glass.left;
    double glassHeight = layout.glass.bottom - layout.glass.top;
    double originX = layout.glass.left - layout.region.left;
    double originY = layout.glass.top - layout.region.top;
    double centerX = originX + glassWidth * 0.5;
    double centerY = originY + glassHeight * 0.5;
    double halfMin = std::min(glassWidth, glassHeight) * 0.5;
    double bezel = std::clamp(static_cast<double>(layout.bezel), 1.0, halfMin);
    // The refraction field uses a corner radius of at least the bezel depth.
    // With a smaller one the edge normal flips abruptly along the corner
    // diagonals and shows up as a mitre crease in the glass.
    double fieldRadius =
        std::clamp(std::max(static_cast<double>(layout.radius), bezel), 0.0,
                   halfMin);
    double innerHalfWidth = glassWidth * 0.5 - fieldRadius;
    double innerHalfHeight = glassHeight * 0.5 - fieldRadius;
    // An edge with screen content next to it pulls that content into the
    // rim. An edge flush with the monitor border has nothing beyond it, so it
    // bends the content from inside the glass instead.
    constexpr int kOpenEdgeGap = 8;
    double sign = layout.refraction >= 0 ? 1.0 : -1.0;
    auto edgeDirection = [sign](int gap) {
        return gap >= kOpenEdgeGap ? sign : -sign;
    };
    double directionLeft = edgeDirection(layout.glass.left - layout.monitor.left);
    double directionTop = edgeDirection(layout.glass.top - layout.monitor.top);
    double directionRight =
        edgeDirection(layout.monitor.right - layout.glass.right);
    double directionBottom =
        edgeDirection(layout.monitor.bottom - layout.glass.bottom);

    auto signedDistance = [&](double x, double y) {
        double qx = std::abs(x - centerX) - innerHalfWidth;
        double qy = std::abs(y - centerY) - innerHalfHeight;
        double outsideX = std::max(qx, 0.0);
        double outsideY = std::max(qy, 0.0);
        return std::sqrt(outsideX * outsideX + outsideY * outsideY) +
               std::min(std::max(qx, qy), 0.0) - fieldRadius;
    };

    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        double fy = y + 0.5;
        for (int x = 0; x < width; ++x) {
            double fx = x + 0.5;
            double offsetX = 0.0;
            double offsetY = 0.0;

            double depth = -signedDistance(fx, fy);
            if (depth < bezel) {
                double gradX = signedDistance(fx + 0.75, fy) -
                               signedDistance(fx - 0.75, fy);
                double gradY = signedDistance(fx, fy + 0.75) -
                               signedDistance(fx, fy - 0.75);
                double gradLength = std::sqrt(gradX * gradX + gradY * gradY);
                if (gradLength > 0.0001) {
                    double t =
                        1.0 - std::clamp(depth / bezel, 0.0, 1.0);
                    double amount = t * t;
                    // The normal's components fade to zero towards the
                    // middle of the glass, so switching direction per half
                    // leaves no seam.
                    offsetX = (fx < centerX ? directionLeft : directionRight) *
                              gradX / gradLength * amount;
                    offsetY = (fy < centerY ? directionTop : directionBottom) *
                              gradY / gradLength * amount;
                }
            }

            size_t index = (static_cast<size_t>(y) * width + x) * 4;
            pixels[index + 0] = 0x80;
            pixels[index + 1] = static_cast<BYTE>(std::clamp(
                static_cast<int>(std::lround(128.0 + offsetY * 127.0)), 0,
                255));
            pixels[index + 2] = static_cast<BYTE>(std::clamp(
                static_cast<int>(std::lround(128.0 + offsetX * 127.0)), 0,
                255));
            pixels[index + 3] = 0xFF;
        }
    }

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    g_lensMap = nullptr;
    HRESULT hr = g_d2dContext->CreateBitmap(D2D1::SizeU(width, height),
                                            pixels.data(), width * 4, &props,
                                            g_lensMap.put());
    if (FAILED(hr)) {
        Wh_Log(L"lens map failed hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

bool EnsureShapeResourcesLocked() {
    if (g_shapeResourcesValid) {
        return true;
    }

    const Layout& layout = g_layout;
    float width = static_cast<float>(layout.glass.right - layout.glass.left);
    float height = static_cast<float>(layout.glass.bottom - layout.glass.top);
    float radius = static_cast<float>(layout.radius);

    g_clipGeometry = nullptr;
    g_rimBrush = nullptr;
    g_tintBrush = nullptr;
    g_lensMap = nullptr;

    HRESULT hr = g_d2dFactory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, width, height), radius,
                          radius),
        g_clipGeometry.put());
    if (FAILED(hr)) {
        return false;
    }

    if ((layout.tintArgb >> 24) != 0) {
        g_d2dContext->CreateSolidColorBrush(ColorFromArgb(layout.tintArgb),
                                            g_tintBrush.put());
    }

    if (layout.rim > 0.0f) {
        D2D1_GRADIENT_STOP stops[] = {
            {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, layout.rim)},
            {0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, layout.rim * 0.15f)},
            {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, layout.rim * 0.55f)},
        };
        winrt::com_ptr<ID2D1GradientStopCollection> collection;
        if (SUCCEEDED(g_d2dContext->CreateGradientStopCollection(
                stops, ARRAYSIZE(stops), collection.put()))) {
            g_d2dContext->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, height)),
                collection.get(), g_rimBrush.put());
        }
    }

    if (layout.refraction != 0 && !BuildLensMapLocked()) {
        return false;
    }

    g_shapeResourcesValid = true;
    return true;
}

void DumpBackBufferLocked() {
    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                      backBuffer.put_void()))) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(g_d3dDevice->CreateTexture2D(&desc, nullptr, staging.put()))) {
        return;
    }
    g_d3dContext->CopyResource(staging.get(), backBuffer.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0,
                                 &mapped))) {
        return;
    }

    WCHAR path[MAX_PATH];
    DWORD length = GetTempPathW(ARRAYSIZE(path), path);
    if (length && length < ARRAYSIZE(path) - 32) {
        wcscat_s(path, L"taskbar-liquid-glass.bmp");
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD rowBytes = desc.Width * 4;
            BITMAPFILEHEADER fileHeader{};
            BITMAPINFOHEADER infoHeader{};
            fileHeader.bfType = 0x4D42;
            fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
            fileHeader.bfSize = fileHeader.bfOffBits + rowBytes * desc.Height;
            infoHeader.biSize = sizeof(infoHeader);
            infoHeader.biWidth = static_cast<LONG>(desc.Width);
            infoHeader.biHeight = -static_cast<LONG>(desc.Height);
            infoHeader.biPlanes = 1;
            infoHeader.biBitCount = 32;
            infoHeader.biCompression = BI_RGB;
            DWORD written = 0;
            WriteFile(file, &fileHeader, sizeof(fileHeader), &written,
                      nullptr);
            WriteFile(file, &infoHeader, sizeof(infoHeader), &written,
                      nullptr);
            for (UINT row = 0; row < desc.Height; ++row) {
                WriteFile(file,
                          static_cast<BYTE*>(mapped.pData) +
                              static_cast<size_t>(row) * mapped.RowPitch,
                          rowBytes, &written, nullptr);
            }
            CloseHandle(file);
            Wh_Log(L"dumped %ux%u frame to %s", desc.Width, desc.Height, path);
        }
    }
    g_d3dContext->Unmap(staging.get(), 0);
}

void DrawLocked() {
    if (!g_layoutValid || !g_regionValid || !g_targetBitmap ||
        !g_regionBitmap || !EnsureShapeResourcesLocked()) {
        return;
    }

    const Layout& layout = g_layout;
    float width = static_cast<float>(layout.glass.right - layout.glass.left);
    float height = static_cast<float>(layout.glass.bottom - layout.glass.top);
    float radius = static_cast<float>(layout.radius);

    g_borderEffect->SetInput(0, g_regionBitmap.get());
    g_blurEffect->SetInputEffect(0, g_borderEffect.get());
    g_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                           layout.blur);
    ID2D1Effect* output = g_blurEffect.get();
    if (layout.refraction != 0 && g_lensMap) {
        g_displacementEffect->SetInputEffect(0, g_blurEffect.get());
        g_displacementEffect->SetInput(1, g_lensMap.get());
        // The map stores the offset in [-0.5, 0.5] of the scale.
        g_displacementEffect->SetValue(
            D2D1_DISPLACEMENTMAP_PROP_SCALE,
            2.0f * static_cast<float>(std::abs(layout.refraction)));
        output = g_displacementEffect.get();
    }

    D2D1_POINT_2F offset = D2D1::Point2F(
        static_cast<float>(layout.region.left - layout.glass.left),
        static_cast<float>(layout.region.top - layout.glass.top));

    g_d2dContext->SetTarget(g_targetBitmap.get());
    g_d2dContext->BeginDraw();
    g_d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    g_d2dContext->PushLayer(
        D2D1::LayerParameters1(D2D1::InfiniteRect(), g_clipGeometry.get(),
                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                               D2D1::IdentityMatrix(), 1.0f, nullptr,
                               D2D1_LAYER_OPTIONS1_NONE),
        nullptr);
    g_d2dContext->DrawImage(output, &offset, nullptr,
                            D2D1_INTERPOLATION_MODE_LINEAR,
                            D2D1_COMPOSITE_MODE_SOURCE_OVER);
    if (g_tintBrush) {
        g_d2dContext->FillRectangle(D2D1::RectF(0.0f, 0.0f, width, height),
                                    g_tintBrush.get());
    }
    if (g_rimBrush) {
        float inset = 0.75f;
        g_d2dContext->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(inset, inset, width - inset,
                                          height - inset),
                              std::max(0.0f, radius - inset),
                              std::max(0.0f, radius - inset)),
            g_rimBrush.get(), 1.25f);
    }
    g_d2dContext->PopLayer();
    HRESULT hr = g_d2dContext->EndDraw();
    g_d2dContext->SetTarget(nullptr);
    if (FAILED(hr)) {
        Wh_Log(L"EndDraw failed hr=0x%08X", static_cast<unsigned>(hr));
        g_needRestart = true;
        return;
    }

    if (g_dumpRequested.exchange(false)) {
        DumpBackBufferLocked();
    }

    hr = g_swapChain->Present(0, 0);
    if (FAILED(hr)) {
        Wh_Log(L"Present failed hr=0x%08X", static_cast<unsigned>(hr));
        g_needRestart = true;
    }
}

bool FrameTouchesRegionLocked(capture::Direct3D11CaptureFrame const& frame) {
    if (!g_dirtyRegionsEnabled) {
        return true;
    }

    try {
        if (frame.DirtyRegionMode() !=
            capture::GraphicsCaptureDirtyRegionMode::ReportAndRender) {
            return true;
        }

        RECT region = g_layout.region;
        OffsetRect(&region, -g_layout.monitor.left, -g_layout.monitor.top);
        for (auto const& dirty : frame.DirtyRegions()) {
            RECT dirtyRect{dirty.X, dirty.Y, dirty.X + dirty.Width,
                           dirty.Y + dirty.Height};
            RECT intersection;
            if (IntersectRect(&intersection, &region, &dirtyRect)) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return true;
    }
}

void RenderFrameLocked(capture::Direct3D11CaptureFrame const& frame) {
    if (!g_layoutValid || !g_regionTexture) {
        return;
    }
    if (g_regionValid && !FrameTouchesRegionLocked(frame)) {
        return;
    }

    auto surface = frame.Surface();
    winrt::com_ptr<IDxgiInterfaceAccess> access;
    winrt::check_hresult(
        reinterpret_cast<::IUnknown*>(winrt::get_abi(surface))
            ->QueryInterface(kIID_DxgiInterfaceAccess, access.put_void()));
    winrt::com_ptr<ID3D11Texture2D> frameTexture;
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),
                                              frameTexture.put_void()));

    D3D11_TEXTURE2D_DESC desc{};
    frameTexture->GetDesc(&desc);
    RECT region = g_layout.region;
    OffsetRect(&region, -g_layout.monitor.left, -g_layout.monitor.top);
    if (region.left < 0 || region.top < 0 ||
        region.right > static_cast<LONG>(desc.Width) ||
        region.bottom > static_cast<LONG>(desc.Height)) {
        // The monitor mode changed under us; the timer restarts the capture.
        g_needRestart = true;
        return;
    }

    D3D11_BOX box{static_cast<UINT>(region.left),
                  static_cast<UINT>(region.top),
                  0,
                  static_cast<UINT>(region.right),
                  static_cast<UINT>(region.bottom),
                  1};
    g_d3dContext->CopySubresourceRegion(g_regionTexture.get(), 0, 0, 0, 0,
                                        frameTexture.get(), 0, &box);
    g_regionValid = true;
    DrawLocked();
}

void OnFrameArrived(capture::Direct3D11CaptureFramePool const& sender,
                    winrt::Windows::Foundation::IInspectable const&) {
    g_callbacksInFlight.fetch_add(1);
    try {
        auto frame = sender.TryGetNextFrame();
        if (frame) {
            {
                std::lock_guard lock(g_mutex);
                if (g_capturing) {
                    RenderFrameLocked(frame);
                }
            }
            frame.Close();
        }
    } catch (...) {
    }
    g_callbacksInFlight.fetch_sub(1);
}

bool StartCaptureLocked(HMONITOR monitor) {
    try {
        auto factory =
            winrt::get_activation_factory<capture::GraphicsCaptureItem>();
        winrt::com_ptr<ICaptureItemInterop> interop;
        winrt::check_hresult(
            reinterpret_cast<::IUnknown*>(winrt::get_abi(factory))
                ->QueryInterface(kIID_CaptureItemInterop, interop.put_void()));
        void* rawItem = nullptr;
        winrt::check_hresult(interop->CreateForMonitor(
            monitor, kIID_GraphicsCaptureItem, &rawItem));
        g_captureItem = capture::GraphicsCaptureItem{
            rawItem, winrt::take_ownership_from_abi};

        g_framePool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_captureDevice,
            directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
            g_captureItem.Size());
        g_frameArrivedToken = g_framePool.FrameArrived(OnFrameArrived);
        g_captureSession = g_framePool.CreateCaptureSession(g_captureItem);

        g_dirtyRegionsEnabled = false;
        try {
            if (winrt::Windows::Foundation::Metadata::ApiInformation::
                    IsPropertyPresent(
                        L"Windows.Graphics.Capture.GraphicsCaptureSession",
                        L"DirtyRegionMode")) {
                // Lets frames that changed nothing under the taskbar be
                // skipped without touching the GPU.
                g_captureSession.DirtyRegionMode(
                    capture::GraphicsCaptureDirtyRegionMode::ReportAndRender);
                g_dirtyRegionsEnabled = true;
            }
        } catch (...) {
        }
        try {
            g_captureSession.IsBorderRequired(false);
        } catch (...) {
        }
        try {
            g_captureSession.IsCursorCaptureEnabled(false);
        } catch (...) {
        }

        g_regionValid = false;
        g_capturing = true;
        g_captureMonitor = monitor;
        g_captureSession.StartCapture();
        Wh_Log(L"capture started monitor=%p dirtyRegions=%d", monitor,
               g_dirtyRegionsEnabled);
        return true;
    } catch (winrt::hresult_error const& error) {
        Wh_Log(L"capture start failed hr=0x%08X",
               static_cast<unsigned>(error.code().value));
    } catch (...) {
        Wh_Log(L"capture start failed");
    }

    g_capturing = false;
    g_captureSession = nullptr;
    g_framePool = nullptr;
    g_captureItem = nullptr;
    return false;
}

void StopCapture() {
    capture::GraphicsCaptureSession session{nullptr};
    capture::Direct3D11CaptureFramePool framePool{nullptr};
    winrt::event_token token{};
    {
        std::lock_guard lock(g_mutex);
        g_capturing = false;
        g_captureMonitor = nullptr;
        session = std::move(g_captureSession);
        framePool = std::move(g_framePool);
        token = g_frameArrivedToken;
        g_captureSession = nullptr;
        g_framePool = nullptr;
        g_captureItem = nullptr;
    }

    // Closed outside the lock: a frame callback may be waiting on it.
    try {
        if (framePool) {
            framePool.FrameArrived(token);
        }
        if (session) {
            session.Close();
        }
        if (framePool) {
            framePool.Close();
        }
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Window thread.

bool ComputeLayout(HWND taskbar, Settings const& settings, Layout& layout) {
    RECT windowRect;
    if (!GetWindowRect(taskbar, &windowRect)) {
        return false;
    }
    HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }

    UINT dpi = GetDpiForWindow(taskbar);
    if (!dpi) {
        dpi = 96;
    }
    auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };

    layout.monitor = monitorInfo.rcMonitor;
    layout.glass = {windowRect.left + scale(settings.marginLeft),
                    windowRect.top + scale(settings.marginTop),
                    windowRect.right - scale(settings.marginRight),
                    windowRect.bottom - scale(settings.marginBottom)};
    int width = layout.glass.right - layout.glass.left;
    int height = layout.glass.bottom - layout.glass.top;
    if (width < 8 || height < 8) {
        return false;
    }

    // An auto-hidden taskbar slides off the screen; nothing to draw then.
    RECT visible;
    if (!IntersectRect(&visible, &layout.glass, &layout.monitor) ||
        visible.bottom - visible.top < 8 || visible.right - visible.left < 8) {
        return false;
    }

    layout.radius =
        std::clamp(scale(settings.cornerRadius), 0, std::min(width, height) / 2);
    layout.bezel = std::max(1, scale(settings.bezelWidth));
    layout.refraction = scale(settings.refraction);
    layout.blur = static_cast<float>(scale(settings.blur));
    layout.rim = settings.rimHighlight / 100.0f;
    layout.tintArgb = settings.tintArgb;

    // The blur and an outward refraction both sample beyond the glass.
    int pad = static_cast<int>(std::ceil(layout.blur * 3.0f)) +
              std::abs(layout.refraction) + 2;
    RECT padded = layout.glass;
    InflateRect(&padded, pad, pad);
    IntersectRect(&layout.region, &padded, &layout.monitor);
    return true;
}

bool IsTopmost(HWND hwnd) {
    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// True if the glass sits under the taskbar with nothing visible in between
// that could cover it.
bool IsGlassBelowTaskbar(HWND taskbar, RECT const& glass) {
    if (IsTopmost(taskbar) != IsTopmost(g_hwnd)) {
        return false;
    }
    for (HWND hwnd = GetWindow(taskbar, GW_HWNDNEXT); hwnd;
         hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        if (hwnd == g_hwnd) {
            return true;
        }
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) == taskbar) {
            continue;
        }
        RECT rect;
        RECT intersection;
        if (GetWindowRect(hwnd, &rect) &&
            IntersectRect(&intersection, &rect, &glass)) {
            return false;
        }
    }
    // Not found below the taskbar at all: the glass is above it.
    return false;
}

void PlaceGlassBelowTaskbar(HWND taskbar, RECT const& glass) {
    constexpr UINT kFlags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;

    // Inserting after the taskbar does not reliably carry a window across
    // the topmost boundary, so the band is switched explicitly first. The
    // glass is hidden meanwhile: HWND_TOPMOST briefly puts it over the
    // taskbar.
    bool taskbarTopmost = IsTopmost(taskbar);
    if (taskbarTopmost != IsTopmost(g_hwnd)) {
        ShowWindow(g_hwnd, SW_HIDE);
        SetWindowPos(g_hwnd, taskbarTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, kFlags | SWP_NOMOVE | SWP_NOSIZE);
    }

    SetWindowPos(g_hwnd, taskbar, glass.left, glass.top,
                 glass.right - glass.left, glass.bottom - glass.top, kFlags);
    if (!IsWindowVisible(g_hwnd)) {
        ShowWindow(g_hwnd, SW_SHOWNA);
    }
}

void Tick() {
    if (g_stop) {
        DestroyWindow(g_hwnd);
        return;
    }

    Settings settings = GetSettings();
    HWND taskbar = FindTaskbarInThisProcess();
    UpdateTaskbarAffinity(taskbar, settings.excludeTaskbarFromCapture);

    if (g_needRestart.exchange(false)) {
        StopCapture();
        std::lock_guard lock(g_mutex);
        ReleaseDevicesLocked();
    }

    Layout layout;
    HMONITOR monitor = nullptr;
    bool show = taskbar && IsWindowVisible(taskbar) &&
                ComputeLayout(taskbar, settings, layout);
    if (show) {
        monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
    }

    if (!show) {
        if (IsWindowVisible(g_hwnd)) {
            ShowWindow(g_hwnd, SW_HIDE);
        }
        std::lock_guard lock(g_mutex);
        g_layoutValid = false;
        return;
    }

    bool restartCapture;
    {
        std::lock_guard lock(g_mutex);
        if (!CreateDevicesLocked()) {
            ReleaseDevicesLocked();
            return;
        }
        restartCapture = !g_capturing || g_captureMonitor != monitor ||
                         (g_layoutValid &&
                          !EqualRect(&g_layout.monitor, &layout.monitor));
    }
    if (restartCapture) {
        StopCapture();
    }

    bool moved;
    bool recreateFramePool = false;
    {
        std::lock_guard lock(g_mutex);
        if (restartCapture && !StartCaptureLocked(monitor)) {
            return;
        }

        moved = !g_layoutValid || !(g_layout == layout);
        if (moved) {
            bool regionMoved = !g_layoutValid ||
                               !EqualRect(&g_layout.region, &layout.region);
            g_layout = layout;
            g_layoutValid = true;
            g_shapeResourcesValid = false;
            if (!EnsureSwapChainLocked(layout.glass.right - layout.glass.left,
                                       layout.glass.bottom -
                                           layout.glass.top) ||
                !EnsureRegionLocked(layout.region.right - layout.region.left,
                                    layout.region.bottom -
                                        layout.region.top)) {
                g_layoutValid = false;
                return;
            }

            if (regionMoved) {
                g_regionValid = false;
            }
            if (g_regionValid) {
                DrawLocked();
            } else if (!restartCapture && g_framePool) {
                recreateFramePool = true;
            }
        } else if (g_dumpRequested && g_regionValid) {
            DrawLocked();
        }
    }

    if (recreateFramePool) {
        // The copied pixels belong to the old position. Recreating the pool
        // makes the capture deliver a full fresh frame even if nothing on
        // screen is changing. Done outside the lock: a frame callback may be
        // waiting on it. Only this thread ever replaces the pool.
        try {
            g_framePool.Recreate(
                g_captureDevice,
                directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                g_captureItem.Size());
        } catch (...) {
            g_needRestart = true;
        }
    }

    if (moved || !IsWindowVisible(g_hwnd) ||
        !IsGlassBelowTaskbar(taskbar, layout.glass)) {
        PlaceGlassBelowTaskbar(taskbar, layout.glass);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_TIMER:
            if (wParam == kTimerId) {
                Tick();
            }
            return 0;

        case WM_DISPLAYCHANGE:
            g_needRestart = true;
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Monitor captures get a yellow border unless the process has asked for
// borderless capture access.
void RequestBorderlessCapture() {
    try {
        auto operation = capture::GraphicsCaptureAccess::RequestAccessAsync(
            capture::GraphicsCaptureAccessKind::Borderless);
        if (operation.wait_for(std::chrono::seconds(5)) ==
            winrt::Windows::Foundation::AsyncStatus::Completed) {
            auto status = operation.GetResults();
            Wh_Log(L"borderless capture access status=%d",
                   static_cast<int>(status));
        } else {
            operation.Cancel();
        }
    } catch (winrt::hresult_error const& error) {
        Wh_Log(L"borderless capture access failed hr=0x%08X",
               static_cast<unsigned>(error.code().value));
    } catch (...) {
    }
}

void ThreadMain() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }

    RequestBorderlessCapture();

    // Windows gives every window an owned IME helper window. It keeps its own
    // topmost state, and once that disagrees with the glass window the pair
    // can no longer be reordered and the glass gets stuck under other windows.
    ImmDisableIME(GetCurrentThreadId());

    HINSTANCE instance = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&WndProc), &instance);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    if (!g_stop) {
        g_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                WS_EX_NOREDIRECTIONBITMAP,
            kWindowClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
            instance, nullptr);
    }
    if (g_hwnd) {
        // Without this the glass would capture, blur and refract itself.
        SetWindowDisplayAffinity(g_hwnd, WDA_EXCLUDEFROMCAPTURE);
        SetTimer(g_hwnd, kTimerId, kTimerIntervalMs, nullptr);

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        g_hwnd = nullptr;
    } else if (!g_stop) {
        Wh_Log(L"CreateWindowEx failed error=%lu", GetLastError());
    }

    StopCapture();
    for (int i = 0; i < 200 && g_callbacksInFlight.load() > 0; ++i) {
        Sleep(10);
    }
    UpdateTaskbarAffinity(nullptr, false);
    {
        std::lock_guard lock(g_mutex);
        ReleaseDevicesLocked();
    }
    UnregisterClassW(kWindowClass, instance);

    try {
        winrt::uninit_apartment();
    } catch (...) {
    }
}

BOOL Wh_ModInit() {
    LoadSettings();
    g_thread = std::thread(ThreadMain);
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    g_stop = true;
    if (g_thread.joinable()) {
        g_thread.join();
    }
}
