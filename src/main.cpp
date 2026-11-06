// HoverPreview: rest the mouse on a photo, video or song in File Explorer (or on the
// desktop) and a preview pops up next to the cursor. Move away and it disappears.

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlguid.h>
#include <servprov.h>
#include <uiautomation.h>
#include <wincodec.h>
#include <mfplay.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")

using Microsoft::WRL::ComPtr;

#define WM_APP_SHOW (WM_APP + 1)
#define WM_APP_HIDE (WM_APP + 2)
#define WM_APP_SETTINGS (WM_APP + 3)

enum { T_ANIM = 1, T_PROGRESS = 2, T_TRIM = 3 };

static const COLORREF kBg = RGB(32, 32, 32);
static const COLORREF kText = RGB(240, 240, 240);
static const COLORREF kSub = RGB(165, 165, 165);
static const COLORREF kAccent = RGB(76, 194, 255);
static const COLORREF kTrack = RGB(72, 72, 72);
static const COLORREF kTile = RGB(52, 52, 52);

static const int kDelays[] = { 150, 300, 700 };
static const int kSizes[] = { 400, 600, 850 };

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

static std::atomic<bool> g_enabled{ true };
static std::atomic<bool> g_videoSound{ true };
static std::atomic<int> g_delayMs{ 300 };
static std::atomic<int> g_maxSize{ 600 };

static const wchar_t kRegKey[] = L"Software\\HoverPreview";
static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kAppName[] = L"HoverPreview";

static DWORD RegReadDword(const wchar_t* name, DWORD def)
{
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v;
    return def;
}

static void RegWriteDword(const wchar_t* name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}

static void LoadSettings()
{
    g_enabled = RegReadDword(L"Enabled", 1) != 0;
    g_videoSound = RegReadDword(L"VideoSound", 1) != 0;
    g_delayMs = (int)std::clamp<DWORD>(RegReadDword(L"DelayMs", 300), 50, 3000);
    g_maxSize = (int)std::clamp<DWORD>(RegReadDword(L"MaxSize", 600), 200, 2000);
}

static void SaveSettings()
{
    RegWriteDword(L"Enabled", g_enabled ? 1 : 0);
    RegWriteDword(L"VideoSound", g_videoSound ? 1 : 0);
    RegWriteDword(L"DelayMs", (DWORD)g_delayMs.load());
    RegWriteDword(L"MaxSize", (DWORD)g_maxSize.load());
}

static std::wstring ExePath()
{
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, b, ARRAYSIZE(b));
    return std::wstring(b, n);
}

static bool AutostartEnabled()
{
    wchar_t b[2048];
    DWORD sz = sizeof(b);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, b, &sz) == ERROR_SUCCESS;
}

static void SetAutostart(bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\" --background";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------------------------
// Logging (only with --log; writes %TEMP%\HoverPreview.log)
// ---------------------------------------------------------------------------------------------

static bool g_log = false;

static void Log(const wchar_t* fmt, ...)
{
    if (!g_log)
        return;
    wchar_t buf[2048];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, a);
    va_end(a);
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"HoverPreview.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%llu %s\n", GetTickCount64(), buf);
        fclose(f);
    }
}

// ---------------------------------------------------------------------------------------------
// File types
// ---------------------------------------------------------------------------------------------

enum class Kind { None, Image, Video, Audio };

static const wchar_t* const kImageExt[] = {
    L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"gif", L"bmp", L"dib", L"tif", L"tiff", L"ico", L"webp",
    L"heic", L"heif", L"avif", L"jxr", L"wdp", L"dds", L"jxl",
    L"cr2", L"cr3", L"nef", L"arw", L"dng", L"orf", L"rw2", L"raf", L"srw", L"pef" };
static const wchar_t* const kVideoExt[] = {
    L"mp4", L"m4v", L"mov", L"wmv", L"avi", L"mkv", L"webm", L"3gp", L"3g2", L"mpg", L"mpeg", L"ts",
    L"m2ts", L"mts", L"asf" };
static const wchar_t* const kAudioExt[] = {
    L"mp3", L"wav", L"wma", L"m4a", L"aac", L"flac", L"ogg", L"oga", L"opus", L"aif", L"aiff", L"amr",
    L"ac3", L"mka" };

static const wchar_t* ExtOf(const std::wstring& p)
{
    size_t dot = p.find_last_of(L'.');
    size_t slash = p.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return nullptr;
    return p.c_str() + dot + 1;
}

static Kind KindOf(const std::wstring& p)
{
    const wchar_t* e = ExtOf(p);
    if (!e)
        return Kind::None;
    for (auto x : kImageExt) if (_wcsicmp(e, x) == 0) return Kind::Image;
    for (auto x : kVideoExt) if (_wcsicmp(e, x) == 0) return Kind::Video;
    for (auto x : kAudioExt) if (_wcsicmp(e, x) == 0) return Kind::Audio;
    return Kind::None;
}

static bool ClassIs(HWND h, const wchar_t* cls)
{
    wchar_t b[64];
    return h && GetClassNameW(h, b, ARRAYSIZE(b)) && wcscmp(b, cls) == 0;
}

// ---------------------------------------------------------------------------------------------
// Preview data handed from the detector thread to the UI thread
// ---------------------------------------------------------------------------------------------

struct Preview {
    Kind kind = Kind::None;
    std::wstring path;
    std::wstring fileName;
    POINT pt{};          // cursor position when the preview was requested
    RECT work{};         // work area of the monitor under the cursor
    int dpi = 96;
    SIZE box{};          // max content size in pixels
    SIZE src{};          // original image size (after EXIF rotation)
    ULONGLONG fileSize = 0;
    std::vector<HBITMAP> frames;   // decoded image (1 frame, or many for animated GIFs)
    std::vector<UINT> delays;      // ms per frame
    SIZE img{};
    HBITMAP cover = nullptr;       // album art for audio
    SIZE coverSize{};
    DWORD ownerPid = 0;            // Explorer process that owns the hovered window

    ~Preview()
    {
        for (HBITMAP b : frames) DeleteObject(b);
        if (cover) DeleteObject(cover);
    }
};

static SIZE FitSize(int w, int h, int bw, int bh, double maxUpscale)
{
    if (w <= 0 || h <= 0 || bw <= 0 || bh <= 0)
        return { 0, 0 };
    double s = std::min(maxUpscale, std::min((double)bw / w, (double)bh / h));
    return { std::max(1, (int)(w * s + 0.5)), std::max(1, (int)(h * s + 0.5)) };
}

