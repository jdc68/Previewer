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
#define WM_APP_TRAY (WM_APP + 3)

enum { ID_ENABLED = 100, ID_DELAY_FAST, ID_DELAY_NORMAL, ID_DELAY_SLOW, ID_SIZE_S, ID_SIZE_M, ID_SIZE_L,
       ID_SOUND, ID_AUTOSTART, ID_EXIT };
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
        std::wstring v = L"\"" + ExePath() + L"\"";
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
static HICON g_trayIcon = nullptr;
static UINT g_msgTaskbarCreated = 0;

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
// Tray icon and menu
// ---------------------------------------------------------------------------------------------

static void TrayIcon(DWORD action, bool welcome)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_main;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_trayIcon;
    wcscpy_s(nid.szTip, g_enabled ? L"HoverPreview" : L"HoverPreview (paused)");
    if (welcome) {
        nid.uFlags |= NIF_INFO;
        wcscpy_s(nid.szInfoTitle, L"HoverPreview is running");
        wcscpy_s(nid.szInfo, L"Rest the mouse on a photo, video or song in File Explorer to preview it. "
                             L"Right-click this icon for options.");
        nid.dwInfoFlags = NIIF_INFO;
    }
    Shell_NotifyIconW(action, &nid);
}

static void ShowTrayMenu()
{
    auto chk = [](bool b) { return (UINT)(MF_STRING | (b ? MF_CHECKED : MF_UNCHECKED)); };
    HMENU m = CreatePopupMenu();
    HMENU md = CreatePopupMenu();
    HMENU ms = CreatePopupMenu();
    int d = g_delayMs, s = g_maxSize;
    AppendMenuW(md, chk(d == kDelays[0]), ID_DELAY_FAST, L"Fast (0.15 s)");
    AppendMenuW(md, chk(d == kDelays[1]), ID_DELAY_NORMAL, L"Normal (0.3 s)");
    AppendMenuW(md, chk(d == kDelays[2]), ID_DELAY_SLOW, L"Relaxed (0.7 s)");
    AppendMenuW(ms, chk(s == kSizes[0]), ID_SIZE_S, L"Small");
    AppendMenuW(ms, chk(s == kSizes[1]), ID_SIZE_M, L"Medium");
    AppendMenuW(ms, chk(s == kSizes[2]), ID_SIZE_L, L"Large");

    AppendMenuW(m, chk(g_enabled), ID_ENABLED, L"Previews on\tDouble-click icon");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)md, L"Hover delay");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ms, L"Preview size");
    AppendMenuW(m, chk(g_videoSound), ID_SOUND, L"Play sound in videos");
    AppendMenuW(m, chk(AutostartEnabled()), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void OnCommand(int id)
{
    switch (id) {
    case ID_ENABLED:
        g_enabled = !g_enabled;
        TrayIcon(NIM_MODIFY, false);
        break;
    case ID_DELAY_FAST: g_delayMs = kDelays[0]; break;
    case ID_DELAY_NORMAL: g_delayMs = kDelays[1]; break;
    case ID_DELAY_SLOW: g_delayMs = kDelays[2]; break;
    case ID_SIZE_S: g_maxSize = kSizes[0]; break;
    case ID_SIZE_M: g_maxSize = kSizes[1]; break;
    case ID_SIZE_L: g_maxSize = kSizes[2]; break;
    case ID_SOUND:
        g_videoSound = !g_videoSound;
        if (g_player && g_hasVideo)
            g_player->SetMute(g_videoSound ? FALSE : TRUE);
        break;
    case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
    case ID_EXIT: DestroyWindow(g_main); return;
    }
    SaveSettings();
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbarCreated && msg != 0) {
        TrayIcon(NIM_ADD, false);
        return 0;
    }
    switch (msg) {
    case WM_APP_SHOW: ShowPreview((Preview*)lp); return 0;
    case WM_APP_HIDE: HidePreview(); return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            ShowTrayMenu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
            OnCommand(ID_ENABLED);
        return 0;
    case WM_COMMAND: OnCommand(LOWORD(wp)); return 0;
    case WM_TIMER:
        if (wp == T_TRIM) {
            KillTimer(hwnd, T_TRIM);
            if (!g_cur)   // give memory back after media playback
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
        MessageBoxW(nullptr, L"HoverPreview is already running.\nLook for its icon in the system tray.", kAppName,
                    MB_ICONINFORMATION);
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

    g_trayIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE worker = nullptr;
    if (test) {
        worker = CreateThread(nullptr, 0, TestThread, nullptr, 0, nullptr);
        SetTimer(nullptr, 0, 5000, [](HWND, UINT, UINT_PTR, DWORD) { DestroyWindow(g_main); });
    } else {
        g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        bool firstRun = RegReadDword(L"Welcomed", 0) == 0;
        TrayIcon(NIM_ADD, firstRun);
        if (firstRun)
            RegWriteDword(L"Welcomed", 1);
        worker = CreateThread(nullptr, 0, DetectorThread, nullptr, 0, nullptr);
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
    // Drop previews the detector posted but nobody consumed.
    while (PeekMessageW(&msg, g_main, WM_APP_SHOW, WM_APP_SHOW, PM_REMOVE))
        delete (Preview*)msg.lParam;
    HidePreview();
    if (!test)
        TrayIcon(NIM_DELETE, false);
    CoUninitialize();
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
