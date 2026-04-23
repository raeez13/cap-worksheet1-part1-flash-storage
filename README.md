# Worksheet 1 Part 1 - Flash Storage

## 1) What This Project Is

This is a Raspberry Pi Pico C/C++ project based on the `cap_template`.
It builds one firmware target called `cap_template`.

RAM is volatile memory.
Data in RAM is lost when the Pico resets or loses power.
Flash is non-volatile memory.
Data in flash can remain stored after reset and power loss.

This project uses part of the Pico's external flash as a small persistent storage area.
The code avoids the start of flash, where the application firmware lives.
User data starts at `FLASH_TARGET_OFFSET`.

This repo implements:

- safe flash erase with bounds and alignment checks
- safe flash write using sector buffering
- safe flash read through the XIP memory map
- sector-based CLI commands over USB serial
- one structured metadata record per flash sector

The serial CLI is implemented in `cli.c`.
It accepts one command per line.

| Command | Syntax |
|---|---|
| Write text | `FLASH_WRITE <sector> "text"` |
| Read sector | `FLASH_READ <sector>` |
| Erase sector | `FLASH_ERASE <sector>` |

The target name is `cap_template`.
The build links against `pico_stdlib`.
USB stdio is enabled, so the CLI appears as a USB serial device after flashing.
UART stdio is disabled.

The target and extra outputs are defined in `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(cap_template C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(cap_template
  main.c
  custom_fgets.c
  flash_ops.c
  cli.c
)

pico_enable_stdio_usb(cap_template 1)
pico_enable_stdio_uart(cap_template 0)

pico_add_extra_outputs(cap_template)

target_link_libraries(cap_template pico_stdlib)
```

## 2) Repo Quick Start (Linux)

These commands assume:

- the Pico SDK is installed at `$HOME/Pico/pico-sdk`
- `cmake`, `make`, and `arm-none-eabi-gcc` are installed
- you are in the repo root

Configure and build:

```bash
mkdir -p build
cmake -S . -B build -DPICO_SDK_PATH="$HOME/Pico/pico-sdk"
cmake --build build
```

`pico_add_extra_outputs(cap_template)` makes CMake generate the flashable output files.
The exact target name comes from `CMakeLists.txt`.

Artifacts produced by this build:

| Artifact | Meaning |
|---|---|
| `build/cap_template.uf2` | file copied to the Pico in BOOTSEL mode |
| `build/cap_template.elf` | linked firmware image |
| `build/cap_template.bin` | raw binary firmware image |

Flash with BOOTSEL mode:

1. Hold the Pico `BOOTSEL` button.
2. Plug the Pico into USB.
3. Release `BOOTSEL`.
4. The Pico should mount as `RPI-RP2`.

Linux copy command:

```bash
cp build/cap_template.uf2 /media/$USER/RPI-RP2/
```

If that mount point is not present, check the actual mount point:

```bash
lsblk -f
```

Serial over USB is enabled in `CMakeLists.txt`.
UART stdio is disabled.
The code does not set a baud rate in source.
For USB serial tools, use `115200` as the terminal parameter.
With USB CDC serial, the baud value is mostly a terminal convention.

Find the serial device:

```bash
ls /dev/ttyACM*
```

Open with `screen`:

```bash
screen /dev/ttyACM0 115200
```

Open with `minicom`:

```bash
minicom -D /dev/ttyACM0 -b 115200
```

## 3) Flash Basics You Must Follow

The RP2040 normally executes code directly from external flash.
This is called XIP, which means execute in place.
In XIP mode, flash contents are visible in the processor address space.
This repo reads flash by copying from `XIP_BASE + flash_offset`.

The Pico SDK flash write APIs do not take a normal RAM pointer.
They take a flash offset from the start of flash.
That is why the code keeps two ideas separate:

- `offset`: logical offset inside this project's user data area
- `flash_offset`: absolute offset inside the Pico flash chip

Flash cannot be overwritten like RAM.
It has erase and program granularity:

- erase happens in sectors
- program happens in pages
- programming can change bits from `1` to `0`
- changing bits from `0` back to `1` requires an erase

This is why the write path buffers a whole sector.
If the user only changes a few bytes, the code still has to erase the whole sector.
The buffer preserves the other bytes in that sector.

Interrupts are disabled during erase and program.
This matters on RP2040 because flash is temporarily unavailable while the operation is running.
If an interrupt handler tried to execute from flash at that moment, the system could fault or stall.

