/* WinCE staging for the Linux handoff. Only the physical trampoline copies
 * into the final SDRAM window, after WinCE has stopped running. */
#define LINUX_PAGES ((LINUX_BYTES + PAGE_BYTES - 1) / PAGE_BYTES)
#define LINUX_ROUNDED (LINUX_PAGES * PAGE_BYTES)
#define PAGE_LIST_OFFSET 4u
#define DECOMPRESS_WORKSPACE 0x00100000u
_Static_assert(PAGE_LIST_OFFSET * sizeof(U32) + LINUX_PAGES * sizeof(U16) <= PAGE_BYTES,
               "Linux scatter list must fit in one page");
_Static_assert(LINUX_CALIBRATION_PROP + LINUX_CALIBRATION_SIZE <= LINUX_KERNEL_OFFSET,
               "The calibration field must fit before the kernel");

typedef struct {
    unsigned char *data;
    U32 *table;
    U32 *pfns;
    U32 table_phys;
    int data_locked, table_locked;
} LinuxStage;

static void stage_calibration(unsigned char *field)
{
    for (U32 i = 0; i < LINUX_CALIBRATION_SIZE; ++i)
        field[i] = 0;

    Handle key;
    if (ce_RegOpenKeyExW((Handle)0x80000002u, L"HARDWARE\\DEVICEMAP\\TOUCH", 0, 0, &key))
        return;
    Wide value[LINUX_CALIBRATION_SIZE - 1];
    U32 type = 0, bytes = sizeof(value);
    int status = ce_RegQueryValueExW(key, L"CalibrationData", 0, &type, value, &bytes);
    ce_RegCloseKey(key);
    if (status || type != 1 || bytes < 24 || bytes > sizeof(value) || bytes % sizeof(Wide))
        return;
    const U32 length = bytes / sizeof(Wide) - 1;
    if (value[length])
        return;
    for (U32 i = 0; i < length; ++i)
        if ((value[i] < '0' || value[i] > '9') &&
            value[i] != ',' && value[i] != '-' && value[i] != ' ' && value[i] != '\t')
            return;
    for (U32 i = 0; i < length; ++i)
        field[i] = (unsigned char)value[i];
    field[length] = '\n';
}

#if !LINUX_SIBLING && !defined(LINUX_PATH)
#define LINUX_PATH L"\\Storage Card\\LINUX.BIN"
#endif

