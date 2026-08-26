#include "fat16.h"
#include "ata.h"
#include "serial.h"
#include "mm/heap.h"

#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_LONG_NAME 0x0F
#define FAT16_EOC_MIN      0xFFF8

#define SECTOR_SIZE 512

struct fat16_bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} __attribute__((packed));

struct fat16_dirent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_high; // unused in FAT16
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed));

#define DIRENTS_PER_SECTOR (SECTOR_SIZE / sizeof(struct fat16_dirent))

static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint32_t fat_start_lba;
static uint32_t root_dir_start_lba;
static uint32_t root_dir_sector_count;
static uint32_t data_start_lba;
static uint16_t root_entry_count;

static uint32_t cluster_to_lba(uint16_t cluster) {
    return data_start_lba + (uint32_t)(cluster - 2) * sectors_per_cluster;
}

int fat16_mount(void) {
    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(0, 1, sector)) {
        serial_write_string("[fat16] mount FAILED: could not read boot sector\n");
        return 0;
    }

    struct fat16_bpb *bpb = (struct fat16_bpb *)sector;
    bytes_per_sector = bpb->bytes_per_sector;
    sectors_per_cluster = bpb->sectors_per_cluster;
    root_entry_count = bpb->root_entry_count;

    fat_start_lba = bpb->reserved_sector_count;
    root_dir_start_lba = fat_start_lba + (uint32_t)bpb->num_fats * bpb->sectors_per_fat;
    root_dir_sector_count = ((uint32_t)root_entry_count * sizeof(struct fat16_dirent) + bytes_per_sector - 1) / bytes_per_sector;
    data_start_lba = root_dir_start_lba + root_dir_sector_count;

    serial_write_string("[fat16] mounted: bytes_per_sector=");
    serial_write_hex64(bytes_per_sector);
    serial_write_string(" sectors_per_cluster=");
    serial_write_hex64(sectors_per_cluster);
    serial_write_string(" root_dir_lba=");
    serial_write_hex64(root_dir_start_lba);
    serial_write_string(" data_start_lba=");
    serial_write_hex64(data_start_lba);
    serial_write_string("\n");
    return 1;
}

static uint16_t fat16_next_cluster(uint16_t cluster) {
    uint32_t fat_offset = (uint32_t)cluster * 2;
    uint32_t fat_sector = fat_start_lba + fat_offset / bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    ata_read_sectors(fat_sector, 1, sector);
    uint16_t *entries = (uint16_t *)sector;
    return entries[offset_in_sector / 2];
}

uint32_t fat16_read_file(uint16_t first_cluster, uint32_t size, void *buffer) {
    uint8_t *out = (uint8_t *)buffer;
    uint32_t bytes_read = 0;
    uint16_t cluster = first_cluster;
    uint8_t sector_buf[SECTOR_SIZE];

    while (cluster < FAT16_EOC_MIN && bytes_read < size) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < sectors_per_cluster && bytes_read < size; s++) {
            ata_read_sectors(lba + s, 1, sector_buf);
            uint32_t to_copy = size - bytes_read;
            if (to_copy > bytes_per_sector) {
                to_copy = bytes_per_sector;
            }
            for (uint32_t i = 0; i < to_copy; i++) {
                out[bytes_read + i] = sector_buf[i];
            }
            bytes_read += to_copy;
        }
        cluster = fat16_next_cluster(cluster);
    }
    return bytes_read;
}

static void to_fat_name(const char *name, uint8_t *out11) {
    for (int i = 0; i < 11; i++) {
        out11[i] = ' ';
    }
    int out_i = 0;
    int i = 0;
    while (name[i] != '\0' && name[i] != '.' && out_i < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out11[out_i] = (uint8_t)c;
        out_i++;
        i++;
    }
    while (name[i] != '\0' && name[i] != '.') {
        i++; // skip name characters beyond 8 -- 8.3 only, per this milestone's scope
    }
    if (name[i] == '.') {
        i++;
        int ext_i = 8;
        while (name[i] != '\0' && ext_i < 11) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            out11[ext_i] = (uint8_t)c;
            ext_i++;
            i++;
        }
    }
}

static int fat_name_matches(const uint8_t *entry_name, const uint8_t *target_name) {
    for (int i = 0; i < 11; i++) {
        if (entry_name[i] != target_name[i]) {
            return 0;
        }
    }
    return 1;
}

// Scans one sector's worth of directory entries for target_name.
// Returns 1 (found, *out filled), 0 (not found in this sector, keep
// scanning), or -1 (hit the end-of-directory marker, stop entirely).
static int scan_sector_for_name(const uint8_t *sector, const uint8_t *target_name, struct fat16_dirent *out) {
    const struct fat16_dirent *entries = (const struct fat16_dirent *)sector;
    for (uint32_t e = 0; e < DIRENTS_PER_SECTOR; e++) {
        if (entries[e].name[0] == 0x00) {
            return -1;
        }
        if (entries[e].name[0] == 0xE5) {
            continue; // deleted entry
        }
        if ((entries[e].attr & FAT_ATTR_LONG_NAME) == FAT_ATTR_LONG_NAME) {
            continue; // long-filename entry -- not supported, 8.3 only
        }
        if (entries[e].attr & FAT_ATTR_VOLUME_ID) {
            continue;
        }
        if (fat_name_matches(entries[e].name, target_name)) {
            *out = entries[e];
            return 1;
        }
    }
    return 0;
}

