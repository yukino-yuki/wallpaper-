// ============================================================================
// SmtcCoverBoostInjector.exe
// ----------------------------------------------------------------------------
// 把 SmtcCoverBoostHook.dll 注入到 Wallpaper Engine 的壁纸进程里。
// 常驻运行，持续监视并注入新出现的壁纸进程（切换壁纸时会新开进程）。
//
// 注意位宽：64 位版本注入 wallpaper64.exe，32 位(Win32)版本注入 wallpaper32.exe。
// 大多数 WE 壁纸是 64 位；如果你的壁纸是 32 位，请用 x86 编译的注入器。
// ============================================================================

#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <set>
#include <string>
#include <cstdio>

static std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    auto pos = s.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : s.substr(0, pos + 1);
}

static bool Inject(DWORD pid, const std::wstring& dllPath)
{
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) return false;

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(h); return false; }

    if (!WriteProcessMemory(h, remote, dllPath.c_str(), bytes, nullptr))
    {
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(h, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!t)
    {
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    return true;
}

static std::vector<DWORD> FindProcesses(const wchar_t* name)
{
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, name) == 0)
                out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring dllPath;
    if (argc >= 2)
        dllPath = argv[1];
    else
        dllPath = ExeDir() + L"SmtcCoverBoostHook.dll";

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        wprintf(L"[!] DLL not found: %s\n", dllPath.c_str());
        return 1;
    }

    const wchar_t* targets[] = { L"wallpaper64.exe", L"wallpaper32.exe", L"ui32.exe" };
    std::set<DWORD> done;

    wprintf(L"[*] Watching Wallpaper Engine processes... (Ctrl+C to stop)\n");
    for (;;)
    {
        for (auto t : targets)
        {
            for (DWORD pid : FindProcesses(t))
            {
                if (done.count(pid)) continue;
                if (Inject(pid, dllPath))
                {
                    wprintf(L"[+] injected %s (pid %u)\n", t, pid);
                    done.insert(pid);
                }
            }
        }
        Sleep(3000);
    }
    return 0;
}
