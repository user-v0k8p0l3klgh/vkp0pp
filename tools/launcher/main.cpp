// bll-launcher: starts Minecraft with bll.dll loaded before the game's entry point.
//
//   bll-launcher            launch the game with BedrockLowLatency
//   bll-launcher --force    same, on a Minecraft UWP build other than 1.16.100.x
//                           (the DLL then stays in measurement-only mode)
//   bll-launcher status     show what is installed and where the files are
//   bll-launcher disable    clear the package debug registration (only needed if
//                           a launch was interrupted)
//
// Mechanism (the one documented for debuggers of packaged apps, and the same
// one Stonecutter uses): IPackageDebugSettings::EnableDebugging registers this
// executable as the package's debugger. On activation Windows creates the game
// process suspended and runs "<this exe> --inject -p <pid> -tid <tid>". That
// instance queues LoadLibraryW(bll.dll) on the suspended main thread and
// resumes it. The registration is removed again right after activation, so
// nothing persists: starting the game normally runs it unmodified.
//
// No game file is modified, no protection is bypassed, and the injector
// refuses any process that is not the Minecraft UWP package.
#include <windows.h>
#include <aclapi.h>
#include <appmodel.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/version.h"
#include "platform/package.h"

using namespace bll;

namespace {

const wchar_t* const kFamily = L"Microsoft.MinecraftUWP_8wekyb3d8bbwe";
const wchar_t* const kAppUserModelId = L"Microsoft.MinecraftUWP_8wekyb3d8bbwe!App";
constexpr size_t kPackageFullNameMax = 127; // PACKAGE_FULL_NAME_MAX_LENGTH

// {B1AEC16F-2383-4852-B0E9-8F0B1DC66B4D}
const CLSID kClsidPackageDebugSettings = {0xB1AEC16F, 0x2383, 0x4852, {0xB0, 0xE9, 0x8F, 0x0B, 0x1D, 0xC6, 0x6B, 0x4D}};
// {45BA127D-10A8-46EA-8AB7-56EA9078943C}
const CLSID kClsidApplicationActivationManager = {0x45BA127D, 0x10A8, 0x46EA, {0x8A, 0xB7, 0x56, 0xEA, 0x90, 0x78, 0x94, 0x3C}};

std::filesystem::path self_path() {
    wchar_t buffer[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return buffer;
}

std::filesystem::path dll_path() { return self_path().parent_path() / L"bll.dll"; }

// The game runs in an AppContainer and can only load files that grant read
// and execute to "ALL APPLICATION PACKAGES". The ACE is added to the existing
// ACL (nothing is removed) and addressed by SID, so it works on any UI language.
bool grant_app_packages_access(const std::wstring& file) {
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof sid;
    if (!CreateWellKnownSid(WinBuiltinAnyPackageSid, nullptr, sid, &sid_size)) return false;

    PACL old_acl = nullptr, new_acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_acl,
                              nullptr, &descriptor) != ERROR_SUCCESS)
        return false;

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);

    bool ok = false;
    if (SetEntriesInAclW(1, &access, old_acl, &new_acl) == ERROR_SUCCESS) {
        ok = SetNamedSecurityInfoW(const_cast<LPWSTR>(file.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                   nullptr, new_acl, nullptr) == ERROR_SUCCESS;
        LocalFree(new_acl);
    }
    LocalFree(descriptor);
    return ok;
}

struct Installed {
    std::string full_name;
    PackageIdentity id;
    BuildClass build = BuildClass::Unpackaged;
};

bool find_game(Installed& out) {
    for (const std::string& full : installed_packages(kFamily)) {
        PackageIdentity id;
        if (!parse_package_full_name(full, id)) continue;
        out = {full, id, classify_build(id)};
        if (out.build == BuildClass::Target) return true; // prefer the supported build
    }
    return !out.full_name.empty();
}

void print_game(const Installed& game) {
    std::printf("package   : %s\n", game.full_name.c_str());
    std::printf("game      : %s (%s)\n", game_version(game.id.version).c_str(), game.id.architecture.c_str());
    std::printf("status    : %s\n", to_string(game.build));
}