| Rule | Value in this repo | What it means in practice |
|---|---|---|
| User flash start | `FLASH_TARGET_OFFSET (256u * 1024u)` | user data starts 256 KiB into flash |
| Flash size | `PICO_FLASH_SIZE_BYTES` | used for bounds checks; numeric value not present in this repo |
| Erase size | `FLASH_SECTOR_SIZE` | erase one whole sector; numeric value not present in this repo |
| Program size | `FLASH_PAGE_SIZE` | program one page at a time; numeric value not present in this repo |
| Read base | `XIP_BASE` | read flash through the memory map; numeric value not present in this repo |

## 4) Memory Layout Used By This Project

The application firmware is stored in flash.
This project does not write at the start of flash.
It moves user data to a later region by adding `FLASH_TARGET_OFFSET` to every logical offset.

The logical storage address used by the CLI is therefore not the raw flash address.
The conversion is:

```text
flash_offset = FLASH_TARGET_OFFSET + offset
```

The diagram is conceptual.
The exact application size is produced by the linker at build time.

```text
+------------+----------------------+-------------------------------+
| bootloader | application firmware | user data region              |
+------------+----------------------+-------------------------------+
                                ^
                                |
                         FLASH_TARGET_OFFSET
                         256 KiB from flash base
```

`FLASH_TARGET_OFFSET` is defined in `flash_ops.h`:

```c
#ifndef FLASH_OPS_H
#define FLASH_OPS_H

#include <stdint.h>
#include <stddef.h>

#define FLASH_TARGET_OFFSET (256u * 1024u)

void flash_write_safe(uint32_t offset, const uint8_t *data, size_t data_len);
void flash_read_safe(uint32_t offset, uint8_t *buffer, size_t buffer_len);
void flash_erase_safe(uint32_t offset);

#endif // FLASH_OPS_H
```

The offset is used in `flash_ops.c`:

```c
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
```

## 5) Task 1: Safe Read, Write, And Erase

### `flash_write_safe`

Parameters:

| Parameter | Meaning |
|---|---|
| `offset` | logical byte offset inside the user data region |
| `data` | bytes to store |
| `data_len` | number of bytes to store |

`offset` is not an XIP pointer.
It is also not a CPU address.
It is a project-level storage offset.
The helper converts it into `flash_offset` before calling the Pico SDK flash functions.

Step-by-step:

1. Reject zero-length writes.
2. Reject a null data pointer.
3. Convert `offset` to a real flash offset using `FLASH_TARGET_OFFSET + offset`.
4. Check that the write stays inside `PICO_FLASH_SIZE_BYTES`.
5. Find the sector containing the write.
6. Copy that sector into RAM.
7. Patch the new bytes into the RAM buffer.
8. Disable interrupts.
9. Erase the sector.
10. Program the sector back page by page.
11. Restore interrupts.

The write function:

```c
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
```

Alignment is handled inside the function.
The function rounds the write address down to `sector_base`.
It reads exactly one `FLASH_SECTOR_SIZE` sector into `flash_sector_work_buffer`.
Then it patches only the changed range.
After erase, it writes the whole sector back in `FLASH_PAGE_SIZE` chunks.

This means partial writes are supported.
The caller can write fewer than one sector of data without manually padding to a page or sector boundary.

### `flash_read_safe`

Parameters:

| Parameter | Meaning |
|---|---|
| `offset` | logical byte offset inside the user data region |
| `buffer` | RAM buffer to fill |
| `buffer_len` | number of bytes to read |

Reads are simpler than writes.
No erase is required.
The Pico maps flash into memory, so the implementation can use `memcpy`.
The bounds check still runs before copying.

Step-by-step:

1. Return immediately for a zero-length read.
2. Reject a null buffer.
3. Convert `offset` to a real flash offset.
4. Check bounds.
5. Copy bytes from `XIP_BASE + flash_offset`.

The read function:

```c
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
```

### `flash_erase_safe`

Parameters:

| Parameter | Meaning |
|---|---|
| `offset` | logical byte offset inside the user data region |

Erase works at sector granularity.
The code rejects erase offsets that do not align to `FLASH_SECTOR_SIZE`.
The CLI passes sector-aligned offsets because it multiplies the sector number by `FLASH_SECTOR_SIZE`.

Step-by-step:

1. Convert `offset` to a real flash offset.
2. Check one sector fits inside flash.
3. Check the erase address is sector-aligned.
4. Disable interrupts.
5. Erase one sector.
6. Restore interrupts.

The erase function:

```c
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
```

### What Happens When You Run A Command

Command:

```text
FLASH_WRITE 0 "Hello World"
```

What happens:

1. `cli.c` parses `0` as the sector number.
2. The sector number becomes an offset: `sector_number * FLASH_SECTOR_SIZE`.
3. The CLI reads the old record from that sector.
4. If the old record is valid, `write_count` is incremented.
5. If the old record is invalid or erased, `write_count` starts at `1`.
6. The CLI builds a `flash_record_t` in RAM.
7. `flash_write_safe` adds `FLASH_TARGET_OFFSET`.
8. The full flash sector is buffered in RAM.
9. The new structured record is patched into the sector buffer.
10. Interrupts are disabled.
11. The sector is erased.
12. The sector is programmed back page by page.
13. The CLI prints a success message.

For sector `0`, the logical offset is `0 * FLASH_SECTOR_SIZE`.
The physical flash offset used by the flash layer is `FLASH_TARGET_OFFSET + 0`.
The XIP read address is `XIP_BASE + flash_offset`.

The sector-to-offset logic is in `cli.c`:

```c
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
```

Short output examples from the repo:

```text
FLASH_WRITE OK: sector=0 write_count=1 data_len=11
Metadata: write_count=1 data_len=11
Payload: Hello World
```

## 6) Task 2: Structured Block And Metadata

Task 2 is implemented in this repo.

Each sector stores one `flash_record_t`.
No pointer is stored in flash.
The payload is an inline fixed-size array.
The struct is designed to occupy exactly one flash sector.
That is enforced with `_Static_assert`.

The record uses four `uint32_t` metadata fields.
The rest of the sector is used for payload bytes.
`FLASH_RECORD_PAYLOAD_SIZE` is calculated from `FLASH_SECTOR_SIZE`.

The struct is defined in `cli.c`:

```c
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
```

| Field | Technical role |
|---|---|
| `magic` | fixed marker used to recognise this repo's record format |
| `version` | record format version; currently `1` |
| `write_count` | per-sector counter incremented on each valid rewrite |
| `data_len` | number of valid bytes in `payload` |
| `payload` | inline byte array stored directly in flash |

Empty or uninitialised sectors are detected with `magic` and `version`.
An erased flash sector will not contain the expected `magic` and `version`.
The code also rejects records with a `data_len` larger than the fixed payload array.

The write path starts from an erased-style RAM image by filling the record with `0xFF`.
Then it writes the metadata fields and copies the payload text.
This mirrors flash's erased state and leaves unused payload bytes as `0xFF`.

Record validation:

```c
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
```

Write path:

```c
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
```

Read path:

```c
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
```

The payload print is bounded by `data_len`.
That prevents reading past the valid stored text.
Printable ASCII bytes are written as characters.
Other bytes are printed as hex escape sequences.

## 7) Error Handling And Edge Cases

Most error paths print a message and return.
The flash functions do not return a status code.
The CLI helpers stop the command before calling the lower flash layer when the sector argument is invalid.

| Case | Repo behaviour |
|---|---|
| out-of-range sector | prints `Error: sector number out of range (<sector>). Valid range is 0..<last_sector>` |
| out-of-bounds offset | prints `Error: <op> out-of-bounds (offset=<offset> len=<len>, valid bytes=<bytes>)` |
| offset overflow | prints `Error: <op> offset overflow (offset=<offset>)` |
| payload too large | prints `Error: oversized payload (<len> bytes). Maximum for one sector is <max> bytes` |
| erased or uninitialised sector | prints `Error: sector <n> is empty or uninitialized` |
| zero-length write | prints `Error: write length must be greater than zero` |
| zero-length read | returns without printing |
| null write pointer | prints `Error: write data pointer is NULL` |
| null read buffer | prints `Error: read buffer pointer is NULL` |
| invalid write sector | prints `Error: invalid flash sector number for FLASH_WRITE` |
| invalid read sector | prints `Error: invalid flash sector number for FLASH_READ` |
| invalid erase sector | prints `Error: invalid flash sector number for FLASH_ERASE` |
| missing write args | prints `FLASH_WRITE requires a flash_sector_number and data` |
| missing read args | prints `FLASH_READ requires a flash_sector_number` |
| missing erase args | prints `FLASH_ERASE requires a flash_sector_number` |
| unknown command | prints `Unknown command` |

