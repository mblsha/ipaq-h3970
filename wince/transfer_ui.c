/* WinCE progress window driven by the host's ActiveSync byte counts. */
typedef unsigned int U32;
typedef __WCHAR_TYPE__ Wide;
typedef void *Handle;

typedef struct {
    U32 style;
    int (*proc)(Handle, U32, U32, U32);
    int class_extra, window_extra;
    Handle instance, icon, cursor, background;
    const Wide *menu, *name;
} WindowClass;

typedef struct {
    Handle window;
    U32 message, wparam, lparam, time;
    int x, y;
} WindowMessage;

typedef struct {
    int left, top, right, bottom;
} Rect;

typedef struct {
    Handle dc;
    int erase;
    Rect bounds;
    int restore, update;
    unsigned char reserved[32];
} Paint;

typedef struct {
    U32 magic, stage, done, total;
} Progress;

extern U32 (*const ce_GetModuleFileNameW)(Handle, Wide *, U32);
extern Handle (*const ce_CreateFileW)(const Wide *, U32, U32, void *, U32, U32, Handle);
extern U32 (*const ce_SetFilePointer)(Handle, int, int *, U32);
extern int (*const ce_ReadFile)(Handle, void *, U32, U32 *, void *);
extern int (*const ce_CloseHandle)(Handle);
extern void (*const ce_SystemIdleTimerReset)(void);
extern U32 (*const ce_GetTickCount)(void);
extern void (*const ce_Sleep)(U32);
extern void (*const ce_TerminateProcess)(Handle, U32);
extern Handle (*const ce_GetModuleHandleW)(const Wide *);
extern unsigned short (*const ce_RegisterClassW)(const WindowClass *);
extern Handle (*const ce_CreateWindowExW)(U32, const Wide *, const Wide *, U32,
                                         int, int, int, int, Handle, Handle, Handle, void *);
extern int (*const ce_DefWindowProcW)(Handle, U32, U32, U32);
extern int (*const ce_GetWindowLongW)(Handle, int);
extern int (*const ce_SetWindowLongW)(Handle, int, int);
extern int (*const ce_SetForegroundWindow)(Handle);
extern int (*const ce_DestroyWindow)(Handle);
extern int (*const ce_SetWindowTextW)(Handle, const Wide *);
extern int (*const ce_GetClientRect)(Handle, Rect *);
extern int (*const ce_InvalidateRect)(Handle, const Rect *, int);
extern int (*const ce_UpdateWindow)(Handle);
extern int (*const ce_PeekMessageW)(WindowMessage *, Handle, U32, U32, U32);
extern int (*const ce_TranslateMessage)(const WindowMessage *);
extern int (*const ce_DispatchMessageW)(const WindowMessage *);
extern Handle (*const ce_BeginPaint)(Handle, Paint *);
extern int (*const ce_EndPaint)(Handle, const Paint *);
extern Handle (*const ce_CreateSolidBrush)(U32);
extern int (*const ce_DeleteObject)(Handle);
extern int (*const ce_FillRect)(Handle, const Rect *, Handle);

enum { DUMP = 1, UPLOAD, VERIFY, COMPLETE, FAILED };
#define MAGIC 0x31425355u /* "USB1" in little endian */
#define WS_VISIBLE 0x10000000u
#define WS_CHILD 0x40000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_CAPTION 0x00c00000u
#define WM_PAINT 0x000fu
#define WM_CLOSE 0x0010u
#define INVALID_HANDLE ((Handle)-1)
#define BAR_WIDTH 190

static const Rect bar = {15, 42, 15 + BAR_WIDTH, 59};

static int window_proc(Handle window, U32 message, U32 wparam, U32 lparam)
{
    if (message == WM_CLOSE)
        return 0;
    if (message != WM_PAINT)
        return ce_DefWindowProcW(window, message, wparam, lparam);

    Paint paint;
    Handle dc = ce_BeginPaint(window, &paint);
    if (dc) {
        Rect client;
        if (ce_GetClientRect(window, &client)) {
            Handle background = ce_CreateSolidBrush(0x00ffffff);
            if (background) {
                ce_FillRect(dc, &client, background);
                ce_DeleteObject(background);
            }
        }
        Handle track = ce_CreateSolidBrush(0x00d6c9be);
        Handle fill = ce_CreateSolidBrush(0x00bc6928);
        if (track) {
            ce_FillRect(dc, &bar, track);
            ce_DeleteObject(track);
        }
        int width = ce_GetWindowLongW(window, 0);
        if (fill) {
            if (width > 0) {
                Rect filled = {bar.left, bar.top, bar.left + width, bar.bottom};
                ce_FillRect(dc, &filled, fill);
            }
            ce_DeleteObject(fill);
        }
    }
    ce_EndPaint(window, &paint);
    return 0;
}

static void pump(void)
{
    WindowMessage message;
    while (ce_PeekMessageW(&message, 0, 0, 0, 1)) {
        ce_TranslateMessage(&message);
        ce_DispatchMessageW(&message);
    }
}

static Wide *append(Wide *out, const Wide *text)
{
    while (*text)
        *out++ = *text++;
    *out = 0;
    return out;
}

