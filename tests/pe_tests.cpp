#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

#include "platform/pe_image.h"
#include "test.h"

using namespace bll;

namespace {

template <typename T>
void put(std::vector<uint8_t>& b, size_t off, T v) {
    std::memcpy(b.data() + off, &v, sizeof v);
}

void put_str(std::vector<uint8_t>& b, size_t off, const char* s) { std::memcpy(b.data() + off, s, std::strlen(s) + 1); }

// A minimal PE32+ file with one section whose file offset differs from its RVA.
// `at(rva)` maps an RVA inside the section to its file offset.
std::vector<uint8_t> make_pe() {
    constexpr size_t kRawOffset = 0x400, kSectionRva = 0x1000;
    std::vector<uint8_t> f(kRawOffset + 0x2000, 0);
    auto at = [&](size_t rva) { return rva - kSectionRva + kRawOffset; };

    put<uint16_t>(f, 0, 0x5A4D);
    put<uint32_t>(f, 0x3C, 0x80);
    put<uint32_t>(f, 0x80, 0x00004550);
    put<uint16_t>(f, 0x84, 0x8664); // machine
    put<uint16_t>(f, 0x86, 1);      // sections
    put<uint16_t>(f, 0x94, 240);    // optional header size
    const size_t opt = 0x98;
    put<uint16_t>(f, opt, 0x20B);
    put<uint32_t>(f, opt + 56, 0x3000); // SizeOfImage
    put<uint32_t>(f, opt + 60, 0x400);  // SizeOfHeaders
    put<uint32_t>(f, opt + 108, 16);
    put<uint32_t>(f, opt + 112 + 1 * 8, 0x1000);  // import directory
    put<uint32_t>(f, opt + 112 + 1 * 8 + 4, 60);
    put<uint32_t>(f, opt + 112 + 13 * 8, 0x1800); // delay import directory
    put<uint32_t>(f, opt + 112 + 13 * 8 + 4, 64);
    const size_t sec = opt + 240;
    std::memcpy(f.data() + sec, ".rdata", 6);
    put<uint32_t>(f, sec + 8, 0x2000);
    put<uint32_t>(f, sec + 12, kSectionRva);
    put<uint32_t>(f, sec + 16, 0x2000);
    put<uint32_t>(f, sec + 20, kRawOffset);

    // Import descriptors: d3d11.dll (one name, one ordinal), XINPUT1_4.dll (one name).
    put<uint32_t>(f, at(0x1000), 0x1100);
    put<uint32_t>(f, at(0x1000) + 12, 0x1200);
    put<uint32_t>(f, at(0x1000) + 16, 0x1300);
    put<uint32_t>(f, at(0x1014), 0x1140);
    put<uint32_t>(f, at(0x1014) + 12, 0x1210);
    put<uint32_t>(f, at(0x1014) + 16, 0x1340);
    put<uint64_t>(f, at(0x1100), 0x1400);
    put<uint64_t>(f, at(0x1108), (uint64_t{1} << 63) | 5);
    put<uint64_t>(f, at(0x1140), 0x1420);
    put_str(f, at(0x1200), "d3d11.dll");
    put_str(f, at(0x1210), "XINPUT1_4.dll");
    put_str(f, at(0x1402), "D3D11CreateDevice");
    put_str(f, at(0x1422), "XInputGetState");

    // Delay-load descriptor: dxgi.dll!CreateDXGIFactory1.
    put<uint32_t>(f, at(0x1800), 1);
    put<uint32_t>(f, at(0x1800) + 4, 0x1900);
    put<uint32_t>(f, at(0x1800) + 12, 0x1A00);
    put<uint32_t>(f, at(0x1800) + 16, 0x1A40);
    put_str(f, at(0x1900), "dxgi.dll");
    put<uint64_t>(f, at(0x1A40), 0x1A80);
    put_str(f, at(0x1A82), "CreateDXGIFactory1");

    const char16_t wide[] = u"Windows.Gaming.Input.Gamepad";
    std::memcpy(f.data() + at(0x1B00), wide, sizeof wide);
    return f;
}

const PeImport* find(const PeInfo& info, const char* function) {
    for (const PeImport& i : info.imports)
        if (i.function == function) return &i;
    return nullptr;
}

} // namespace

