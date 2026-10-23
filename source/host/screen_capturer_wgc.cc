#include "host/screen_capturer_wgc.h"

#include <comdef.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <cstring>

#include <QThread>

#include "base/desktop/differ.h"
#include "base/desktop/frame_aligned.h"
#include "base/logging.h"
#include "base/win/windows_version.h"
#include "host/win/screen_capture_utils.h"

using Microsoft::WRL::ComPtr;

namespace {

namespace wgc = ABI::Windows::Graphics::Capture;
namespace wgd = ABI::Windows::Graphics::DirectX::Direct3D11;
namespace wf = ABI::Windows::Foundation;
// Not part of the ABI namespaces: the interop helpers are declared in the classic WinRT namespace.
namespace wgdx = ::Windows::Graphics::DirectX::Direct3D11;

// Number of buffers in the frame pool. Two is enough: we copy every frame into our own buffer and
// never hold a frame for longer than one capture cycle.
constexpr int kFramePoolBufferCount = 2;

// Alignment of the frame rows. Must be at least the alignment required by the encoders/scaler.
constexpr int kFrameAlignment = 16;

// How long captureFrame() waits for the very first frame after the capture session has started.
constexpr int kFirstFrameTimeoutMs = 250;

// Delay before the capture session is recreated after a failure. Without it the host would call
// CreateForMonitor() on every capture tick while the session is unavailable.
constexpr int kSessionRetryDelayMs = 500;

// Windows 7 has neither the WinRT activation entry points nor the Direct3D interop helper, and a
// static import of them would make the whole executable unloadable there - the host would lose GDI
// capture as well. They are resolved by hand, and the capturer reports itself unsupported when they
// are not there.
struct WinrtFunctions
{
    HRESULT (WINAPI* ro_initialize)(DWORD);
    void (WINAPI* ro_uninitialize)();
    HRESULT (WINAPI* ro_get_activation_factory)(HSTRING, REFIID, void**);
    HRESULT (WINAPI* windows_create_string)(LPCWSTR, UINT32, HSTRING*);
    HRESULT (WINAPI* windows_delete_string)(HSTRING);
    HRESULT (WINAPI* create_d3d11_device_from_dxgi_device)(IUnknown*, IInspectable**);
};

//--------------------------------------------------------------------------------------------------
const WinrtFunctions& winrtFunctions()
{
    static const WinrtFunctions functions = []()
    {
        WinrtFunctions result = {};

        HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (combase)
        {
            result.ro_initialize = reinterpret_cast<decltype(result.ro_initialize)>(
                GetProcAddress(combase, "RoInitialize"));
            result.ro_uninitialize = reinterpret_cast<decltype(result.ro_uninitialize)>(
                GetProcAddress(combase, "RoUninitialize"));
            result.ro_get_activation_factory =
                reinterpret_cast<decltype(result.ro_get_activation_factory)>(
                    GetProcAddress(combase, "RoGetActivationFactory"));
            result.windows_create_string = reinterpret_cast<decltype(result.windows_create_string)>(
                GetProcAddress(combase, "WindowsCreateString"));
            result.windows_delete_string = reinterpret_cast<decltype(result.windows_delete_string)>(
                GetProcAddress(combase, "WindowsDeleteString"));
        }

        HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (d3d11)
        {
            result.create_d3d11_device_from_dxgi_device =
                reinterpret_cast<decltype(result.create_d3d11_device_from_dxgi_device)>(
                    GetProcAddress(d3d11, "CreateDirect3D11DeviceFromDXGIDevice"));
        }

        return result;
    }();

    return functions;
}

//--------------------------------------------------------------------------------------------------
bool isWinrtAvailable()
{
    const WinrtFunctions& functions = winrtFunctions();
    return functions.ro_initialize && functions.ro_uninitialize &&
           functions.ro_get_activation_factory && functions.windows_create_string &&
           functions.windows_delete_string &&
           functions.create_d3d11_device_from_dxgi_device;
}

//--------------------------------------------------------------------------------------------------
class ScopedHString final
{
public:
    explicit ScopedHString(const wchar_t* value)
    {
        const WinrtFunctions& functions = winrtFunctions();
        if (functions.windows_create_string)
        {
            functions.windows_create_string(
                value, static_cast<UINT32>(std::char_traits<wchar_t>::length(value)), &value_);
        }
    }

