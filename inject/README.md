# SmtcCoverBoost 注入式拦截（让现有 Wallpaper Engine 壁纸自动显示高清封面）

这个子目录是「魔改 SMTC」的**原生注入方案**：把一段 DLL 注入到 Wallpaper Engine 的壁纸进程里，
钩住它读取封面的 WinRT 调用，把 SMTC 的低清封面**原地替换**成高清封面。

这样你**已有的壁纸**（用「Bind Album Cover」的场景壁纸、用媒体监听的网页壁纸）不需要改动，就能自动显示高清封面。

> ⚠️ 这是**未经真机测试的首版**，属于逆向/系统级代码，脆弱且可能需要多轮调试。请务必先读完「风险与调试」一节。

## 工作原理

```
音乐软件 ──低清封面──> SMTC ──> Wallpaper Engine 读 get_Thumbnail() ──> 壁纸显示
                                        ▲
                                        │ 这里被钩住，返回高清封面
                          SmtcCoverBoostHook.dll (注入到 wallpaper64.exe)
                                        ▲
                        后台 Node 服务生成的高清封面文件
                  %LOCALAPPDATA%\SmtcCoverBoost\cover.jpg
```

## 组成（三个部件配合）

1. **后台 Node 服务**（本仓库 `service\`）：按歌名/歌手去各平台搜高清封面，写到 `%LOCALAPPDATA%\SmtcCoverBoost\cover.jpg`（或 .png/.webp）。
2. **`SmtcCoverBoostHook.dll`**（本目录 `hook\`）：注入到 WE 壁纸进程，钩住 `get_Thumbnail`，返回上面的高清封面文件。
3. **`SmtcCoverBoostInjector.exe`**（本目录 `injector\`）：把 DLL 注入到 `wallpaper64.exe` / `wallpaper32.exe`，并持续监视新进程。

## 编译前提

- **Visual Studio 2022**（Community 免费版即可），安装时勾选 **「使用 C++ 的桌面开发」(Desktop development with C++)** 工作负载（自带 MSVC + Windows SDK）。
- 需要 NuGet 包 **Microsoft.Windows.CppWinRT**（C++/WinRT 工具，编译时自动从 Windows SDK 生成 WinRT 头）。

## 编译步骤

1. 双击打开 `SmtcCoverBoostInject.sln`。
   - 工程里已经写好了 `Microsoft.Windows.CppWinRT` 的 NuGet 引用（版本 2.0.240111.5），VS 打开时通常会自动还原。
2. 如果生成时报「找不到 winrt/base.h」「找不到 winrt/Windows.Media.Control.h」或 NuGet 还原失败：
   - 右键 **SmtcCoverBoostHook** 项目 → **管理 NuGet 程序包** → 搜索 **Microsoft.Windows.CppWinRT** → 安装/更新到最新版即可。
3. 顶部配置选 **Release + x64**，菜单「生成 → 生成解决方案」。
   - 产物在 `x64\Release\` 下：`SmtcCoverBoostHook.dll` 和 `SmtcCoverBoostInjector.exe`。
4. 如果你的壁纸是 **32 位**（很少见），再选 **Release + x86**（Win32）生成一遍，产物在 `Release\` 下。

## 使用步骤

1. 先把后台 Node 服务跑起来（仓库根目录 `启动.bat`），确认 `%LOCALAPPDATA%\SmtcCoverBoost\cover.jpg` 会随切歌更新。
2. 把 `SmtcCoverBoostHook.dll` 和 `SmtcCoverBoostInjector.exe` 放到**同一个文件夹**里。
3. 双击运行 `SmtcCoverBoostInjector.exe`（会常驻监视，窗口里显示 `[+] injected wallpaper64.exe` 即成功）。
4. 播放音乐，切歌。你的现有壁纸现在读到的就是高清封面。

> 注入器会每 3 秒扫一次进程，切换壁纸导致新开 `wallpaper64.exe` 时也会自动注入，所以保持它常驻即可。

## 位宽说明（重要）

- **64 位壁纸**（绝大多数）→ 用 x64 编译的注入器，注入 `wallpaper64.exe`。
- **32 位壁纸** → 用 x86 编译的注入器，注入 `wallpaper32.exe`。
- 位宽不匹配时 `OpenProcess`/注入会静默失败（窗口不报 `[+]`），切换对应位宽即可。

## 风险与已知脆弱点（务必读）

1. **vtable 槽位是硬编码的 15**（`IGlobalSystemMediaTransportControlsSessionMediaProperties::get_Thumbnail`）。如果某次 Windows 更新调整了接口方法顺序，钩子会指错函数 → 直接导致 `wallpaper64.exe` 崩溃。届时需核对接口并改 `hook.cpp` 里的 `kThumbnailSlot`。
2. **Wallpaper Engine 或 Windows 更新**可能改变 SMTC 读取路径，导致钩子失效（壁纸退回低清），不会报错。
3. **杀软可能把注入行为判为恶意**（CreateRemoteThread + LoadLibrary 是典型注入手法）。若被杀软拦截，需给注入器/ DLL 加白名单。
4. 注入器需要能 `OpenProcess` WE 进程；一般用户权限即可，但若 WE 以更高权限运行则注入器也要提权。
5. 钩子返回的是「高清封面文件」的流引用；后台服务写入用的是「临时文件 + 重命名」原子替换，所以切歌瞬间不会读到半张图。

## 不起作用时，按顺序排查

1. 注入器窗口有没有打印 `[+] injected wallpaper64.exe ...`？
   - 没有 → 位宽不对、或没找到进程、或被杀软拦了。
2. `%LOCALAPPDATA%\SmtcCoverBoost\` 下有没有 `cover.jpg`？没有 → 后台 Node 服务没跑或没搜到封面（先解决服务）。
3. 有 `cover.jpg`、也注入了，但壁纸还是低清 → 可能是槽位/接口变了，或 WE 在注入前就已缓存了缩略图。试试**先开注入器、再启动 WE**。
4. 壁纸进程崩溃 → 基本可以确定是 vtable 槽位不对，需要对着当前 Windows SDK 的 `windows.media.control.h` 重新核对方法顺序。

## 与后台服务的约定

- 钩子固定读取 `%LOCALAPPDATA%\SmtcCoverBoost\` 下的 `cover.jpg` / `cover.png` / `cover.webp` / `cover.gif`。
- 后台服务默认正是把封面写到这个目录。若你改过服务的 `outputDir`，请同步改 `hook.cpp` 里的 `g_dir` 路径。

## 免责声明

注入式 Hook 属于系统级操作，可能影响 Wallpaper Engine 稳定性、触发杀软告警。请在理解风险的前提下使用；若壁纸进程频繁崩溃，停掉注入器即可完全恢复原状（不残留、不改系统）。
