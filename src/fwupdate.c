#include "dmod.h"
#include "dmflash_ioctl.h"
#include <errno.h>
#include <string.h>

/*
 * fwupdate - writes a firmware image to a bank of the internal flash and tells the platform to start from it.
 *
 *   fwupdate status
 *   fwupdate write <image> [bank] [--select | --trial]
 *   fwupdate verify <bank>
 *   fwupdate select <bank> [--trial]
 *   fwupdate confirm
 *
 * confirm: the running firmware says it works (Dmod_FirmwareReady): a unit of the image runs it once everything the image
 * needs is up (After=/Requires= say what), see README.md. The banks, their state, the verification and the choice belong to the platform (the Dmod_Firmware* functions of the
 * SAL); the bytes go through the dmflash node of the bank (/dev/dmflashx/bank_<a, b, ...>), which reaches only the
 * sectors of its bank. The running bank is never written: without [bank] the image goes to the other one.
 */

#define NODE_FORMAT         "/dev/dmflashx/bank_%c"
#define CHUNK_SIZE          4096u

/* Header of an image (the first bytes of a bank, written by the tool of the platform that makes the image) */
#define IMAGE_MAGIC         0x4B424D44u     /* 'DMBK' */
#define IMAGE_HEADER_AREA   512u            /* Written last: a bank whose write was interrupted has no header */
#define IMAGE_SIZE_OFFSET   8u

/* A switch, not a table of pointers: a module has no relocated data */
static const char* state_name(uint32_t state)
{
    switch(state)
    {
        case Dmod_FirmwareBankState_Empty:      return "empty";
        case Dmod_FirmwareBankState_Invalid:    return "invalid";
        case Dmod_FirmwareBankState_Valid:      return "valid";
        case Dmod_FirmwareBankState_Trial:      return "trial";
        case Dmod_FirmwareBankState_Confirmed:  return "confirmed";
        default:                                return "?";
    }
}

static char bank_letter(int bank)
{
    return (char)('a' + bank);
}

/* "a", "B" or "0" -> the number of the bank; -1 when it is none */
static int parse_bank(const char* text)
{
    if (text[0] == '\0' || text[1] != '\0')
        return -1;
    if (text[0] >= '0' && text[0] <= '9')
        return text[0] - '0';
    if (text[0] >= 'a' && text[0] <= 'z')
        return text[0] - 'a';
    if (text[0] >= 'A' && text[0] <= 'Z')
        return text[0] - 'A';
    return -1;
}

static int check_bank(int bank)
{
    int count = Dmod_FirmwareGetBankCount();
    if (count < 1)
    {
        Dmod_Printf("fwupdate: this firmware has no banks (%d)\n", count);
        return -ENOSYS;
    }
    if (bank < 0 || bank >= count)
    {
        Dmod_Printf("fwupdate: there is no such bank (there are %d)\n", count);
        return -EINVAL;
    }
    return 0;
}

static int cmd_status(void)
{
    int count = Dmod_FirmwareGetBankCount();
    if (count < 1)
    {
        Dmod_Printf("fwupdate: this firmware has no banks (%d)\n", count);
        return 1;
    }
    int running = Dmod_FirmwareGetRunningBank();
    for (int bank = 0; bank < count; bank++)
    {
        Dmod_FirmwareBankInfo_t info;
        memset(&info, 0, sizeof(info));
        info.StructSize = sizeof(info);
        int ret = Dmod_FirmwareGetBankInfo(bank, &info);
        if (ret != 0)
        {
            Dmod_Printf("%c: error %d\n", 'A' + bank, ret);
            continue;
        }
        Dmod_Printf("%c%s: %-9s version %u, %u of %u bytes, 0x%08x, starts without confirming: %u\n", 'A' + bank,
                    bank == running ? " (running)" : "", state_name(info.State), (unsigned)info.Version,
                    (unsigned)info.Size, (unsigned)info.Capacity, (unsigned)info.Address, (unsigned)info.Tries);
    }
    return 0;
}

static int cmd_verify(int bank)
{
    if (check_bank(bank) != 0)
        return 1;
    int ret = Dmod_FirmwareVerifyBank(bank);
    if (ret == 0)
        Dmod_Printf("fwupdate: the image in the bank %c is good\n", 'A' + bank);
    else if (ret == -ENODATA)
        Dmod_Printf("fwupdate: the bank %c is empty\n", 'A' + bank);
    else if (ret == -EBADMSG)
        Dmod_Printf("fwupdate: the image in the bank %c is damaged\n", 'A' + bank);
    else
        Dmod_Printf("fwupdate: cannot verify the bank %c (%d)\n", 'A' + bank, ret);
    return ret == 0 ? 0 : 1;
}

