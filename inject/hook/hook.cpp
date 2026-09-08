// ============================================================================
// SmtcCoverBoostHook.dll
// ----------------------------------------------------------------------------
// 注入到 Wallpaper Engine 壁纸进程（wallpaper64.exe / wallpaper32.exe）后，
// 钩住 WinRT 接口 IGlobalSystemMediaTransportControlsSessionMediaProperties
// 的 get_Thumbnail 方法，把 Wallpaper Engine 读到的 SMTC 低清封面，
// 替换成后台服务(SmtcCoverBoost)生成的高清封面文件。
//
// 高清封面文件路径（与 Node 后台服务一致）：
//     %LOCALAPPDATA%\SmtcCoverBoost\cover.jpg  (或 .png / .webp / .gif)
//
// vtable 槽位推导：
//     IUnknown   : QueryInterface(0) AddRef(1) Release(2)
//     IInspectable: GetIids(3) GetRuntimeClassName(4) GetTrustLevel(5)
//     接口方法从 6 开始，声明顺序：
//       6  get_PlaybackType
//       7  get_TrackNumber
//       8  get_Title
//       9  get_Subtitle
//       10 get_Artist
//       11 get_AlbumArtist
//       12 get_AlbumTitle
//       13 get_AlbumTrackCount
//       14 get_Genres
//       15 get_Thumbnail   <-- 我们要钩的
// ============================================================================

#include <windows.h>
#include <atomic>
#include <thread>
#include <string>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

namespace
{
    constexpr int kThumbnailSlot = 15;

    std::atomic<IUnknown*> g_coverRef{ nullptr };   // 高清封面 IRandomAccessStreamReference（以 IUnknown 持有）
    std::atomic<void*>    g_original{ nullptr };    // 原始 get_Thumbnail 函数指针
    bool                  g_hooked = false;
    std::wstring          g_dir;
    FILETIME              g_lastWrite{};

    using GetThumbnailFn = HRESULT(STDMETHODCALLTYPE*)(void* self, void** value);

    std::wstring FindCoverFile()
    {
        const wchar_t* exts[] = { L".jpg", L".png", L".webp", L".gif" };
        for (auto ext : exts)
        {
            std::wstring p = g_dir + L"\\cover" + ext;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                return p;
        }
        return L"";
    }

    // 替换后的 get_Thumbnail。签名按 ABI：this + 输出 IRandomAccessStreamReference**。
    HRESULT STDMETHODCALLTYPE HookGetThumbnail(void* self, void** value)
    {
        IUnknown* ref = g_coverRef.load(std::memory_order_acquire);
        if (ref)
        {
            ref->AddRef();
            *value = ref;
            return S_OK;
        }
        // 没有高清封面时，回退到原始实现（低清缩略图）。
        auto original = reinterpret_cast<GetThumbnailFn>(g_original.load(std::memory_order_acquire));
        if (original)
            return original(self, value);
        *value = nullptr;
        return E_FAIL;
    }

    void RefreshCover()
    {
        std::wstring path = FindCoverFile();
        if (path.empty())
        {
            IUnknown* old = g_coverRef.exchange(nullptr);
            if (old) old->Release();
            return;
        }

        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        {
            if (attr.ftLastWriteTime.dwLowDateTime == g_lastWrite.dwLowDateTime &&
                attr.ftLastWriteTime.dwHighDateTime == g_lastWrite.dwHighDateTime)
                return; // 文件没变，不重建引用
            g_lastWrite = attr.ftLastWriteTime;
        }

        try
        {
            StorageFile file = StorageFile::GetFileFromPathAsync(winrt::hstring(path)).get();
            RandomAccessStreamReference ref = RandomAccessStreamReference::CreateFromFile(file);
            IUnknown* raw = reinterpret_cast<IUnknown*>(winrt::get_abi(ref));
            raw->AddRef(); // 为全局缓存持有一份引用
            IUnknown* old = g_coverRef.exchange(raw);
            if (old) old->Release();
        }
        catch (...)
        {
            // 创建失败：沿用上一次的引用（或回退原始）
        }
    }

    void TryPatchVtable()
    {
        if (g_hooked) return;
        try
        {
            auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = manager.GetCurrentSession();
            if (!session) return; // 此刻没有媒体会话，稍后重试
            auto props = session.TryGetMediaPropertiesAsync().get();
            if (!props) return;

            void** vtbl = *reinterpret_cast<void***>(winrt::get_abi(props));
            void* original = vtbl[kThumbnailSlot];

            DWORD oldProtect = 0;
            if (!VirtualProtect(&vtbl[kThumbnailSlot], sizeof(void*), PAGE_READWRITE, &oldProtect))
                return;
            vtbl[kThumbnailSlot] = reinterpret_cast<void*>(&HookGetThumbnail);
            VirtualProtect(&vtbl[kThumbnailSlot], sizeof(void*), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), &vtbl[kThumbnailSlot], sizeof(void*));

            g_original.store(original, std::memory_order_release);
            g_hooked = true;
        }
        catch (...)
        {
        }
    }

    void HookThread()
    {
        Sleep(200); // 等 LoadLibrary 的加载器锁释放，避免死锁
        try
        {
            init_apartment(apartment_type::multi_threaded);
        }
        catch (...) {}

        for (;;)
        {
            TryPatchVtable();
            RefreshCover();
            Sleep(1000);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        wchar_t buf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH))
            g_dir = std::wstring(buf) + L"\\SmtcCoverBoost";
        else
            g_dir = L".";
        std::thread(HookThread).detach();
    }
    return TRUE;
}
