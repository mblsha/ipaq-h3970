/* Pocket PC 2002 launcher for a single-page, RAM-only PXA250 handoff test. */
typedef unsigned int U32;
typedef __WCHAR_TYPE__ Wide;
typedef void *Handle;

#include "handoff_blob.h"
/* The serial settings used by the on-device LINKTEST diagnostic. */
typedef unsigned short U16;
typedef struct {
    U32 length, baud, flags;
    U16 reserved, xon_limit, xoff_limit;
    unsigned char byte_size, parity, stop_bits;
    char xon_char, xoff_char, error_char, eof_char, event_char;
    U16 reserved1;
} TraceDcb;
_Static_assert(sizeof(TraceDcb) == 28, "WinCE DCB layout");

extern Handle (*const ce_CreateFileW)(const Wide *, U32, U32, void *, U32, U32, Handle);
extern int (*const ce_CloseHandle)(Handle);
extern U32 (*const ce_GetModuleFileNameW)(Handle, Wide *, U32);
extern int (*const ce_ReadFile)(Handle, void *, U32, U32 *, void *);
extern int (*const ce_GetCommState)(Handle, TraceDcb *);
extern int (*const ce_SetCommState)(Handle, TraceDcb *);
extern int (*const ce_WriteFile)(Handle, const void *, U32, U32 *, void *);
extern void (*const ce_Sleep)(U32);

static Handle open_trace(TraceDcb *saved)
{
    Handle port = ce_CreateFileW(L"COM1:", 0xc0000000u, 0, 0, 3, 0, 0);
    if (port == (Handle)-1)
        return 0;
    saved->length = sizeof *saved;
    if (ce_GetCommState(port, saved)) {
        TraceDcb state = *saved;
        state.baud = 115200;
        state.flags = 1;
        state.byte_size = 8;
        state.parity = 0;
        state.stop_bits = 0;
        if (ce_SetCommState(port, &state))
            return port;
    }
    ce_CloseHandle(port);
    return 0;
}

static void trace(Handle port, const Wide *message, U32 value)
{
    if (!port)
        return;
    static const char digits[] = "0123456789ABCDEF";
    char line[110];
    U32 n = 0, count;
    while (*message && n < sizeof line - 12)
        line[n++] = (char)*message++;
    line[n++] = ' ';
    for (int shift = 28; shift >= 0; shift -= 4)
        line[n++] = digits[(value >> shift) & 15];
    line[n++] = '\r';
    line[n++] = '\n';
    ce_WriteFile(port, line, n, &count, 0);
}


extern void *(*const ce_VirtualAlloc)(void *, U32, U32, U32);
extern int (*const ce_VirtualCopy)(void *, void *, U32, U32);
extern int (*const ce_VirtualFree)(void *, U32, U32);
extern int (*const ce_LockPages)(void *, U32, U32 *, U32);
extern int (*const ce_UnlockPages)(void *, U32);
extern int (*const ce_SetKMode)(int);
extern U32 (*const ce_GetLastError)(void);
extern int (*const ce_MessageBoxW)(Handle, const Wide *, const Wide *, U32);
extern int (*const ce_TerminateProcess)(Handle, U32);

#define PAGE_BYTES 4096u
#define SWEEP_BYTES 32768u
#define RAM_BASE 0xa0000000u
#define RAM_LIMIT 0xa4000000u
#define MEM_COMMIT_RESERVE 0x3000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u
#define PAGE_READWRITE 4u
#define PAGE_NOACCESS 1u
#define PHYSICAL_RW_UNCACHED 0x604u

_Static_assert(sizeof(handoff_blob) < PAGE_BYTES, "handoff must fit in a page");

static void release(void *address)
{
    if (address)
        ce_VirtualFree(address, 0, MEM_RELEASE);
}

static volatile U32 *physical_map(U32 address, U32 length)
{
    void *reserved = ce_VirtualAlloc(0, length, MEM_RESERVE, PAGE_NOACCESS);
    if (!reserved)
        return 0;
    if (!ce_VirtualCopy(reserved, (void *)(address >> 8), length, PHYSICAL_RW_UNCACHED)) {
        release(reserved);
        return 0;
    }
    return reserved;
}

static int in_ram(U32 address, U32 length)
{
    return address >= RAM_BASE && address <= RAM_LIMIT - length;
}

static U32 fcse_address(U32 address, U32 pid)
{
    return address < 0x02000000u ? address | pid : address;
}