static HBITMAP MakeDib(int w, int h, void** bits)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// Flatten premultiplied BGRA onto a subtle checkerboard so transparent images stay readable.
static void FlattenAlpha(UINT32* px, int w, int h, int cell)
{
    bool any = false;
    for (size_t i = 0, n = (size_t)w * h; i < n && !any; i++)
        any = (px[i] >> 24) != 255;
    if (!any)
        return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            UINT32& c = px[(size_t)y * w + x];
            UINT a = c >> 24;
            if (a == 255) continue;
            UINT bgc = (((x / cell) + (y / cell)) & 1) ? 58 : 44;
            UINT ia = 255 - a;
            UINT b = std::min<UINT>(255, (c & 0xFF) + bgc * ia / 255);
            UINT g = std::min<UINT>(255, ((c >> 8) & 0xFF) + bgc * ia / 255);
            UINT r = std::min<UINT>(255, ((c >> 16) & 0xFF) + bgc * ia / 255);
            c = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

static std::wstring FormatSize(ULONGLONG b)
{
    wchar_t s[64];
    if (b < 1024) swprintf_s(s, L"%llu B", b);
    else if (b < 1024ull * 1024) swprintf_s(s, L"%.0f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) swprintf_s(s, L"%.1f MB", b / (1024.0 * 1024));
    else swprintf_s(s, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    return s;
}

static std::wstring FormatTime(LONGLONG hns)
{
    LONGLONG t = std::max<LONGLONG>(0, hns / 10000000);
    wchar_t s[32];
    if (t >= 3600) swprintf_s(s, L"%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else swprintf_s(s, L"%lld:%02lld", t / 60, t % 60);
    return s;
}

// ---------------------------------------------------------------------------------------------
// Detector thread: watches the cursor, finds the file under it, decodes images
// ---------------------------------------------------------------------------------------------

static HWND g_main = nullptr;
static HWND g_popup = nullptr;
static HWND g_video = nullptr;
static HANDLE g_quit = nullptr;

struct HitItem {
    std::wstring name;
    RECT rect{};
    HWND hwnd = nullptr;
    HWND root = nullptr;
    bool desktop = false;
};

class Detector {
public:
    void Init();
    void Run();
    Preview* Create(const std::wstring& path, POINT pt);

private:
    ComPtr<IUIAutomation> uia_;
    ComPtr<IUIAutomationTreeWalker> walker_;
    ComPtr<IWICImagingFactory> wic_;

    bool HitTest(POINT pt, HitItem& out);
    bool ResolvePath(const HitItem& hit, std::wstring& path);
    bool ExplorerFolder(const HitItem& hit, std::wstring& folder, ComPtr<IFolderView>& view);
    Preview* Build(POINT pt);
    void ComputeBox(Preview* p);

    bool DecodeImage(Preview* p);
    bool DecodeGif(IWICBitmapDecoder* dec, UINT count, Preview* p);
    HBITMAP Render(IWICBitmapSource* src, UINT sw, UINT sh, UINT pw, UINT ph, WICBitmapTransformOptions xf,
                   int ow, int oh, int cell);
    UINT BitsPerPixel(REFWICPixelFormatGUID fmt);
};

static UINT MetaUInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT def)
{
    if (!q)
        return def;
    PROPVARIANT v;
    PropVariantInit(&v);
    UINT r = def;
    if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
        switch (v.vt) {
        case VT_UI1: r = v.bVal; break;
        case VT_UI2: r = v.uiVal; break;
        case VT_UI4: r = v.ulVal; break;
        case VT_I2: r = (UINT)v.iVal; break;
        case VT_I4: r = (UINT)v.lVal; break;
        }
    }
    PropVariantClear(&v);
    return r;
}

static int ReadOrientation(IWICBitmapFrameDecode* fr)
{
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(fr->GetMetadataQueryReader(&q)))
        return 1;
    const wchar_t* names[] = { L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}" };
    for (auto n : names) {
        UINT o = MetaUInt(q.Get(), n, 0);
        if (o >= 1 && o <= 8)
            return (int)o;
    }
    return 1;
}

static WICBitmapTransformOptions OrientationTransform(int o)
{
    switch (o) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

static HBITMAP ShellImage(const std::wstring& path, int size, bool thumbOnly, SIZE* outSize)
{
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))))
        return nullptr;
    HBITMAP b = nullptr;
    SIIGBF flags = SIIGBF_RESIZETOFIT;
    if (thumbOnly)
        flags = (SIIGBF)(flags | SIIGBF_THUMBNAILONLY);
    if (FAILED(f->GetImage({ size, size }, flags, &b)) || !b)
        return nullptr;
    BITMAP bm{};
    GetObjectW(b, sizeof(bm), &bm);
    if (outSize)
        *outSize = { bm.bmWidth, std::abs(bm.bmHeight) };
    return b;
}

UINT Detector::BitsPerPixel(REFWICPixelFormatGUID fmt)
{
    ComPtr<IWICComponentInfo> ci;
    ComPtr<IWICPixelFormatInfo> pfi;
    UINT bpp = 0;
    if (SUCCEEDED(wic_->CreateComponentInfo(fmt, &ci)) && SUCCEEDED(ci.As(&pfi)))
        pfi->GetBitsPerPixel(&bpp);
    return bpp;
}

HBITMAP Detector::Render(IWICBitmapSource* src, UINT sw, UINT sh, UINT pw, UINT ph, WICBitmapTransformOptions xf,
                         int ow, int oh, int cell)
{
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(wic_->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom)))
        return nullptr;
    ComPtr<IWICBitmapSource> cur = cv;
    if (pw != sw || ph != sh) {
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(wic_->CreateBitmapScaler(&sc)))
            return nullptr;
        if (FAILED(sc->Initialize(cur.Get(), pw, ph, WICBitmapInterpolationModeHighQualityCubic)) &&
            FAILED(sc->Initialize(cur.Get(), pw, ph, WICBitmapInterpolationModeFant)))
            return nullptr;
        cur = sc;
    }
    if (xf != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> fr;
        if (FAILED(wic_->CreateBitmapFlipRotator(&fr)) || FAILED(fr->Initialize(cur.Get(), xf)))
            return nullptr;
        cur = fr;
    }
    void* bits = nullptr;
    HBITMAP b = MakeDib(ow, oh, &bits);
    if (!b)
        return nullptr;
    if (FAILED(cur->CopyPixels(nullptr, ow * 4, (UINT)(ow * oh * 4), (BYTE*)bits))) {
        DeleteObject(b);
        return nullptr;
    }
    FlattenAlpha((UINT32*)bits, ow, oh, cell);
    return b;
}

bool Detector::DecodeImage(Preview* p)
{
    if (!wic_)
        return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic_->CreateDecoderFromFilename(p->path.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    double up = p->dpi / 96.0;
    int cell = std::max(4, MulDiv(8, p->dpi, 96));

    GUID cf{};
    dec->GetContainerFormat(&cf);
    UINT count = 0;
    dec->GetFrameCount(&count);
    if (cf == GUID_ContainerFormatGif && count > 1 && DecodeGif(dec.Get(), count, p))
        return true;

    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr)))
        return false;
    UINT sw = 0, sh = 0;
    fr->GetSize(&sw, &sh);
    if (!sw || !sh)
        return false;

    int o = ReadOrientation(fr.Get());
    bool swap = o >= 5 && o <= 8;
    int ow = swap ? (int)sh : (int)sw, oh = swap ? (int)sw : (int)sh;
    p->src = { ow, oh };
    SIZE t = FitSize(ow, oh, p->box.cx, p->box.cy, up);
    if (!t.cx)
        return false;
    UINT pw = swap ? t.cy : t.cx, ph = swap ? t.cx : t.cy;

    // Let the decoder downscale natively (JPEG can decode at 1/2, 1/4, 1/8 size): much faster.
    ComPtr<IWICBitmapSource> base = fr;
    UINT bw = sw, bh = sh;
    ComPtr<IWICBitmapSourceTransform> st;
    if (pw * 2 <= sw && SUCCEEDED(fr.As(&st))) {
        UINT cw = pw, ch = ph;
        if (SUCCEEDED(st->GetClosestSize(&cw, &ch)) && (cw < pw || ch < ph)) {
            cw = pw * 2;
            ch = ph * 2;
            st->GetClosestSize(&cw, &ch);
        }
        if (cw >= pw && ch >= ph && cw < sw) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            if (SUCCEEDED(st->GetClosestPixelFormat(&fmt))) {
                UINT bpp = BitsPerPixel(fmt);
                if (bpp) {
                    UINT stride = ((cw * bpp + 7) / 8 + 3) & ~3u;
                    std::vector<BYTE> buf((size_t)stride * ch);
                    ComPtr<IWICBitmap> bmp;
                    if (SUCCEEDED(st->CopyPixels(nullptr, cw, ch, &fmt, WICBitmapTransformRotate0, stride,
                                                 (UINT)buf.size(), buf.data())) &&
                        SUCCEEDED(wic_->CreateBitmapFromMemory(cw, ch, fmt, stride, (UINT)buf.size(), buf.data(),
                                                               &bmp))) {
                        base = bmp;
                        bw = cw;
                        bh = ch;
                    }
                }
            }
        }
    }

    HBITMAP b = Render(base.Get(), bw, bh, pw, ph, OrientationTransform(o), t.cx, t.cy, cell);
    if (!b)
        return false;
    p->frames.push_back(b);
    p->delays.push_back(0);
    p->img = t;
    return true;
}