static int cmd_select(int bank, bool trial)
{
    if (check_bank(bank) != 0)
        return 1;
    int ret = Dmod_FirmwareVerifyBank(bank);
    if (ret != 0)
    {
        Dmod_Printf("fwupdate: the bank %c cannot be selected: its image is not good (%d)\n", 'A' + bank, ret);
        return 1;
    }
    ret = Dmod_FirmwareSelectBank(bank, trial);
    if (ret != 0)
    {
        Dmod_Printf("fwupdate: cannot select the bank %c (%d)\n", 'A' + bank, ret);
        return 1;
    }
    Dmod_Printf("fwupdate: the bank %c starts after the next reset%s\n", 'A' + bank,
                trial ? " (on trial: it stays when it confirms)" : "");
    return 0;
}

/* Reads `size` bytes at `offset` of a file; 0 on success */
static int file_read(void* fp, uint32_t offset, void* buffer, uint32_t size)
{
    if (Dmod_FileSeek(fp, (Dmod_FileOffset_t)offset, DMOD_SEEK_SET) != 0)
        return -EIO;
    return Dmod_FileRead(buffer, 1, size, fp) == size ? 0 : -EIO;
}

static int file_write(void* fp, uint32_t offset, const void* buffer, uint32_t size)
{
    if (Dmod_FileSeek(fp, (Dmod_FileOffset_t)offset, DMOD_SEEK_SET) != 0)
        return -EIO;
    return Dmod_FileWrite(buffer, 1, size, fp) == size ? 0 : -EIO;
}

/* Erases the sectors of the node that [0, size) touches */
static int erase_image_area(void* node, uint32_t size)
{
    dmflash_info_t info;
    memset(&info, 0, sizeof(info));
    int ret = Dmod_Ioctl(node, DMFLASH_IOCTL_GET_INFO, &info);
    if (ret != 0)
        return ret;
    for (uint32_t i = 0; i < info.sector_count; i++)
    {
        dmflash_sector_arg_t arg = { .index = i };
        ret = Dmod_Ioctl(node, DMFLASH_IOCTL_GET_SECTOR, &arg);
        if (ret != 0)
            return ret;
        if (arg.sector.offset >= size)
            break;
        Dmod_Printf("erase %u kB at 0x%x\n", (unsigned)(arg.sector.size / 1024u), (unsigned)arg.sector.offset);
        dmdrvi_block_range_t range = { .offset = arg.sector.offset, .length = arg.sector.size };
        ret = Dmod_Ioctl(node, DMDRVI_IOCTL_BLOCK_ERASE, &range);
        if (ret != 0)
            return ret;
    }
    return 0;
}

/* Copies [from, to) of the image to the node, the other way round than it lies: nothing is checked on the way */
static int copy_range(void* image, void* node, uint32_t from, uint32_t to, uint8_t* buffer, uint8_t* readback)
{
    for (uint32_t offset = from; offset < to; offset += CHUNK_SIZE)
    {
        uint32_t size = to - offset < CHUNK_SIZE ? to - offset : CHUNK_SIZE;
        int ret = file_read(image, offset, buffer, size);
        if (ret == 0)
            ret = file_write(node, offset, buffer, size);
        if (ret == 0)
            ret = file_read(node, offset, readback, size);
        if (ret != 0 || memcmp(buffer, readback, size) != 0)
        {
            Dmod_Printf("fwupdate: writing at 0x%x failed\n", (unsigned)offset);
            return ret != 0 ? ret : -EIO;
        }
    }
    return 0;
}

