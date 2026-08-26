#ifndef NEOOS_ATA_H
#define NEOOS_ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

struct ata_identify_info {
    uint32_t sector_count;
};

// Issues IDENTIFY DEVICE to the primary channel's master drive. Returns
// 1 on success (info->sector_count filled in), 0 on failure (logged).
int ata_identify(struct ata_identify_info *info);

// Reads `count` (1-255) sectors starting at `lba` into `buffer`, which
// must be at least count * ATA_SECTOR_SIZE bytes. Returns 1 on success,
// 0 on failure (logged to serial).
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);

// Writes `count` (1-255) sectors of `buffer` (must be at least count *
// ATA_SECTOR_SIZE bytes) to disk starting at `lba`, flushing the
// drive's cache before returning so the write is durable. Returns 1
// on success, 0 on failure (logged to serial).
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);

#endif
