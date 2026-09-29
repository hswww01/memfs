#include <Windows.h>
#include <stdio.h>

#include "memfs_resource.h"

static int check_resource(HMODULE module, WORD id, DWORD minimum_size, const char* label) {
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    DWORD size;

    if (resource == NULL) {
        fprintf(stderr, "missing resource: %s (id=%u) error=%lu\n",
                label, (unsigned)id, GetLastError());
        return 0;
    }

    size = SizeofResource(module, resource);
    if (size < minimum_size) {
        fprintf(stderr, "resource too small: %s size=%lu expected>=%lu\n",
                label, size, minimum_size);
        return 0;
    }

    printf("%s resource size=%lu\n", label, size);
    return 1;
}

int wmain(int argc, wchar_t** argv) {
    HMODULE module;
    int ok;

    if (argc != 2) {
        fwprintf(stderr, L"usage: %s <memfs.exe>\n", argv[0]);
        return 2;
    }

    module = LoadLibraryExW(argv[1], NULL,
                            LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                            LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (module == NULL) {
        fwprintf(stderr, L"LoadLibraryExW(%s) failed: %lu\n", argv[1], GetLastError());
        return 1;
    }

#if defined(MEMFS_WINFSP_STATIC)
    ok = check_resource(module, IDR_MEMFS_WINFSP_SYS, 100U * 1024U, "WinFsp SYS");
#else
    ok = check_resource(module, IDR_MEMFS_WINFSP_DLL, 100U * 1024U, "WinFsp DLL") &&
         check_resource(module, IDR_MEMFS_WINFSP_SYS, 100U * 1024U, "WinFsp SYS");
#endif

    FreeLibrary(module);
    return ok ? 0 : 1;
}
