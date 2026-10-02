#include "game/Capture.h"

#include "core/Tool.h"
#include "game/Convert.h"

#include <Windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002 // Windows 8.1's, missing from older headers
#endif

namespace eluvian::game
{

namespace
{

using Microsoft::WRL::ComPtr;

// The process's largest visible top-level window: the game's own.
struct Search
{
    DWORD process = 0;
    HWND window = nullptr;
    long area = 0;
};

BOOL CALLBACK Consider(HWND window, LPARAM param)
{
    auto &search = *reinterpret_cast<Search *>(param);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != search.process || IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr)
    {
        return TRUE;
    }
    RECT client{};
    GetClientRect(window, &client);
    const long area = (client.right - client.left) * (client.bottom - client.top);
    if (area > search.area)
    {
        search.window = window;
        search.area = area;
    }
    return TRUE;
}

HWND GameWindow()
{
    Search search{GetCurrentProcessId()};
    EnumWindows(&Consider, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

void Check(HRESULT result, const char *what)
{
    if (FAILED(result))
    {
        throw ToolError(std::format("screenshot: {} failed ({:#010x})", what, static_cast<unsigned long>(result)));
    }
}

// COM on this thread for the call's length. A thread COM already knows in
// another mode (RPC_E_CHANGED_MODE) can use it as it is, and must not
// uninitialise what it did not initialise.
class ComScope
{
  public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }
    ~ComScope()
    {
        if (SUCCEEDED(result_))
        {
            CoUninitialize();
        }
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;

  private:
    HRESULT result_;
};

// A picture's source written as a JPEG of `quality`, with nothing but the
// picture in it.
std::string EncodeSource(IWICImagingFactory *factory, IWICBitmapSource *source, UINT width, UINT height, float quality)
{
    ComPtr<IWICFormatConverter> converter;
    Check(factory->CreateFormatConverter(&converter), "making a converter");
    Check(converter->Initialize(source, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom),
          "converting the pixels");
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "making a stream");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder), "making the JPEG encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "starting the encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    Check(encoder->CreateNewFrame(&frame, &options), "making the frame");
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = quality;
    Check(options->Write(1, &option, &value), "setting the quality");
    Check(frame->Initialize(options.Get()), "starting the frame");
    Check(frame->SetSize(width, height), "sizing the frame");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    Check(frame->SetPixelFormat(&format), "setting the frame's format");
    Check(frame->WriteSource(converter.Get(), nullptr), "writing the frame");
    Check(frame->Commit(), "finishing the frame");
    Check(encoder->Commit(), "finishing the JPEG");

    HGLOBAL memory = nullptr;
    Check(GetHGlobalFromStream(stream.Get(), &memory), "reading the JPEG back");
    STATSTG stat{};
    Check(stream->Stat(&stat, STATFLAG_NONAME), "measuring the JPEG");
    const auto *bytes = static_cast<const char *>(GlobalLock(memory));
    if (bytes == nullptr)
    {
        throw ToolError("screenshot: reading the JPEG back failed");
    }
    std::string jpeg(bytes, static_cast<std::size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(memory);
    return jpeg;
}

std::string EncodeJpeg(const std::vector<std::uint8_t> &bgra, int width, int height, float quality)
{
    const ComScope com;
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "making the image factory");
    ComPtr<IWICBitmap> bitmap;
    Check(factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height),
                                          GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(width) * 4,
                                          static_cast<UINT>(bgra.size()), const_cast<BYTE *>(bgra.data()), &bitmap),
          "taking the pixels");
    return EncodeSource(factory.Get(), bitmap.Get(), static_cast<UINT>(width), static_cast<UINT>(height), quality);
}

} // namespace

std::optional<std::string> CoveredAt(int x, int y)
{
    HWND game = GameWindow();
    if (game == nullptr)
    {
        return std::nullopt;
    }
    HWND at = WindowFromPoint(POINT{x, y});
    if (at == nullptr)
    {
        return std::string("nothing: the pixel is off the desktop");
    }
    HWND top = GetAncestor(at, GA_ROOT);
    if (top == game)
    {
        return std::nullopt;
    }
    std::wstring name(256, L'\0');
    name.resize(static_cast<std::size_t>(GetWindowTextW(top, name.data(), static_cast<int>(name.size()))));
    if (name.empty())
    {
        name.resize(256);
        name.resize(static_cast<std::size_t>(GetClassNameW(top, name.data(), static_cast<int>(name.size()))));
    }
    return Narrow(name);
}

WindowPlace GameWindowPlace()
{
    WindowPlace place;
    HWND window = GameWindow();
    if (window == nullptr)
    {
        return place;
    }
    place.found = true;
    place.minimised = IsIconic(window) != FALSE;
    place.foreground = GetForegroundWindow() == window;
    RECT client{};
    GetClientRect(window, &client);
    POINT origin{0, 0};
    ClientToScreen(window, &origin);
    place.x = origin.x;
    place.y = origin.y;
    place.width = client.right - client.left;
    place.height = client.bottom - client.top;
    return place;
}

Picture ReencodeJpeg(const std::string &jpeg, float quality)
{
    const ComScope com;
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "making the image factory");
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream), "making a stream");
    Check(stream->InitializeFromMemory(reinterpret_cast<BYTE *>(const_cast<char *>(jpeg.data())),
                                       static_cast<DWORD>(jpeg.size())),
          "reading the picture's bytes");
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder),
          "decoding the picture");
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame), "reading the picture's frame");
    UINT width = 0;
    UINT height = 0;
    Check(frame->GetSize(&width, &height), "measuring the picture");
    return Picture{EncodeSource(factory.Get(), frame.Get(), width, height, quality), static_cast<int>(width),
                   static_cast<int>(height)};
}

Picture CaptureGameWindow(float quality)
{
    HWND window = GameWindow();
    if (window == nullptr)
    {
        throw ToolError("screenshot: the game has no window to take yet");
    }
    if (IsIconic(window) != FALSE)
    {
        throw ToolError("screenshot: the game's window is minimised; restore it to take a picture");
    }
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        throw ToolError("screenshot: the game's window has no size");
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down, as the encoder reads rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    std::vector<std::uint8_t> pixels;
    HDC windowDc = GetDC(window);
    HDC memoryDc = windowDc != nullptr ? CreateCompatibleDC(windowDc) : nullptr;
    void *bits = nullptr;
    HBITMAP bitmap =
        memoryDc != nullptr ? CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bitmap != nullptr)
    {
        HGDIOBJ previous = SelectObject(memoryDc, bitmap);
        const BOOL drawn = PrintWindow(window, memoryDc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        GdiFlush();
        if (drawn != FALSE && bits != nullptr)
        {
            pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
            std::memcpy(pixels.data(), bits, pixels.size());
        }
        SelectObject(memoryDc, previous);
        DeleteObject(bitmap);
    }
    if (memoryDc != nullptr)
    {
        DeleteDC(memoryDc);
    }
    if (windowDc != nullptr)
    {
        ReleaseDC(window, windowDc);
    }
    if (pixels.empty())
    {
        throw ToolError("screenshot: Windows could not draw the game's window");
    }
    // Black all over, by a sample of the pixels (every 97th, so no column
    // or row is the only one read).
    bool black = true;
    for (std::size_t i = 0; black && i + 2 < pixels.size(); i += 4 * 97)
    {
        black = pixels[i] == 0 && pixels[i + 1] == 0 && pixels[i + 2] == 0;
    }
    return Picture{EncodeJpeg(pixels, width, height, quality), width, height, black};
}

} // namespace eluvian::game
