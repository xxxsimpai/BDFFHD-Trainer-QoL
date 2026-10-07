// BDFFHDTrainer.exe - portable launcher: finds or launches the game and injects bdffhd_payload.dll.
#include "common.h"
#include <tlhelp32.h>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <conio.h>
#include <shellapi.h>
#include <commdlg.h>
#include <cwctype>

static DWORD FindProcess(const wchar_t* exeName) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)}; DWORD pid = 0;
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
        if (_wcsicmp(pe.szExeFile, exeName) == 0) { pid = pe.th32ProcessID; break; }
    CloseHandle(s);
    return pid;
}

static bool HasModule(DWORD pid, const wchar_t* mod) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{sizeof(me)}; bool found = false;
    for (BOOL ok = Module32FirstW(s, &me); ok; ok = Module32NextW(s, &me))
        if (_wcsicmp(me.szModule, mod) == 0) { found = true; break; }
    CloseHandle(s);
    return found;
}

static bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static bool IsGameExe(const std::wstring& path) {
    size_t split = path.find_last_of(L"\\/");
    std::wstring name = split == std::wstring::npos ? path : path.substr(split + 1);
    return _wcsicmp(name.c_str(), L"BDFFHD.exe") == 0 && FileExists(path) &&
           FileExists(DirOf(path) + L"\\GameAssembly.dll");
}

// A Steam app manifest beside the detected install identifies Steam-managed
// copies. Steam itself remains responsible for validating the current account.
static bool IsSteamInstall(const std::wstring& exe) {
    std::wstring lower = exe;
    for (auto& ch : lower) ch = (wchar_t)towlower(ch);
    const std::wstring marker = L"\\steamapps\\common\\";
    size_t at = lower.find(marker);
    if (at == std::wstring::npos) return false;
    std::wstring library = exe.substr(0, at);
    return FileExists(library + L"\\steamapps\\appmanifest_2833580.acf");
}

static bool LaunchGame(const std::wstring& exe, std::wstring& error) {
    if (IsSteamInstall(exe)) {
        HINSTANCE result = ShellExecuteW(nullptr, L"open", L"steam://run/2833580", nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)result <= 32) {
            error = L"Steam-managed installation detected, but Steam could not launch the game. Start Steam and try again.";
            return false;
        }
        return true;
    }

    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, DirOf(exe).c_str(), &si, &pi)) {
        error = L"Could not launch the game.";
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

static std::wstring ReadGamePath() {
    wchar_t path[32768]{}; DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BDFFHDTrainer", L"GamePath",
                    RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS && IsGameExe(path)) return path;
    return L"";
}

static void RememberGamePath(const std::wstring& path) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BDFFHDTrainer", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"GamePath", 0, REG_SZ, (const BYTE*)path.c_str(),
                       (DWORD)((path.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

// Check the folder itself and its immediate children; avoid scanning entire drives.
static std::wstring FindInFolder(const std::wstring& folder) {
    std::wstring candidate = folder + L"\\BDFFHD.exe";
    if (IsGameExe(candidate)) return candidate;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        candidate = folder + L"\\" + fd.cFileName + L"\\BDFFHD.exe";
        if (IsGameExe(candidate)) { found = candidate; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static std::wstring FindGameExe() {
    std::wstring saved = ReadGamePath();
    if (!saved.empty()) return saved;
    std::vector<std::wstring> libs;
    wchar_t steam[32768]{}; DWORD sz = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                    nullptr, steam, &sz) == ERROR_SUCCESS) libs.push_back(steam);
    sz = sizeof(steam);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath",
                    RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, steam, &sz) == ERROR_SUCCESS)
        libs.push_back(steam);
    const size_t steamRoots = libs.size();
    for (size_t root = 0; root < steamRoots; ++root) {
        std::ifstream vdf(libs[root] + L"\\steamapps\\libraryfolders.vdf");
        std::string line;
        while (std::getline(vdf, line)) {
            size_t k = line.find("\"path\"");
            if (k == std::string::npos) continue;
            size_t a = line.find('"', k + 6);
            if (a == std::string::npos) continue;
            size_t b = line.find('"', a + 1);
            if (b == std::string::npos) continue;
            std::wstring path = Widen(line.substr(a + 1, b - a - 1)), decoded;
            for (size_t i = 0; i < path.size(); ++i) {
                decoded += path[i];
                if (path[i] == L'\\' && i + 1 < path.size() && path[i + 1] == L'\\') ++i;
            }
            libs.push_back(decoded);
        }
    }
    for (const auto& lib : libs) {
        std::wstring found = FindInFolder(lib + L"\\steamapps\\common");
        if (!found.empty()) return found;
    }
    std::wstring found = FindInFolder(ModuleDir(nullptr));
    if (!found.empty()) return found;
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        std::wstring drive(1, (wchar_t)(L'A' + i)); drive += L":\\";
        if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) continue;
        for (const wchar_t* folder : {L"Games", L"SteamLibrary\\steamapps\\common",
                                     L"Steam\\steamapps\\common", L"Program Files (x86)\\Steam\\steamapps\\common"}) {
            found = FindInFolder(drive + folder);
            if (!found.empty()) return found;
        }
    }
    return L"";
}