static const Wide *prepare_linux(LinuxStage *stage, Handle port, U32 *value)
{
#if LINUX_SIBLING
    Wide path[260];
    U32 length = ce_GetModuleFileNameW(0, path, 260);
    if (length < 5 || length >= 260 || path[length - 4] != L'.') {
        *value = length;
        return L"Locate the Doom launcher";
    }
    path[length - 3] = L'B';
    path[length - 2] = L'I';
    path[length - 1] = L'N';
    path[length] = 0;
#else
    const Wide *path = LINUX_PATH;
#endif
    stage->data = ce_VirtualAlloc(0, LINUX_ROUNDED, MEM_COMMIT_RESERVE, PAGE_READWRITE);
    stage->table = ce_VirtualAlloc(0, PAGE_BYTES, MEM_COMMIT_RESERVE, PAGE_READWRITE);
    stage->pfns = ce_VirtualAlloc(0, LINUX_PAGES * sizeof(U32), MEM_COMMIT_RESERVE, PAGE_READWRITE);
    if (!stage->data || !stage->table || !stage->pfns) {
        *value = ce_GetLastError();
        return L"Allocate Linux staging RAM";
    }
    Handle file = ce_CreateFileW(path, 0x80000000u, 1, 0, 3, 0, 0);
    if (file == (Handle)-1) {
        *value = ce_GetLastError();
        return L"Open staged Linux payload";
    }
    U32 total = 0;
    const Wide *error = 0;
    while (total < LINUX_BYTES) {
        U32 count = LINUX_BYTES - total, got;
        if (count > 32768)
            count = 32768;
        if (!ce_ReadFile(file, stage->data + total, count, &got, 0)) {
            *value = ce_GetLastError();
            error = L"Read LINUX.BIN";
            break;
        }
        if (!got || got > count) {
            *value = total;
            error = L"LINUX.BIN size mismatch";
            break;
        }
        total += got;
    }
    if (!error) {
        unsigned char extra;
        U32 got;
        if (!ce_ReadFile(file, &extra, 1, &got, 0) || got) {
            *value = total;
            error = L"Check LINUX.BIN end of file";
        }
    }
    ce_CloseHandle(file);
    if (error)
        return error;

    U32 crc = 0xffffffffu;
    for (U32 i = 0; i < LINUX_BYTES; ++i) {
        crc ^= stage->data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    *value = ~crc;
    if (*value != LINUX_CRC32)
        return L"LINUX.BIN CRC mismatch";
    trace(port, L"H3970 verified Linux SD CRC", *value);
    stage_calibration(stage->data + LINUX_CALIBRATION_PROP);

    stage->data_locked = ce_LockPages(stage->data, LINUX_ROUNDED,
                                      stage->pfns, 1);
    if (!stage->data_locked) {
        *value = ce_GetLastError();
        return L"Pin Linux payload pages";
    }
    U32 table_pfn;
    stage->table_locked = ce_LockPages(stage->table, PAGE_BYTES, &table_pfn, 1);
    if (!stage->table_locked) {
        *value = ce_GetLastError();
        return L"Pin Linux scatter table";
    }
    stage->table_phys = lock_address(table_pfn);
    if ((stage->table_phys & (PAGE_BYTES - 1)) || !in_ram(stage->table_phys, PAGE_BYTES)) {
        *value = table_pfn;
        return L"Linux scatter table outside SDRAM";
    }
    U16 *pages = (U16 *)(stage->table + PAGE_LIST_OFFSET);
    for (U32 i = 0; i < LINUX_PAGES; ++i) {
        U32 *page = &stage->pfns[i];
        U32 physical = lock_address(*page);
        if ((physical & (PAGE_BYTES - 1)) || !in_ram(physical, PAGE_BYTES)) {
            *value = *page;
            return L"Linux payload page outside SDRAM";
        }
        pages[i] = (physical - RAM_BASE) / PAGE_BYTES;
    }
    release(stage->pfns);
    stage->pfns = 0;
    return 0;
}

static int overlaps(U32 a, U32 alen, U32 b, U32 blen)
{
    return a < b + blen && b < a + alen;
}

static const Wide *place_linux(LinuxStage *stage, U32 code, U32 l1, U32 pid, U32 *value)
{
    /* Leave the first 16 MiB for zImage's decompressed kernel and workspace. */
    const U16 *pages = (U16 *)(stage->table + PAGE_LIST_OFFSET);
    U32 target = RAM_BASE + 0x01000000u;
    U32 window = LINUX_ROUNDED + DECOMPRESS_WORKSPACE;
    for (; in_ram(target, window); target += 0x00100000u) {
        if (overlaps(target, LINUX_ROUNDED, code, PAGE_BYTES) ||
            overlaps(target, LINUX_ROUNDED, l1, 16384) ||
            overlaps(target, LINUX_ROUNDED, stage->table_phys, PAGE_BYTES))
            continue;
        U32 i;
        for (i = 0; i < LINUX_PAGES; ++i)
            if (overlaps(target, LINUX_ROUNDED, RAM_BASE + (U32)pages[i] * PAGE_BYTES, PAGE_BYTES))
                break;
        if (i == LINUX_PAGES)
            break;
    }
    if (!in_ram(target, window)) {
        *value = LINUX_ROUNDED;
        return L"No non-overlapping Linux destination in SDRAM";
    }
    stage->table[0] = target;
    stage->table[1] = target + LINUX_KERNEL_OFFSET;
    stage->table[2] = target;
    stage->table[3] = LINUX_PAGES;

    U32 initrd_start = target + LINUX_INITRD_OFFSET;
    U32 initrd_end = initrd_start + LINUX_INITRD_BYTES;
    for (unsigned i = 0; i < 4; ++i) {
        stage->data[LINUX_INITRD_START_PROP + i] = initrd_start >> (24 - i * 8);
        stage->data[LINUX_INITRD_END_PROP + i] = initrd_end >> (24 - i * 8);
    }
    U32 va = (U32)stage->data;
    U32 first = va < 0x02000000u && LINUX_ROUNDED > 0x02000000u - va
        ? 0x02000000u - va : LINUX_ROUNDED;
    clean_range(fcse_address(va, pid), first);
    if (first < LINUX_ROUNDED)
        clean_range(va + first, LINUX_ROUNDED - first);
    clean_range(fcse_address((U32)stage->table, pid),
                PAGE_LIST_OFFSET * sizeof(U32) + LINUX_PAGES * sizeof(U16));
    return 0;
}

static void release_linux(LinuxStage *stage)
{
    if (stage->data_locked)
        ce_UnlockPages(stage->data, LINUX_ROUNDED);
    if (stage->table_locked)
        ce_UnlockPages(stage->table, PAGE_BYTES);
    release(stage->pfns);
    release(stage->table);
    release(stage->data);
}
