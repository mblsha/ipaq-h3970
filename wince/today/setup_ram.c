typedef unsigned int U32;
typedef __WCHAR_TYPE__ Wide;
typedef void *Handle;

extern Handle (*const ce_GetForegroundWindow)(void);
extern int (*const ce_GetClassNameW)(Handle, Wide *, int);
extern int (*const ce_PostMessageW)(Handle, U32, U32, int);
extern int (*const ce_RegOpenKeyExW)(Handle, const Wide *, U32, U32, Handle *);
extern int (*const ce_RegSetValueExW)(Handle, const Wide *, U32, U32, const void *, U32);
extern int (*const ce_RegCloseKey)(Handle);
extern Handle (*const ce_CreateFileW)(const Wide *, U32, U32, void *, U32, U32, Handle);
extern int (*const ce_ReadFile)(Handle, void *, U32, U32 *, void *);
extern int (*const ce_WriteFile)(Handle, const void *, U32, U32 *, void *);
extern int (*const ce_CloseHandle)(Handle);
extern void (*const ce_Sleep)(U32);
extern void (*const ce_TerminateProcess)(Handle, U32);

static int equals(const Wide *a, const Wide *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static int restore_calibration(void)
{
    Handle input = ce_CreateFileW(L"\\My Documents\\RT83CA.DAT",
                                 0x80000000, 0, 0, 3, 0x80, 0);
    if (input == (Handle)-1)
        return -2;
    Wide saved[96];
    U32 bytes = 0;
    int read_ok = ce_ReadFile(input, saved, sizeof(saved), &bytes, 0);
    ce_CloseHandle(input);
    if (!read_ok || bytes < 24 || bytes >= sizeof(saved) || (bytes & 1) || saved[bytes/2-1])
        return -3;

    Handle key;
    int status = ce_RegOpenKeyExW((Handle)0x80000002,
                                 L"HARDWARE\\DEVICEMAP\\TOUCH", 0, 0, &key);
    if (status)
        return status;
    status = ce_RegSetValueExW(key, L"CalibrationData", 0, 1, saved, bytes);
    ce_RegCloseKey(key);
    return status;
}

void setup_main(void)
{
    struct {
        int expected_window;
        int calibration_result;
        int closed_requested;
        Wide resulting_class[128];
    } result;
    result.expected_window = 0;
    result.calibration_result = -1;
    result.closed_requested = 0;
    result.resulting_class[0] = 0;

    Handle active = ce_GetForegroundWindow();
    Wide name[128];
    if (active && ce_GetClassNameW(active, name, 128) > 0 && equals(name, L"Welcome")) {
        result.expected_window = 1;
        result.calibration_result = restore_calibration();
        if (result.calibration_result == 0 && ce_GetForegroundWindow() == active) {
            result.closed_requested = ce_PostMessageW(active, 0x0010, 0, 0);
            ce_Sleep(1200);
        }
    }
    for (unsigned i = 0; i < 128; ++i)
        result.resulting_class[i] = 0;
    active = ce_GetForegroundWindow();
    if (active)
        ce_GetClassNameW(active, result.resulting_class, 128);
    Handle output = ce_CreateFileW(L"\\My Documents\\RT83GO.BIN",
                                  0x40000000, 0, 0, 2, 0x80, 0);
    U32 written = 0;
    if (output != (Handle)-1) {
        ce_WriteFile(output, &result, sizeof(result), &written, 0);
        ce_CloseHandle(output);
    }
    ce_TerminateProcess((Handle)-1, written != sizeof(result));
}
