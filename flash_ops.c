#include "flash_ops.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define FLASH_USER_REGION_SIZE (PICO_FLASH_SIZE_BYTES - FLASH_TARGET_OFFSET)
static uint8_t flash_sector_work_buffer[FLASH_SECTOR_SIZE];

static bool validate_flash_range(uint32_t flash_offset, size_t length) {
    uint64_t end = (uint64_t)flash_offset + length;
    return flash_offset >= FLASH_TARGET_OFFSET && end <= PICO_FLASH_SIZE_BYTES;
}

static bool offset_to_flash_offset(uint32_t offset, size_t length, uint32_t *flash_offset_out, const char *op_name) {
    uint64_t flash_offset_64 = (uint64_t)FLASH_TARGET_OFFSET + offset;

    if (flash_offset_64 > UINT32_MAX) {
        printf("Error: %s offset overflow (offset=%lu)\n", op_name, (unsigned long)offset);
        return false;
    }

    uint32_t flash_offset = (uint32_t)flash_offset_64;
    if (!validate_flash_range(flash_offset, length)) {
        printf(
            "Error: %s out-of-bounds (offset=%lu len=%zu, valid bytes=%lu)\n",
            op_name,
            (unsigned long)offset,
            length,
            (unsigned long)FLASH_USER_REGION_SIZE
        );
        return false;
    }

    *flash_offset_out = flash_offset;
    return true;
}

// Function: flash_write_safe
// Writes data to flash memory at a specified offset, ensuring safety checks.
//
// Parameters:
// - offset: The offset from FLASH_TARGET_OFFSET where data is to be written.
// - data: Pointer to the data to be written.
// - data_len: Length of the data to be written.
//
// Note: This function erases the flash sector before writing new data.
void flash_write_safe(uint32_t offset, const uint8_t *data, size_t data_len) {
    if (data_len == 0) {
        printf("Error: write length must be greater than zero\n");
        return;
    }

    if (data == NULL) {
        printf("Error: write data pointer is NULL\n");
        return;
    }

    uint32_t flash_offset = 0;
    if (!offset_to_flash_offset(offset, data_len, &flash_offset, "write")) {
        return;
    }

    size_t remaining = data_len;
    const uint8_t *src = data;
    uint32_t current_flash_offset = flash_offset;

    while (remaining > 0) {
        uint32_t sector_base = current_flash_offset - (current_flash_offset % FLASH_SECTOR_SIZE);
        size_t sector_offset = current_flash_offset - sector_base;
        size_t chunk_len = FLASH_SECTOR_SIZE - sector_offset;
        if (chunk_len > remaining) {
            chunk_len = remaining;
        }

        // Buffering the full sector allows safe partial writes even though flash can only
        // erase whole sectors and program aligned pages.
        memcpy(flash_sector_work_buffer, (const void *)(XIP_BASE + sector_base), FLASH_SECTOR_SIZE);
        memcpy(flash_sector_work_buffer + sector_offset, src, chunk_len);

        // Interrupts are disabled during erase/program because flash is unavailable while
        // these operations run, and interrupt handlers may otherwise fetch code/data from flash.
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(sector_base, FLASH_SECTOR_SIZE);

        for (uint32_t page = 0; page < FLASH_SECTOR_SIZE; page += FLASH_PAGE_SIZE) {
            flash_range_program(sector_base + page, flash_sector_work_buffer + page, FLASH_PAGE_SIZE);
        }
        restore_interrupts(ints);

        src += chunk_len;
        remaining -= chunk_len;
        current_flash_offset += (uint32_t)chunk_len;
    }
}

// Function: flash_read_safe
// Reads data from flash memory into a buffer.
//
// Parameters:
// - offset: The offset from FLASH_TARGET_OFFSET where data is to be read.
// - buffer: Pointer to the buffer where read data will be stored.
// - buffer_len: Number of bytes to read.
//
// Note: The function performs bounds checking to ensure safe access.
void flash_read_safe(uint32_t offset, uint8_t *buffer, size_t buffer_len) {
    if (buffer_len == 0) {
        return;
    }

    if (buffer == NULL) {
        printf("Error: read buffer pointer is NULL\n");
        return;
    }

    uint32_t flash_offset = 0;
    if (!offset_to_flash_offset(offset, buffer_len, &flash_offset, "read")) {
        return;
    }

    memcpy(buffer, (const void *)(XIP_BASE + flash_offset), buffer_len);
}

// Function: flash_erase_safe
// Erases a sector of the flash memory.
//
// Parameters:
// - offset: The offset from FLASH_TARGET_OFFSET where the erase starts.
//
// Note: This function checks that the operation stays within valid bounds.
void flash_erase_safe(uint32_t offset) {
    uint32_t flash_offset = 0;
    if (!offset_to_flash_offset(offset, FLASH_SECTOR_SIZE, &flash_offset, "erase")) {
        return;
    }

    if ((flash_offset % FLASH_SECTOR_SIZE) != 0) {
        printf(
            "Error: erase offset must align to %u bytes (offset=%lu)\n",
            FLASH_SECTOR_SIZE,
            (unsigned long)offset
        );
        return;
    }

    // Interrupts are disabled because the flash erase operation temporarily blocks XIP access.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(flash_offset, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}