static std::wstring PickGameExe() {
    wchar_t file[32768]{};
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.lpstrFilter = L"Bravely Default game (BDFFHD.exe)\0BDFFHD.exe\0Executable files (*.exe)\0*.exe\0\0";
    picker.lpstrFile = file; picker.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    picker.lpstrTitle = L"Locate the game - select BDFFHD.exe";
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    while (GetOpenFileNameW(&picker)) {
        if (IsGameExe(file)) return file;
        MessageBoxW(nullptr, L"Select BDFFHD.exe in the game installation folder, beside GameAssembly.dll.",
                    L"Choose the game", MB_OK | MB_ICONINFORMATION);
    }
    return L"";
}

static void ShowError(const std::wstring& message) {
    if (!AllocConsole()) { MessageBoxW(nullptr, message.c_str(), L"BDFFHD Trainer", MB_OK | MB_ICONERROR); return; }
    SetConsoleTitleW(L"BDFFHD Trainer - Error");
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    wprintf(L"BDFFHD Trainer\n\n%s\n\nPress any key to close...", message.c_str());
    _getwch();
}

static bool Inject(DWORD pid, const std::wstring& dll, std::wstring& error) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!p) { error = L"Could not access the game process. Try running the trainer as administrator."; return false; }
    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    if (remote && WriteProcessMemory(p, remote, dll.c_str(), bytes, nullptr)) {
        auto load = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE t = CreateRemoteThread(p, nullptr, 0, load, remote, 0, nullptr);
        if (t) {
            DWORD wait = WaitForSingleObject(t, 10000);
            DWORD code = 0;
            ok = wait == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code != 0 && code != STILL_ACTIVE;
            // A timed-out loader may still read this argument. Keep it until process exit.
            if (wait != WAIT_OBJECT_0) remote = nullptr;
            CloseHandle(t);
        }
    }
    if (remote) VirtualFreeEx(p, remote, 0, MEM_RELEASE);
    CloseHandle(p);
    if (!ok) error = L"Injection failed. Try starting the game first and running the trainer as administrator.";
    return ok;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    struct LaunchGuard { HANDLE handle; ~LaunchGuard() { if (handle) CloseHandle(handle); } };
    LaunchGuard guard{CreateMutexW(nullptr, FALSE, L"Local\\BDFFHDTrainerLauncher")};
    if (guard.handle && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring here = ModuleDir(nullptr);
    std::wstring payload = here + L"\\bdffhd_payload.dll";
    if (!FileExists(here + L"\\features.ini")) { ShowError(L"Missing features.ini next to this EXE."); if (argv) LocalFree(argv); return 1; }
    if (!FileExists(payload)) { ShowError(L"Missing bdffhd_payload.dll next to this EXE."); if (argv) LocalFree(argv); return 1; }

    std::wstring exe = argc > 1 ? argv[1] : L"";
    if (argv) LocalFree(argv);
    DWORD pid = FindProcess(L"BDFFHD.exe");
    if (!pid) {
        if (!IsGameExe(exe)) exe = FindGameExe();
        if (exe.empty()) exe = PickGameExe();
        if (exe.empty()) return 0;  // User cancelled the file picker.
        RememberGamePath(exe);
        std::wstring launchError;
        if (!LaunchGame(exe, launchError)) { ShowError(launchError); return 1; }
    }
    for (int i = 0; i < 600 && !pid; ++i) { pid = FindProcess(L"BDFFHD.exe"); Sleep(200); }
    for (int i = 0; i < 600 && pid && !HasModule(pid, L"GameAssembly.dll"); ++i) {
        Sleep(200);
        pid = FindProcess(L"BDFFHD.exe");
    }
    if (!pid || !HasModule(pid, L"GameAssembly.dll")) { ShowError(L"Timed out waiting for the game to load. Start the game fully, then try again."); return 1; }
    if (HasModule(pid, L"bdffhd_payload.dll")) return 0;
    Sleep(3000);  // let Unity/IL2CPP finish initialising

    std::wstring error;
    if (!Inject(pid, payload, error)) { ShowError(error); return 1; }
    return 0;
}
