// ==WindhawkMod==
// @id              per-monitor-next-wallpaper
// @name            Per-Monitor Next Desktop Background
// @description     Makes "Next desktop background" change the monitor you right-clicked, and adds an option to sync all monitors
// @version         1.0.0
// @author          Sp1d3rM0nk3y
// @github          https://github.com/andrew-brainerd
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -luuid -lshell32 -lshlwapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Per-Monitor Next Desktop Background

With a slideshow wallpaper on several monitors, Windows' **Next desktop
background** menu item changes the monitors in turn, no matter which one you
right-clicked. This mod makes it change the monitor you right-clicked.

It also adds a **Sync desktop backgrounds** item, which copies the
right-clicked monitor's image to every other monitor. You can also set
**Next desktop background** to change all monitors to the same new image.

![Desktop context menu](https://raw.githubusercontent.com/andrew-brainerd/per-monitor-next-wallpaper/main/images/context-menu.png)

## How it works

**Next desktop background** still runs Windows' own slideshow advance. Windows
picks the monitor that gets the next image in turn
(`CMultimonWallpaperState::_GetNextMonitor` in `shell32.dll`), and the mod
makes that pick return the monitor you right-clicked. Windows' shuffle order,
image history and saved settings work as usual.

Windows can't show the same slideshow image on several monitors, so **Sync
desktop backgrounds** and the all-monitors option set the image with
`IDesktopWallpaper::SetWallpaper`. That turns the slideshow off, so the mod
then turns it back on with the same folder and settings.

## Saved changes

Sync and the all-monitors option change your saved wallpaper, the same as
picking an image in Settings. If one of these changes is interrupted, for
example by Explorer restarting, the slideshow can be left off. The mod turns
it back on the next time it loads.

## Limitations

- The slideshow timer still changes one monitor at a time, so synced monitors
  go out of sync at the next timed change.
- The all-monitors option picks images itself, from the slideshow folder and
  its subfolders, separately from Windows' shuffle order.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- nextMode: clicked
  $name: Next desktop background changes
  $options:
  - clicked: The monitor you right-clicked
  - all: All monitors, to the same new image
- syncMenuItem: true
  $name: Show "Sync desktop backgrounds" menu item
  $description: Copies the right-clicked monitor's image to every other monitor
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <strsafe.h>

#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

enum class Command { Next, Sync };
enum class Action { NextAll, Sync };

std::atomic<bool> g_nextAll;
std::atomic<bool> g_syncMenuItem;

const CLSID kClsidSlideshowContextMenu = {
    0x0bf754aa,
    0xc967,
    0x445c,
    {0xab, 0x3d, 0xd8, 0xfd, 0xa9, 0xba, 0xe7, 0xef}};
const wchar_t kSyncMenuText[] = L"Sync desktop backgrounds";
const wchar_t kSyncHelpText[] =
    L"Copies this monitor's background to every other monitor";
const wchar_t kNextVerb[] = L"NextDesktopBackground";
const wchar_t kSyncVerb[] = L"SyncDesktopBackgrounds";
constexpr DWORD kPendingMonitorTimeoutMs = 15000;

std::atomic<UINT> g_syncOffset{UINT_MAX};
std::atomic<UINT> g_handlerOffsetEnd{0};
std::atomic<DWORD> g_menuMessagePos{0};
void* g_slideshowMenuVtable;
HMODULE g_handlerModule;

std::atomic<HMONITOR> g_pendingMonitor{nullptr};
std::atomic<DWORD> g_pendingMonitorTick{0};

struct WorkItem {
    Action action;
    POINT pt;
};

HANDLE g_workerThread;
HANDLE g_stopEvent;
HANDLE g_workEvent;
std::mutex g_pendingWorkMutex;
std::optional<WorkItem> g_pendingWork;

struct Monitor {
    std::wstring id;
    RECT rect;
    std::wstring wallpaper;
};

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring GetWallpaper(IDesktopWallpaper* dw, const std::wstring& id) {
    LPWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(dw->GetWallpaper(id.c_str(), &path)) && path) {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

std::vector<Monitor> GetMonitors(IDesktopWallpaper* dw) {
    std::vector<Monitor> monitors;
    UINT count = 0;
    if (FAILED(dw->GetMonitorDevicePathCount(&count))) {
        return monitors;
    }
    for (UINT i = 0; i < count; i++) {
        LPWSTR id = nullptr;
        if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) {
            continue;
        }
        Monitor monitor{id};
        CoTaskMemFree(id);
        // Disconnected monitors fail here.
        if (FAILED(dw->GetMonitorRECT(monitor.id.c_str(), &monitor.rect))) {
            continue;
        }
        monitor.wallpaper = GetWallpaper(dw, monitor.id);
        monitors.push_back(std::move(monitor));
    }
    return monitors;
}

int FindMonitorAt(const std::vector<Monitor>& monitors, POINT pt) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST),
                       &info)) {
        for (size_t i = 0; i < monitors.size(); i++) {
            if (EqualRect(&monitors[i].rect, &info.rcMonitor)) {
                return (int)i;
            }
        }
    }
    for (size_t i = 0; i < monitors.size(); i++) {
        if (PtInRect(&monitors[i].rect, pt)) {
            return (int)i;
        }
    }
    return monitors.empty() ? -1 : 0;
}

