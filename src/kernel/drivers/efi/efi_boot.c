#include "efi_boot.h"
#include "efi.h"
#include "efi_secureboot.h"
#include "klib.h"

#define LOAD_OPTION_ACTIVE   0x00000001u
#define BOOT_VARIABLE_ATTRS  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define DP_MEDIA             4u        /* Media Device Path (§10.3.5) */
#define DP_MEDIA_HARD_DRIVE  1u
#define DP_MEDIA_FILE_PATH   4u
#define DP_END               0x7Fu     /* End of Hardware Device Path (§10.3.1) */
#define DP_END_ENTIRE        0xFFu
#define DP_HARD_DRIVE_BYTES  42u
#define HD_FORMAT_GPT        0x02u
#define HD_SIGNATURE_GUID    0x02u
#define BOOT_NAME_MOST       512u      /* a variable's name the walk takes, in CHAR16 */
#define BOOT_OPTION_MOST     4096u     /* an option's bytes the walk reads */
#define BOOT_NUMBERS         0x10000u
#define BOOT_NUMBERS_TAKEN_MOST 64u    /* options naming one file taken away in one call */

static void Put16(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void Put32(uint8_t* p, uint32_t v)
{
    Put16(p, v);
    Put16(p + 2, v >> 16);
}

static void Put64(uint8_t* p, uint64_t v)
{
    Put32(p, (uint32_t)v);
    Put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t Get16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* The file path list — Hard Drive · File Path · End — into `out` (NULL to
 * count): its bytes. */
static uint32_t PathListPut(uint8_t* out, const EfiPartition* part, const char* path)
{
    uint32_t chars = (uint32_t)strlen(path) + 2u;      /* the leading '\' and the terminator */
    uint32_t file  = 4u + 2u * chars;
    if (out) {
        uint8_t* p = out;
        p[0] = DP_MEDIA, p[1] = DP_MEDIA_HARD_DRIVE;
        Put16(p + 2, DP_HARD_DRIVE_BYTES);
        Put32(p + 4, part->number);
        Put64(p + 8, part->first);
        Put64(p + 16, part->blocks);
        memcpy(p + 24, part->unique, 16);
        p[40] = HD_FORMAT_GPT, p[41] = HD_SIGNATURE_GUID;
        p += DP_HARD_DRIVE_BYTES;
        p[0] = DP_MEDIA, p[1] = DP_MEDIA_FILE_PATH;
        Put16(p + 2, file);
        Put16(p + 4, '\\');
        for (uint32_t i = 0; path[i]; i++)
            Put16(p + 6 + 2u * i, path[i] == '/' ? '\\' : (uint8_t)path[i]);
        Put16(p + 4 + 2u * (chars - 1u), 0);
        p += file;
        p[0] = DP_END, p[1] = DP_END_ENTIRE;
        Put16(p + 2, 4);
    }
    return DP_HARD_DRIVE_BYTES + file + 4u;
}

/* Whether the option `opt` (`bytes`) names the file path list `list`. */
static bool OptionNames(const uint8_t* opt, uint64_t bytes, const uint8_t* list, uint32_t list_bytes)
{
    if (bytes < 6u || Get16(opt + 4) != list_bytes)
        return false;
    uint64_t at = 6;
    while (at + 1u < bytes && Get16(opt + at) != 0)
        at += 2;
    at += 2;
    return at + list_bytes <= bytes && memcmp(opt + at, list, list_bytes) == 0;
}

static int HexDigit(uint16_t c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* The number of a Boot#### variable's name, or -1 for any other name. */
static int32_t BootNumber(const uint16_t* name)
{
    if (name[0] != 'B' || name[1] != 'o' || name[2] != 'o' || name[3] != 't' || name[8] != 0)
        return -1;
    int32_t n = 0;
    for (uint32_t i = 4; i < 8u; i++) {
        int d = HexDigit(name[i]);
        if (d < 0)
            return -1;
        n = n << 4 | d;
    }
    return n;
}

/* Every Boot#### of the firmware walked: the numbers taken into `used`, and
 * the first that names `list` into *same (-1 for none). False when the
 * firmware would not be walked. */
static bool OptionsWalked(uint8_t* used, const uint8_t* list, uint32_t list_bytes, int32_t* same)
{
    uint16_t* name = kmalloc(BOOT_NAME_MOST * sizeof(uint16_t));
    uint8_t*  opt  = kmalloc(BOOT_OPTION_MOST);
    EfiGuid   vendor;
    bool      walked = name && opt;
    *same            = -1;
    if (walked) {
        name[0] = 0;
        memset(&vendor, 0, sizeof(vendor));
    }
    while (walked) {
        uint64_t  size = BOOT_NAME_MOST * sizeof(uint16_t);
        EfiStatus s    = efi_get_next_variable_name(&size, name, &vendor);
        if (s == EFI_STATUS_NOT_FOUND)
            break;
        if (s != EFI_STATUS_SUCCESS) {
            walked = false;
            break;
        }
        int32_t n = BootNumber(name);
        if (n < 0 || memcmp(&vendor, &EFI_GLOBAL_VARIABLE_GUID, sizeof(EfiGuid)) != 0)
            continue;
        used[n / 8] |= (uint8_t)(1u << (n % 8));
        uint64_t bytes = BOOT_OPTION_MOST;
        uint32_t attrs = 0;
        if (*same < 0 && efi_get_variable(name, &EFI_GLOBAL_VARIABLE_GUID, &attrs, &bytes, opt) == EFI_STATUS_SUCCESS &&
            OptionNames(opt, bytes, list, list_bytes))
            *same = n;
    }
    kfree(name);
    kfree(opt);
    return walked;
}

/* `number` put in BootOrder: first, or last when it is not there. */
static error_t OrderPut(uint16_t number, bool first)
{
    uint64_t  bytes = 0;
    uint32_t  attrs = 0;
    uint16_t* order = NULL;
    EfiStatus s     = efi_get_variable_ascii("BootOrder", &EFI_GLOBAL_VARIABLE_GUID, &attrs, &bytes, NULL);
    if (s == EFI_STATUS_BUFFER_TOO_SMALL) {
        order = kmalloc(bytes + 2u);
        if (!order)
            return ERR_NO_MEMORY;
        s = efi_get_variable_ascii("BootOrder", &EFI_GLOBAL_VARIABLE_GUID, &attrs, &bytes, order);
    }
    if (s != EFI_STATUS_SUCCESS)
        bytes = 0;      /* no order yet: this option is all of it */
    uint64_t  count = bytes / 2u, kept = 0;
    uint16_t* now   = kmalloc((count + 1u) * sizeof(uint16_t));
    if (!now) {
        kfree(order);
        return ERR_NO_MEMORY;
    }
    bool there = false;
    for (uint64_t i = 0; i < count; i++)
        there |= order[i] == number;
    if (first)
        now[kept++] = number;
    for (uint64_t i = 0; i < count; i++)
        if (!first || order[i] != number)
            now[kept++] = order[i];
    if (!first && !there)
        now[kept++] = number;
    s = efi_set_variable_ascii("BootOrder", &EFI_GLOBAL_VARIABLE_GUID, BOOT_VARIABLE_ATTRS, kept * sizeof(uint16_t),
                               now);
    kfree(order);
    kfree(now);
    if (s != EFI_STATUS_SUCCESS) {
        kprintf("[EFI] the firmware refused BootOrder (status 0x%llx)\n", (unsigned long long)s);
        return ERR_WRITE_FAILED;
    }
    return OK;
}

error_t EfiBootOptionPut(const char* description, const EfiPartition* part, const char* path, const void* data,
                         uint32_t data_bytes, bool first, uint16_t* number)
{
    if (!efi_runtime_available())
        return ERR_UNSUPPORTED;
    uint32_t list_bytes = PathListPut(NULL, part, path);
    uint32_t desc_chars = (uint32_t)strlen(description) + 1u;
    uint64_t bytes      = 6u + 2u * desc_chars + list_bytes + data_bytes;
    uint8_t* opt        = kmalloc(bytes);
    uint8_t* used       = kmalloc(BOOT_NUMBERS / 8u);
    if (!opt || !used) {
        kfree(opt);
        kfree(used);
        return ERR_NO_MEMORY;
    }
    memset(used, 0, BOOT_NUMBERS / 8u);
    Put32(opt, LOAD_OPTION_ACTIVE);
    Put16(opt + 4, list_bytes);
    for (uint32_t i = 0; i < desc_chars; i++)
        Put16(opt + 6 + 2u * i, (uint8_t)description[i]);
    uint8_t* list = opt + 6 + 2u * desc_chars;
    PathListPut(list, part, path);
    if (data_bytes)
        memcpy(list + list_bytes, data, data_bytes);

    int32_t same = -1, chosen = -1;
    error_t rc   = OptionsWalked(used, list, list_bytes, &same) ? OK : ERR_WRITE_FAILED;
    if (rc != OK)
        kprintf("[EFI] the firmware's variables would not be walked — no boot option written\n");
    chosen = same;
    for (uint32_t n = 0; rc == OK && chosen < 0 && n < BOOT_NUMBERS; n++)
        if (!(used[n / 8] & (1u << (n % 8))))
            chosen = (int32_t)n;
    if (rc == OK && chosen < 0)
        rc = ERR_QUOTA_EXCEEDED;
    if (rc == OK) {
        static const char hex[] = "0123456789ABCDEF";
        char name[9] = { 'B', 'o', 'o', 't', hex[chosen >> 12 & 15], hex[chosen >> 8 & 15], hex[chosen >> 4 & 15],
                         hex[chosen & 15], 0 };
        EfiStatus s = efi_set_variable_ascii(name, &EFI_GLOBAL_VARIABLE_GUID, BOOT_VARIABLE_ATTRS, bytes, opt);
        if (s != EFI_STATUS_SUCCESS) {
            kprintf("[EFI] the firmware refused %s (status 0x%llx)\n", name, (unsigned long long)s);
            rc = ERR_WRITE_FAILED;
        } else {
            rc = OrderPut((uint16_t)chosen, first);
            if (rc == OK) {
                kprintf("[EFI] %s \"%s\" names %s on GPT partition %u, %s in the boot order\n", name, description,
                        path, part->number, first ? "first" : same >= 0 ? "where it stood" : "last");
                *number = (uint16_t)chosen;
            }
        }
    }
    kfree(opt);
    kfree(used);
    return rc;
}

/* The numbers of every Boot#### that names `list` into `numbers` (room for
 * `most`): how many, or -1 when the firmware would not be walked. */
static int32_t OptionsNaming(const uint8_t* list, uint32_t list_bytes, uint16_t* numbers, uint32_t most)
{
    uint16_t* name  = kmalloc(BOOT_NAME_MOST * sizeof(uint16_t));
    uint8_t*  opt   = kmalloc(BOOT_OPTION_MOST);
    EfiGuid   vendor;
    int32_t   found = name && opt ? 0 : -1;
    if (found == 0) {
        name[0] = 0;
        memset(&vendor, 0, sizeof(vendor));
    }
    while (found >= 0) {
        uint64_t  size = BOOT_NAME_MOST * sizeof(uint16_t);
        EfiStatus s    = efi_get_next_variable_name(&size, name, &vendor);
        if (s == EFI_STATUS_NOT_FOUND)
            break;
        if (s != EFI_STATUS_SUCCESS) {
            found = -1;
            break;
        }
        int32_t n = BootNumber(name);
        if (n < 0 || memcmp(&vendor, &EFI_GLOBAL_VARIABLE_GUID, sizeof(EfiGuid)) != 0)
            continue;
        uint64_t bytes = BOOT_OPTION_MOST;
        uint32_t attrs = 0;
        if (efi_get_variable(name, &EFI_GLOBAL_VARIABLE_GUID, &attrs, &bytes, opt) == EFI_STATUS_SUCCESS &&
            OptionNames(opt, bytes, list, list_bytes) && (uint32_t)found < most)
            numbers[found++] = (uint16_t)n;
    }
    kfree(name);
    kfree(opt);
    return found;
}

/* `numbers` (`count` of them) taken out of BootOrder; an order left empty
 * deleted. */
static error_t OrderTakeAway(const uint16_t* numbers, uint32_t count)
{
    uint64_t  bytes = 0;
    uint32_t  attrs = 0;
    EfiStatus s     = efi_get_variable_ascii("BootOrder", &EFI_GLOBAL_VARIABLE_GUID, &attrs, &bytes, NULL);
    if (s != EFI_STATUS_BUFFER_TOO_SMALL)
        return OK;          /* no order: nothing to take out of it */
    uint16_t* order = kmalloc(bytes + 2u);
    if (!order)
        return ERR_NO_MEMORY;
    s = efi_get_variable_ascii("BootOrder", &EFI_GLOBAL_VARIABLE_GUID, &attrs, &bytes, order);
    uint64_t kept = 0;
    for (uint64_t i = 0; s == EFI_STATUS_SUCCESS && i < bytes / 2u; i++) {
        bool gone = false;
        for (uint32_t k = 0; k < count; k++)
            gone |= order[i] == numbers[k];
        if (!gone)
            order[kept++] = order[i];
    }
    if (s == EFI_STATUS_SUCCESS)
        s = efi_set_variable_ascii("BootOrder", &EFI_GLOBAL_VARIABLE_GUID, kept ? BOOT_VARIABLE_ATTRS : 0,
                                   kept * sizeof(uint16_t), kept ? order : NULL);
    kfree(order);
    if (s != EFI_STATUS_SUCCESS) {
        kprintf("[EFI] the firmware refused BootOrder (status 0x%llx)\n", (unsigned long long)s);
        return ERR_WRITE_FAILED;
    }
    return OK;
}

error_t EfiBootOptionsTakeAway(const EfiPartition* part, const char* path, uint32_t* removed)
{
    if (removed)
        *removed = 0;
    if (!efi_runtime_available())
        return ERR_UNSUPPORTED;
    uint32_t  list_bytes = PathListPut(NULL, part, path);
    uint8_t*  list       = kmalloc(list_bytes);
    uint16_t* numbers    = kmalloc(BOOT_NUMBERS_TAKEN_MOST * sizeof(uint16_t));
    if (!list || !numbers) {
        kfree(list);
        kfree(numbers);
        return ERR_NO_MEMORY;
    }
    PathListPut(list, part, path);
    int32_t found = OptionsNaming(list, list_bytes, numbers, BOOT_NUMBERS_TAKEN_MOST);
    error_t rc    = found < 0 ? ERR_WRITE_FAILED : OK;
    if (found < 0)
        kprintf("[EFI] the firmware's variables would not be walked — no boot option taken away\n");
    static const char hex[] = "0123456789ABCDEF";
    for (int32_t i = 0; rc == OK && i < found; i++) {
        char      name[9] = { 'B', 'o', 'o', 't', hex[numbers[i] >> 12 & 15], hex[numbers[i] >> 8 & 15],
                              hex[numbers[i] >> 4 & 15], hex[numbers[i] & 15], 0 };
        EfiStatus s       = efi_set_variable_ascii(name, &EFI_GLOBAL_VARIABLE_GUID, 0, 0, NULL);
        if (s != EFI_STATUS_SUCCESS) {
            kprintf("[EFI] the firmware refused to let %s go (status 0x%llx)\n", name, (unsigned long long)s);
            rc = ERR_WRITE_FAILED;
        } else {
            kprintf("[EFI] %s, which named %s on GPT partition %u, is taken away\n", name, path, part->number);
        }
    }
    if (rc == OK && found > 0)
        rc = OrderTakeAway(numbers, (uint32_t)found);
    if (rc == OK && removed)
        *removed = (uint32_t)found;
    kfree(list);
    kfree(numbers);
    return rc;
}
