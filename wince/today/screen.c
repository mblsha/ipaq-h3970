typedef unsigned int U32;
typedef unsigned short U16;
typedef __WCHAR_TYPE__ Wide;
typedef void *Handle;

extern Handle (*const ce_GetDC)(Handle);
extern int (*const ce_ReleaseDC)(Handle, Handle);
extern Handle (*const ce_CreateCompatibleDC)(Handle);
extern int (*const ce_DeleteDC)(Handle);
extern Handle (*const ce_CreateDIBSection)(Handle, const void *, U32, void **, Handle, U32);
extern Handle (*const ce_SelectObject)(Handle, Handle);
extern int (*const ce_DeleteObject)(Handle);
extern int (*const ce_BitBlt)(Handle, int, int, int, int, Handle, int, int, U32);
extern int (*const ce_GetSystemMetrics)(int);
extern Handle (*const ce_CreateFileW)(const Wide *, U32, U32, void *, U32, U32, Handle);
extern int (*const ce_WriteFile)(Handle, const void *, U32, U32 *, void *);
extern int (*const ce_CloseHandle)(Handle);
extern void (*const ce_TerminateProcess)(Handle, U32);

struct Dib {
    U32 size;
    int width, height;
    U16 planes, bits;
    U32 compression, image_size, x_resolution, y_resolution, used, important;
};
struct __attribute__((packed)) Bmp {
    U16 signature;
    U32 size;
    U16 reserved1, reserved2;
    U32 pixel_offset;
    struct Dib image;
};
_Static_assert(sizeof(struct Bmp) == 54, "bitmap header size");

static int put(Handle file, const void *data, U32 size)
{
    U32 sent;
    return ce_WriteFile(file, data, size, &sent, 0) && sent == size;
}

void screen_main(void)
{
    int width = ce_GetSystemMetrics(0), height = ce_GetSystemMetrics(1);
    if (width != 240 || height != 320) {
        ce_TerminateProcess((Handle)-1, 1);
        return;
    }
    U32 stride = ((U32)width * 3 + 3) & ~3u;
    U32 size = stride * (U32)height;
    const struct Dib info = {40, width, height, 1, 24, 0, size, 0, 0, 0, 0};
    const struct Bmp header = {0x4d42, size + 54, 0, 0, 54, info};
    Handle dc = ce_GetDC(0);
    Handle mem = dc ? ce_CreateCompatibleDC(dc) : 0;
    void *pixels = 0;
    Handle bitmap = mem ? ce_CreateDIBSection(mem, &info, 0, &pixels, 0, 0) : 0;
    int ok = 0;
    if (bitmap && pixels) {
        Handle original = ce_SelectObject(mem, bitmap);
        if (original && ce_BitBlt(mem, 0, 0, width, height, dc, 0, 0, 0xcc0020)) {
            Handle file = ce_CreateFileW(L"\\My Documents\\RT83SC.BMP",
                                        0x40000000, 0, 0, 2, 0x80, 0);
            if (file != (Handle)-1) {
                ok = put(file, &header, sizeof(header)) && put(file, pixels, size);
                ce_CloseHandle(file);
            }
            ce_SelectObject(mem, original);
        }
        ce_DeleteObject(bitmap);
    }
    if (mem)
        ce_DeleteDC(mem);
    if (dc)
        ce_ReleaseDC(0, dc);
    ce_TerminateProcess((Handle)-1, !ok);
}

void *memcpy(void *destination, const void *source, unsigned size)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    while (size--)
        *out++ = *in++;
    return destination;
}