TEST(pe_reads_imports_and_delay_imports) {
    const std::vector<uint8_t> image = map_pe_file(make_pe());
    CHECK_EQ(image.size(), size_t{0x3000});
    const PeInfo info = parse_pe_image(image.data(), image.size());
    CHECK(info.valid);
    CHECK_EQ(info.machine, kMachineAmd64);
    CHECK_EQ(info.import_dlls.size(), size_t{3});
    if (info.import_dlls.size() != 3) return;
    CHECK(info.import_dlls[0] == "d3d11.dll");
    CHECK(info.import_dlls[1] == "xinput1_4.dll"); // normalised to lower case
    CHECK(info.import_dlls[2] == "dxgi.dll");
    CHECK_EQ(info.imports.size(), size_t{4});

    const PeImport* create = find(info, "D3D11CreateDevice");
    CHECK(create && create->iat_rva == 0x1300 && !create->delay_load);
    const PeImport* xinput = find(info, "XInputGetState");
    CHECK(xinput && xinput->iat_rva == 0x1340 && xinput->dll == "XINPUT1_4.dll");
    const PeImport* factory = find(info, "CreateDXGIFactory1");
    CHECK(factory && factory->iat_rva == 0x1A00 && factory->delay_load);
    bool ordinal_seen = false;
    for (const PeImport& i : info.imports) ordinal_seen |= i.function.empty() && i.ordinal == 5 && i.iat_rva == 0x1308;
    CHECK(ordinal_seen);

    CHECK(contains_utf16(image.data(), image.size(), "Windows.Gaming.Input.Gamepad"));
    CHECK(!contains_utf16(image.data(), image.size(), "Windows.Gaming.Input.RawGameController"));
    CHECK(contains_ascii(image.data(), image.size(), "XInputGetState"));
}

TEST(pe_rejects_non_pe_and_survives_corruption) {
    CHECK(map_pe_file({}).empty());
    CHECK(!parse_pe_image(nullptr, 0).valid);
    const std::vector<uint8_t> text{'h', 'e', 'l', 'l', 'o'};
    CHECK(map_pe_file(text).empty());
    CHECK(!parse_pe_image(text.data(), text.size()).valid);

    // Truncations and random corruption must never read out of bounds
    // (run under ASan/UBSan in CI) and never loop forever.
    const std::vector<uint8_t> good = make_pe();
    for (size_t cut : {size_t{1}, size_t{0x40}, size_t{0x90}, size_t{0x120}, size_t{0x400}, size_t{0x500}}) {
        std::vector<uint8_t> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        const std::vector<uint8_t> image = map_pe_file(t);
        parse_pe_image(image.data(), image.size());
        parse_pe_image(t.data(), t.size());
    }
    std::mt19937 rng(99);
    for (int round = 0; round < 3000; ++round) {
        std::vector<uint8_t> bad = good;
        for (int k = 0; k < 6; ++k) bad[rng() % 0x420] = static_cast<uint8_t>(rng());
        for (int k = 0; k < 6; ++k) bad[0x400 + rng() % 0x1C00] = static_cast<uint8_t>(rng());
        const std::vector<uint8_t> image = map_pe_file(bad);
        const PeInfo info = parse_pe_image(image.data(), image.size());
        CHECK(info.imports.size() <= 200000);
    }
}

// When the cross-compiled DLL is available, check the parser against a real
// linker-produced image as well.
TEST(pe_reads_the_built_dll_when_available) {
    const char* path = std::getenv("BLL_TEST_PE");
    if (!path || !*path) {
        std::printf("    skipped: BLL_TEST_PE not set\n");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!file.empty());
    const std::vector<uint8_t> image = map_pe_file(file);
    const PeInfo info = parse_pe_image(image.data(), image.size());
    CHECK(info.valid);
    CHECK_EQ(info.machine, kMachineAmd64);
    bool kernel32 = false, dxgi = false;
    for (const std::string& d : info.import_dlls) {
        kernel32 |= d == "kernel32.dll";
        dxgi |= d == "dxgi.dll";
    }
    CHECK(kernel32);
    CHECK(dxgi);
    CHECK(find(info, "QueryPerformanceCounter") != nullptr);
    std::printf("    %zu imports from %zu DLLs in %s\n", info.imports.size(), info.import_dlls.size(), path);
}
