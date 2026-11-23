// bll-probe: static inspection of a game executable.
//
//   bll-probe <path-to-Minecraft.Windows.exe>
//
// Reports the architecture and which graphics / input APIs the executable
// references, so the renderer and controller back-end of a specific build can
// be identified before anything is hooked. It only reads the file's import
// tables and searches for API names; nothing is modified, unpacked or copied.
//
// Store-installed packages are encrypted at rest and cannot be read this way;
// in that case the same information is logged at run time by bll.dll.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "platform/pe_image.h"

namespace {

bool imports(const bll::PeInfo& info, const char* prefix) {
    for (const std::string& d : info.import_dlls)
        if (d.rfind(prefix, 0) == 0) return true;
    return false;
}

const char* yn(bool b) { return b ? "yes" : "no"; }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: bll-probe <executable>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s (Store-installed packages are not readable; use the bll.dll log)\n",
                     argv[1]);
        return 1;
    }
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::vector<uint8_t> image = bll::map_pe_file(file);
    const bll::PeInfo info = bll::parse_pe_image(image.data(), image.size());
    if (!info.valid) {
        std::fprintf(stderr, "%s is not a readable 64-bit PE image (encrypted, 32-bit or not an executable)\n",
                     argv[1]);
        return 1;
    }

    const uint8_t* d = image.data();
    const size_t n = image.size();
    auto text = [&](const char* s) { return bll::contains_ascii(d, n, s) || bll::contains_utf16(d, n, s); };

    std::printf("file              : %s\n", argv[1]);
    std::printf("architecture      : %s (machine 0x%04X)\n", info.machine == bll::kMachineAmd64 ? "x64" : "other",
                info.machine);
    std::printf("imported DLLs     : %zu\n", info.import_dlls.size());
    for (const std::string& dll : info.import_dlls) std::printf("    %s\n", dll.c_str());

    const bool d3d11 = imports(info, "d3d11");
    const bool d3d12 = imports(info, "d3d12");
    std::printf("\ngraphics\n");
    std::printf("  imports d3d11.dll                 : %s\n", yn(d3d11));
    std::printf("  imports d3d12.dll                 : %s\n", yn(d3d12));
    std::printf("  imports dxgi.dll                  : %s\n", yn(imports(info, "dxgi")));
    std::printf("  references D3D12CreateDevice      : %s\n", yn(text("D3D12CreateDevice")));
    std::printf("  references bgfx / RenderDragon    : %s\n", yn(text("bgfx") || text("RenderDragon")));
    std::printf("  references CoreWindow             : %s\n", yn(text("Windows.UI.Core.CoreWindow")));

    std::printf("\ncontroller input\n");
    std::printf("  imports xinput*.dll               : %s\n", yn(imports(info, "xinput")));
    std::printf("  references XInputGetState         : %s\n", yn(text("XInputGetState")));
    std::printf("  references Windows.Gaming.Input.Gamepad           : %s\n", yn(text("Windows.Gaming.Input.Gamepad")));
    std::printf("  references Windows.Gaming.Input.RawGameController : %s\n",
                yn(text("Windows.Gaming.Input.RawGameController")));
    std::printf("  references Windows.Gaming.Input.ArcadeStick       : %s\n",
                yn(text("Windows.Gaming.Input.ArcadeStick")));

    std::printf("\nassessment\n");
    if (text("bgfx") || text("RenderDragon") || d3d12)
        std::printf("  This looks like a RenderDragon-era build. BedrockLowLatency targets the\n"
                    "  pre-RenderDragon 1.16.100 client and will stay in measurement-only mode.\n");
    else if (d3d11)
        std::printf("  Direct3D 11 without RenderDragon markers: consistent with the 1.16.100 target.\n");
    else
        std::printf("  No Direct3D import found; the renderer may be loaded dynamically. Check the bll.dll log.\n");
    std::printf("  Static references show what the executable can use, not what it does use at run time;\n"
                "  the bll.dll log reports the API calls actually observed.\n");
    return 0;
}