static void show_failure(const Wide *message, U32 value)
{
    static const Wide digits[] = L"0123456789ABCDEF";
    Wide detail[128];
    U32 i = 0;
    while (*message)
        detail[i++] = *message++;
    detail[i++] = L' ';
    for (int shift = 28; shift >= 0; shift -= 4)
        detail[i++] = digits[(value >> shift) & 15];
    detail[i] = 0;
    Handle report = ce_CreateFileW(L"\\My Documents\\DOOMN3-FAIL.TXT",
                                  0x40000000u, 0, 0, 2, 0x80, 0);
    if (report != (Handle)-1) {
        U32 written;
        ce_WriteFile(report, detail, i * sizeof(Wide), &written, 0);
        ce_CloseHandle(report);
    }
}

static void clean_range(U32 mva, U32 length)
{
    for (U32 offset = 0; offset < length; offset += 32) {
        U32 line = mva + offset;
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(line) : "memory");
    }
    U32 zero = 0;
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4\n"
                     "mcr p15, 0, %0, c7, c5, 0\n"
                     "mrc p15, 0, %0, c2, c0, 0\n"
                     "mov %0, %0\n"
                     "sub pc, pc, #4"
                     : "+r"(zero) :: "memory");
}

static U32 lock_address(U32 value)
{
    if (value >= (RAM_BASE >> 8) && value < (RAM_LIMIT >> 8))
        return value << 8;
    if (value >= (RAM_BASE >> 12) && value < (RAM_LIMIT >> 12))
        return value << 12;
    return value;
}

#if HANDOFF_LINUX
#include "linux_stage.h"
#endif

void handoff_main(void)
{
    TraceDcb saved_dcb;
    /* Let the USB desktop finish RAPI and unconfigure WinCE before takeover. */
    ce_Sleep(12000);
    Handle port = open_trace(&saved_dcb);
    if (HANDOFF_RETURN && !HANDOFF_LINUX && !port) {
        show_failure(L"Could not configure COM1", ce_GetLastError());
        ce_TerminateProcess((Handle)-1, 1);
        return;
    }
    trace(port, L"H3970 HANDOFF entry", 0);
    const Wide *failure = L"Allocate test pages";
    U32 value = 0;
    unsigned char *code = ce_VirtualAlloc(0, PAGE_BYTES, MEM_COMMIT_RESERVE, PAGE_READWRITE);
    unsigned char *sweep = HANDOFF_RETURN ? 0 :
        ce_VirtualAlloc(0, SWEEP_BYTES, MEM_COMMIT_RESERVE, PAGE_READWRITE);
    volatile U32 *l1 = 0, *check = 0, *dma = 0, *usb = 0;
    U32 code_pfn = 0, sweep_pfns[SWEEP_BYTES / PAGE_BYTES];
    int code_locked = 0, sweep_locked = 0, kernel_mode = 0, previous = 0;
#if HANDOFF_LINUX
    LinuxStage linux_stage = {0};
#endif
    if (!code || (!HANDOFF_RETURN && !sweep))
        goto failed;

    failure = L"LockPages code or scratch";
    code_locked = ce_LockPages(code, PAGE_BYTES, &code_pfn, 1);
    if (!code_locked)
        goto failed;
    if (!HANDOFF_RETURN) {
        sweep_locked = ce_LockPages(sweep, SWEEP_BYTES, sweep_pfns, 1);
        if (!sweep_locked)
            goto failed;
        for (U32 i = 0; i < SWEEP_BYTES; i += PAGE_BYTES)
            ((volatile unsigned char *)sweep)[i] = 0;
    }
    for (U32 i = 0; i < sizeof(handoff_blob); ++i)
        code[i] = handoff_blob[i];
#if HANDOFF_LINUX
    failure = prepare_linux(&linux_stage, port, &value);
    if (failure)
        goto report;
#endif
    previous = ce_SetKMode(1);
    kernel_mode = 1;
    U32 cpsr, ttbr, control, pid, dacr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    if ((cpsr & 31) != 0x1f && (cpsr & 31) != 0x13) {
        failure = L"Privileged ARM mode unavailable";
        value = cpsr;
        goto report;
    }
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr));
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(control));
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 0" : "=r"(pid));
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(dacr));
    trace(port, L"H3970 TTBR", ttbr);
    U32 l1_phys = ttbr & 0xffffc000u;
    if (!(control & 1) || !in_ram(l1_phys, 16384)) {
        failure = L"Unexpected MMU or L1 table";
        value = ttbr;
        goto report;
    }
    failure = L"Map L1 table";
    l1 = physical_map(l1_phys, 16384);
    if (!l1)
        goto failed;

    U32 mva = fcse_address((U32)code, pid & 0xfe000000u);
    U32 first = l1[mva >> 20];
    trace(port, L"H3970 code MVA", mva);
    trace(port, L"H3970 code L1 word", first);
    trace(port, L"H3970 LockPages value", code_pfn);
    U32 physical = lock_address(code_pfn);
    if (!(first & 3) || !in_ram(physical, PAGE_BYTES) || (physical & 4095)) {
        failure = L"LockPages did not identify a code page in SDRAM";
        value = code_pfn;
        goto report;
    }
    trace(port, L"H3970 pinned code physical", physical);
    volatile U32 *slot = &l1[physical >> 20];
    U32 original_slot = *slot;
    U32 offset = (physical & 0xfff00000u) - RAM_BASE;
    /* These VAs alias the first and second NOR windows in the OEM map. */
    U32 nor_base = offset < 0x02000000u ? offset : offset + 0x02000000u;
    U32 kind = original_slot & 3;
    if ((kind && (kind != 2 || (original_slot & 0xfff00000u) != nor_base)) ||
        (mva >> 20) == (physical >> 20)) {
        failure = L"Identity slot differs from the H3970 OEM map";
        value = original_slot;
        goto report;
    }
    U32 domain = (first >> 5) & 15, access = (dacr >> (domain * 2)) & 3;
    if (access != 1 && access != 3) {
        failure = L"Code domain unavailable";
        value = dacr;
        goto report;
    }

    failure = L"Map physical code page";
    check = physical_map(physical, PAGE_BYTES);
    if (!check)
        goto failed;
    if (!HANDOFF_RETURN) {
        failure = L"Map PXA peripherals";
        dma = physical_map(0x40000000u, PAGE_BYTES);
        usb = physical_map(0x40600000u, PAGE_BYTES);
        if (!dma || !usb)
            goto failed;
    }
    clean_range(mva, sizeof handoff_blob);
    volatile const unsigned char *verified = (volatile const unsigned char *)check;
    for (U32 i = 0; i < sizeof(handoff_blob); ++i) {
        if (verified[i] != handoff_blob[i]) {
            failure = L"Uncached RAM payload readback mismatch at offset";
            value = i;
            goto report;
        }
    }

    U32 linux_table = 0;