bool Detector::DecodeGif(IWICBitmapDecoder* dec, UINT count, Preview* p)
{
    ComPtr<IWICMetadataQueryReader> gq;
    dec->GetMetadataQueryReader(&gq);
    UINT cw = MetaUInt(gq.Get(), L"/logscrw", 0), ch = MetaUInt(gq.Get(), L"/logscrh", 0);
    if (!cw || !ch) {
        ComPtr<IWICBitmapFrameDecode> f0;
        if (FAILED(dec->GetFrame(0, &f0)))
            return false;
        f0->GetSize(&cw, &ch);
    }
    if (!cw || !ch || (UINT64)cw * ch > 40000000ull)
        return false;
    SIZE t = FitSize((int)cw, (int)ch, p->box.cx, p->box.cy, p->dpi / 96.0);
    if (!t.cx)
        return false;
    int cell = std::max(4, MulDiv(8, p->dpi, 96));
    UINT64 perFrame = (UINT64)t.cx * t.cy * 4;
    UINT maxFrames = (UINT)std::clamp<UINT64>((48ull << 20) / perFrame, 1, 300);
    count = std::min(count, maxFrames);

    std::vector<UINT32> canvas((size_t)cw * ch, 0), saved;
    for (UINT i = 0; i < count; i++) {
        ComPtr<IWICBitmapFrameDecode> f;
        if (FAILED(dec->GetFrame(i, &f)))
            break;
        ComPtr<IWICMetadataQueryReader> q;
        f->GetMetadataQueryReader(&q);
        UINT fl = MetaUInt(q.Get(), L"/imgdesc/Left", 0);
        UINT ft = MetaUInt(q.Get(), L"/imgdesc/Top", 0);
        UINT delay = MetaUInt(q.Get(), L"/grctlext/Delay", 10);
        UINT disposal = MetaUInt(q.Get(), L"/grctlext/Disposal", 0);
        UINT fw = 0, fh = 0;
        f->GetSize(&fw, &fh);

        ComPtr<IWICFormatConverter> cv;
        if (FAILED(wic_->CreateFormatConverter(&cv)) ||
            FAILED(cv->Initialize(f.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                  WICBitmapPaletteTypeCustom)))
            break;
        std::vector<UINT32> fp((size_t)fw * fh);
        if (FAILED(cv->CopyPixels(nullptr, fw * 4, (UINT)(fp.size() * 4), (BYTE*)fp.data())))
            break;

        if (disposal == 3)
            saved = canvas;
        for (UINT y = 0; y < fh && ft + y < ch; y++)
            for (UINT x = 0; x < fw && fl + x < cw; x++) {
                UINT32 px = fp[(size_t)y * fw + x];
                if (px >> 24)
                    canvas[(size_t)(ft + y) * cw + fl + x] = px;
            }

        ComPtr<IWICBitmap> cb;
        if (FAILED(wic_->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppPBGRA, cw * 4,
                                                (UINT)(canvas.size() * 4), (BYTE*)canvas.data(), &cb)))
            break;
        HBITMAP b = Render(cb.Get(), cw, ch, t.cx, t.cy, WICBitmapTransformRotate0, t.cx, t.cy, cell);
        if (!b)
            break;
        p->frames.push_back(b);
        p->delays.push_back(delay < 2 ? 100 : delay * 10);

        if (disposal == 2) {
            for (UINT y = 0; y < fh && ft + y < ch; y++)
                for (UINT x = 0; x < fw && fl + x < cw; x++)
                    canvas[(size_t)(ft + y) * cw + fl + x] = 0;
        } else if (disposal == 3 && !saved.empty()) {
            canvas.swap(saved);
        }
    }
    if (p->frames.empty())
        return false;
    p->img = t;
    p->src = { (LONG)cw, (LONG)ch };
    return true;
}

bool Detector::HitTest(POINT pt, HitItem& out)
{
    HWND h = WindowFromPoint(pt);
    if (!h)
        return false;
    HWND root = GetAncestor(h, GA_ROOT);
    bool desktop = ClassIs(h, L"SysListView32") && (ClassIs(root, L"Progman") || ClassIs(root, L"WorkerW"));
    bool explorer = ClassIs(h, L"DirectUIHWND") && ClassIs(root, L"CabinetWClass");
    if (!desktop && !explorer)
        return false;
    if (!uia_)
        return false;

    ComPtr<IUIAutomationElement> el;
    if (FAILED(uia_->ElementFromPoint(pt, &el)) || !el)
        return false;
    CONTROLTYPEID ct = 0;
    for (int i = 0; i < 4 && el; i++) {
        el->get_CurrentControlType(&ct);
        if (ct == UIA_ListItemControlTypeId)
            break;
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker_->GetParentElement(el.Get(), &parent)))
            parent.Reset();
        el = parent;
    }
    if (!el || ct != UIA_ListItemControlTypeId)
        return false;

    BSTR name = nullptr;
    if (FAILED(el->get_CurrentName(&name)) || !name)
        return false;
    out.name.assign(name, SysStringLen(name));
    SysFreeString(name);
    if (out.name.empty())
        return false;
    el->get_CurrentBoundingRectangle(&out.rect);
    out.hwnd = h;
    out.root = root;
    out.desktop = desktop;
    return true;
}

bool Detector::ExplorerFolder(const HitItem& hit, std::wstring& folder, ComPtr<IFolderView>& view)
{
    HWND tab = hit.hwnd;
    while (tab && !ClassIs(tab, L"ShellTabWindowClass"))
        tab = GetParent(tab);

    ComPtr<IShellWindows> sw;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw))))
        return false;
    long n = 0;
    sw->get_Count(&n);
    for (long i = 0; i < n; i++) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        ComPtr<IDispatch> d;
        if (FAILED(sw->Item(v, &d)) || !d)
            continue;
        ComPtr<IWebBrowserApp> wb;
        if (FAILED(d.As(&wb)))
            continue;
        SHANDLE_PTR hw = 0;
        if (FAILED(wb->get_HWND(&hw)) || (HWND)hw != hit.root)
            continue;
        ComPtr<IServiceProvider> sp;
        ComPtr<IShellBrowser> sb;
        if (FAILED(d.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))))
            continue;
        if (tab) {
            HWND th = nullptr;
            if (FAILED(sb->GetWindow(&th)) || th != tab)
                continue;   // another tab of the same window
        }
        ComPtr<IShellView> sv;
        ComPtr<IFolderView> fv;
        if (FAILED(sb->QueryActiveShellView(&sv)) || FAILED(sv.As(&fv)))
            continue;
        view = fv;
        ComPtr<IPersistFolder2> pf;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->GetCurFolder(&pidl)) && pidl) {
            wchar_t buf[2048];
            if (SHGetPathFromIDListEx(pidl, buf, ARRAYSIZE(buf), GPFIDL_DEFAULT))
                folder = buf;
            CoTaskMemFree(pidl);
        }
        return true;
    }
    return false;
}

static bool ResolveInFolder(const std::wstring& folder, const std::wstring& name, std::wstring& out)
{
    if (folder.empty())
        return false;
    std::wstring base = folder;
    if (base.back() != L'\\')
        base += L'\\';
    std::wstring exact = base + name;
    DWORD a = GetFileAttributesW(exact.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && KindOf(exact) != Kind::None) {
        out = exact;
        return true;
    }
    if (a != INVALID_FILE_ATTRIBUTES)
        return false;   // it's a folder or an unsupported file

    // Explorer hides known extensions by default: look for "name.<ext>".
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((base + name + L".*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::wstring fn = fd.cFileName;
        size_t dot = fn.find_last_of(L'.');
        if (dot == std::wstring::npos || _wcsicmp(fn.substr(0, dot).c_str(), name.c_str()) != 0)
            continue;
        if (KindOf(fn) != Kind::None) {
            out = base + fn;
            found = true;
        }
    } while (!found && FindNextFileW(f, &fd));
    FindClose(f);
    return found;
}

// For views that aren't a plain folder (search results, libraries, Home, Gallery) ask the view itself.
static bool ResolveInView(IFolderView* fv, const std::wstring& name, std::wstring& out)
{
    ComPtr<IFolderView2> fv2;
    if (!fv || FAILED(fv->QueryInterface(IID_PPV_ARGS(&fv2))))
        return false;
    int count = 0;
    if (FAILED(fv2->ItemCount(SVGIO_ALLVIEW, &count)))
        return false;
    count = std::min(count, 3000);
    for (int i = 0; i < count; i++) {
        ComPtr<IShellItem> si;
        if (FAILED(fv2->GetItem(i, IID_PPV_ARGS(&si))))
            continue;
        PWSTR dn = nullptr;
        bool match = false;
        if (SUCCEEDED(si->GetDisplayName(SIGDN_PARENTRELATIVE, &dn)) && dn) {
            match = _wcsicmp(dn, name.c_str()) == 0;
            CoTaskMemFree(dn);
        }
        if (!match)
            continue;
        PWSTR fs = nullptr;
        if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &fs)) && fs) {
            out = fs;
            CoTaskMemFree(fs);
            if (KindOf(out) != Kind::None)
                return true;
        }
    }
    return false;
}