    ~ScopedHString()
    {
        const WinrtFunctions& functions = winrtFunctions();
        if (value_ && functions.windows_delete_string)
            functions.windows_delete_string(value_);
    }

    HSTRING get() const { return value_; }

private:
    HSTRING value_ = nullptr;

    Q_DISABLE_COPY_MOVE(ScopedHString)
};

//--------------------------------------------------------------------------------------------------
// The activation factory of |runtime_class|, or an error when it cannot be obtained.
HRESULT activationFactory(const wchar_t* runtime_class, REFIID iid, void** factory)
{
    const WinrtFunctions& functions = winrtFunctions();
    if (!functions.ro_get_activation_factory)
        return REGDB_E_CLASSNOTREG;

    ScopedHString name(runtime_class);
    if (!name.get())
        return E_OUTOFMEMORY;

    return functions.ro_get_activation_factory(name.get(), iid, factory);
}

// Closes a WinRT object that implements Windows.Foundation.IClosable.
void closeWinrtObject(IInspectable* object)
{
    if (!object)
        return;

    ComPtr<wf::IClosable> closable;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&closable))))
        closable->Close();
}

} // namespace

//--------------------------------------------------------------------------------------------------
ScreenCapturerWgc::ScreenCapturerWgc(QObject* parent)
    : ScreenCapturerWin(Type::WIN_WGC, parent)
{
    LOG(INFO) << "Ctor";

    const HRESULT hr = winrtFunctions().ro_initialize(RO_INIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE)
        ro_initialized_ = true;
    else if (hr != RPC_E_CHANGED_MODE)
        LOG(ERROR) << "RoInitialize failed:" << _com_error(hr);

    desktop_rect_ = ScreenCaptureUtils::fullScreenRect();
}

//--------------------------------------------------------------------------------------------------
ScreenCapturerWgc::~ScreenCapturerWgc()
{
    LOG(INFO) << "Dtor";

    closeSession();

    capture_device_.Reset();
    d3d_context_.Reset();
    d3d_device_.Reset();

    if (ro_initialized_)
    {
        ro_initialized_ = false;
        winrtFunctions().ro_uninitialize();
    }
}