static int find_in_root(const uint8_t *target_name, struct fat16_dirent *out) {
    uint8_t sector[SECTOR_SIZE];
    for (uint32_t s = 0; s < root_dir_sector_count; s++) {
        ata_read_sectors(root_dir_start_lba + s, 1, sector);
        int result = scan_sector_for_name(sector, target_name, out);
        if (result != 0) {
            return result > 0;
        }
    }
    return 0;
}

static int find_in_directory_cluster(uint16_t dir_cluster, const uint8_t *target_name, struct fat16_dirent *out) {
    uint8_t sector[SECTOR_SIZE];
    uint16_t cluster = dir_cluster;
    while (cluster < FAT16_EOC_MIN) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < sectors_per_cluster; s++) {
            ata_read_sectors(lba + s, 1, sector);
            int result = scan_sector_for_name(sector, target_name, out);
            if (result != 0) {
                return result > 0;
            }
        }
        cluster = fat16_next_cluster(cluster);
    }
    return 0;
}

int fat16_find(const char *path, uint16_t *out_cluster, uint32_t *out_size) {
    if (path[0] == '/') {
        path++;
    }
    if (*path == '\0') {
        return 0; // empty path (or just "/") is not a valid file lookup
    }

    struct fat16_dirent entry;
    int in_root = 1;
    uint16_t current_dir_cluster = 0;

    while (*path != '\0') {
        char component[13]; // 8 + '.' + 3 + NUL -- 8.3 only
        int i = 0;
        while (path[i] != '\0' && path[i] != '/' && i < 12) {
            component[i] = path[i];
            i++;
        }
        component[i] = '\0';

        uint8_t fat_name[11];
        to_fat_name(component, fat_name);

        int found = in_root ? find_in_root(fat_name, &entry)
                             : find_in_directory_cluster(current_dir_cluster, fat_name, &entry);
        if (!found) {
            return 0;
        }

        path += i;
        if (*path == '/') {
            path++;
            if (!(entry.attr & FAT_ATTR_DIRECTORY)) {
                return 0; // tried to descend into a non-directory
            }
            current_dir_cluster = entry.first_cluster_low;
            in_root = 0;
        }
    }

    *out_cluster = entry.first_cluster_low;
    *out_size = entry.file_size;
    return 1;
}

static int buffer_equals_string(const uint8_t *buffer, uint32_t len, const char *expected) {
    for (uint32_t i = 0; i < len; i++) {
        if ((char)buffer[i] != expected[i]) {
            return 0;
        }
    }
    return expected[len] == '\0';
}

void fat16_selftest(void) {
    uint16_t cluster;
    uint32_t size;
    uint8_t *buffer = (uint8_t *)kmalloc(8192);
    if (!buffer) {
        serial_write_string("[fat16] selftest FAILED: kmalloc returned NULL\n");
        return;
    }

    if (!fat16_find("/HELLO.TXT", &cluster, &size)) {
        serial_write_string("[fat16] selftest FAILED: /HELLO.TXT not found\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size ||
        !buffer_equals_string(buffer, size, "Hello from NeoOS FAT16!\n")) {
        serial_write_string("[fat16] selftest FAILED: /HELLO.TXT contents mismatch\n");
        return;
    }

    if (!fat16_find("/BIGFILE.TXT", &cluster, &size) || size != 8192) {
        serial_write_string("[fat16] selftest FAILED: /BIGFILE.TXT not found or wrong size\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size) {
        serial_write_string("[fat16] selftest FAILED: /BIGFILE.TXT short read\n");
        return;
    }
    for (uint32_t i = 0; i < size; i++) {
        if (buffer[i] != 'N') {
            serial_write_string("[fat16] selftest FAILED: /BIGFILE.TXT byte mismatch at offset ");
            serial_write_hex64(i);
            serial_write_string("\n");
            return;
        }
    }

    if (!fat16_find("/DIR/NESTED.TXT", &cluster, &size)) {
        serial_write_string("[fat16] selftest FAILED: /DIR/NESTED.TXT not found\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size ||
        !buffer_equals_string(buffer, size, "nested file contents\n")) {
        serial_write_string("[fat16] selftest FAILED: /DIR/NESTED.TXT contents mismatch\n");
        return;
    }

    if (fat16_find("/DIR/MISSING.TXT", &cluster, &size)) {
        serial_write_string("[fat16] selftest FAILED: /DIR/MISSING.TXT should not be found\n");
        return;
    }

    kfree(buffer);
    serial_write_string("[fat16] selftest passed\n");
}
