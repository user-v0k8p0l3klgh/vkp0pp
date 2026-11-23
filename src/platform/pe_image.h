// Minimal PE32+ reader used for two things:
//   - reporting which graphics / input DLLs a module imports (backend detection),
//   - locating import-address-table slots so imported functions can be redirected.
// It works on images in their *mapped* layout (as loaded in a process). Files
// on disk are first expanded into that layout with map_pe_file().
// Every access is bounds-checked against the image size; malformed input
// yields empty results instead of undefined behaviour.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bll {

struct PeImport {
    std::string dll;      // as written in the import table
    std::string function; // empty for imports by ordinal
    uint16_t ordinal = 0;
    uint32_t iat_rva = 0; // RVA of the pointer-sized IAT slot
    bool delay_load = false;
};

struct PeInfo {
    bool valid = false;
    uint16_t machine = 0; // 0x8664 = x64
    bool is_64bit = false;
    uint32_t size_of_image = 0;
    std::vector<PeImport> imports;
    std::vector<std::string> import_dlls; // unique, lower-case, normal then delay-load
};

// Parses an image in mapped layout.
PeInfo parse_pe_image(const uint8_t* base, size_t size);

// Expands a PE file into mapped layout. Returns an empty vector if it is not a PE file.
std::vector<uint8_t> map_pe_file(const std::vector<uint8_t>& file);

// True if `needle` occurs in the data as UTF-16LE text (how WinRT class names
// such as "Windows.Gaming.Input.Gamepad" are stored in a binary).
bool contains_utf16(const uint8_t* data, size_t size, std::string_view needle);
bool contains_ascii(const uint8_t* data, size_t size, std::string_view needle);

inline constexpr uint16_t kMachineAmd64 = 0x8664;

} // namespace bll