bool Detector::ResolvePath(const HitItem& hit, std::wstring& path)
{
    if (hit.desktop) {
        const KNOWNFOLDERID* ids[] = { &FOLDERID_Desktop, &FOLDERID_PublicDesktop };
        for (auto id : ids) {
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &p)) && p) {
                std::wstring folder = p;
                CoTaskMemFree(p);
                if (ResolveInFolder(folder, hit.name, path))
                    return true;
            }
        }
        return false;
    }
    std::wstring folder;
    ComPtr<IFolderView> view;
    if (!ExplorerFolder(hit, folder, view))
        return false;
    if (ResolveInFolder(folder, hit.name, path))
        return true;
    if (folder.empty())
        return ResolveInView(view.Get(), hit.name, path);
    return false;
}

void Detector::ComputeBox(Preview* p)
{
    HMONITOR mon = MonitorFromPoint(p->pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    p->work = mi.rcWork;
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)))
        p->dpi = (int)dx;
    auto S = [&](int v) { return MulDiv(v, p->dpi, 96); };

    int maxS = S(g_maxSize);
    int off = S(20), m = S(8), pad = S(6), footer = S(30);
    const RECT& wa = p->work;
    int right = wa.right - (p->pt.x + off) - m;
    int left = (p->pt.x - off) - wa.left - m;
    int side = std::max(right, left);
    if (side >= S(260)) {
        p->box.cx = std::min(maxS, side - 2 * pad);
        p->box.cy = std::min(maxS, (int)(wa.bottom - wa.top) - 2 * m - pad - footer);
    } else {
        int below = wa.bottom - (p->pt.y + off) - m;
        int above = (p->pt.y - off) - wa.top - m;
        p->box.cx = std::min(maxS, (int)(wa.right - wa.left) - 2 * m - 2 * pad);
        p->box.cy = std::min(maxS, std::max(below, above) - pad - footer);
    }
    p->box.cx = std::max<LONG>(p->box.cx, S(120));
    p->box.cy = std::max<LONG>(p->box.cy, S(90));
}

Preview* Detector::Build(POINT pt)
{
    ULONGLONG t0 = GetTickCount64();
    HitItem hit;
    if (!HitTest(pt, hit))
        return nullptr;
    std::wstring path;
    if (!ResolvePath(hit, path)) {
        Log(L"no media file for \"%s\"", hit.name.c_str());
        return nullptr;
    }
    Preview* p = Create(path, pt);
    if (!p)
        return nullptr;
    GetWindowThreadProcessId(hit.root, &p->ownerPid);
    Log(L"preview %s (%llu ms)", path.c_str(), GetTickCount64() - t0);
    // The decode could have taken a moment; drop it if the mouse already left the item.
    POINT now;
    GetCursorPos(&now);
    if (!PtInRect(&hit.rect, now)) {
        delete p;
        return nullptr;
    }
    return p;
}

Preview* Detector::Create(const std::wstring& path, POINT pt)
{
    Kind k = KindOf(path);
    if (k == Kind::None)
        return nullptr;
    Preview* p = new Preview;
    p->kind = k;
    p->path = path;
    size_t slash = path.find_last_of(L'\\');
    p->fileName = slash == std::wstring::npos ? path : path.substr(slash + 1);
    p->pt = pt;
    ComputeBox(p);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        p->fileSize = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    if (k == Kind::Image) {
        if (!DecodeImage(p)) {
            SIZE sz{};
            HBITMAP b = ShellImage(path, std::max(p->box.cx, p->box.cy), true, &sz);
            if (!b) {
                Log(L"cannot decode %s", path.c_str());
                delete p;
                return nullptr;
            }
            p->frames.push_back(b);
            p->delays.push_back(0);
            p->img = sz;
        }
    } else if (k == Kind::Audio) {
        p->cover = ShellImage(path, MulDiv(128, p->dpi, 96), true, &p->coverSize);
    }
    return p;
}

static bool CursorOverPopup(POINT pt)
{
    HWND h = WindowFromPoint(pt);
    return h && GetAncestor(h, GA_ROOT) == g_popup;
}

void Detector::Init()
{
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia_));
    if (uia_)
        uia_->get_RawViewWalker(&walker_);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
}

void Detector::Run()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Init();

    POINT last{ LONG_MIN, LONG_MIN };
    ULONGLONG stillSince = 0, lastRecheck = 0, hiddenAt = 0;
    bool checked = false, shown = false;
    RECT shownRect{};
    std::wstring shownName;

    auto hide = [&](ULONGLONG now) {
        PostMessageW(g_main, WM_APP_HIDE, 0, 0);
        shown = false;
        hiddenAt = now;
        stillSince = now;
        checked = false;
    };

    for (;;) {
        DWORD r = MsgWaitForMultipleObjectsEx(1, &g_quit, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0)
            break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        ULONGLONG now = GetTickCount64();
        POINT pt;
        if (!GetCursorPos(&pt))
            continue;
        bool buttons = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
                         GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
        bool moved = std::abs(pt.x - last.x) > 2 || std::abs(pt.y - last.y) > 2;
        if (moved) {
            last = pt;
            stillSince = now;
            checked = false;
        }

        if (!g_enabled) {
            if (shown)
                hide(now);
            continue;
        }

        if (shown) {
            bool gone = buttons || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) || !PtInRect(&shownRect, pt) ||
                        CursorOverPopup(pt);
            if (!gone && !moved && now - lastRecheck >= 400) {
                // The list may have scrolled under a resting cursor.
                lastRecheck = now;
                HitItem hit;
                if (!HitTest(pt, hit) || hit.name != shownName)
                    gone = true;
                else
                    shownRect = hit.rect;
            }
            if (gone)
                hide(now);
            continue;
        }

        if (checked || buttons)
            continue;
        // Right after a preview closed, the next one opens faster so you can glide over files.
        int delay = g_delayMs;
        if (now - hiddenAt < 1500)
            delay = std::min(delay, 100);
        if (now - stillSince < (ULONGLONG)delay)
            continue;
        checked = true;

        HitItem probe;
        if (!HitTest(pt, probe))
            continue;
        Preview* p = Build(pt);
        if (!p)
            continue;
        shown = true;
        shownRect = probe.rect;
        shownName = probe.name;
        lastRecheck = GetTickCount64();
        if (!PostMessageW(g_main, WM_APP_SHOW, 0, (LPARAM)p)) {
            delete p;
            shown = false;
        }
    }

    walker_.Reset();
    uia_.Reset();
    wic_.Reset();
    CoUninitialize();
}

static DWORD WINAPI DetectorThread(LPVOID)
{
    Detector d;
    d.Run();
    return 0;
}

// --test <file>: show one preview next to the cursor for a few seconds, then exit.
static std::wstring g_testFile;

static DWORD WINAPI TestThread(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        Detector d;
        d.Init();
        POINT pt;
        GetCursorPos(&pt);
        ULONGLONG t0 = GetTickCount64();
        Preview* p = d.Create(g_testFile, pt);
        Log(L"test %s -> %s (%llu ms)", g_testFile.c_str(), p ? L"ok" : L"FAILED", GetTickCount64() - t0);
        if (p && !PostMessageW(g_main, WM_APP_SHOW, 0, (LPARAM)p))
            delete p;
    }
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// UI thread: preview window and media playback
// ---------------------------------------------------------------------------------------------