bool IsImageFile(PCWSTR name) {
    static const PCWSTR extensions[] = {
        L".jpg",  L".jpeg", L".jpe",  L".jfif", L".png",  L".bmp",
        L".dib",  L".gif",  L".tif",  L".tiff", L".wdp",  L".jxr",
        L".heic", L".heif", L".webp", L".avif",
    };
    PCWSTR ext = PathFindExtensionW(name);
    return std::any_of(std::begin(extensions), std::end(extensions),
                       [&](PCWSTR e) { return _wcsicmp(ext, e) == 0; });
}

void CollectImages(const std::wstring& dir,
                   std::vector<std::wstring>& out,
                   int depth = 0) {
    if (depth > 16) {
        return;
    }
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic,
                                   &data, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (data.dwFileAttributes &
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
            continue;
        }
        std::wstring path = dir + L"\\" + data.cFileName;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Skip junctions and symlinks, but not cloud (OneDrive) folders.
            bool isLink =
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                IsReparseTagNameSurrogate(data.dwReserved0);
            if (!isLink && wcscmp(data.cFileName, L".") != 0 &&
                wcscmp(data.cFileName, L"..") != 0) {
                CollectImages(path, out, depth + 1);
            }
        } else if (IsImageFile(data.cFileName)) {
            out.push_back(std::move(path));
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

std::vector<std::wstring> GetSlideshowPaths(IShellItemArray* slideshow) {
    std::vector<std::wstring> paths;
    DWORD count = 0;
    if (FAILED(slideshow->GetCount(&count))) {
        return paths;
    }
    for (DWORD i = 0; i < count; i++) {
        IShellItem* item = nullptr;
        if (FAILED(slideshow->GetItemAt(i, &item))) {
            continue;
        }
        LPWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.push_back(path);
            CoTaskMemFree(path);
        }
        item->Release();
    }
    return paths;
}

std::vector<std::wstring> GetSlideshowImages(IShellItemArray* slideshow) {
    std::vector<std::wstring> images;
    for (const std::wstring& path : GetSlideshowPaths(slideshow)) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            CollectImages(path, images);
        } else if (IsImageFile(path.c_str())) {
            images.push_back(path);
        }
    }
    std::sort(images.begin(), images.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
              });
    images.erase(std::unique(images.begin(), images.end(), SamePath),
                 images.end());
    return images;
}