Important details:

- erased flash is treated as invalid structured data
- invalid `magic` or `version` means "empty or uninitialized"
- oversized payloads are rejected before the sector is erased
- invalid sector numbers are rejected before flash access
- zero-length reads are accepted as a no-op
- zero-length writes are rejected

## 8) Testing

There is no automated test helper in this repo.
There is no separate test CLI command in this repo.
Testing is done over the serial CLI.

Build and flash first.
Then open the serial console.

These tests exercise both the CLI layer and the flash layer.
They are repeatable because each run starts by erasing sector `0`.

Copy/paste these commands:

```text
FLASH_ERASE 0
FLASH_READ 0
FLASH_WRITE 0 "Hello World"
FLASH_READ 0
FLASH_WRITE 0 "Second write"
FLASH_READ 0
FLASH_WRITE 999999 "X"
FLASH_READ 999999
```

Expected behaviour:

| Test | Expected result |
|---|---|
| `FLASH_ERASE 0` | prints `FLASH_ERASE OK: sector=0` |
| `FLASH_READ 0` after erase | prints `Error: sector 0 is empty or uninitialized` |
| first write | prints `write_count=1` and `data_len=11` |
| first read | prints metadata and `Payload: Hello World` |
| second write same sector | prints `write_count=2` |
| second read | prints metadata and `Payload: Second write` |
| huge sector write/read | prints `Error: sector number out of range (999999). Valid range is 0..<last_sector>` |

## 9) Demo Video

The demo recording is included in this repo:

- [worksheet1-part1-demo.mov](demo/worksheet1-part1-demo.mov)

What the demo is showing, in repo terms:

1. The Pico is flashed with `build/cap_template.uf2`.
   At that point the `cap_template` firmware becomes the program that runs after reset.

2. The board exposes the CLI over USB serial.
   This comes from `pico_enable_stdio_usb(cap_template 1)` and the command loop in `main.c`.

3. A sector number typed by the user is converted into a byte offset.
   The CLI does this with `sector_number * FLASH_SECTOR_SIZE`.

4. The flash layer adds `FLASH_TARGET_OFFSET`.
   This moves the operation away from the application image and into the reserved user-data region.

5. If the demo shows `FLASH_ERASE 0`, one flash sector is erased.
   That clears the structured record stored in sector `0`.
   After that, `FLASH_READ 0` reports the sector as empty or uninitialized because the `magic` and `version` fields are no longer valid.

6. If the demo shows `FLASH_WRITE 0 "Hello World"`, the CLI builds a `flash_record_t` in RAM.
   It fills in `magic`, `version`, `write_count`, and `data_len`, then copies `"Hello World"` into `payload`.

7. The low-level write does not program only those 11 bytes.
   It reads the whole sector into RAM, patches the new record into that RAM buffer, disables interrupts, erases the sector, and writes the sector back page by page.

8. If the demo then shows `FLASH_READ 0`, the code reads one whole sector into `cli_read_record`.
   It validates the record using `magic`, `version`, and `data_len`.
   Then it prints `write_count`, `data_len`, and the payload bytes.

9. If the demo shows a second write to the same sector, `write_count` increases.
   That happens because the code first reads the existing record and only starts back at `1` when the old record is invalid.

10. If the demo shows an invalid sector number, the command is rejected in the CLI layer before flash access happens.
    That is why the error appears immediately.

The important technical point is that the demo is not writing plain text directly into flash.
It is writing a full sector-sized structured record, and the visible CLI output is the user-facing result of that lower-level flash logic.

## 10) Key Files

| File | Contents |
|---|---|
| `CMakeLists.txt` | target name, source list, USB stdio setup, and extra output generation |
| `main.c` | USB stdio setup and the command loop that calls `execute_command` |
| `cli.c` | command parsing, sector mapping, record metadata, and CLI output |
| `cli.h` | CLI function declaration |
| `flash_ops.c` | low-level flash bounds checks, XIP reads, sector erase, and page programming |
| `flash_ops.h` | flash function declarations and `FLASH_TARGET_OFFSET` |
| `custom_fgets.c` | simple serial input helper |
| `custom_fgets.h` | input helper declaration |
| `pico_sdk_import.cmake` | Pico SDK import logic |