static Preview* g_cur = nullptr;
static IMFPMediaPlayer* g_player = nullptr;
static bool g_card = false;          // compact card layout (audio, errors)
static bool g_hasVideo = false;
static SIZE g_content{};
static UINT g_frame = 0;
static LONGLONG g_duration = 0;
static std::wstring g_error;
static int g_fontDpi = 0;
static HFONT g_fName = nullptr, g_fInfo = nullptr, g_fIcon = nullptr;

static int S(int v) { return MulDiv(v, g_cur ? g_cur->dpi : 96, 96); }

static void EnsureFonts(int dpi)
{
    if (dpi == g_fontDpi)
        return;
    if (g_fName) DeleteObject(g_fName);
    if (g_fInfo) DeleteObject(g_fInfo);
    if (g_fIcon) DeleteObject(g_fIcon);
    g_fName = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fInfo = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fIcon = CreateFontW(-MulDiv(28, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
    g_fontDpi = dpi;
}

static RECT ContentRect()
{
    RECT rc;
    GetClientRect(g_popup, &rc);
    int x = (rc.right - g_content.cx) / 2;
    return { x, S(6), x + g_content.cx, S(6) + g_content.cy };
}

static void PlaceAndShow(int W, int H)
{
    const Preview* p = g_cur;
    const RECT& wa = p->work;
    int off = S(20), m = S(8);
    auto clampi = [](int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); };
    int x, y;
    if (p->pt.x + off + W <= wa.right - m) {
        x = p->pt.x + off;
        y = clampi(p->pt.y - H / 2, wa.top + m, wa.bottom - m - H);
    } else if (p->pt.x - off - W >= wa.left + m) {
        x = p->pt.x - off - W;
        y = clampi(p->pt.y - H / 2, wa.top + m, wa.bottom - m - H);
    } else {
        x = clampi(p->pt.x - W / 2, wa.left + m, wa.right - m - W);
        y = (p->pt.y + off + H <= wa.bottom - m) ? p->pt.y + off : p->pt.y - off - H;
        y = clampi(y, wa.top + m, wa.bottom - m - H);
    }
    SetWindowPos(g_popup, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_popup, nullptr, FALSE);
}

static void LayoutMedia()
{
    g_card = false;
    int W = std::max<int>(g_content.cx + 2 * S(6), S(280));
    int H = g_content.cy + S(6) + S(30);
    PlaceAndShow(W, H);
    if (g_hasVideo) {
        RECT cr = ContentRect();
        SetWindowPos(g_video, nullptr, cr.left, cr.top, g_content.cx, g_content.cy,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (g_player)
            g_player->UpdateVideo();
    } else {
        ShowWindow(g_video, SW_HIDE);
    }
}

static void LayoutCard()
{
    g_card = true;
    g_hasVideo = false;
    ShowWindow(g_video, SW_HIDE);
    PlaceAndShow(S(400), S(92));
}

// Explorer pops its own "Type / Size / Date" tooltip after a moment, on top of the preview.
// While a preview is visible, close those tooltips the instant Explorer shows them.
static HWINEVENTHOOK g_tipHook = nullptr;

static void HideExplorerTip(HWND h)
{
    if (!ClassIs(h, L"tooltips_class32") || !IsWindowVisible(h))
        return;
    PostMessageW(h, WM_USER + 28 /* TTM_POP */, 0, 0);
    ShowWindowAsync(h, SW_HIDE);
    Log(L"closed explorer tooltip");
}

static void CALLBACK TipEventProc(HWINEVENTHOOK, DWORD, HWND h, LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && g_cur)
        HideExplorerTip(h);
}

static void StartTipBlocker(DWORD pid)
{
    if (!pid || g_tipHook)
        return;
    g_tipHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, TipEventProc, pid, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // One may already be on screen (long hover delay).
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"tooltips_class32", nullptr)) != nullptr;) {
        DWORD owner = 0;
        GetWindowThreadProcessId(h, &owner);
        if (owner == pid)
            HideExplorerTip(h);
    }
}

static void StopTipBlocker()
{
    if (g_tipHook) {
        UnhookWinEvent(g_tipHook);
        g_tipHook = nullptr;
    }
}

static void StopPlayer()
{
    if (g_player) {
        IMFPMediaPlayer* p = g_player;
        g_player = nullptr;
        p->Shutdown();
        p->Release();
    }
}

static void HidePreview()
{
    StopTipBlocker();
    KillTimer(g_popup, T_ANIM);
    KillTimer(g_popup, T_PROGRESS);
    ShowWindow(g_popup, SW_HIDE);
    ShowWindow(g_video, SW_HIDE);
    StopPlayer();
    if (g_cur) {
        delete g_cur;
        g_cur = nullptr;
        SetTimer(g_main, T_TRIM, 10000, nullptr);
    }
    g_hasVideo = false;
    g_error.clear();
    g_duration = 0;
}

static void ShowError(const wchar_t* msg)
{
    StopPlayer();
    KillTimer(g_popup, T_PROGRESS);
    g_error = msg;
    LayoutCard();
}

class PlayerCallback : public IMFPMediaPlayerCallback {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* h) override;
};
static PlayerCallback g_playerCb;

void PlayerCallback::OnMediaPlayerEvent(MFP_EVENT_HEADER* h)
{
    if (!g_player || !g_cur || h->pMediaPlayer != g_player)
        return;
    switch (h->eEventType) {
    case MFP_EVENT_TYPE_MEDIAITEM_SET: {
        if (FAILED(h->hrEvent)) {
            ShowError(L"Windows can't play this file");
            return;
        }
        BOOL hasVideo = FALSE, selected = FALSE;
        MFP_MEDIAITEM_SET_EVENT* e = MFP_GET_MEDIAITEM_SET_EVENT(h);
        if (e && e->pMediaItem)
            e->pMediaItem->HasVideo(&hasVideo, &selected);
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(g_player->GetDuration(GUID_NULL, &pv)))
            g_duration = (pv.vt == VT_UI8 || pv.vt == VT_I8) ? (LONGLONG)pv.uhVal.QuadPart : 0;
        PropVariantClear(&pv);

        SIZE vs{}, ar{};
        if (g_cur->kind == Kind::Video && hasVideo && selected &&
            SUCCEEDED(g_player->GetNativeVideoSize(&vs, &ar)) && vs.cx > 0 && vs.cy > 0) {
            int dispW = vs.cx;
            if (ar.cx > 0 && ar.cy > 0)
                dispW = MulDiv(vs.cy, ar.cx, ar.cy);
            g_cur->src = { dispW, vs.cy };
            g_content = FitSize(dispW, vs.cy, g_cur->box.cx, g_cur->box.cy, g_cur->dpi / 96.0);
            g_hasVideo = true;
            if (!g_videoSound)
                g_player->SetMute(TRUE);
            LayoutMedia();
        } else {
            LayoutCard();
        }
        g_player->Play();
        SetTimer(g_popup, T_PROGRESS, 250, nullptr);
        break;
    }
    case MFP_EVENT_TYPE_PLAYBACK_ENDED: {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = 0;
        g_player->SetPosition(GUID_NULL, &pv);
        g_player->Play();
        break;
    }
    case MFP_EVENT_TYPE_ERROR:
        if (FAILED(h->hrEvent))
            ShowError(L"Windows can't play this file");
        break;
    default:
        break;
    }
}

