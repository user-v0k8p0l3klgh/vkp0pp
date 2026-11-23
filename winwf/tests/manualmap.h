// Minimal manual mapper, as used by many game injectors: maps sections,
// applies relocations, resolves imports, registers unwind data, runs TLS
// callbacks and DllMain, but (like most of them) never sets up the module's
// implicit TLS slot. Used to check winwf.dll survives that.
#pragma once
#include <windows.h>
#include <stdio.h>

static void *mm_load(const wchar_t *file) {
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("mm: open failed\n"); return NULL; }
    DWORD size = GetFileSize(f, NULL), got = 0;
    unsigned char *raw = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ReadFile(f, raw, size, &got, NULL);
    CloseHandle(f);

    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(raw + ((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    unsigned char *img = VirtualAlloc(NULL, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(img, raw, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].SizeOfRawData) memcpy(img + sec[i].VirtualAddress, raw + sec[i].PointerToRawData, sec[i].SizeOfRawData);

    IMAGE_DATA_DIRECTORY *dd = nt->OptionalHeader.DataDirectory;
    LONGLONG delta = (LONGLONG)(ULONG_PTR)img - (LONGLONG)nt->OptionalHeader.ImageBase;
    if (delta && dd[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
        IMAGE_BASE_RELOCATION *r = (IMAGE_BASE_RELOCATION *)(img + dd[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
        unsigned char *end = (unsigned char *)r + dd[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
        while ((unsigned char *)r < end && r->SizeOfBlock) {
            WORD *e = (WORD *)(r + 1);
            for (DWORD k = 0; k < (r->SizeOfBlock - sizeof *r) / 2; k++)
                if ((e[k] >> 12) == IMAGE_REL_BASED_DIR64) *(ULONGLONG *)(img + r->VirtualAddress + (e[k] & 0xFFF)) += delta;
            r = (IMAGE_BASE_RELOCATION *)((unsigned char *)r + r->SizeOfBlock);
        }
    }
    if (dd[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(img + dd[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress); d->Name; d++) {
            HMODULE m = LoadLibraryA((char *)img + d->Name);
            if (!m) { printf("mm: cannot load %s\n", (char *)img + d->Name); return NULL; }
            IMAGE_THUNK_DATA64 *name = (IMAGE_THUNK_DATA64 *)(img + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(img + d->FirstThunk);
            for (; name->u1.AddressOfData; name++, iat++) {
                FARPROC p = IMAGE_SNAP_BY_ORDINAL64(name->u1.Ordinal)
                    ? GetProcAddress(m, (LPCSTR)(ULONG_PTR)IMAGE_ORDINAL64(name->u1.Ordinal))
                    : GetProcAddress(m, ((IMAGE_IMPORT_BY_NAME *)(img + name->u1.AddressOfData))->Name);
                iat->u1.Function = (ULONGLONG)(ULONG_PTR)p;
            }
        }
    }
    if (dd[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size)
        RtlAddFunctionTable((PRUNTIME_FUNCTION)(img + dd[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress),
                            dd[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size / sizeof(RUNTIME_FUNCTION), (DWORD64)(ULONG_PTR)img);
    if (dd[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        IMAGE_TLS_DIRECTORY64 *t = (IMAGE_TLS_DIRECTORY64 *)(img + dd[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        printf("mm: module has implicit TLS (%llu bytes) - not initialised, as with most manual mappers\n",
               (unsigned long long)(t->EndAddressOfRawData - t->StartAddressOfRawData + t->SizeOfZeroFill));
        for (PIMAGE_TLS_CALLBACK *cb = (PIMAGE_TLS_CALLBACK *)(ULONG_PTR)t->AddressOfCallBacks; cb && *cb; cb++)
            (*cb)(img, DLL_PROCESS_ATTACH, NULL);
    } else {
        printf("mm: module has no implicit TLS\n");
    }
    BOOL(WINAPI * entry)(HINSTANCE, DWORD, LPVOID) = (void *)(img + nt->OptionalHeader.AddressOfEntryPoint);
    entry((HINSTANCE)img, DLL_PROCESS_ATTACH, NULL);
    VirtualFree(raw, 0, MEM_RELEASE);
    return img;
}

static void *mm_load_a(const char *file) {
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, file, -1, w, MAX_PATH);
    return mm_load(w);
}
