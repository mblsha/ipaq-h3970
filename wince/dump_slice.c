/* Read a bounded H3970 NOR slice into the WinCE RAM object store. */
typedef unsigned int U32;
typedef __WCHAR_TYPE__ Wide;
typedef void *Handle;

extern U32 (*const ce_GetModuleFileNameW)(Handle, Wide *, U32);
extern const Wide *(*const ce_GetCommandLineW)(void);
extern int (*const ce_GetDiskFreeSpaceExW)(const Wide *, U32 *, U32 *, U32 *);
extern Handle (*const ce_CreateFileW)(const Wide *, U32, U32, void *, U32, U32, Handle);
extern int (*const ce_WriteFile)(Handle, const void *, U32, U32 *, void *);
extern int (*const ce_CloseHandle)(Handle);
extern int (*const ce_FlushFileBuffers)(Handle);
extern int (*const ce_MoveFileW)(const Wide *, const Wide *);
extern void *(*const ce_VirtualAlloc)(void *, U32, U32, U32);
extern int (*const ce_VirtualCopy)(void *, void *, U32, U32);
extern int (*const ce_VirtualFree)(void *, U32, U32);
extern void (*const ce_SystemIdleTimerReset)(void);
extern U32 (*const ce_GetLastError)(void);
extern void (*const ce_TerminateProcess)(Handle, U32);

#define INVALID_HANDLE ((Handle)-1)
#define MEM_RESERVE 0x2000u
#define MEM_COMMIT 0x1000u
#define MEM_RELEASE 0x8000u
#define PAGE_READONLY 2u
#define PAGE_READWRITE 4u
#define PAGE_NOCACHE 0x200u
#define PAGE_PHYSICAL 0x400u
#define CHUNK (128u * 1024u)
#define BLOCK (16u * 1024u)
#define MAX_SLICE (4u * 1024u * 1024u)
#define FLASH_SIZE 0x03000000u
#define MAGIC 0x324d4f52u /* ROM2 */