static int cmd_write(const char* path, int bank, bool select, bool trial)
{
    if (check_bank(bank) != 0)
        return 1;
    if (bank == Dmod_FirmwareGetRunningBank())
    {
        Dmod_Printf("fwupdate: the bank %c is the one that runs - write the other one\n", 'A' + bank);
        return 1;
    }
    Dmod_FirmwareBankInfo_t bank_info;
    memset(&bank_info, 0, sizeof(bank_info));
    bank_info.StructSize = sizeof(bank_info);
    if (Dmod_FirmwareGetBankInfo(bank, &bank_info) != 0)
    {
        Dmod_Printf("fwupdate: cannot read the state of the bank %c\n", 'A' + bank);
        return 1;
    }

    void* image = Dmod_FileOpen(path, "r");
    if (image == NULL)
    {
        Dmod_Printf("fwupdate: cannot open '%s'\n", path);
        return 1;
    }

    uint32_t header[IMAGE_SIZE_OFFSET / 4u + 1u];
    uint32_t image_size = 0;
    int result = 1;
    if (file_read(image, 0, header, sizeof(header)) != 0 || header[0] != IMAGE_MAGIC)
    {
        Dmod_Printf("fwupdate: '%s' is not a firmware image\n", path);
        goto close_image;
    }
    image_size = header[IMAGE_SIZE_OFFSET / 4u];
    if (image_size <= IMAGE_HEADER_AREA || image_size > bank_info.Capacity || image_size > Dmod_FileSize(image))
    {
        Dmod_Printf("fwupdate: the image is %u bytes: it does not fit the bank (%u bytes) or the file is cut\n",
                    (unsigned)image_size, (unsigned)bank_info.Capacity);
        goto close_image;
    }

    char node_path[32];
    Dmod_SnPrintf(node_path, sizeof(node_path), NODE_FORMAT, bank_letter(bank));
    void* node = Dmod_FileOpen(node_path, "r+");
    if (node == NULL)
    {
        Dmod_Printf("fwupdate: cannot open '%s' (is the node of the bank configured?)\n", node_path);
        goto close_image;
    }

    uint8_t* buffer = Dmod_Malloc(2u * CHUNK_SIZE);
    if (buffer == NULL)
    {
        Dmod_Printf("fwupdate: out of memory\n");
        goto close_node;
    }

    Dmod_Printf("writing %u bytes to the bank %c (%s)\n", (unsigned)image_size, 'A' + bank, node_path);
    int ret = erase_image_area(node, image_size);
    if (ret != 0)
        Dmod_Printf("fwupdate: erase failed (%d)\n", ret);
    /* The header goes last: until it is there the platform sees an empty bank, not a half image */
    if (ret == 0)
        ret = copy_range(image, node, IMAGE_HEADER_AREA, image_size, buffer, buffer + CHUNK_SIZE);
    if (ret == 0)
        ret = copy_range(image, node, 0, IMAGE_HEADER_AREA, buffer, buffer + CHUNK_SIZE);
    if (ret == 0)
    {
        Dmod_Printf("written, verifying\n");
        result = cmd_verify(bank);
        if (result == 0 && select)
            result = cmd_select(bank, trial);
    }

    Dmod_Free(buffer);
close_node:
    Dmod_FileClose(node);
close_image:
    Dmod_FileClose(image);
    return result;
}

static int cmd_confirm(void)
{
    int ret = Dmod_FirmwareReady();
    if (ret != 0)
    {
        Dmod_Printf("fwupdate: cannot confirm (%d)\n", ret);
        return 1;
    }
    Dmod_Printf("fwupdate: the running firmware is confirmed\n");
    return 0;
}

static int usage(void)
{
    Dmod_Printf("usage: fwupdate status\n"
                "       fwupdate write <image> [bank] [--select | --trial]\n"
                "       fwupdate verify <bank>\n"
                "       fwupdate select <bank> [--trial]\n"
                "       fwupdate confirm\n");
    return 1;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
        return usage();

    const char* positional[2] = { NULL, NULL };
    int positional_count = 0;
    bool select = false;
    bool trial = false;
    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--select") == 0)
            select = true;
        else if (strcmp(argv[i], "--trial") == 0)
            trial = select = true;
        else if (positional_count < 2 && argv[i][0] != '-')
            positional[positional_count++] = argv[i];
        else
            return usage();
    }

    if (strcmp(argv[1], "status") == 0 && positional_count == 0)
        return cmd_status();

    if (strcmp(argv[1], "confirm") == 0 && positional_count == 0 && !select)
        return cmd_confirm();

    if (strcmp(argv[1], "write") == 0 && positional_count >= 1)
    {
        int bank;
        if (positional_count == 2)
            bank = parse_bank(positional[1]);
        else
        {
            /* The other bank than the running one */
            int running = Dmod_FirmwareGetRunningBank();
            int count = Dmod_FirmwareGetBankCount();
            if (count != 2 || running < 0 || running >= count)
            {
                Dmod_Printf("fwupdate: say which bank to write\n");
                return 1;
            }
            bank = 1 - running;
        }
        return cmd_write(positional[0], bank, select, trial);
    }

    if (strcmp(argv[1], "verify") == 0 && positional_count == 1 && !select)
        return cmd_verify(parse_bank(positional[0]));

    if (strcmp(argv[1], "select") == 0 && positional_count == 1)
        return cmd_select(parse_bank(positional[0]), trial);

    return usage();
}
