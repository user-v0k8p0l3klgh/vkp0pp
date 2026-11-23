#include "platform/package.h"

#include <windows.h>
#include <appmodel.h>

#include <fstream>
#include <sstream>

namespace bll {

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string current_package_full_name() {
    UINT32 length = 0;
    if (GetCurrentPackageFullName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER) return {};
    std::wstring name(length, L'\0');
    if (GetCurrentPackageFullName(&length, name.data()) != ERROR_SUCCESS) return {};
    name.resize(length ? length - 1 : 0);
    return narrow(name);
}

std::vector<std::string> installed_packages(const std::wstring& family_name) {
    UINT32 count = 0, buffer_length = 0;
    LONG rc = GetPackagesByPackageFamily(family_name.c_str(), &count, nullptr, &buffer_length, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0) return {};
    std::vector<PWSTR> names(count);
    std::wstring buffer(buffer_length, L'\0');
    rc = GetPackagesByPackageFamily(family_name.c_str(), &count, names.data(), &buffer_length, buffer.data());
    if (rc != ERROR_SUCCESS) return {};
    std::vector<std::string> out;
    for (UINT32 i = 0; i < count; ++i) out.push_back(narrow(names[i]));
    return out;
}

namespace {
std::wstring env(const wchar_t* name) {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return n > 0 && n < std::size(buffer) ? std::wstring(buffer, n) : std::wstring();
}
} // namespace

std::filesystem::path data_directory() {
    if (const std::wstring home = env(L"BLL_HOME"); !home.empty()) return home;
    return std::filesystem::path(env(L"LOCALAPPDATA")) / L"BedrockLowLatency";
}

std::filesystem::path data_directory_for_package(const std::wstring& family_name) {
    return std::filesystem::path(env(L"LOCALAPPDATA")) / L"Packages" / family_name / L"AC" / L"BedrockLowLatency";
}

std::string read_text_file(const std::filesystem::path& path, bool* found) {
    std::ifstream in(path, std::ios::binary);
    if (found) *found = static_cast<bool>(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace bll