static void init_crc(U32 *table)
{
    for (U32 i = 0; i < 256; ++i) {
        U32 crc = i;
        for (U32 bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
        table[i] = crc;
    }
}

static U32 crc_chunk(const U32 *table, U32 crc, const unsigned char *data, U32 length)
{
    for (U32 i = 0; i < length; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 255];
    return crc;
}

/* Map one chunk read-only. Copy 32-bit words from the NOR bus into RAM before
 * giving the buffer to the filesystem, then release the physical mapping. */
static int read_flash(U32 physical, U32 *buffer)
{
    volatile const U32 *mapping = ce_VirtualAlloc(0, CHUNK, MEM_RESERVE, PAGE_READONLY);
    if (!mapping)
        return 0;
    int ok = ce_VirtualCopy((void *)mapping, (void *)(physical >> 8), CHUNK,
                            PAGE_READONLY | PAGE_PHYSICAL | PAGE_NOCACHE);
    if (ok) {
        for (U32 i = 0; i < CHUNK / sizeof(U32); ++i)
            buffer[i] = mapping[i];
    }
    int released = ce_VirtualFree((void *)mapping, 0, MEM_RELEASE);
    return ok && released;
}

/* Pack a 128 KiB read in place. One bit per 16 KiB block selects a single
 * repeated word or the full block. The output pointer never passes the input. */
static U32 pack_chunk(U32 *record)
{
    U32 *out = record + 1;
    U32 flags = 0;
    for (U32 i = 0; i < CHUNK / BLOCK; ++i) {
        U32 *block = record + 1 + i * (BLOCK / sizeof(U32));
        U32 word = block[0];
        U32 j = 1;
        while (j < BLOCK / sizeof(U32) && block[j] == word)
            ++j;
        if (j == BLOCK / sizeof(U32)) {
            flags |= 1u << i;
            *out++ = word;
        } else {
            if (out != block) {
                for (j = 0; j < BLOCK / sizeof(U32); ++j)
                    out[j] = block[j];
            }
            out += BLOCK / sizeof(U32);
        }
    }
    record[0] = flags;
    return (U32)(out - record) * sizeof(U32);
}

static int hex32(const Wide **text, U32 *value)
{
    U32 number = 0;
    for (unsigned i = 0; i < 8; ++i) {
        Wide c = **text;
        U32 nibble;
        if (c >= L'0' && c <= L'9')
            nibble = c - L'0';
        else if (c >= L'a' && c <= L'f')
            nibble = c - L'a' + 10;
        else
            return 0;
        ++*text;
        number = (number << 4) | nibble;
    }
    *value = number;
    return 1;
}

static int request(U32 *offset, U32 *size)
{
    const Wide *p = ce_GetCommandLineW();
    for (; p && *p; ++p) {
        const Wide *prefix = L"/dump:";
        const Wide *tail = p;
        while (*prefix && *tail == *prefix) {
            ++tail;
            ++prefix;
        }
        if (*prefix)
            continue;
        if (!hex32(&tail, offset) || *tail != L':')
            return 0;
        ++tail;
        if (!hex32(&tail, size) || (*tail && *tail != L' '))
            return 0;
        return *size && *size <= MAX_SLICE && *offset < FLASH_SIZE &&
               *size <= FLASH_SIZE - *offset && !((*offset | *size) & (CHUNK - 1));
    }
    return 0;
}

static void suffix(Wide *path, const Wide *extension)
{
    for (unsigned i = 0; i <= 3; ++i)
        path[i] = extension[i];
}

static Handle create(const Wide *path)
{
    return ce_CreateFileW(path, 0x40000000u, 0, 0, 1, 0x80, 0);
}

static int write_all(Handle file, const void *bytes, U32 size)
{
    U32 written = 0;
    return ce_WriteFile(file, bytes, size, &written, 0) && written == size;
}

static U32 dump(Wide *path, Wide *extension, Wide *destination, U32 offset, U32 size)
{
    U32 disk_sizes[7];
    U32 *free_bytes = (U32 *)(((__UINTPTR_TYPE__)disk_sizes + 7) & ~(__UINTPTR_TYPE__)7);
    if (!ce_GetDiskFreeSpaceExW(L"\\My Documents", free_bytes, free_bytes + 2, free_bytes + 4))
        return 2;
    if (free_bytes[1] == 0 && free_bytes[0] < size + 1024u * 1024u)
        return 3;

    suffix(extension, L"PRT");
    Handle file = create(path);
    if (file == INVALID_HANDLE)
        return 4;
    U32 *record = ce_VirtualAlloc(0, CHUNK + sizeof(U32), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    U32 status = record ? 0 : 5;
    U32 table[256], crc = 0xffffffffu;
    init_crc(table);
    for (U32 written = 0; !status && written < size; written += CHUNK) {
        U32 position = offset + written;
        U32 physical = position + (position >= 0x02000000u ? 0x02000000u : 0);
        ce_SystemIdleTimerReset();
        if (!read_flash(physical, record + 1)) {
            status = 6;
        } else {
            crc = crc_chunk(table, crc, (const unsigned char *)(record + 1), CHUNK);
            U32 packed_size = pack_chunk(record);
            if (!write_all(file, record, packed_size))
                status = 7;
        }
    }
    if (!status) {
        U32 receipt[4];
        receipt[0] = MAGIC;
        receipt[1] = offset;
        receipt[2] = size;
        receipt[3] = ~crc;
        if (!write_all(file, receipt, sizeof(receipt)) || !ce_FlushFileBuffers(file))
            status = 8;
    }
    if (!ce_CloseHandle(file) && !status)
        status = 9;
    if (record)
        ce_VirtualFree(record, 0, MEM_RELEASE);
    if (!status && !ce_MoveFileW(path, destination))
        status = 10;
    return status;
}

__attribute__((section(".text.start"))) void dump_slice_start(void)
{
    Wide path[260], destination[260];
    U32 length = ce_GetModuleFileNameW(0, path, 260);
    if (length < 5 || length >= 260 || path[length - 4] != L'.') {
        ce_TerminateProcess((Handle)66, 1);
        return;
    }
    for (U32 i = 0; i <= length; ++i)
        destination[i] = path[i];
    Wide *extension = path + length - 3;
    suffix(destination + length - 3, L"BIN");
    U32 offset, size;
    U32 status = request(&offset, &size) ? dump(path, extension, destination, offset, size) : 1;
    if (status) {
        U32 result[3], sent = 0;
        result[0] = MAGIC;
        result[1] = status;
        result[2] = ce_GetLastError();
        suffix(extension, L"ERR");
        Handle file = create(path);
        if (file != INVALID_HANDLE) {
            ce_WriteFile(file, result, sizeof(result), &sent, 0);
            ce_CloseHandle(file);
        }
    }
    ce_TerminateProcess((Handle)66, !!status);
}
