#include "platform/pe_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace bll {

namespace {

class Reader {
public:
    Reader(const uint8_t* base, size_t size) : base_(base), size_(size) {}

    template <typename T>
    bool read(size_t offset, T& out) const {
        if (offset > size_ || size_ - offset < sizeof(T)) return false;
        std::memcpy(&out, base_ + offset, sizeof(T));
        return true;
    }

    // NUL-terminated ASCII string with a sanity limit.
    bool read_string(size_t offset, std::string& out) const {
        out.clear();
        for (size_t i = offset; i < size_ && out.size() < 512; ++i) {
            if (base_[i] == 0) return true;
            out.push_back(static_cast<char>(base_[i]));
        }
        return false;
    }

private:
    const uint8_t* base_;
    size_t size_;
};

struct Headers {
    uint32_t nt = 0;        // offset of "PE\0\0"
    uint16_t machine = 0;
    uint16_t sections = 0;
    uint16_t optional_size = 0;
    uint16_t magic = 0;
    uint32_t size_of_image = 0;
    uint32_t size_of_headers = 0;
    uint32_t dir_count = 0;
    uint32_t dirs = 0;      // offset of the data directory array
};

bool read_headers(const Reader& r, Headers& h) {
    uint16_t mz = 0;
    uint32_t sig = 0;
    if (!r.read(0, mz) || mz != 0x5A4D) return false;
    if (!r.read(0x3C, h.nt) || !r.read(h.nt, sig) || sig != 0x00004550) return false;
    const uint32_t file_header = h.nt + 4;
    const uint32_t optional = file_header + 20;
    if (!r.read(file_header, h.machine) || !r.read(file_header + 2, h.sections) ||
        !r.read(file_header + 16, h.optional_size) || !r.read(optional, h.magic))
        return false;
    if (h.magic != 0x20B) return false; // PE32+ only
    if (!r.read(optional + 56, h.size_of_image) || !r.read(optional + 60, h.size_of_headers) ||
        !r.read(optional + 108, h.dir_count))
        return false;
    h.dirs = optional + 112;
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

constexpr uint64_t kOrdinalFlag = uint64_t{1} << 63;
constexpr size_t kMaxEntries = 65536;

// Walks one name table / address table pair of an import descriptor.
void read_thunks(const Reader& r, const std::string& dll, uint32_t names_rva, uint32_t iat_rva, bool delay,
                 std::vector<PeImport>& out) {
    for (size_t i = 0; i < kMaxEntries; ++i) {
        uint64_t thunk = 0;
        if (!r.read(names_rva + i * 8, thunk) || thunk == 0) break;
        PeImport imp;
        imp.dll = dll;
        imp.iat_rva = iat_rva + static_cast<uint32_t>(i * 8);
        imp.delay_load = delay;
        if (thunk & kOrdinalFlag) {
            imp.ordinal = static_cast<uint16_t>(thunk & 0xFFFF);
        } else if (!r.read_string(static_cast<size_t>(thunk) + 2, imp.function)) {
            break;
        }
        out.push_back(std::move(imp));
    }
}

} // namespace

PeInfo parse_pe_image(const uint8_t* base, size_t size) {
    PeInfo info;
    const Reader r(base, size);
    Headers h;
    if (!base || !read_headers(r, h)) return info;
    info.valid = true;
    info.machine = h.machine;
    info.is_64bit = true;
    info.size_of_image = h.size_of_image;

    auto directory = [&](uint32_t index, uint32_t& rva, uint32_t& dir_size) {
        rva = dir_size = 0;
        return index < h.dir_count && r.read(h.dirs + index * 8, rva) && r.read(h.dirs + index * 8 + 4, dir_size) &&
               rva != 0;
    };
    auto add_dll = [&](const std::string& name) {
        const std::string l = lower(name);
        if (std::find(info.import_dlls.begin(), info.import_dlls.end(), l) == info.import_dlls.end())
            info.import_dlls.push_back(l);
    };

    uint32_t rva = 0, dir_size = 0;
    if (directory(1, rva, dir_size)) { // IMAGE_DIRECTORY_ENTRY_IMPORT
        for (size_t i = 0; i < kMaxEntries; ++i) {
            const size_t d = rva + i * 20;
            uint32_t original_first_thunk = 0, name_rva = 0, first_thunk = 0;
            if (!r.read(d, original_first_thunk) || !r.read(d + 12, name_rva) || !r.read(d + 16, first_thunk)) break;
            if (name_rva == 0 && first_thunk == 0) break;
            std::string dll;
            if (!r.read_string(name_rva, dll)) break;
            add_dll(dll);
            // Bound images may have no name table; the IAT then holds the names until binding.
            read_thunks(r, dll, original_first_thunk ? original_first_thunk : first_thunk, first_thunk, false,
                        info.imports);
        }
    }
    if (directory(13, rva, dir_size)) { // IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT
        for (size_t i = 0; i < kMaxEntries; ++i) {
            const size_t d = rva + i * 32;
            uint32_t attributes = 0, name_rva = 0, iat_rva = 0, int_rva = 0;
            if (!r.read(d, attributes) || !r.read(d + 4, name_rva) || !r.read(d + 12, iat_rva) ||
                !r.read(d + 16, int_rva))
                break;
            if (name_rva == 0) break;
            // Only RVA-based descriptors (attribute bit 0); the VA form predates x64.
            if (!(attributes & 1)) break;
            std::string dll;
            if (!r.read_string(name_rva, dll)) break;
            add_dll(dll);
            read_thunks(r, dll, int_rva, iat_rva, true, info.imports);
        }
    }
    return info;
}

std::vector<uint8_t> map_pe_file(const std::vector<uint8_t>& file) {
    const Reader r(file.data(), file.size());
    Headers h;
    if (!read_headers(r, h)) return {};
    // Refuse absurd sizes rather than allocating them.
    if (h.size_of_image == 0 || h.size_of_image > (512u << 20) || h.size_of_headers > file.size()) return {};

    std::vector<uint8_t> image(h.size_of_image, 0);
    std::memcpy(image.data(), file.data(), std::min<size_t>(h.size_of_headers, image.size()));

    const size_t table = h.nt + 24 + h.optional_size;
    for (uint16_t i = 0; i < h.sections; ++i) {
        const size_t s = table + static_cast<size_t>(i) * 40;
        uint32_t virtual_size = 0, virtual_address = 0, raw_size = 0, raw_offset = 0;
        if (!r.read(s + 8, virtual_size) || !r.read(s + 12, virtual_address) || !r.read(s + 16, raw_size) ||
            !r.read(s + 20, raw_offset))
            return {};
        if (raw_offset > file.size() || virtual_address > image.size()) continue;
        size_t n = std::min<size_t>(raw_size, file.size() - raw_offset);
        n = std::min<size_t>(n, image.size() - virtual_address);
        std::memcpy(image.data() + virtual_address, file.data() + raw_offset, n);
    }
    return image;
}

bool contains_ascii(const uint8_t* data, size_t size, std::string_view needle) {
    if (needle.empty() || size < needle.size()) return false;
    const uint8_t* end = data + size;
    return std::search(data, end, needle.begin(), needle.end(),
                       [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); }) != end;
}

bool contains_utf16(const uint8_t* data, size_t size, std::string_view needle) {
    std::string wide;
    wide.reserve(needle.size() * 2);
    for (char c : needle) {
        wide.push_back(c);
        wide.push_back('\0');
    }
    return contains_ascii(data, size, wide);
}

} // namespace bll