std::wstring PickNextImage(const std::vector<std::wstring>& images,
                           const std::wstring& current,
                           const std::vector<std::wstring>& avoid,
                           bool shuffle) {
    auto isAvoided = [&](const std::wstring& path) {
        return std::any_of(avoid.begin(), avoid.end(),
                           [&](const std::wstring& a) {
                               return SamePath(a, path);
                           });
    };

    std::vector<size_t> candidates;
    for (size_t i = 0; i < images.size(); i++) {
        if (!isAvoided(images[i])) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) {
        for (size_t i = 0; i < images.size(); i++) {
            if (!SamePath(images[i], current)) {
                candidates.push_back(i);
            }
        }
    }
    if (candidates.empty()) {
        return {};
    }

    if (shuffle) {
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<size_t> pick(0, candidates.size() - 1);
        return images[candidates[pick(rng)]];
    }

    auto currentIt = std::find_if(
        images.begin(), images.end(),
        [&](const std::wstring& path) { return SamePath(path, current); });
    if (currentIt != images.end()) {
        size_t currentIndex = currentIt - images.begin();
        for (size_t index : candidates) {
            if (index > currentIndex) {
                return images[index];
            }
        }
    }
    return images[candidates.front()];
}

// Returns false on timeout or when the mod is unloading.
bool WaitFor(const std::function<bool()>& condition, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    while (!condition()) {
        if (GetTickCount() - start >= timeoutMs ||
            WaitForSingleObject(g_stopEvent, 100) == WAIT_OBJECT_0) {
            return false;
        }
    }
    return true;
}

bool IsSlideshowActive(IDesktopWallpaper* dw) {
    DESKTOP_SLIDESHOW_STATE state{};
    return SUCCEEDED(dw->GetStatus(&state)) && (state & DSS_SLIDESHOW);
}

void SaveRearmState(const std::vector<std::wstring>& paths) {
    std::wstring joined;
    for (const std::wstring& path : paths) {
        joined += path;
        joined += L'\n';
    }
    Wh_SetStringValue(L"rearmPaths", joined.c_str());
    Wh_SetIntValue(L"rearmPending", 1);
}

void ClearRearmState() {
    Wh_DeleteValue(L"rearmPending");
    Wh_DeleteValue(L"rearmPaths");
}

HRESULT CreateItemArray(const std::vector<std::wstring>& paths,
                        IShellItemArray** result) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const std::wstring& path : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(
                SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) {
            pidls.push_back(pidl);
        }
    }
    HRESULT hr = pidls.empty()
                     ? E_FAIL
                     : SHCreateShellItemArrayFromIDLists(
                           (UINT)pidls.size(),
                           (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), result);
    for (PIDLIST_ABSOLUTE pidl : pidls) {
        CoTaskMemFree(pidl);
    }
    return hr;
}

// Turns the slideshow back on if an earlier sync was interrupted after it
// turned the slideshow off.
void RecoverInterruptedChange() {
    if (!Wh_GetIntValue(L"rearmPending", 0)) {
        return;
    }

    std::vector<WCHAR> buffer(32768);
    Wh_GetStringValue(L"rearmPaths", buffer.data(), buffer.size());
    std::vector<std::wstring> paths;
    std::wstring current;
    for (const WCHAR* p = buffer.data(); *p; p++) {
        if (*p == L'\n') {
            paths.push_back(std::move(current));
            current.clear();
        } else {
            current += *p;
        }
    }

    // Explorer may still be starting, so retry for a while.
    IDesktopWallpaper* dw = nullptr;
    WaitFor(
        [&] {
            return SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr,
                                              CLSCTX_ALL, IID_PPV_ARGS(&dw)));
        },
        30000);
    if (!dw) {
        return;
    }

    if (!IsSlideshowActive(dw)) {
        IShellItemArray* slideshow = nullptr;
        HRESULT hr = CreateItemArray(paths, &slideshow);
        if (SUCCEEDED(hr)) {
            hr = dw->SetSlideshow(slideshow);
            slideshow->Release();
        }
        Wh_Log(L"Re-enabled the slideshow after an interrupted change: "
               L"0x%08X",
               hr);
    }
    ClearRearmState();
    dw->Release();
}

