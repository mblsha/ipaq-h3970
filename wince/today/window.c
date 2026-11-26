typedef unsigned int U32;
typedef __WCHAR_TYPE__ Wide;
typedef void *Handle;

extern Handle (*const ce_GetForegroundWindow)(void);
extern int (*const ce_GetClassNameW)(Handle, Wide *, int);
extern int (*const ce_GetWindowTextW)(Handle, Wide *, int);
extern Handle (*const ce_FindWindowW)(const Wide *, const Wide *);
extern Handle (*const ce_CreateFileW)(const Wide *, U32, U32, void *, U32, U32, Handle);
extern int (*const ce_WriteFile)(Handle, const void *, U32, U32 *, void *);
extern int (*const ce_CloseHandle)(Handle);
extern void (*const ce_TerminateProcess)(Handle, U32);

void window_main(void)
{
    struct {
        Wide class_name[128];
        Wide title[128];
        U32 today_is_foreground;
    } report;
    for (unsigned index = 0; index < 128; ++index) {
        report.class_name[index] = 0;
        report.title[index] = 0;
    }
    Handle foreground = ce_GetForegroundWindow();
    if (foreground) {
        ce_GetClassNameW(foreground, report.class_name, 128);
        ce_GetWindowTextW(foreground, report.title, 128);
    }
    report.today_is_foreground = foreground &&
        foreground == ce_FindWindowW(L"DesktopExplorerWindow", 0);
    Handle file = ce_CreateFileW(L"\\My Documents\\RT83WN.BIN",
                                0x40000000, 0, 0, 2, 0x80, 0);
    U32 written = 0;
    if (file != (Handle)-1) {
        ce_WriteFile(file, &report, sizeof(report), &written, 0);
        ce_CloseHandle(file);
    }
    ce_TerminateProcess((Handle)-1, written != sizeof(report));
}
