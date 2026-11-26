typedef unsigned int U32;
typedef __WCHAR_TYPE__ Wide;
typedef void *Handle;
struct Rectangle { int left, top, right, bottom; };
struct Point { int x, y; };

extern Handle (*const ce_GetForegroundWindow)(void);
extern int (*const ce_GetClassNameW)(Handle, Wide *, int);
extern int (*const ce_GetClientRect)(Handle, struct Rectangle *);
extern int (*const ce_ScreenToClient)(Handle, struct Point *);
extern int (*const ce_PostMessageW)(Handle, U32, U32, int);
extern Handle (*const ce_CreateFileW)(const Wide *, U32, U32, void *, U32, U32, Handle);
extern int (*const ce_WriteFile)(Handle, const void *, U32, U32 *, void *);
extern int (*const ce_CloseHandle)(Handle);
extern void (*const ce_Sleep)(U32);
extern void (*const ce_TerminateProcess)(Handle, U32);

static int is_splash(Handle window)
{
    Wide name[32];
    const Wide *expected = L"SHOWBMP";
    if (!window || ce_GetClassNameW(window, name, 32) != 7)
        return 0;
    for (unsigned index = 0; index < 8; ++index)
        if (name[index] != expected[index])
            return 0;
    return 1;
}

void dismiss_main(void)
{
    struct Rectangle rect;
    struct Point point = {120, 225};
    Handle foreground = ce_GetForegroundWindow();
    U32 flags = 0, sent = 0;
    if (is_splash(foreground) && ce_GetClientRect(foreground, &rect) &&
        ce_ScreenToClient(foreground, &point) &&
        point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom &&
        ce_GetForegroundWindow() == foreground) {
        flags |= 1;
        const int button = (point.y << 16) | point.x;
        if (ce_PostMessageW(foreground, 0x0201, 1, button))
            flags |= 2;
        ce_Sleep(70);
        if (ce_PostMessageW(foreground, 0x0202, 0, button))
            flags |= 4;
        ce_Sleep(1000);
    }
    Handle report = ce_CreateFileW(L"\\My Documents\\RT83TP.BIN",
                                  0x40000000, 0, 0, 2, 0x80, 0);
    if (report != (Handle)-1) {
        ce_WriteFile(report, &flags, sizeof(flags), &sent, 0);
        ce_CloseHandle(report);
    }
    ce_TerminateProcess((Handle)-1, sent != sizeof(flags));
}