// Setting a wallpaper turns the slideshow off, so it is re-armed afterwards
// once Windows has finished switching it off.
void ApplyWallpapers(
    IDesktopWallpaper* dw,
    IShellItemArray* slideshow,
    const std::vector<std::pair<std::wstring, std::wstring>>& changes) {
    DESKTOP_SLIDESHOW_OPTIONS options{};
    UINT tick = 0;
    bool haveOptions = SUCCEEDED(dw->GetSlideshowOptions(&options, &tick));

    SaveRearmState(GetSlideshowPaths(slideshow));

    for (const auto& [id, path] : changes) {
        HRESULT hr = dw->SetWallpaper(id.c_str(), path.c_str());
        if (FAILED(hr)) {
            Wh_Log(L"SetWallpaper(%s) failed: 0x%08X", path.c_str(), hr);
        }
    }

    WaitFor(
        [&] {
            if (IsSlideshowActive(dw)) {
                return false;
            }
            return std::all_of(changes.begin(), changes.end(),
                               [&](const auto& change) {
                                   return SamePath(
                                       GetWallpaper(dw, change.first),
                                       change.second);
                               });
        },
        5000);

    HRESULT hr = dw->SetSlideshow(slideshow);
    if (FAILED(hr)) {
        Wh_Log(L"SetSlideshow failed: 0x%08X", hr);
    }

    if (haveOptions) {
        DESKTOP_SLIDESHOW_OPTIONS newOptions{};
        UINT newTick = 0;
        if (SUCCEEDED(dw->GetSlideshowOptions(&newOptions, &newTick)) &&
            (newOptions != options || newTick != tick)) {
            dw->SetSlideshowOptions(options, tick);
        }
    }

    if (WaitFor([&] { return IsSlideshowActive(dw); }, 5000)) {
        ClearRearmState();
    } else {
        Wh_Log(L"Slideshow did not turn back on");
    }
}

void RunAction(const WorkItem& item) {
    IDesktopWallpaper* dw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&dw));
    if (FAILED(hr)) {
        Wh_Log(L"CoCreateInstance(DesktopWallpaper) failed: 0x%08X", hr);
        return;
    }

    IShellItemArray* slideshow = nullptr;
    std::vector<Monitor> monitors = GetMonitors(dw);
    int target = FindMonitorAt(monitors, item.pt);
    std::vector<std::pair<std::wstring, std::wstring>> changes;

    if (!IsSlideshowActive(dw) || FAILED(dw->GetSlideshow(&slideshow)) ||
        target < 0) {
        if (item.action == Action::NextAll) {
            dw->AdvanceSlideshow(nullptr, DSD_FORWARD);
        }
    } else if (item.action == Action::Sync) {
        const std::wstring& source = monitors[target].wallpaper;
        for (const Monitor& monitor : monitors) {
            if (!source.empty() && !SamePath(monitor.wallpaper, source)) {
                changes.emplace_back(monitor.id, source);
            }
        }
    } else {
        DESKTOP_SLIDESHOW_OPTIONS options{};
        UINT tick = 0;
        dw->GetSlideshowOptions(&options, &tick);
        bool shuffle = options & DSO_SHUFFLEIMAGES;

        std::vector<std::wstring> showing;
        for (const Monitor& monitor : monitors) {
            showing.push_back(monitor.wallpaper);
        }

        std::wstring next =
            PickNextImage(GetSlideshowImages(slideshow),
                          monitors[target].wallpaper, showing, shuffle);
        if (next.empty()) {
            dw->AdvanceSlideshow(nullptr, DSD_FORWARD);
        } else {
            for (const Monitor& monitor : monitors) {
                changes.emplace_back(monitor.id, next);
            }
        }
    }

    if (!changes.empty()) {
        ApplyWallpapers(dw, slideshow, changes);
    }

    if (slideshow) {
        slideshow->Release();
    }
    dw->Release();
}

DWORD WINAPI WorkerThread(void*) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hrInit)) {
        RecoverInterruptedChange();
    }

    HANDLE handles[] = {g_stopEvent, g_workEvent};
    while (WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE,
                                  INFINITE) == WAIT_OBJECT_0 + 1) {
        std::optional<WorkItem> item;
        {
            std::lock_guard lock(g_pendingWorkMutex);
            item = std::exchange(g_pendingWork, std::nullopt);
        }
        if (item && SUCCEEDED(hrInit)) {
            RunAction(*item);
        }
    }

    if (SUCCEEDED(hrInit)) {
        CoUninitialize();
    }
    return 0;
}