#if HANDOFF_LINUX
    failure = place_linux(&linux_stage, physical, l1_phys, pid & 0xfe000000u, &value);
    if (failure)
        goto report;
    linux_table = linux_stage.table_phys;
    trace(port, L"H3970 Linux zImage destination", linux_stage.table[1]);
    trace(port, L"H3970 Linux DTB destination", linux_stage.table[2]);
#endif
    if (HANDOFF_LCD) {
        volatile const U32 *lcd = (volatile const U32 *)0xaf400000u; /* WinCE LCD alias */
        trace(port, L"H3970 stock LCCR0", lcd[0]);
        trace(port, L"H3970 stock LCCR1", lcd[1]);
        trace(port, L"H3970 stock LCCR2", lcd[2]);
        trace(port, L"H3970 stock LCCR3", lcd[3]);
    }
    U32 return_state[] = {original_slot, ttbr, pid, dacr, linux_table};
    U32 hw[] = {(U32)dma, (U32)usb, fcse_address((U32)sweep, pid & 0xfe000000u), original_slot};
    U32 descriptor = (physical & 0xfff00000u) | (domain << 5) | 0xc02u;
    int (*takeover)(U32, volatile U32 *, U32, const U32 *) = (void *)code;
    trace(port, L"H3970 entering trampoline", descriptor);
    /* Leave COM1 open to retain its transceiver and clock through the jump. */
    if (port)
        ce_Sleep(35);
    value = takeover(physical, slot, descriptor, HANDOFF_RETURN ? return_state : hw);
    if (HANDOFF_RETURN) {
        ce_SetKMode(previous);
        kernel_mode = 0;
        if (!HANDOFF_LINUX && value == 0) {
            trace(port, L"H3970 WINCE RETURN OK", 0);
            goto cleanup;
        }
        failure = HANDOFF_LINUX ? L"Linux handoff refused before takeover"
                               : L"Round-trip probe returned before completing the marker";
        goto report;
    }
    failure = L"Trampoline rejected changed MMU state";
    goto report;

failed:
    value = ce_GetLastError();
report:
    if (kernel_mode) {
        ce_SetKMode(previous);
        kernel_mode = 0;
    }
    trace(port, failure, value);
    show_failure(failure, value);
cleanup:
#if HANDOFF_LINUX
    release_linux(&linux_stage);
#endif
    if (port) {
        if (HANDOFF_RETURN) {
            ce_Sleep(35);
            ce_SetCommState(port, &saved_dcb);
        }
        ce_CloseHandle(port);
    }
    release((void *)usb);
    release((void *)dma);
    release((void *)check);
    release((void *)l1);
    if (sweep_locked)
        ce_UnlockPages(sweep, SWEEP_BYTES);
    if (code_locked)
        ce_UnlockPages(code, PAGE_BYTES);
    release(sweep);
    release(code);
    ce_TerminateProcess((Handle)-1, 0);
}