//--------------------------------------------------------------------------------------------------
// static
bool ScreenCapturerWgc::isSupported()
{
    if (windowsVersion() < VERSION_WIN10)
        return false;

    if (!isWinrtAvailable())
    {
        LOG(INFO) << "WGC is not supported: the WinRT entry points are not available";
        return false;
    }

    const HRESULT initialized = winrtFunctions().ro_initialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    {
        LOG(ERROR) << "RoInitialize failed:" << _com_error(initialized);
        return false;
    }

    // Direct3D11CaptureFramePool.CreateFreeThreaded() does not need a DispatcherQueue, so it can be
    // used from the worker thread. It is available since Windows 10 1809; without it the frame pool
    // can only be created on a thread with a DispatcherQueue.
    ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> pool_statics;
    HRESULT hr = activationFactory(
        RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool,
        IID_PPV_ARGS(&pool_statics));
    if (FAILED(hr))
    {
        LOG(INFO) << "WGC is not supported:"
                  << _com_error(hr);
        return false;
    }

    ComPtr<IGraphicsCaptureItemInterop> item_interop;
    hr = activationFactory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem,
                           IID_PPV_ARGS(&item_interop));
    if (FAILED(hr))
    {
        LOG(INFO) << "WGC is not supported:"
                  << _com_error(hr);
        return false;
    }

    // GraphicsCaptureSession.IsSupported() exists since Windows 11 24H2. When it is available it is
    // the authoritative answer: some systems have window/monitor capture disabled by policy.
    ComPtr<wgc::IGraphicsCaptureSessionStatics> session_statics;
    hr = activationFactory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
                           IID_PPV_ARGS(&session_statics));
    if (SUCCEEDED(hr))
    {
        boolean supported = FALSE;
        hr = session_statics->IsSupported(&supported);
        if (SUCCEEDED(hr) && !supported)
        {
            LOG(INFO) << "WGC is not supported: capture is disabled on this system";
            return false;
        }
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
int ScreenCapturerWgc::screenCount()
{
    return ScreenCaptureUtils::screenCount();
}

//--------------------------------------------------------------------------------------------------
bool ScreenCapturerWgc::screenList(ScreenList* screens)
{
    return ScreenCaptureUtils::screenList(screens);
}

//--------------------------------------------------------------------------------------------------
bool ScreenCapturerWgc::selectScreen(ScreenId screen_id)
{
    LOG(INFO) << "Select screen with ID:" << screen_id;

    std::wstring device_key;
    if (!ScreenCaptureUtils::isScreenValid(screen_id, &device_key))
    {
        LOG(ERROR) << "Invalid screen";
        return false;
    }

    const QRect screen_rect = ScreenCaptureUtils::screenRect(screen_id, device_key);
    if (screen_rect.isEmpty())
    {
        LOG(ERROR) << "Failed to get the screen rect";
        return false;
    }

    RECT native_rect = {};
    native_rect.left = screen_rect.left();
    native_rect.top = screen_rect.top();
    native_rect.right = screen_rect.right();
    native_rect.bottom = screen_rect.bottom();

    HMONITOR monitor = MonitorFromRect(&native_rect, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
    {
        PLOG(ERROR) << "MonitorFromRect failed";
        return false;
    }

    const bool monitor_changed =
        (monitor != monitor_) || (screen_rect != screen_rect_) || (current_screen_id_ != screen_id);

    current_screen_id_ = screen_id;
    current_device_key_ = device_key;
    monitor_ = monitor;
    screen_rect_ = screen_rect;

    if (monitor_changed)
    {
        closeSession();
        next_session_attempt_ = std::chrono::steady_clock::time_point();
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
const Frame* ScreenCapturerWgc::captureFrame(Error* error)
{
    DCHECK(error);
    *error = Error::TEMPORARY;

    // Keep the desktop rect up to date: the client uses it to map the input coordinates.
    desktop_rect_ = ScreenCaptureUtils::fullScreenRect();

    // A resolution or position change requires a new capture item for a new monitor rect.
    const QRect screen_rect =
        ScreenCaptureUtils::screenRect(current_screen_id_, current_device_key_);
    if (!screen_rect.isEmpty() && screen_rect != screen_rect_)
    {
        LOG(INFO) << "Screen rect changed from" << screen_rect_ << "to" << screen_rect;
        screen_rect_ = screen_rect;
        closeSession();
        next_session_attempt_ = std::chrono::steady_clock::time_point();
    }

    if (std::chrono::steady_clock::now() < next_session_attempt_)
    {
        // Do not call CreateForMonitor() on every tick while the session is unavailable. Keep
        // showing the last captured frame (if any) instead of reporting an error to the client.
        Frame* retained = queue_.currentFrame();
        if (!retained)
            return nullptr;

        retained->updatedRegion()->clear();
        *error = Error::SUCCEEDED;
        return retained;
    }

    if (!createSession())
    {
        // The capture session can be unavailable for a while: when an RDP window is minimized the
        // session has no display output to capture at all. Retry later instead of reporting a
        // permanent error, because in such a session neither DXGI nor GDI can capture anything, and
        // a switch to them would never recover when the session becomes visible again.
        ++session_fail_count_;
        next_session_attempt_ =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kSessionRetryDelayMs);

        if (session_fail_count_ == 1 || session_fail_count_ % 60 == 0)
        {
            LOG(ERROR) << "Unable to create a WGC capture session (attempt" << session_fail_count_
                       << "), will retry";
        }

        *error = Error::TEMPORARY;
        return nullptr;
    }

    session_fail_count_ = 0;
    next_session_attempt_ = std::chrono::steady_clock::time_point();

    // Wait for the first frame: it can take a few milliseconds after StartCapture(). Reporting an
    // error instead would look like a capture failure to the client.
    const int wait_ms = queue_.currentFrame() ? 0 : kFirstFrameTimeoutMs;

    ComPtr<wgc::IDirect3D11CaptureFrame> frame;
    if (!acquireFrame(&frame, wait_ms))
    {
        // No new frame. WGC only delivers frames when the content changes, so a static desktop is
        // the normal case here. Return the retained frame with an empty updated region: the
        // encoder then skips the frame instead of sending an identical picture again.
        Frame* current = queue_.currentFrame();
        if (!current)
            return nullptr;

        current->updatedRegion()->clear();
        *error = Error::SUCCEEDED;
        return current;
    }

    ABI::Windows::Graphics::SizeInt32 content_size = {};
    HRESULT hr = frame->get_ContentSize(&content_size);
    if (FAILED(hr))
    {
        LOG(ERROR) << "get_ContentSize failed:" << _com_error(hr);
        closeWinrtObject(frame.Get());
        return nullptr;
    }

    const QSize size(content_size.Width, content_size.Height);
    if (size.isEmpty())
    {
        LOG(ERROR) << "Empty frame content size";
        closeWinrtObject(frame.Get());
        return nullptr;
    }

    ComPtr<wgd::IDirect3DSurface> surface;
    hr = frame->get_Surface(&surface);
    if (FAILED(hr))
    {
        LOG(ERROR) << "get_Surface failed:" << _com_error(hr);
        closeWinrtObject(frame.Get());
        return nullptr;
    }

    ComPtr<wgdx::IDirect3DDxgiInterfaceAccess> dxgi_access;
    hr = surface.As(&dxgi_access);
    if (FAILED(hr))
    {
        LOG(ERROR) << "Failed to get IDirect3DDxgiInterfaceAccess:" << _com_error(hr);
        closeWinrtObject(frame.Get());
        return nullptr;
    }

    ComPtr<ID3D11Texture2D> texture;
    hr = dxgi_access->GetInterface(IID_PPV_ARGS(&texture));
    if (FAILED(hr))
    {
        LOG(ERROR) << "Failed to get ID3D11Texture2D:" << _com_error(hr);
        closeWinrtObject(frame.Get());
        return nullptr;
    }

    const bool copied = copyTexture(texture.Get(), size);

    // The frame holds a buffer of the pool: release it only after the pixels have been copied.
    closeWinrtObject(frame.Get());

    if (!copied)
        return nullptr;

    *error = Error::SUCCEEDED;
    return queue_.currentFrame();
}

//--------------------------------------------------------------------------------------------------
const QRect& ScreenCapturerWgc::desktopRect() const
{
    return desktop_rect_;
}

//--------------------------------------------------------------------------------------------------
const QRect& ScreenCapturerWgc::currentScreenRect() const
{
    return screen_rect_;
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerWgc::reset()
{
    ScreenCapturerWin::reset();

    closeSession();
}

//--------------------------------------------------------------------------------------------------
bool ScreenCapturerWgc::initializeDevice()
{
    if (d3d_device_)
        return true;

    static const D3D_FEATURE_LEVEL kFeatureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_10_0;

    HRESULT hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   kFeatureLevels,
                                   _countof(kFeatureLevels),
                                   D3D11_SDK_VERSION,
                                   &d3d_device_,
                                   &feature_level,
                                   &d3d_context_);
    if (FAILED(hr))
    {
        LOG(ERROR) << "D3D11CreateDevice failed:" << _com_error(hr);
        d3d_device_.Reset();
        d3d_context_.Reset();
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    hr = d3d_device_.As(&dxgi_device);
    if (FAILED(hr))
    {
        LOG(ERROR) << "Failed to get IDXGIDevice:" << _com_error(hr);
        return false;
    }

    ComPtr<IInspectable> inspectable;
    hr = winrtFunctions().create_d3d11_device_from_dxgi_device(
        dxgi_device.Get(), inspectable.GetAddressOf());
    if (FAILED(hr))
    {
        LOG(ERROR) << "CreateDirect3D11DeviceFromDXGIDevice failed:" << _com_error(hr);
        return false;
    }

    hr = inspectable.As(&capture_device_);
    if (FAILED(hr))
    {
        LOG(ERROR) << "Failed to get IDirect3DDevice:" << _com_error(hr);
        return false;
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
bool ScreenCapturerWgc::createSession()
{
    if (capture_session_ && frame_pool_ && capture_item_)
        return true;

    // Do not flood the log while the session is unavailable (a minimized RDP window, for example):
    // the first failure is logged, then every sixtieth one.
    const bool log_errors = (session_fail_count_ == 0) || (session_fail_count_ % 60 == 0);

    auto log_error = [log_errors](const char* message, HRESULT hr)
    {
        if (log_errors)
            LOG(ERROR) << message << _com_error(hr);
    };

    closeSession();

    // The monitor handle resolved by selectScreen() can become stale: a session that loses its
    // display and later gets a new one (an RDP window that is minimized and then restored) is
    // identified by a different HMONITOR. Re-resolve it on every attempt, otherwise
    // CreateForMonitor() keeps failing with 0x80070424 forever.
    if (!screen_rect_.isEmpty())
    {
        RECT native_rect = {};
        native_rect.left = screen_rect_.left();
        native_rect.top = screen_rect_.top();
        native_rect.right = screen_rect_.right();
        native_rect.bottom = screen_rect_.bottom();

        HMONITOR monitor = MonitorFromRect(&native_rect, MONITOR_DEFAULTTONEAREST);
        if (monitor && monitor != monitor_)
        {
            if (log_errors)
                LOG(INFO) << "Monitor handle changed, using the new one";

            monitor_ = monitor;
        }
    }

    if (!monitor_ || screen_rect_.isEmpty())
    {
        if (log_errors)
            LOG(ERROR) << "No screen is selected";
        return false;
    }

    if (!initializeDevice())
        return false;

    ComPtr<IGraphicsCaptureItemInterop> item_interop;
    HRESULT hr = activationFactory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem,
                                   IID_PPV_ARGS(&item_interop));
    if (FAILED(hr))
    {
        log_error("RoGetActivationFactory(GraphicsCaptureItem) failed: ", hr);
        return false;
    }

    hr = item_interop->CreateForMonitor(monitor_, IID_PPV_ARGS(&capture_item_));
    if (FAILED(hr))
    {
        // CreateForMonitor() does not necessarily notice that the cached handle is stale. Retry
        // once with the monitor the desktop window is currently displayed on.
        HMONITOR desktop_monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        if (desktop_monitor && desktop_monitor != monitor_)
        {
            monitor_ = desktop_monitor;

            hr = item_interop->CreateForMonitor(monitor_, IID_PPV_ARGS(&capture_item_));
        }

        if (FAILED(hr))
        {
            log_error("CreateForMonitor failed: ", hr);
            return false;
        }
    }

    ABI::Windows::Graphics::SizeInt32 item_size = {};
    hr = capture_item_->get_Size(&item_size);
    if (FAILED(hr))
    {
        log_error("GraphicsCaptureItem::get_Size failed: ", hr);
        return false;
    }

    ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> pool_statics;
    hr = activationFactory(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool,
                           IID_PPV_ARGS(&pool_statics));
    if (FAILED(hr))
    {
        log_error("RoGetActivationFactory(Direct3D11CaptureFramePool) failed: ", hr);
        return false;
    }

    hr = pool_statics->CreateFreeThreaded(capture_device_.Get(),
                                          ABI::Windows::Graphics::DirectX::
                                              DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                          kFramePoolBufferCount,
                                          item_size,
                                          &frame_pool_);
    if (FAILED(hr))
    {
        log_error("CreateFreeThreaded failed: ", hr);
        return false;
    }

    hr = frame_pool_->CreateCaptureSession(capture_item_.Get(), &capture_session_);
    if (FAILED(hr))
    {
        log_error("CreateCaptureSession failed: ", hr);
        return false;
    }

    // The host sends the cursor shape separately, so do not draw the system cursor into the frames
    // (otherwise the client would show it twice).
    ComPtr<wgc::IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(capture_session_.As(&session2)))
        session2->put_IsCursorCaptureEnabled(FALSE);

    hr = capture_session_->StartCapture();
    if (FAILED(hr))
    {
        log_error("StartCapture failed: ", hr);
        return false;
    }

    LOG(INFO) << "Capture session started. Monitor:" << reinterpret_cast<qulonglong>(monitor_)
              << "Size:" << QSize(item_size.Width, item_size.Height);

    return true;
}

//--------------------------------------------------------------------------------------------------
void ScreenCapturerWgc::closeSession()
{
    if (capture_session_)
    {
        closeWinrtObject(capture_session_.Get());
        capture_session_.Reset();
    }

    frame_pool_.Reset();
    capture_item_.Reset();

    staging_texture_.Reset();
    staging_size_ = QSize();
    staging_format_ = DXGI_FORMAT_UNKNOWN;

    differ_.reset();
    queue_.reset();
}

//--------------------------------------------------------------------------------------------------
bool ScreenCapturerWgc::acquireFrame(ComPtr<wgc::IDirect3D11CaptureFrame>* frame, int wait_ms)
{
    DCHECK(frame);

    constexpr int kWaitStepMs = 5;

    for (int waited = 0;; waited += kWaitStepMs)
    {
        // Drain the pool: only the newest frame matters, older ones must be released.
        ComPtr<wgc::IDirect3D11CaptureFrame> newest;

        for (;;)
        {
            ComPtr<wgc::IDirect3D11CaptureFrame> next;
            HRESULT hr = frame_pool_->TryGetNextFrame(&next);
            if (FAILED(hr))
            {
                LOG(ERROR) << "TryGetNextFrame failed:" << _com_error(hr);
                break;
            }

            if (!next)
                break;

            if (newest)
                closeWinrtObject(newest.Get());

            newest = next;
        }

        if (newest)
        {
            *frame = newest;
            return true;
        }

        if (waited >= wait_ms)
            return false;

        QThread::msleep(kWaitStepMs);
    }
}

//--------------------------------------------------------------------------------------------------
bool ScreenCapturerWgc::copyTexture(ID3D11Texture2D* texture, const QSize& size)
{
    if (!texture || size.isEmpty())
        return false;

    D3D11_TEXTURE2D_DESC source_desc = {};
    texture->GetDesc(&source_desc);

    if (!staging_texture_ || staging_size_ != size || staging_format_ != source_desc.Format)
    {
        D3D11_TEXTURE2D_DESC staging_desc = source_desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;
        staging_desc.ArraySize = 1;
        staging_desc.MipLevels = 1;
        staging_desc.SampleDesc.Count = 1;
        staging_desc.SampleDesc.Quality = 0;

        staging_texture_.Reset();

        HRESULT hr = d3d_device_->CreateTexture2D(&staging_desc, nullptr, &staging_texture_);
        if (FAILED(hr))
        {
            LOG(ERROR) << "CreateTexture2D(staging) failed:" << _com_error(hr);
            return false;
        }

        staging_size_ = size;
        staging_format_ = source_desc.Format;
    }

    d3d_context_->CopyResource(staging_texture_.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = d3d_context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        LOG(ERROR) << "ID3D11DeviceContext::Map failed:" << _com_error(hr);
        return false;
    }

    queue_.moveToNextFrame();

    Frame* current = queue_.currentFrame();
    if (!current || current->size() != size)
    {
        std::unique_ptr<FrameAligned> frame = FrameAligned::create(size, kFrameAlignment);
        if (!frame)
        {
            d3d_context_->Unmap(staging_texture_.Get(), 0);
            LOG(ERROR) << "Failed to allocate a frame";
            return false;
        }

        frame->setCapturerType(static_cast<quint32>(type()));
        queue_.replaceCurrentFrame(std::move(frame));
        current = queue_.currentFrame();
    }

    const int row_size = std::min(static_cast<int>(mapped.RowPitch), current->stride());
    const quint8* source = static_cast<const quint8*>(mapped.pData);
    quint8* destination = current->frameData();

    if (!destination || row_size <= 0)
    {
        d3d_context_->Unmap(staging_texture_.Get(), 0);
        return false;
    }

    for (int y = 0; y < size.height(); ++y)
    {
        memcpy(destination + static_cast<size_t>(y) * static_cast<size_t>(current->stride()),
               source + static_cast<size_t>(y) * static_cast<size_t>(mapped.RowPitch),
               static_cast<size_t>(row_size));
    }

    d3d_context_->Unmap(staging_texture_.Get(), 0);

    current->setTopLeft(screen_rect_.topLeft() - desktop_rect_.topLeft());
    current->setDpi(QPoint(GetDeviceCaps(nullptr, LOGPIXELSX), GetDeviceCaps(nullptr, LOGPIXELSY)));

    Frame* previous = queue_.previousFrame();
    if (!previous || previous->size() != current->size() || !previous->frameData())
    {
        differ_ = std::make_unique<Differ>(current->size(), current->stride());
        *current->updatedRegion() += QRect(QPoint(0, 0), current->size());
    }
    else
    {
        if (!differ_)
            differ_ = std::make_unique<Differ>(current->size(), current->stride());

        differ_->calcDirtyRegion(
            previous->frameData(), current->frameData(), current->updatedRegion());
    }

    return true;
}