void QueueWork(Action action, POINT pt) {
    {
        std::lock_guard lock(g_pendingWorkMutex);
        g_pendingWork = WorkItem{action, pt};
    }
    SetEvent(g_workEvent);
}

void SetPendingMonitor(HMONITOR monitor) {
    g_pendingMonitorTick = GetTickCount();
    g_pendingMonitor = monitor;
}

HMONITOR TakePendingMonitor() {
    HMONITOR monitor = g_pendingMonitor.exchange(nullptr);
    if (monitor &&
        GetTickCount() - g_pendingMonitorTick > kPendingMonitorTimeoutMs) {
        return nullptr;
    }
    return monitor;
}

using GetMonitorCount_t = int(WINAPI*)(const void* self);
GetMonitorCount_t GetMonitorCount;

using GetMonitorAt_t = HMONITOR(WINAPI*)(void* self, int index);
GetMonitorAt_t GetMonitorAt;

using GetNextMonitor_t = int(WINAPI*)(void* self,
                                      int lastIndex,
                                      int orientation,
                                      int mode,
                                      PCWSTR imagePath,
                                      bool flag);
GetNextMonitor_t GetNextMonitor_Original;

// Picks the monitor that gets the next slideshow image.
int WINAPI GetNextMonitor_Hook(void* self,
                               int lastIndex,
                               int orientation,
                               int mode,
                               PCWSTR imagePath,
                               bool flag) {
    if (HMONITOR target = TakePendingMonitor()) {
        int count = GetMonitorCount(self);
        for (int i = 0; i < count; i++) {
            if (GetMonitorAt(self, i) == target) {
                return i;
            }
        }
    }
    return GetNextMonitor_Original(self, lastIndex, orientation, mode,
                                   imagePath, flag);
}

bool IsSlideshowMenu(IContextMenu* self) {
    return self && *(void**)self == g_slideshowMenuVtable;
}

std::optional<Command> ClassifyCommand(CMINVOKECOMMANDINFO* info) {
    if (IS_INTRESOURCE(info->lpVerb)) {
        UINT offset = LOWORD((UINT_PTR)info->lpVerb);
        if (offset == g_syncOffset) {
            return Command::Sync;
        }
        if (offset < g_handlerOffsetEnd) {
            return Command::Next;
        }
        return std::nullopt;
    }

    std::wstring verb;
    if (info->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) &&
        (info->fMask & CMIC_MASK_UNICODE)) {
        auto* infoEx = (CMINVOKECOMMANDINFOEX*)info;
        if (infoEx->lpVerbW && !IS_INTRESOURCE(infoEx->lpVerbW)) {
            verb = infoEx->lpVerbW;
        }
    }
    if (verb.empty()) {
        int length =
            MultiByteToWideChar(CP_ACP, 0, info->lpVerb, -1, nullptr, 0);
        if (length > 0) {
            verb.resize(length);
            MultiByteToWideChar(CP_ACP, 0, info->lpVerb, -1, verb.data(),
                                length);
            verb.resize(length - 1);
        }
    }

    if (_wcsicmp(verb.c_str(), kNextVerb) == 0) {
        return Command::Next;
    }
    if (_wcsicmp(verb.c_str(), kSyncVerb) == 0) {
        return Command::Sync;
    }
    return std::nullopt;
}

POINT GetInvokePoint(CMINVOKECOMMANDINFO* info) {
    if (info->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) &&
        (info->fMask & CMIC_MASK_PTINVOKE)) {
        return ((CMINVOKECOMMANDINFOEX*)info)->ptInvoke;
    }
    DWORD pos = g_menuMessagePos;
    return {GET_X_LPARAM(pos), GET_Y_LPARAM(pos)};
}