static void ShowPreview(Preview* p)
{
    HidePreview();
    KillTimer(g_main, T_TRIM);
    g_cur = p;
    g_frame = 0;
    EnsureFonts(p->dpi);
    StartTipBlocker(p->ownerPid);

    if (p->kind == Kind::Image) {
        g_content = p->img;
        g_hasVideo = false;
        LayoutMedia();
        if (p->frames.size() > 1)
            SetTimer(g_popup, T_ANIM, p->delays[0], nullptr);
        return;
    }

    HWND target = p->kind == Kind::Video ? g_video : nullptr;
    HRESULT hr = MFPCreateMediaPlayer(p->path.c_str(), FALSE, 0, &g_playerCb, target, &g_player);
    if (FAILED(hr) || !g_player) {
        g_player = nullptr;
        ShowError(L"Windows can't play this file");
        return;
    }
    if (p->kind == Kind::Audio)
        LayoutCard();   // show the card right away; playback starts when the file is opened
}

static LONGLONG PlayerPosition()
{
    if (!g_player)
        return 0;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    LONGLONG r = 0;
    if (SUCCEEDED(g_player->GetPosition(GUID_NULL, &pv)) && (pv.vt == VT_I8 || pv.vt == VT_UI8))
        r = pv.hVal.QuadPart;
    PropVariantClear(&pv);
    return r;
}

static void FillRectColor(HDC dc, const RECT& r, COLORREF c)
{
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void DrawText1(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT flags)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
}

static int TextWidth(HDC dc, const std::wstring& s, HFONT f)
{
    SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    return sz.cx;
}

static std::wstring ExtUpper(const std::wstring& path)
{
    const wchar_t* e = ExtOf(path);
    std::wstring s = e ? e : L"";
    for (auto& ch : s) ch = (wchar_t)towupper(ch);
    return s;
}

static void PaintMedia(HDC dc, const RECT& rc)
{
    RECT cr = ContentRect();
    if (!g_hasVideo && g_cur->kind == Kind::Image && !g_cur->frames.empty()) {
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(src, g_cur->frames[std::min<size_t>(g_frame, g_cur->frames.size() - 1)]);
        BitBlt(dc, cr.left, cr.top, g_content.cx, g_content.cy, src, 0, 0, SRCCOPY);
        SelectObject(src, old);
        DeleteDC(src);
    }

    std::wstring info;
    wchar_t dim[64];
    if (g_cur->src.cx > 0) {
        swprintf_s(dim, L"%ld × %ld", g_cur->src.cx, g_cur->src.cy);
        info = dim;
    }
    if (g_hasVideo) {
        info += L"  ·  " + FormatTime(PlayerPosition()) + L" / " + FormatTime(g_duration);
        if (!g_videoSound)
            info += L"  ·  muted";
    }
    if (g_cur->fileSize)
        info += (info.empty() ? L"" : L"  ·  ") + FormatSize(g_cur->fileSize);

    int left = S(10), right = rc.right - S(10);
    RECT footer = { left, cr.bottom, right, rc.bottom };
    if (g_hasVideo && g_duration > 0) {
        int w = cr.right - cr.left;
        int filled = (int)((double)w * std::clamp<double>((double)PlayerPosition() / g_duration, 0, 1));
        RECT track = { cr.left, cr.bottom + S(3), cr.right, cr.bottom + S(3) + std::max(2, S(2)) };
        FillRectColor(dc, track, kTrack);
        track.right = track.left + filled;
        FillRectColor(dc, track, kAccent);
        footer.top += S(3);
    }
    int iw = std::min(TextWidth(dc, info, g_fInfo), (right - left) / 2);
    RECT ri = { right - iw, footer.top, right, footer.bottom };
    DrawText1(dc, info, ri, g_fInfo, kSub, DT_RIGHT | DT_END_ELLIPSIS);
    RECT rn = { left, footer.top, right - iw - S(12), footer.bottom };
    DrawText1(dc, g_cur->fileName, rn, g_fName, kText, DT_LEFT | DT_END_ELLIPSIS);
}

static void PaintCard(HDC dc, const RECT& rc)
{
    int tile = S(64);
    RECT tr = { S(14), (rc.bottom - tile) / 2, S(14) + tile, (rc.bottom - tile) / 2 + tile };
    if (g_cur->cover && g_error.empty()) {
        SIZE f = FitSize(g_cur->coverSize.cx, g_cur->coverSize.cy, tile, tile, 100.0);
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(src, g_cur->cover);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, tr.left + (tile - f.cx) / 2, tr.top + (tile - f.cy) / 2, f.cx, f.cy, src, 0, 0,
                   g_cur->coverSize.cx, g_cur->coverSize.cy, SRCCOPY);
        SelectObject(src, old);
        DeleteDC(src);
    } else {
        SetDCBrushColor(dc, kTile);
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, tr.left, tr.top, tr.right + 1, tr.bottom + 1, S(12), S(12));
        DrawText1(dc, g_error.empty() ? L"" : L"", tr, g_fIcon, g_error.empty() ? kAccent : RGB(255, 185, 0),
                  DT_CENTER);
    }

    int x0 = tr.right + S(14), x1 = rc.right - S(16);
    RECT rn = { x0, S(14), x1, S(36) };
    DrawText1(dc, g_cur->fileName, rn, g_fName, kText, DT_LEFT | DT_END_ELLIPSIS);

    std::wstring info = g_error.empty() ? ExtUpper(g_cur->path) : g_error;
    if (g_error.empty() && g_cur->fileSize)
        info += L"  ·  " + FormatSize(g_cur->fileSize);
    RECT ri = { x0, S(36), x1, S(54) };
    DrawText1(dc, info, ri, g_fInfo, kSub, DT_LEFT | DT_END_ELLIPSIS);

    if (g_error.empty()) {
        LONGLONG pos = PlayerPosition();
        std::wstring t = FormatTime(pos) + L" / " + FormatTime(g_duration);
        int tw = TextWidth(dc, t, g_fInfo);
        int y = S(68);
        RECT rt = { x1 - tw, y - S(9), x1, y + S(9) };
        DrawText1(dc, t, rt, g_fInfo, kSub, DT_RIGHT);
        RECT track = { x0, y - S(2), x1 - tw - S(12), y + S(2) };
        FillRectColor(dc, track, kTrack);
        if (g_duration > 0) {
            track.right = track.left + (int)((track.right - track.left) *
                                             std::clamp<double>((double)pos / g_duration, 0, 1));
            FillRectColor(dc, track, kAccent);
        }
    }
}

