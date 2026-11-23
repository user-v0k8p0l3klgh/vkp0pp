// Shared by the Wine tests: makes the current .exe look like Minecraft Bedrock
// 1.16.100 to the original module (PE timestamp, image size, the 16-byte
// prologues it verifies, and the four vtable slots it hooks).
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string.h>

static unsigned char big[0x3A00000]; // makes the image big enough for every RVA below

#define SLOT_OPEN 0x3301908
#define SLOT_TEARDOWN 0x3301910
#define SLOT_INPUT 0x3305FF8
#define SLOT_TICK 0x33018E8
#define FN_OPEN 0xC50D70
#define FN_TEARDOWN 0xC50E00
#define FN_INPUT 0xC14120
#define FN_TICK 0xC52A70
#define FN_NAV 0x777450

static const struct { uint32_t rva; unsigned char b[16]; } sigs[] = {
    {FN_NAV, {0x48,0x8b,0xc4,0x57,0x48,0x83,0xec,0x60,0x48,0xc7,0x40,0xb8,0xfe,0xff,0xff,0xff}},
    {FN_TICK, {0x48,0x8b,0xc4,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0xb8,0x48,0x81,0xec,0x30}},
    {FN_OPEN, {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xe8,0x12,0xe3,0xf4,0xff,0x0f,0xb6}},
    {FN_TEARDOWN, {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x89,0x28,0x09,0x00,0x00}},
    {FN_INPUT, {0x40,0x53,0x48,0x83,0xec,0x20,0x41,0x83,0x38,0x00,0x48,0x8b,0xd9,0x75,0x31,0x8b}},
};

static unsigned char *base;

static void protect(void *p, SIZE_T n, DWORD prot) { DWORD old; VirtualProtect(p, n, prot, &old); }

__attribute__((unused)) static uint64_t slot(uint32_t rva) { return *(volatile uint64_t *)(base + rva); }

static int fake_mc_setup(void) {
    base = (unsigned char *)GetModuleHandleW(NULL);
    if (base + FN_NAV < big || base + SLOT_INPUT + 8 > big + sizeof big) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    protect(base, 0x1000, PAGE_READWRITE);
    nt->FileHeader.TimeDateStamp = 0x5f9cbe99;
    nt->OptionalHeader.SizeOfImage = 0x39eb000;
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) memcpy(base + sigs[i].rva, sigs[i].b, 16);
    const uint32_t slots[4][2] = {{SLOT_OPEN, FN_OPEN}, {SLOT_TEARDOWN, FN_TEARDOWN}, {SLOT_INPUT, FN_INPUT}, {SLOT_TICK, FN_TICK}};
    for (int i = 0; i < 4; i++) *(uint64_t *)(base + slots[i][0]) = (uint64_t)(base + slots[i][1]);
    return 1;
}

// after the module has verified the prologues, make the "game" functions callable
__attribute__((unused)) static void put_jump(uint32_t rva, void *target) {
    unsigned char *p = base + rva;
    protect(p, 16, PAGE_EXECUTE_READWRITE);
    p[0] = 0x48; p[1] = 0xB8; memcpy(p + 2, &target, 8); p[10] = 0xFF; p[11] = 0xE0; // mov rax, imm64; jmp rax
    FlushInstructionCache(GetCurrentProcess(), p, 16);
}