using QueryContextMenu_t = HRESULT(STDMETHODCALLTYPE*)(IContextMenu*,
                                                       HMENU,
                                                       UINT,
                                                       UINT,
                                                       UINT,
                                                       UINT);
QueryContextMenu_t QueryContextMenu_Original;

HRESULT STDMETHODCALLTYPE QueryContextMenu_Hook(IContextMenu* self,
                                                HMENU menu,
                                                UINT indexMenu,
                                                UINT idCmdFirst,
                                                UINT idCmdLast,
                                                UINT flags) {
    if (!IsSlideshowMenu(self)) {
        return QueryContextMenu_Original(self, menu, indexMenu, idCmdFirst,
                                         idCmdLast, flags);
    }

    g_menuMessagePos = GetMessagePos();
    g_syncOffset = UINT_MAX;

    int countBefore = GetMenuItemCount(menu);
    HRESULT hr = QueryContextMenu_Original(self, menu, indexMenu, idCmdFirst,
                                           idCmdLast, flags);
    if (FAILED(hr)) {
        return hr;
    }

    UINT nextOffset = HRESULT_CODE(hr);
    g_handlerOffsetEnd = nextOffset;
    if (!g_syncMenuItem || (flags & CMF_DEFAULTONLY)) {
        return hr;
    }

    int inserted = GetMenuItemCount(menu) - countBefore;
    if (nextOffset == 0 || inserted <= 0 ||
        idCmdFirst + nextOffset > idCmdLast) {
        return hr;
    }

    if (!InsertMenuW(menu, indexMenu + inserted, MF_BYPOSITION | MF_STRING,
                     idCmdFirst + nextOffset, kSyncMenuText)) {
        return hr;
    }
    g_syncOffset = nextOffset;
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, nextOffset + 1);
}

using GetCommandString_t = HRESULT(STDMETHODCALLTYPE*)(IContextMenu*,
                                                       UINT_PTR,
                                                       UINT,
                                                       UINT*,
                                                       CHAR*,
                                                       UINT);
GetCommandString_t GetCommandString_Original;

// The Windows 11 menu identifies items by verb, which the original handler
// can't supply for the added item.
HRESULT STDMETHODCALLTYPE GetCommandString_Hook(IContextMenu* self,
                                                UINT_PTR idCmd,
                                                UINT type,
                                                UINT* reserved,
                                                CHAR* name,
                                                UINT cchMax) {
    if (!IsSlideshowMenu(self) || idCmd != g_syncOffset) {
        return GetCommandString_Original(self, idCmd, type, reserved, name,
                                         cchMax);
    }

    switch (type) {
        case GCS_VERBW:
            return StringCchCopyW((PWSTR)name, cchMax, kSyncVerb);
        case GCS_VERBA:
            return StringCchCopyA(name, cchMax, "SyncDesktopBackgrounds");
        case GCS_HELPTEXTW:
            return StringCchCopyW((PWSTR)name, cchMax, kSyncHelpText);
        case GCS_VALIDATEW:
        case GCS_VALIDATEA:
            return S_OK;
    }
    return E_INVALIDARG;
}

using InvokeCommand_t = HRESULT(STDMETHODCALLTYPE*)(IContextMenu*,
                                                    CMINVOKECOMMANDINFO*);
InvokeCommand_t InvokeCommand_Original;

HRESULT STDMETHODCALLTYPE InvokeCommand_Hook(IContextMenu* self,
                                             CMINVOKECOMMANDINFO* info) {
    std::optional<Command> command;
    if (IsSlideshowMenu(self) && info) {
        command = ClassifyCommand(info);
    }
    if (!command) {
        return InvokeCommand_Original(self, info);
    }

    POINT pt = GetInvokePoint(info);
    if (*command == Command::Next && !g_nextAll) {
        SetPendingMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
        return InvokeCommand_Original(self, info);
    }

    QueueWork(*command == Command::Sync ? Action::Sync : Action::NextAll, pt);
    return S_OK;
}