static void PaintPopup(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRectColor(mem, rc, kBg);
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(mem, g_fInfo ? g_fInfo : GetStockObject(DEFAULT_GUI_FONT));
    if (g_cur) {
        if (g_card) PaintCard(mem, rc);
        else PaintMedia(mem, rc);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: return 0;
    case WM_PAINT: PaintPopup(hwnd); return 0;
    case WM_TIMER:
        if (wp == T_ANIM && g_cur && g_cur->frames.size() > 1) {
            g_frame = (g_frame + 1) % (UINT)g_cur->frames.size();
            RECT cr = ContentRect();
            InvalidateRect(hwnd, &cr, FALSE);
            SetTimer(hwnd, T_ANIM, std::max<UINT>(20, g_cur->delays[g_frame]), nullptr);
        } else if (wp == T_PROGRESS) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK VideoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_player && g_hasVideo)
            g_player->UpdateVideo();
        else
            FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Explorer right-click menu entry (under "Show more options" on Windows 11)
// ---------------------------------------------------------------------------------------------

static const wchar_t* const kMenuKeys[] = {
    L"Software\\Classes\\Directory\\Background\\shell\\HoverPreview",
    L"Software\\Classes\\DesktopBackground\\shell\\HoverPreview" };

static void SetRegString(const std::wstring& key, const wchar_t* name, const std::wstring& v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static bool ExplorerMenuEnabled()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kMenuKeys[0], 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

static void SetExplorerMenu(bool on)
{
    std::wstring exe = ExePath();
    for (auto key : kMenuKeys) {
        if (on) {
            SetRegString(key, nullptr, L"HoverPreview settings");
            SetRegString(key, L"Icon", exe + L",0");
            SetRegString(std::wstring(key) + L"\\command", nullptr, L"\"" + exe + L"\"");
        } else {
            RegDeleteTreeW(HKEY_CURRENT_USER, key);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Settings window (opened by running HoverPreview again, or from Explorer's right-click menu)
// ---------------------------------------------------------------------------------------------

enum { SI_NONE = -1, SI_ENABLED, SI_DELAY0, SI_DELAY1, SI_DELAY2, SI_SIZE0, SI_SIZE1, SI_SIZE2, SI_SOUND,
       SI_AUTOSTART, SI_MENU, SI_QUIT, SI_CLOSE };

struct SetHit {
    RECT r;
    int id;
};

struct SetRow {
    const wchar_t* label;
    const wchar_t* sub;
    int id;
    const wchar_t* const* segments;
};

static const wchar_t* const kDelayNames[] = { L"Fast", L"Normal", L"Relaxed" };
static const wchar_t* const kSizeNames[] = { L"Small", L"Medium", L"Large" };
static const SetRow kRows[] = {
    { L"Previews", L"Show a preview when the mouse rests on a file", SI_ENABLED, nullptr },
    { L"Hover delay", nullptr, SI_DELAY0, kDelayNames },
    { L"Preview size", nullptr, SI_SIZE0, kSizeNames },
    { L"Play sound in videos", nullptr, SI_SOUND, nullptr },
    { L"Start with Windows", L"Runs quietly in the background", SI_AUTOSTART, nullptr },
    { L"Explorer right-click menu", L"Listed under “Show more options”", SI_MENU, nullptr },
};

static const int kSetW = 460, kSetH = 580;
static const COLORREF kWinBg = RGB(32, 32, 32);
static const COLORREF kCard = RGB(43, 43, 43);
static const COLORREF kCardHover = RGB(52, 52, 52);
static const COLORREF kOnAccent = RGB(16, 16, 16);

static HWND g_settings = nullptr;
static std::vector<SetHit> g_setHits;
static int g_setHover = SI_NONE, g_setPressed = SI_NONE;
static int g_setDpi = 96, g_setFontDpi = 0;
static HFONT g_sfTitle = nullptr, g_sfLabel = nullptr, g_sfSub = nullptr, g_sfBtn = nullptr;
static HICON g_setIcon = nullptr;
static ULONG_PTR g_gdiplus = 0;

static int SS(int v) { return MulDiv(v, g_setDpi, 96); }

static void SettingsResources()
{
    if (g_setFontDpi == g_setDpi)
        return;
    for (HFONT f : { g_sfTitle, g_sfLabel, g_sfSub, g_sfBtn })
        if (f) DeleteObject(f);
    if (g_setIcon) DestroyIcon(g_setIcon);
    auto font = [](int px, int weight) {
        return CreateFontW(-SS(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                           L"Segoe UI");
    };
    g_sfTitle = font(20, FW_SEMIBOLD);
    g_sfLabel = font(14, FW_NORMAL);
    g_sfSub = font(12, FW_NORMAL);
    g_sfBtn = font(13, FW_SEMIBOLD);
    g_setIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, SS(32), SS(32), 0);
    g_setFontDpi = g_setDpi;
}

static bool ToggleOn(int id)
{
    switch (id) {
    case SI_ENABLED: return g_enabled;
    case SI_SOUND: return g_videoSound;
    case SI_AUTOSTART: return AutostartEnabled();
    case SI_MENU: return ExplorerMenuEnabled();
    }
    return false;
}

static int SelectedSegment(int firstId)
{
    const int* values = firstId == SI_DELAY0 ? kDelays : kSizes;
    int v = firstId == SI_DELAY0 ? g_delayMs.load() : g_maxSize.load();
    for (int i = 0; i < 3; i++)
        if (values[i] == v) return i;
    return -1;
}

static Gdiplus::Color GpColor(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

static void FillRound(Gdiplus::Graphics* g, const RECT& r, float radius, COLORREF fill, COLORREF border = CLR_INVALID)
{
    float x = (float)r.left, y = (float)r.top, w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    float d = std::min({ radius * 2, w, h });
    Gdiplus::GraphicsPath p;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush b(GpColor(fill));
        g->FillPath(&b, &p);
    }
    if (border != CLR_INVALID) {
        Gdiplus::Pen pen(GpColor(border), (float)std::max(1, SS(1)));
        g->DrawPath(&pen, &p);
    }
}

static void TextIn(HDC dc, const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT flags)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
}

// Called twice per paint: first with g (shapes), then without (text), so GDI+ and ClearType text don't mix.
static void DrawSettings(HDC dc, Gdiplus::Graphics* g)
{
    int W = SS(kSetW), H = SS(kSetH), pad = SS(20);
    if (g)
        g_setHits.clear();

    if (!g) {
        DrawIconEx(dc, pad, SS(24), g_setIcon, SS(32), SS(32), 0, nullptr, DI_NORMAL);
        TextIn(dc, L"HoverPreview", { SS(64), SS(16), W - pad, SS(44) }, g_sfTitle, kText,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        TextIn(dc, L"Rest the mouse on a photo, video or song in File Explorer or on the desktop to preview it.",
               { SS(64), SS(46), W - pad, SS(88) }, g_sfSub, kSub, DT_LEFT | DT_WORDBREAK);
    }

    int y = SS(100), rowH = SS(56), gap = SS(4);
    for (const SetRow& row : kRows) {
        RECT rr = { pad, y, W - pad, y + rowH };
        int mid = (rr.top + rr.bottom) / 2;
        int right = rr.right - SS(16);
        int labelRight = right;

        if (row.segments) {
            int sw = SS(76), sh = SS(30);
            RECT box = { right - 3 * sw, mid - sh / 2, right, mid + sh / 2 };
            labelRight = box.left - SS(12);
            int sel = SelectedSegment(row.id);
            if (g) {
                FillRound(g, rr, (float)SS(6), kCard);
                FillRound(g, box, (float)SS(5), kTile);
            }
            for (int i = 0; i < 3; i++) {
                RECT seg = { box.left + i * sw, box.top, box.left + (i + 1) * sw, box.bottom };
                RECT inner = { seg.left + SS(2), seg.top + SS(2), seg.right - SS(2), seg.bottom - SS(2) };
                if (g) {
                    if (i == sel) FillRound(g, inner, (float)SS(4), kAccent);
                    else if (g_setHover == row.id + i) FillRound(g, inner, (float)SS(4), RGB(66, 66, 66));
                    g_setHits.push_back({ seg, row.id + i });
                } else {
                    TextIn(dc, row.segments[i], inner, g_sfSub, i == sel ? kOnAccent : kText,
                           DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                }
            }
        } else {
            bool on = ToggleOn(row.id);
            int tw = SS(40), th = SS(20);
            RECT tr = { right - tw, mid - th / 2, right, mid + th / 2 };
            labelRight = tr.left - SS(50);
            if (g) {
                FillRound(g, rr, (float)SS(6), g_setHover == row.id ? kCardHover : kCard);
                bool hot = g_setHover == row.id;
                float d = (float)(hot ? SS(14) : SS(12));
                float ky = mid - d / 2;
                if (on) {
                    FillRound(g, tr, th / 2.0f, kAccent);
                    Gdiplus::SolidBrush kb(GpColor(kOnAccent));
                    g->FillEllipse(&kb, tr.right - SS(4) - d, ky, d, d);
                } else {
                    FillRound(g, tr, th / 2.0f, CLR_INVALID, kSub);
                    Gdiplus::SolidBrush kb(GpColor(kSub));
                    g->FillEllipse(&kb, (float)(tr.left + SS(4)), ky, d, d);
                }
                g_setHits.push_back({ rr, row.id });
            } else {
                TextIn(dc, on ? L"On" : L"Off", { tr.left - SS(46), rr.top, tr.left - SS(10), rr.bottom }, g_sfSub,
                       kText, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            }
        }

        if (!g) {
            RECT lr = { rr.left + SS(16), rr.top, labelRight, rr.bottom };
            if (row.sub) {
                TextIn(dc, row.label, { lr.left, rr.top + SS(8), lr.right, mid + SS(1) }, g_sfLabel, kText,
                       DT_LEFT | DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS);
                TextIn(dc, row.sub, { lr.left, mid + SS(2), lr.right, rr.bottom - SS(6) }, g_sfSub, kSub,
                       DT_LEFT | DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS);
            } else {
                TextIn(dc, row.label, lr, g_sfLabel, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }
        y += rowH + gap;
    }

    if (!g) {
        TextIn(dc,
               L"To open this window again, run HoverPreview, or right-click empty space in a folder "
               L"› Show more options › HoverPreview settings.",
               { pad, y + SS(10), W - pad, H - SS(64) }, g_sfSub, kSub, DT_LEFT | DT_WORDBREAK);
    }

    int bh = SS(32), by = H - pad - bh;
    RECT quit = { pad, by, pad + SS(150), by + bh };
    RECT close = { W - pad - SS(100), by, W - pad, by + bh };
    if (g) {
        FillRound(g, quit, (float)SS(5), g_setHover == SI_QUIT ? RGB(66, 66, 66) : kTile);
        FillRound(g, close, (float)SS(5), g_setHover == SI_CLOSE ? RGB(120, 212, 255) : kAccent);
        g_setHits.push_back({ quit, SI_QUIT });
        g_setHits.push_back({ close, SI_CLOSE });
    } else {
        TextIn(dc, L"Quit HoverPreview", quit, g_sfBtn, RGB(255, 153, 164), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        TextIn(dc, L"Close", close, g_sfBtn, kOnAccent, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}

static void PaintSettings(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRectColor(mem, rc, kWinBg);
    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        DrawSettings(mem, &g);
    }
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(mem, g_sfLabel);
    DrawSettings(mem, nullptr);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static int SettingsHitTest(POINT pt)
{
    for (const SetHit& h : g_setHits)
        if (PtInRect(&h.r, pt)) return h.id;
    return SI_NONE;
}

static void SettingsClick(int id)
{
    switch (id) {
    case SI_ENABLED: g_enabled = !g_enabled; break;
    case SI_DELAY0: case SI_DELAY1: case SI_DELAY2: g_delayMs = kDelays[id - SI_DELAY0]; break;
    case SI_SIZE0: case SI_SIZE1: case SI_SIZE2: g_maxSize = kSizes[id - SI_SIZE0]; break;
    case SI_SOUND:
        g_videoSound = !g_videoSound;
        if (g_player && g_hasVideo)
            g_player->SetMute(g_videoSound ? FALSE : TRUE);
        break;
    case SI_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
    case SI_MENU: SetExplorerMenu(!ExplorerMenuEnabled()); break;
    case SI_QUIT:
        DestroyWindow(g_settings);
        DestroyWindow(g_main);
        return;
    case SI_CLOSE:
        DestroyWindow(g_settings);
        return;
    default: return;
    }
    SaveSettings();
    InvalidateRect(g_settings, nullptr, FALSE);
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintSettings(hwnd); return 0;
    case WM_MOUSEMOVE: {
        int h = SettingsHitTest({ (short)LOWORD(lp), (short)HIWORD(lp) });
        if (h != g_setHover) {
            g_setHover = h;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_setHover = SI_NONE;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_setHover != SI_NONE) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        g_setPressed = SettingsHitTest({ (short)LOWORD(lp), (short)HIWORD(lp) });
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int h = SettingsHitTest({ (short)LOWORD(lp), (short)HIWORD(lp) });
        int pressed = g_setPressed;
        g_setPressed = SI_NONE;
        if (h != SI_NONE && h == pressed)
            SettingsClick(h);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        g_setDpi = HIWORD(wp);
        SettingsResources();
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        g_settings = nullptr;
        g_setHits.clear();
        g_setHover = g_setPressed = SI_NONE;
        if (IsWindow(g_main))
            SetTimer(g_main, T_TRIM, 5000, nullptr);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowSettings()
{
    if (g_settings) {
        if (IsIconic(g_settings))
            ShowWindow(g_settings, SW_RESTORE);
        SetForegroundWindow(g_settings);
        return;
    }
    if (!g_gdiplus) {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr);
    }
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    g_setDpi = (int)dx;
    SettingsResources();

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, SS(kSetW), SS(kSetH) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_setDpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    const RECT& wa = mi.rcWork;
    int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;
    g_settings = CreateWindowExW(0, L"HoverPreviewSettings", L"HoverPreview settings", style, x, y, w, h, nullptr,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_settings)
        return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_settings, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    ShowWindow(g_settings, SW_SHOW);
    SetForegroundWindow(g_settings);
}

// ---------------------------------------------------------------------------------------------
// Main hidden window and startup
// ---------------------------------------------------------------------------------------------

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_SHOW: ShowPreview((Preview*)lp); return 0;
    case WM_APP_HIDE: HidePreview(); return 0;
    case WM_APP_SETTINGS: ShowSettings(); return 0;
    case WM_TIMER:
        if (wp == T_TRIM) {
            KillTimer(hwnd, T_TRIM);
            if (!g_cur)   // give memory back after media playback / closing settings
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmd, int)
{
    g_log = cmd && wcsstr(cmd, L"--log");
    bool background = cmd && wcsstr(cmd, L"--background");
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i++)
        if (wcscmp(argv[i], L"--test") == 0)
            g_testFile = argv[i + 1];
    if (argv)
        LocalFree(argv);
    bool test = !g_testFile.empty();

    HANDLE mutex = test ? nullptr : CreateMutexW(nullptr, TRUE, L"Local\\HoverPreview.Instance");
    if (!test && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: ask that instance to open its settings window.
        if (!background) {
            HWND w = nullptr;
            for (int i = 0; i < 30 && !(w = FindWindowW(L"HoverPreviewMain", nullptr)); i++)
                Sleep(100);
            if (w) {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(w, WM_APP_SETTINGS, 0, 0);
            }
        }
        if (mutex)
            CloseHandle(mutex);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadSettings();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"HoverPreviewMain";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = PopupProc;
    wc.lpszClassName = L"HoverPreviewPopup";
    wc.style = CS_DROPSHADOW;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = VideoProc;
    wc.lpszClassName = L"HoverPreviewVideo";
    wc.style = 0;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = L"HoverPreviewSettings";
    wc.hbrBackground = nullptr;
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, L"HoverPreviewMain", kAppName, WS_POPUP, 0, 0, 0, 0, nullptr,
                             nullptr, hInst, nullptr);
    g_popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"HoverPreviewPopup", kAppName,
                              WS_POPUP | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, hInst, nullptr);
    g_video = CreateWindowExW(WS_EX_NOACTIVATE, L"HoverPreviewVideo", nullptr, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 16,
                              16, g_popup, nullptr, hInst, nullptr);
    if (!g_main || !g_popup || !g_video)
        return 1;

    // Windows 11 rounded corners + border (attributes 33 and 34; ignored on Windows 10).
    DWORD corner = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(g_popup, 33, &corner, sizeof(corner));
    COLORREF border = RGB(64, 64, 64);
    DwmSetWindowAttribute(g_popup, 34, &border, sizeof(border));

    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE worker = nullptr;
    if (test) {
        worker = CreateThread(nullptr, 0, TestThread, nullptr, 0, nullptr);
        SetTimer(nullptr, 0, 5000, [](HWND, UINT, UINT_PTR, DWORD) { DestroyWindow(g_main); });
    } else {
        // First run of this version: add the Explorer menu entry. Afterwards keep the stored
        // paths pointing at this exe in case it was moved.
        if (RegReadDword(L"MenuSetup", 0) == 0) {
            SetExplorerMenu(true);
            RegWriteDword(L"MenuSetup", 1);
        } else if (ExplorerMenuEnabled()) {
            SetExplorerMenu(true);
        }
        if (AutostartEnabled())
            SetAutostart(true);
        worker = CreateThread(nullptr, 0, DetectorThread, nullptr, 0, nullptr);
        if (!background)
            PostMessageW(g_main, WM_APP_SETTINGS, 0, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_quit);
    if (worker) {
        WaitForSingleObject(worker, 3000);
        CloseHandle(worker);
    }
    HidePreview();
    if (g_gdiplus)
        Gdiplus::GdiplusShutdown(g_gdiplus);
    CoUninitialize();
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