// --- injector side (run by Windows as the package's "debugger") -------------

void inject_log(const char* message, DWORD code = 0) {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::ofstream out(std::filesystem::path(temp) / L"bll-inject.log", std::ios::app);
    out << message;
    if (code) out << " (error " << code << ")";
    out << "\n";
}

int inject(DWORD thread_id) {
    const HANDLE thread =
        OpenThread(THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
    if (!thread) {
        inject_log("OpenThread failed", GetLastError());
        return 1;
    }
    // Whatever happens below, the game's main thread is resumed so that a
    // failure degrades to a normal, unmodified start instead of a hang.
    int result = 1;
    const DWORD pid = GetProcessIdOfThread(thread);
    const HANDLE process = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        inject_log("OpenProcess failed", GetLastError());
    } else {
        wchar_t full_name[kPackageFullNameMax + 1] = {};
        UINT32 length = static_cast<UINT32>(std::size(full_name));
        PackageIdentity id;
        BOOL wow64 = FALSE;
        const std::wstring dll = dll_path().wstring();
        if (GetPackageFullName(process, &length, full_name) != ERROR_SUCCESS ||
            !parse_package_full_name(narrow(full_name), id) || classify_build(id) == BuildClass::Foreign ||
            classify_build(id) == BuildClass::Unpackaged) {
            inject_log("refused: the target process is not the Minecraft UWP package");
        } else if (IsWow64Process(process, &wow64) && wow64) {
            inject_log("refused: the target process is not 64-bit");
        } else if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            inject_log("bll.dll not found next to bll-launcher.exe");
        } else {
            const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
            void* const remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            // kernel32 is mapped at the same address in every process of a boot
            // session, so LoadLibraryW's local address is valid in the target.
            if (!remote || !WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
                inject_log("could not write the DLL path into the game process", GetLastError());
            } else if (!QueueUserAPC(reinterpret_cast<PAPCFUNC>(reinterpret_cast<void*>(&LoadLibraryW)), thread,
                                     reinterpret_cast<ULONG_PTR>(remote))) {
                inject_log("QueueUserAPC failed", GetLastError());
            } else {
                inject_log(("queued bll.dll for " + narrow(full_name)).c_str());
                result = 0;
            }
        }
        CloseHandle(process);
    }
    ResumeThread(thread);
    CloseHandle(thread);
    return result;
}

// --- launcher side ----------------------------------------------------------

