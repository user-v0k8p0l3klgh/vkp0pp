// Windows package identity and the per-user data directory.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "core/version.h"

namespace bll {

std::string narrow(const std::wstring& s);
std::wstring widen(const std::string& s);

// Full name of the package the current process runs in; empty if unpackaged.
std::string current_package_full_name();

// Full names of installed packages of a family for the current user.
std::vector<std::string> installed_packages(const std::wstring& family_name);

// Directory holding bll.ini, bll.log and frames.csv, as seen from inside the
// process. For the game's AppContainer %LOCALAPPDATA% is redirected to the
// package's own AC folder, the only place it may write to. BLL_HOME overrides.
std::filesystem::path data_directory();

// The same directory as seen from outside the container (used by the launcher).
std::filesystem::path data_directory_for_package(const std::wstring& family_name);

std::string read_text_file(const std::filesystem::path& path, bool* found = nullptr);

} // namespace bll
