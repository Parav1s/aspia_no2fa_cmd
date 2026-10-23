#ifndef HOST_SCREEN_CAPTURER_WGC_H
#define HOST_SCREEN_CAPTURER_WGC_H

#include <QRect>
#include <QSize>

#include <memory>
#include <string>

#include <chrono>

#include <d3d11.h>
#include <wrl/client.h>

#include <windows.graphics.capture.h>
#include <windows.graphics.directx.direct3d11.h>

#include "host/screen_capturer_win.h"

#include <wrl/client.h>

class Differ;

// Screen capturer based on Windows.Graphics.Capture (WGC, Windows 10 1803+). Unlike GDI
// (BitBlt from the screen DC) and DXGI Desktop Duplication it also works in sessions that are
// rendered by an indirect display driver - for example an RDP session on Windows 10/11, where both
// BitBlt and IDXGIOutput1::DuplicateOutput fail with access denied.
class ScreenCapturerWgc final : public ScreenCapturerWin
{
    Q_OBJECT

public:
    explicit ScreenCapturerWgc(QObject* parent = nullptr);
    ~ScreenCapturerWgc() final;

    // Returns true if this capturer can be used on the current system.
    static bool isSupported();

    // ScreenCapturer implementation.
    int screenCount() final;
    bool screenList(ScreenList* screens) final;
    bool selectScreen(ScreenId screen_id) final;
    const Frame* captureFrame(Error* error) final;
    const QRect& desktopRect() const final;
    const QRect& currentScreenRect() const final;

protected:
    // ScreenCapturer implementation.
    void reset() final;

private:
    bool initializeDevice();
    bool createSession();
    void closeSession();
    bool acquireFrame(Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame>*
                          frame,
                      int wait_ms);
    bool copyTexture(ID3D11Texture2D* texture, const QSize& size);

    // Whether this object initialized COM as MTA for the capture thread, so that the destructor
    // knows whether to uninitialize it.
    bool ro_initialized_ = false;

    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
    Microsoft::WRL::ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> capture_device_;

    HMONITOR monitor_ = nullptr;
    QRect screen_rect_;
    QRect desktop_rect_;
    std::wstring current_device_key_;

    Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> capture_item_;
    Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool> frame_pool_;
    Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession> capture_session_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;
    QSize staging_size_;
    DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;

    int session_fail_count_ = 0;
    std::chrono::steady_clock::time_point next_session_attempt_;

    std::unique_ptr<Differ> differ_;
    FrameQueue<Frame> queue_;

    Q_DISABLE_COPY_MOVE(ScreenCapturerWgc)
};

#endif // HOST_SCREEN_CAPTURER_WGC_H