static Wide *append_number(Wide *out, U32 value)
{
    static const U32 places[] = {100000, 10000, 1000, 100, 10, 1};
    int started = 0;
    for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); ++i) {
        U32 place = places[i];
        U32 digit = 0;
        while (value >= place) {
            ++digit;
            value -= place;
        }
        if (started || digit || place == 1) {
            *out++ = (Wide)(L'0' + digit);
            started = 1;
        }
    }
    *out = 0;
    return out;
}

static void update(Handle window, Handle status, Handle amount, const Progress *p)
{
    static const Wide *const labels[] = {
        L"", L"Dumping WinCE ROM", L"Uploading Linux Doom", L"Verifying Linux Doom",
        L"Transfer complete", L"Transfer interrupted"
    };
    Wide count[32];
    Wide *end = append_number(count, p->done >> 10);
    end = append(end, L" / ");
    end = append_number(end, p->total >> 10);
    append(end, L" KiB");
    ce_SetWindowTextW(status, labels[p->stage]);
    ce_SetWindowTextW(amount, count);

    /* Each addition advances by done / total of a pixel, with a carried remainder. */
    U32 remainder = 0;
    int width = 0;
    for (int x = 0; x < BAR_WIDTH; ++x) {
        remainder += p->done;
        if (remainder >= p->total) {
            remainder -= p->total;
            ++width;
        }
    }
    ce_SetWindowLongW(window, 0, width);
    ce_InvalidateRect(window, &bar, 0);
    ce_UpdateWindow(window);
    pump();
}

static int read_progress(const Wide *path, Progress *progress)
{
    Handle file = ce_CreateFileW(path, 0x80000000u, 2, 0, 3, 0x80, 0);
    if (file == INVALID_HANDLE)
        return 0;
    U32 received = 0;
    /* The host appends records. Read the most recent complete one. */
    U32 offset = ce_SetFilePointer(file, -(int)sizeof(*progress), 0, 2);
    int ok = offset != 0xffffffffu && offset % sizeof(*progress) == 0 &&
             ce_ReadFile(file, progress, sizeof(*progress), &received, 0);
    ce_CloseHandle(file);
    return ok && received == sizeof(*progress) && progress->magic == MAGIC &&
           progress->stage >= DUMP && progress->stage <= FAILED &&
           progress->total && progress->total <= (128u << 20) &&
           progress->done <= progress->total;
}

static void acknowledge(Wide *path, U32 length)
{
    Wide *extension = path + length - 3;
    append(extension, L"ACK");
    Handle file = ce_CreateFileW(path, 0x40000000u, 0, 0, 1, 0x80, 0);
    if (file != INVALID_HANDLE)
        ce_CloseHandle(file);
    append(extension, L"PRG");
}

static int show_transfer(Wide *path, U32 length)
{
    append(path + length - 3, L"PRG");
    Progress current;
    if (!read_progress(path, &current))
        return 1;

    Handle instance = ce_GetModuleHandleW(0);
    const Wide *name = L"H3970TransferProgress";
    WindowClass type = {0, window_proc, 0, 4, instance, 0, 0, (Handle)6, 0, name};
    if (!ce_RegisterClassW(&type))
        return 2;
    Handle window = ce_CreateWindowExW(8, name, L"USB ActiveSync",
                                        WS_VISIBLE | WS_CAPTION | WS_CLIPCHILDREN,
                                        8, 80, 224, 122, 0, 0, instance, 0);
    if (!window)
        return 3;
    Handle status = ce_CreateWindowExW(0, L"STATIC", L"", WS_VISIBLE | WS_CHILD,
                                        15, 13, BAR_WIDTH, 22, window, 0, instance, 0);
    Handle amount = ce_CreateWindowExW(0, L"STATIC", L"", WS_VISIBLE | WS_CHILD,
                                        15, 67, BAR_WIDTH, 22, window, 0, instance, 0);
    if (!status || !amount) {
        ce_DestroyWindow(window);
        return 4;
    }

    ce_SetForegroundWindow(window);
    update(window, status, amount, &current);
    acknowledge(path, length);
    U32 changed = ce_GetTickCount();
    while (current.stage < COMPLETE) {
        Progress next;
        ce_SystemIdleTimerReset();
        pump();
        if (read_progress(path, &next) &&
            (current.stage != next.stage || current.done != next.done || current.total != next.total)) {
            current.stage = next.stage;
            current.done = next.done;
            current.total = next.total;
            changed = ce_GetTickCount();
            update(window, status, amount, &current);
        }
        if (ce_GetTickCount() - changed >= 90000) {
            current.stage = FAILED;
            update(window, status, amount, &current);
            break;
        }
        ce_Sleep(250);
    }
    for (int i = 0; i < 6; ++i) {
        pump();
        ce_Sleep(250);
    }
    ce_DestroyWindow(window);
    acknowledge(path, length);
    return 0;
}

__attribute__((section(".text.start"))) void transfer_start(void)
{
    Wide path[260];
    U32 length = ce_GetModuleFileNameW(0, path, 260);
    U32 result = 1;
    if (length >= 5 && length < 260 && path[length - 4] == L'.')
        result = show_transfer(path, length);
    ce_TerminateProcess((Handle)66, result);
}