int launch(bool force) {
    Installed game;
    if (!find_game(game)) {
        std::printf("Minecraft (Microsoft.MinecraftUWP) is not installed for this user.\n"
                    "BedrockLowLatency supports the UWP release 1.16.100 only; GDK builds are not supported.\n");
        return 1;
    }
    print_game(game);
    if (game.build != BuildClass::Target && !force) {
        std::printf("\nThis is not Minecraft 1.16.100.x (x64). Nothing was started.\n"
                    "Use --force to start it anyway; bll.dll will then only measure and change nothing.\n");
        return 1;
    }

    const std::filesystem::path dll = dll_path();
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("bll.dll must be in the same folder as bll-launcher.exe.\n");
        return 1;
    }
    if (!grant_app_packages_access(dll.wstring()))
        std::printf("warning: could not grant the game read access to bll.dll (error %lu); loading may fail.\n",
                    GetLastError());

    // Configuration and log live in the package's own data folder, the only
    // place the sandboxed game can write to.
    const std::filesystem::path data = data_directory_for_package(kFamily);
    std::error_code ec;
    std::filesystem::create_directories(data, ec);
    const std::filesystem::path ini = data / L"bll.ini", log = data / L"bll.log";
    if (!std::filesystem::exists(ini, ec)) {
        std::ofstream out(ini, std::ios::binary);
        out << kDefaultConfigText;
    }
    std::filesystem::remove(log, ec);
    std::printf("config    : %ls\nlog       : %ls\n\n", ini.c_str(), log.c_str());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;
    IPackageDebugSettings* settings = nullptr;
    IApplicationActivationManager* activation = nullptr;
    hr = CoCreateInstance(kClsidPackageDebugSettings, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&settings));
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(kClsidApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&activation));
    if (FAILED(hr)) {
        std::printf("could not reach the Windows package services (0x%08lX).\n", static_cast<unsigned long>(hr));
        return 1;
    }

    const std::wstring full = widen(game.full_name);
    const std::wstring debugger = L"\"" + self_path().wstring() + L"\" --inject";
    // A running instance has to be closed: the DLL must be loaded at start-up.
    settings->TerminateAllProcesses(full.c_str());
    hr = settings->EnableDebugging(full.c_str(), debugger.c_str(), nullptr);
    DWORD pid = 0;
    if (SUCCEEDED(hr)) hr = activation->ActivateApplication(kAppUserModelId, nullptr, AO_NONE, &pid);
    // Always undo the registration, also when activation failed.
    const HRESULT undo = settings->DisableDebugging(full.c_str());
    activation->Release();
    settings->Release();
    CoUninitialize();

    if (FAILED(hr)) {
        std::printf("launch failed (0x%08lX). The game was not modified; start it normally to play without "
                    "BedrockLowLatency.\n", static_cast<unsigned long>(hr));
        return 1;
    }
    if (FAILED(undo))
        std::printf("warning: could not clear the debug registration (0x%08lX); run \"bll-launcher disable\".\n",
                    static_cast<unsigned long>(undo));

    std::printf("Minecraft started (process %lu). Waiting for bll.dll to report in", pid);
    for (int i = 0; i < 40; ++i) {
        if (std::filesystem::exists(log, ec) && std::filesystem::file_size(log, ec) > 0) {
            std::printf("\nbll.dll is active. First log lines:\n\n");
            std::ifstream in(log);
            std::string line;
            for (int n = 0; n < 12 && std::getline(in, line); ++n) std::printf("  %s\n", line.c_str());
            return 0;
        }
        std::printf(".");
        std::fflush(stdout);
        Sleep(500);
    }
    std::printf("\nbll.dll did not report in. The game is running unmodified. See %%TEMP%%\\bll-inject.log.\n");
    return 2;
}

int status() {
    Installed game;
    if (find_game(game)) print_game(game);
    else std::printf("package   : Minecraft UWP is not installed for this user\n");
    const std::filesystem::path data = data_directory_for_package(kFamily);
    std::error_code ec;
    std::printf("bll.dll   : %ls (%s)\n", dll_path().c_str(),
                std::filesystem::exists(dll_path(), ec) ? "present" : "MISSING");
    std::printf("config    : %ls (%s)\n", (data / L"bll.ini").c_str(),
                std::filesystem::exists(data / L"bll.ini", ec) ? "present" : "created on first launch");
    std::printf("log       : %ls\n", (data / L"bll.log").c_str());
    return 0;
}

int disable() {
    Installed game;
    if (!find_game(game)) return 1;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    IPackageDebugSettings* settings = nullptr;
    HRESULT hr = CoCreateInstance(kClsidPackageDebugSettings, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&settings));
    if (SUCCEEDED(hr)) {
        hr = settings->DisableDebugging(widen(game.full_name).c_str());
        settings->Release();
    }
    CoUninitialize();
    std::printf("debug registration cleared: %s (0x%08lX)\n", SUCCEEDED(hr) ? "yes" : "no",
                static_cast<unsigned long>(hr));
    return SUCCEEDED(hr) ? 0 : 1;
}

} // namespace

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool force = false;
    std::wstring command = L"launch";
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring arg = argv[i];
        // Appended by Windows when it starts us as the package debugger.
        if (arg == L"-tid" && i + 1 < argc) return inject(static_cast<DWORD>(wcstoul(argv[i + 1], nullptr, 10)));
        if (arg == L"--force") force = true;
        else if (arg == L"status" || arg == L"disable" || arg == L"launch") command = arg;
        else if (arg != L"--inject" && arg != L"-p" && !(i > 1 && std::wstring(argv[i - 1]) == L"-p")) {
            std::printf("usage: bll-launcher [launch] [--force] | status | disable\n");
            return 2;
        }
    }
    if (command == L"status") return status();
    if (command == L"disable") return disable();
    return launch(force);
}
