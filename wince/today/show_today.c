typedef unsigned int U32;
typedef __WCHAR_TYPE__ Wide;
typedef void *Handle;
extern Handle (*const ce_GetForegroundWindow)(void);
extern Handle (*const ce_FindWindowW)(const Wide *, const Wide *);
extern int (*const ce_PostMessageW)(Handle, U32, U32, int);
extern void (*const ce_TerminateProcess)(Handle, U32);
void today_main(void)
{
    Handle desktop = ce_FindWindowW(L"DesktopExplorerWindow", L"Desktop");
    int sent = desktop && desktop == ce_GetForegroundWindow() &&
        ce_PostMessageW(desktop, 0x0625, 1, 1);
    ce_TerminateProcess((Handle)-1, !sent);
}