struct ContextMenuMethods {
    void* vtable;
    QueryContextMenu_t queryContextMenu;
    InvokeCommand_t invokeCommand;
    GetCommandString_t getCommandString;
};

// Runs on its own thread so COM can be initialized as needed.
DWORD WINAPI FindContextMenuMethods(void* param) {
    auto* methods = (ContextMenuMethods*)param;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IContextMenu* menu = nullptr;
    HRESULT hr =
        CoCreateInstance(kClsidSlideshowContextMenu, nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&menu));
    if (SUCCEEDED(hr)) {
        void** vtable = *(void***)menu;
        // Keep the handler's DLL loaded while the hooks exist.
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               (PCWSTR)vtable[4], &g_handlerModule)) {
            methods->vtable = vtable;
            methods->queryContextMenu = (QueryContextMenu_t)vtable[3];
            methods->invokeCommand = (InvokeCommand_t)vtable[4];
            methods->getCommandString = (GetCommandString_t)vtable[5];
        }
        menu->Release();
    } else {
        Wh_Log(L"CoCreateInstance(SlideshowContextMenu) failed: 0x%08X", hr);
    }

    if (SUCCEEDED(hrInit)) {
        CoUninitialize();
    }
    return 0;
}

bool HookShell32() {
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32) {
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK shell32DllHooks[] = {
        {
            {LR"(public: int __cdecl CMultimonWallpaperState::_GetNextMonitor(int,enum IMAGE_ORIENTATION,enum CMultimonWallpaperState::GET_NEXT_MONITOR_MODE,unsigned short const *,bool))"},
            &GetNextMonitor_Original,
            GetNextMonitor_Hook,
        },
        {
            {LR"(public: int __cdecl CMultimonWallpaperState::GetCount(void)const )"},
            &GetMonitorCount,
        },
        {
            {LR"(public: struct HMONITOR__ * __cdecl CMultimonWallpaperState::GetMonitorAt(int))"},
            &GetMonitorAt,
        },
    };
    return WindhawkUtils::HookSymbols(shell32, shell32DllHooks,
                                     ARRAYSIZE(shell32DllHooks));
}

void LoadSettings() {
    g_nextAll =
        wcscmp(WindhawkUtils::StringSetting::make(L"nextMode"), L"all") == 0;
    g_syncMenuItem = Wh_GetIntSetting(L"syncMenuItem");
}

void Cleanup() {
    if (g_workerThread) {
        SetEvent(g_stopEvent);
        WaitForSingleObject(g_workerThread, INFINITE);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
    if (g_workEvent) {
        CloseHandle(g_workEvent);
        g_workEvent = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_handlerModule) {
        FreeLibrary(g_handlerModule);
        g_handlerModule = nullptr;
    }
}

BOOL Wh_ModInit() {
    LoadSettings();

    if (!HookShell32()) {
        Wh_Log(L"Failed to hook shell32.dll");
        return FALSE;
    }

    ContextMenuMethods methods{};
    HANDLE thread =
        CreateThread(nullptr, 0, FindContextMenuMethods, &methods, 0, nullptr);
    if (!thread) {
        return FALSE;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    if (!methods.vtable) {
        Wh_Log(L"Slideshow context menu handler not found");
        Cleanup();
        return FALSE;
    }
    g_slideshowMenuVtable = methods.vtable;

    WindhawkUtils::SetFunctionHook(methods.queryContextMenu,
                                   QueryContextMenu_Hook,
                                   &QueryContextMenu_Original);
    WindhawkUtils::SetFunctionHook(methods.invokeCommand, InvokeCommand_Hook,
                                   &InvokeCommand_Original);
    WindhawkUtils::SetFunctionHook(methods.getCommandString,
                                   GetCommandString_Hook,
                                   &GetCommandString_Original);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_workEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_workerThread =
        g_stopEvent && g_workEvent
            ? CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr)
            : nullptr;
    if (!g_workerThread) {
        Wh_Log(L"Failed to start the worker thread");
        Cleanup();
        return FALSE;
    }
    return TRUE;
}

void Wh_ModUninit() {
    Cleanup();
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}
