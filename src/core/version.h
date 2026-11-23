// Target-build identification. The DLL only changes behaviour on the package
// it was written for; anything else gets measurement-only mode unless the
// user explicitly overrides it.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace bll {

inline constexpr std::string_view kTargetFamilyName = "Microsoft.MinecraftUWP_8wekyb3d8bbwe";
inline constexpr std::string_view kTargetPackageName = "Microsoft.MinecraftUWP";

struct PackageVersion {
    uint16_t major = 0, minor = 0, build = 0, revision = 0;
    bool operator==(const PackageVersion&) const = default;
};

enum class BuildClass {
    Target,       // Minecraft UWP x64, game version 1.16.100.x
    OtherBedrock, // Minecraft UWP package, different game version or architecture
    Foreign,      // some other package
    Unpackaged,   // no package identity (test harness, GDK builds)
};

const char* to_string(BuildClass c);

struct PackageIdentity {
    std::string name;         // "Microsoft.MinecraftUWP"
    PackageVersion version;
    std::string architecture; // "x64"
    std::string publisher_id; // "8wekyb3d8bbwe"
};

// Parses "Name_1.2.3.4_arch_resourceid_publisherid". Returns false if malformed.
bool parse_package_full_name(std::string_view full_name, PackageIdentity& out);

BuildClass classify_build(const PackageIdentity& id);

// Bedrock packs the game's patch and revision numbers into the package build
// field: package 1.16.10004.0 is game version 1.16.100.4.
std::string game_version(const PackageVersion& v);

} // namespace bll
