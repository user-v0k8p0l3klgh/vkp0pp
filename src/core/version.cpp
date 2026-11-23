#include "core/version.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <vector>

namespace bll {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

bool parse_u16(std::string_view s, uint16_t& out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

} // namespace

const char* to_string(BuildClass c) {
    switch (c) {
    case BuildClass::Target: return "target (Minecraft UWP x64, release 1.16.100.4)";
    case BuildClass::OtherBedrock: return "other Minecraft UWP build";
    case BuildClass::Foreign: return "foreign package";
    default: return "unpackaged process";
    }
}

bool parse_package_full_name(std::string_view full, PackageIdentity& out) {
    std::vector<std::string_view> parts;
    while (true) {
        const size_t sep = full.find('_');
        parts.push_back(full.substr(0, sep));
        if (sep == std::string_view::npos) break;
        full.remove_prefix(sep + 1);
    }
    // Name_Version_Architecture_ResourceId_PublisherId; the resource id is usually empty.
    if (parts.size() != 5 || parts[0].empty() || parts[4].empty()) return false;

    PackageVersion v;
    uint16_t* const fields[4] = {&v.major, &v.minor, &v.build, &v.revision};
    std::string_view ver = parts[1];
    for (int i = 0; i < 4; ++i) {
        const size_t dot = ver.find('.');
        if ((dot == std::string_view::npos) != (i == 3)) return false;
        if (!parse_u16(ver.substr(0, dot), *fields[i])) return false;
        if (i < 3) ver.remove_prefix(dot + 1);
    }

    out.name = std::string(parts[0]);
    out.version = v;
    out.architecture = std::string(parts[2]);
    out.publisher_id = std::string(parts[4]);
    return true;
}

BuildClass classify_build(const PackageIdentity& id) {
    if (id.name.empty()) return BuildClass::Unpackaged;
    if (!iequals(id.name, kTargetPackageName) || !iequals(id.publisher_id, "8wekyb3d8bbwe")) return BuildClass::Foreign;
    const PackageVersion& v = id.version;
    // Exactly the release build. Betas of the 1.16.100 cycle shipped under the
    // same package name as far as is known, and some (1.16.100.54, .59) used RenderDragon,
    // so "any 1.16.100.x" would be too wide.
    const bool target_version = v.major == 1 && v.minor == 16 && v.build == kTargetPackageBuild;
    return target_version && iequals(id.architecture, "x64") ? BuildClass::Target : BuildClass::OtherBedrock;
}

std::string game_version(const PackageVersion& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.build / 100) + "." +
           std::to_string(v.build % 100);
}

} // namespace bll
