#include "cli.h"
#include "flash_ops.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "custom_fgets.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"

#define FLASH_RECORD_MAGIC   0x464C5348u // "FLSH"
#define FLASH_RECORD_VERSION 1u
#define FLASH_RECORD_PAYLOAD_SIZE (FLASH_SECTOR_SIZE - (4u * sizeof(uint32_t)))

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t write_count;
    uint32_t data_len;
    uint8_t payload[FLASH_RECORD_PAYLOAD_SIZE];
} flash_record_t;

_Static_assert(sizeof(flash_record_t) == FLASH_SECTOR_SIZE, "flash record must occupy exactly one sector");

static flash_record_t cli_existing_record;
static flash_record_t cli_write_record;
static flash_record_t cli_read_record;

static uint32_t flash_user_sector_count(void) {
    return (PICO_FLASH_SIZE_BYTES - FLASH_TARGET_OFFSET) / FLASH_SECTOR_SIZE;
}

static bool parse_sector_number(const char *token, uint32_t *sector_number) {
    if (token == NULL || sector_number == NULL) {
        return false;
    }

    char *end = NULL;
    long value = strtol(token, &end, 10);
    if (end == token || *end != '\0' || value < 0) {
        return false;
    }

    *sector_number = (uint32_t)value;
    return true;
}

static bool sector_to_offset(uint32_t sector_number, uint32_t *offset) {
    uint32_t sector_count = flash_user_sector_count();
    if (sector_number >= sector_count) {
        printf(
            "\nError: sector number out of range (%lu). Valid range is 0..%lu\n",
            (unsigned long)sector_number,
            (unsigned long)(sector_count - 1)
        );
        return false;
    }

    *offset = sector_number * FLASH_SECTOR_SIZE;
    return true;
}

static size_t max_payload_len(void) {
    return FLASH_RECORD_PAYLOAD_SIZE;
}

static bool record_is_valid(const flash_record_t *record) {
    if (record->magic != FLASH_RECORD_MAGIC || record->version != FLASH_RECORD_VERSION) {
        return false;
    }

    if (record->data_len > max_payload_len()) {
        return false;
    }

    return true;
}

// Function: execute_command
// Parses and executes commands related to flash memory operations.
//
// Parameters:
// - command: A string containing the command and its arguments.
//
// The function supports the following commands:
// - FLASH_WRITE: Writes data to flash memory.
// - FLASH_READ: Reads data from flash memory.
// - FLASH_ERASE: Erases a sector of flash memory.
//
// Each command expects specific arguments following the command name.
void execute_command(char *command) {
    // Split the command string into tokens
    char *token = strtok(command, " ");

    // Check for an empty or invalid command
    if (token == NULL) {
        printf("\nInvalid command\n");
        return;
    }

    // Handle the FLASH_WRITE command
    if (strcmp(token, "FLASH_WRITE") == 0) {
        // Parse the sector number
        token = strtok(NULL, " ");
        if (token == NULL) {
            printf("\nFLASH_WRITE requires a flash_sector_number and data\n");
            return;
        }
        uint32_t sector_number = 0;
        if (!parse_sector_number(token, &sector_number)) {
            printf("\nError: invalid flash sector number for FLASH_WRITE\n");
            return;
        }

        uint32_t offset = 0;
        if (!sector_to_offset(sector_number, &offset)) {
            return;
        }

        // Parse the data, assuming it's enclosed in quotes
        token = strtok(NULL, "\"");
        if (token == NULL) {
            printf("\nInvalid data format for FLASH_WRITE\n");
            return;
        }

        size_t payload_len = strlen(token);
        if (payload_len > max_payload_len()) {
            printf(
                "\nError: oversized payload (%zu bytes). Maximum for one sector is %zu bytes\n",
                payload_len,
                max_payload_len()
            );
            return;
        }

        flash_read_safe(offset, (uint8_t *)&cli_existing_record, sizeof(cli_existing_record));

        uint32_t next_write_count = 1;
        if (record_is_valid(&cli_existing_record)) {
            next_write_count = cli_existing_record.write_count + 1;
        }

        memset(&cli_write_record, 0xFF, sizeof(cli_write_record));
        cli_write_record.magic = FLASH_RECORD_MAGIC;
        cli_write_record.version = FLASH_RECORD_VERSION;
        cli_write_record.write_count = next_write_count;
        cli_write_record.data_len = (uint32_t)payload_len;
        memcpy(cli_write_record.payload, token, payload_len);

        flash_write_safe(offset, (const uint8_t *)&cli_write_record, sizeof(cli_write_record));
        printf(
            "\nFLASH_WRITE OK: sector=%lu write_count=%lu data_len=%lu\n",
            (unsigned long)sector_number,
            (unsigned long)cli_write_record.write_count,
            (unsigned long)cli_write_record.data_len
        );
    }
    // Handle the FLASH_READ command
    else if (strcmp(token, "FLASH_READ") == 0) {
        // Parse the sector number
        token = strtok(NULL, " ");
        if (token == NULL) {
            printf("\nFLASH_READ requires a flash_sector_number\n");
            return;
        }

        uint32_t sector_number = 0;
        if (!parse_sector_number(token, &sector_number)) {
            printf("\nError: invalid flash sector number for FLASH_READ\n");
            return;
        }

        uint32_t offset = 0;
        if (!sector_to_offset(sector_number, &offset)) {
            return;
        }

        flash_read_safe(offset, (uint8_t *)&cli_read_record, sizeof(cli_read_record));

        if (!record_is_valid(&cli_read_record)) {
            printf("\nError: sector %lu is empty or uninitialized\n", (unsigned long)sector_number);
            return;
        }

        printf(
            "\nMetadata: write_count=%lu data_len=%lu\n",
            (unsigned long)cli_read_record.write_count,
            (unsigned long)cli_read_record.data_len
        );

        printf("Payload: ");
        for (uint32_t i = 0; i < cli_read_record.data_len; ++i) {
            uint8_t ch = cli_read_record.payload[i];
            if (ch >= 32 && ch <= 126) {
                putchar(ch);
            } else {
                printf("\\x%02X", ch);
            }
        }
        printf("\n");
    }
    // Handle the FLASH_ERASE command
    else if (strcmp(token, "FLASH_ERASE") == 0) {
        // Parse the sector number
        token = strtok(NULL, " ");
        if (token == NULL) {
            printf("\nFLASH_ERASE requires a flash_sector_number\n");
            return;
        }
        uint32_t sector_number = 0;
        if (!parse_sector_number(token, &sector_number)) {
            printf("\nError: invalid flash sector number for FLASH_ERASE\n");
            return;
        }

        uint32_t offset = 0;
        if (!sector_to_offset(sector_number, &offset)) {
            return;
        }

        // Execute the erase operation
        flash_erase_safe(offset);
        printf("\nFLASH_ERASE OK: sector=%lu\n", (unsigned long)sector_number);
    }
    // Handle unknown commands
    else {
        printf("\nUnknown command\n");
    }
}
