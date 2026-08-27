#include "fat16.h"
#include "ata.h"
#include "serial.h"
#include "mm/heap.h"
#include "errno.h"

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

// Per-volume geometry. Was eight file-scope globals; becoming a struct
// is what lets a second volume exist at all. FAT32 fields are unused
// until the variant work lands, but live here from the start so that
// change touches only the code that reads them.
struct fat_volume {
    uint8_t  drive;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t sectors_per_fat;
    uint32_t root_dir_start_lba;    // FAT16 only
    uint32_t root_dir_sector_count; // FAT16 only
    uint16_t root_entry_count;      // FAT16 only
    uint32_t root_cluster;          // FAT32 only
};

// The one volume the legacy fat16_* API operates on. It disappears
// when that API does; until then it keeps this refactor invisible to
// callers.
static struct fat_volume legacy_volume;

static uint32_t cluster_to_lba(struct fat_volume *v, uint16_t cluster) {
    return v->data_start_lba + (uint32_t)(cluster - 2) * v->sectors_per_cluster;
}

int fat16_mount(void) {
    struct fat_volume *v = &legacy_volume;
    v->drive = 0;
    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(v->drive, 0, 1, sector)) {
        serial_write_string("[fat16] mount FAILED: could not read boot sector\n");
        return 0;
    }

    struct fat16_bpb *bpb = (struct fat16_bpb *)sector;
    v->bytes_per_sector = bpb->bytes_per_sector;
    v->sectors_per_cluster = bpb->sectors_per_cluster;
    v->root_entry_count = bpb->root_entry_count;
    v->sectors_per_fat = bpb->sectors_per_fat;

    v->fat_start_lba = bpb->reserved_sector_count;
    v->root_dir_start_lba = v->fat_start_lba + (uint32_t)bpb->num_fats * bpb->sectors_per_fat;
    v->root_dir_sector_count = ((uint32_t)v->root_entry_count * sizeof(struct fat16_dirent) + v->bytes_per_sector - 1) / v->bytes_per_sector;
    v->data_start_lba = v->root_dir_start_lba + v->root_dir_sector_count;

    serial_write_string("[fat16] mounted: bytes_per_sector=");
    serial_write_hex64(v->bytes_per_sector);
    serial_write_string(" sectors_per_cluster=");
    serial_write_hex64(v->sectors_per_cluster);
    serial_write_string(" root_dir_lba=");
    serial_write_hex64(v->root_dir_start_lba);
    serial_write_string(" data_start_lba=");
    serial_write_hex64(v->data_start_lba);
    serial_write_string("\n");
    return 1;
}

static uint16_t fat16_next_cluster(struct fat_volume *v, uint16_t cluster) {
    uint32_t fat_offset = (uint32_t)cluster * 2;
    uint32_t fat_sector = v->fat_start_lba + fat_offset / v->bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % v->bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    ata_read_sectors(v->drive, fat_sector, 1, sector);
    uint16_t *entries = (uint16_t *)sector;
    return entries[offset_in_sector / 2];
}

static void fat16_set_next_cluster(struct fat_volume *v, uint16_t cluster, uint16_t value) {
    uint32_t fat_offset = (uint32_t)cluster * 2;
    uint32_t fat_sector = v->fat_start_lba + fat_offset / v->bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % v->bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    ata_read_sectors(v->drive, fat_sector, 1, sector);
    uint16_t *entries = (uint16_t *)sector;
    entries[offset_in_sector / 2] = value;
    ata_write_sectors(v->drive, fat_sector, 1, sector);
}

// Scans the FAT linearly from cluster 2 for a free (0x0000) entry,
// marks it as a fresh chain's end (0xFFFF), and returns it. Returns 0
// if the disk is full.
static uint16_t fat16_alloc_cluster(struct fat_volume *v) {
    uint32_t total_entries = ((uint32_t)v->sectors_per_fat * v->bytes_per_sector) / 2;
    for (uint16_t cluster = 2; cluster < total_entries; cluster++) {
        if (fat16_next_cluster(v, cluster) == 0x0000) {
            fat16_set_next_cluster(v, cluster, 0xFFFF);
            return cluster;
        }
    }
    return 0;
}

// Walks a cluster chain from first_cluster, zeroing every FAT entry.
static void fat16_free_chain(struct fat_volume *v, uint16_t first_cluster) {
    uint16_t cluster = first_cluster;
    while (cluster >= 2 && cluster < FAT16_EOC_MIN) {
        uint16_t next = fat16_next_cluster(v, cluster);
        fat16_set_next_cluster(v, cluster, 0x0000);
        cluster = next;
    }
}

// Returns the cluster containing byte offset `byte_offset` within the
// chain starting at `first_cluster`, by walking from the start every
// call. O(chain length) per call -- acceptable at this project's file
// sizes; a future milestone could cache the last-accessed cluster per
// fd if it ever matters. Caller must ensure the chain is long enough.
static uint16_t cluster_at_offset(struct fat_volume *v, uint16_t first_cluster, uint32_t byte_offset) {
    uint32_t cluster_size_bytes = (uint32_t)v->sectors_per_cluster * v->bytes_per_sector;
    uint32_t cluster_index = byte_offset / cluster_size_bytes;
    uint16_t cluster = first_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        cluster = fat16_next_cluster(v, cluster);
    }
    return cluster;
}

// Writes `len` bytes into the cluster chain starting at chain_start,
// beginning at byte offset write_position. If zero_fill is set, zero
// bytes are written instead of reading from src (used for gap-filling
// past old EOF). Does read-modify-write for any partial sector.
// Assumes the chain already has enough clusters to cover
// write_position+len -- callers must extend it first.
static void write_range(struct fat_volume *v, uint16_t chain_start, uint32_t write_position, const void *src, uint32_t len, int zero_fill) {
    const uint8_t *in = (const uint8_t *)src;
    uint32_t cluster_size_bytes = (uint32_t)v->sectors_per_cluster * v->bytes_per_sector;
    uint32_t written = 0;

    while (written < len) {
        uint32_t abs_offset = write_position + written;
        uint32_t offset_in_cluster = abs_offset % cluster_size_bytes;
        uint32_t sector_index = offset_in_cluster / v->bytes_per_sector;
        uint32_t offset_in_sector = offset_in_cluster % v->bytes_per_sector;

        uint16_t cluster = cluster_at_offset(v, chain_start, abs_offset);
        uint32_t lba = cluster_to_lba(v, cluster) + sector_index;

        uint32_t to_write = v->bytes_per_sector - offset_in_sector;
        if (to_write > len - written) {
            to_write = len - written;
        }

        uint8_t sector[SECTOR_SIZE];
        if (offset_in_sector != 0 || to_write != v->bytes_per_sector) {
            ata_read_sectors(v->drive, lba, 1, sector); // partial sector: preserve untouched bytes
        }
        for (uint32_t i = 0; i < to_write; i++) {
            sector[offset_in_sector + i] = zero_fill ? 0 : in[written + i];
        }
        ata_write_sectors(v->drive, lba, 1, sector);

        written += to_write;
    }
}

uint32_t fat16_read_file(uint16_t first_cluster, uint32_t size, void *buffer) {
    struct fat_volume *v = &legacy_volume;
    uint8_t *out = (uint8_t *)buffer;
    uint32_t bytes_read = 0;
    uint16_t cluster = first_cluster;
    uint8_t sector_buf[SECTOR_SIZE];

    while (cluster < FAT16_EOC_MIN && bytes_read < size) {
        uint32_t lba = cluster_to_lba(v, cluster);
        for (uint8_t s = 0; s < v->sectors_per_cluster && bytes_read < size; s++) {
            ata_read_sectors(v->drive, lba + s, 1, sector_buf);
            uint32_t to_copy = size - bytes_read;
            if (to_copy > v->bytes_per_sector) {
                to_copy = v->bytes_per_sector;
            }
            for (uint32_t i = 0; i < to_copy; i++) {
                out[bytes_read + i] = sector_buf[i];
            }
            bytes_read += to_copy;
        }
        cluster = fat16_next_cluster(v, cluster);
    }
    return bytes_read;
}

void fat16_read_at(uint16_t first_cluster, uint32_t position, void *buf, uint32_t len) {
    struct fat_volume *v = &legacy_volume;
    uint8_t *out = (uint8_t *)buf;
    uint32_t cluster_size_bytes = (uint32_t)v->sectors_per_cluster * v->bytes_per_sector;
    uint32_t read_so_far = 0;

    while (read_so_far < len) {
        uint32_t abs_offset = position + read_so_far;
        uint32_t offset_in_cluster = abs_offset % cluster_size_bytes;
        uint32_t sector_index = offset_in_cluster / v->bytes_per_sector;
        uint32_t offset_in_sector = offset_in_cluster % v->bytes_per_sector;

        uint16_t cluster = cluster_at_offset(v, first_cluster, abs_offset);
        uint32_t lba = cluster_to_lba(v, cluster) + sector_index;

        uint32_t to_read = v->bytes_per_sector - offset_in_sector;
        if (to_read > len - read_so_far) {
            to_read = len - read_so_far;
        }

        uint8_t sector[SECTOR_SIZE];
        ata_read_sectors(v->drive, lba, 1, sector);
        for (uint32_t i = 0; i < to_read; i++) {
            out[read_so_far + i] = sector[offset_in_sector + i];
        }

        read_so_far += to_read;
    }
}

int fat16_write_file(uint16_t first_cluster, uint32_t current_size, uint32_t position,
                      const void *buf, uint32_t len,
                      uint16_t *out_first_cluster, uint32_t *out_new_size) {
    struct fat_volume *v = &legacy_volume;
    if (len == 0) {
        *out_first_cluster = first_cluster;
        *out_new_size = current_size;
        return 0;
    }

    uint32_t cluster_size_bytes = (uint32_t)v->sectors_per_cluster * v->bytes_per_sector;
    uint32_t end_position = position + len;
    uint32_t clusters_needed = (end_position + cluster_size_bytes - 1) / cluster_size_bytes;

    uint16_t chain_start = first_cluster;
    uint16_t last_cluster = 0;
    uint32_t existing_clusters = 0;

    if (chain_start != 0) {
        uint16_t c = chain_start;
        existing_clusters = 1;
        while (fat16_next_cluster(v, c) < FAT16_EOC_MIN) {
            c = fat16_next_cluster(v, c);
            existing_clusters++;
        }
        last_cluster = c;
    }

    while (existing_clusters < clusters_needed) {
        uint16_t new_cluster = fat16_alloc_cluster(v);
        if (new_cluster == 0) {
            return -ENOSPC;
        }
        if (chain_start == 0) {
            chain_start = new_cluster;
        } else {
            fat16_set_next_cluster(v, last_cluster, new_cluster);
        }
        last_cluster = new_cluster;
        existing_clusters++;
    }

    if (position > current_size) {
        write_range(v, chain_start, current_size, NULL, position - current_size, 1);
    }
    write_range(v, chain_start, position, buf, len, 0);

    *out_first_cluster = chain_start;
    *out_new_size = end_position > current_size ? end_position : current_size;
    return (int)len;
}

void fat16_truncate(uint16_t first_cluster, uint32_t dir_lba, uint16_t dir_offset, uint16_t *out_first_cluster) {
    struct fat_volume *v = &legacy_volume;
    if (first_cluster != 0) {
        fat16_free_chain(v, first_cluster);
    }
    *out_first_cluster = 0;
    fat16_update_entry_size(dir_lba, dir_offset, 0, 0);
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
// Returns 1 (found, *out filled, *out_lba/*out_offset set to the
// entry's on-disk location, both nullable), 0 (not found in this
// sector, keep scanning), or -1 (hit the end-of-directory marker).
static int scan_sector_for_name(const uint8_t *sector, uint32_t sector_lba, const uint8_t *target_name,
                                  struct fat16_dirent *out, uint32_t *out_lba, uint16_t *out_offset) {
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
            if (out_lba) {
                *out_lba = sector_lba;
            }
            if (out_offset) {
                *out_offset = (uint16_t)(e * sizeof(struct fat16_dirent));
            }
            return 1;
        }
    }
    return 0;
}

static int find_in_root(struct fat_volume *v, const uint8_t *target_name, struct fat16_dirent *out,
                          uint32_t *out_lba, uint16_t *out_offset) {
    uint8_t sector[SECTOR_SIZE];
    for (uint32_t s = 0; s < v->root_dir_sector_count; s++) {
        uint32_t lba = v->root_dir_start_lba + s;
        ata_read_sectors(v->drive, lba, 1, sector);
        int result = scan_sector_for_name(sector, lba, target_name, out, out_lba, out_offset);
        if (result != 0) {
            return result > 0;
        }
    }
    return 0;
}

static int find_in_directory_cluster(struct fat_volume *v, uint16_t dir_cluster, const uint8_t *target_name, struct fat16_dirent *out,
                                       uint32_t *out_lba, uint16_t *out_offset) {
    uint8_t sector[SECTOR_SIZE];
    uint16_t cluster = dir_cluster;
    while (cluster < FAT16_EOC_MIN) {
        uint32_t lba = cluster_to_lba(v, cluster);
        for (uint8_t s = 0; s < v->sectors_per_cluster; s++) {
            ata_read_sectors(v->drive, lba + s, 1, sector);
            int result = scan_sector_for_name(sector, lba + s, target_name, out, out_lba, out_offset);
            if (result != 0) {
                return result > 0;
            }
        }
        cluster = fat16_next_cluster(v, cluster);
    }
    return 0;
}

static void write_dirent(struct fat16_dirent *entry, const uint8_t *fat_name, uint8_t attr,
                           uint16_t first_cluster, uint32_t size) {
    for (int i = 0; i < 11; i++) {
        entry->name[i] = fat_name[i];
    }
    entry->attr = attr;
    entry->nt_reserved = 0;
    entry->create_time_tenth = 0;
    entry->create_time = 0;
    entry->create_date = 0;
    entry->access_date = 0;
    entry->first_cluster_high = 0;
    entry->write_time = 0;
    entry->write_date = 0;
    entry->first_cluster_low = first_cluster;
    entry->file_size = size;
}

// Finds a free slot (0x00 never-used or 0xE5 deleted) in the given
// directory (in_root selects the fixed-size root directory over
// dir_cluster) and writes a new entry there. For a non-root directory
// that's completely full, allocates and links one more cluster before
// retrying. Returns 1 on success (fills *out_lba/*out_offset with the
// new entry's location), or -ENOSPC (root full, or disk full when a
// non-root directory needs to grow).
static int create_entry_in_directory(struct fat_volume *v, uint16_t dir_cluster, int in_root, const uint8_t *fat_name,
                                       uint8_t attr, uint16_t first_cluster, uint32_t size,
                                       uint32_t *out_lba, uint16_t *out_offset) {
    uint8_t sector[SECTOR_SIZE];

    if (in_root) {
        for (uint32_t s = 0; s < v->root_dir_sector_count; s++) {
            uint32_t lba = v->root_dir_start_lba + s;
            ata_read_sectors(v->drive, lba, 1, sector);
            struct fat16_dirent *entries = (struct fat16_dirent *)sector;
            for (uint32_t e = 0; e < DIRENTS_PER_SECTOR; e++) {
                if (entries[e].name[0] == 0x00 || entries[e].name[0] == 0xE5) {
                    write_dirent(&entries[e], fat_name, attr, first_cluster, size);
                    ata_write_sectors(v->drive, lba, 1, sector);
                    if (out_lba) {
                        *out_lba = lba;
                    }
                    if (out_offset) {
                        *out_offset = (uint16_t)(e * sizeof(struct fat16_dirent));
                    }
                    return 1;
                }
            }
        }
        return -ENOSPC;
    }

    uint16_t cluster = dir_cluster;
    uint16_t last_cluster = dir_cluster;
    while (cluster < FAT16_EOC_MIN) {
        uint32_t lba = cluster_to_lba(v, cluster);
        for (uint8_t s = 0; s < v->sectors_per_cluster; s++) {
            ata_read_sectors(v->drive, lba + s, 1, sector);
            struct fat16_dirent *entries = (struct fat16_dirent *)sector;
            for (uint32_t e = 0; e < DIRENTS_PER_SECTOR; e++) {
                if (entries[e].name[0] == 0x00 || entries[e].name[0] == 0xE5) {
                    write_dirent(&entries[e], fat_name, attr, first_cluster, size);
                    ata_write_sectors(v->drive, lba + s, 1, sector);
                    if (out_lba) {
                        *out_lba = lba + s;
                    }
                    if (out_offset) {
                        *out_offset = (uint16_t)(e * sizeof(struct fat16_dirent));
                    }
                    return 1;
                }
            }
        }
        last_cluster = cluster;
        cluster = fat16_next_cluster(v, cluster);
    }

    uint16_t new_cluster = fat16_alloc_cluster(v);
    if (new_cluster == 0) {
        return -ENOSPC;
    }
    fat16_set_next_cluster(v, last_cluster, new_cluster);

    uint8_t zero_sector[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        zero_sector[i] = 0;
    }
    uint32_t new_lba = cluster_to_lba(v, new_cluster);
    for (uint8_t s = 0; s < v->sectors_per_cluster; s++) {
        ata_write_sectors(v->drive, new_lba + s, 1, zero_sector);
    }

    struct fat16_dirent *entries = (struct fat16_dirent *)zero_sector;
    write_dirent(&entries[0], fat_name, attr, first_cluster, size);
    ata_write_sectors(v->drive, new_lba, 1, zero_sector);
    if (out_lba) {
        *out_lba = new_lba;
    }
    if (out_offset) {
        *out_offset = 0;
    }
    return 1;
}

// Resolves the parent directory of `path` (its last '/'-separated
// component is the new name being created; everything before it must
// already exist and be a directory). On success (1), fills
// *out_in_root/*out_dir_cluster/*out_fat_name (11 bytes). On failure,
// returns -ENOENT (a parent component doesn't exist) or -ENOTDIR (a
// parent component exists but isn't a directory).
static int resolve_parent(struct fat_volume *v, const char *path, int *out_in_root, uint16_t *out_dir_cluster, uint8_t *out_fat_name) {
    if (path[0] == '/') {
        path++;
    }
    if (*path == '\0') {
        return -ENOENT;
    }

    int in_root = 1;
    uint16_t current_dir_cluster = 0;

    for (;;) {
        char component[13];
        int i = 0;
        while (path[i] != '\0' && path[i] != '/' && i < 12) {
            component[i] = path[i];
            i++;
        }
        component[i] = '\0';

        int is_last = (path[i] != '/');
        if (is_last) {
            to_fat_name(component, out_fat_name);
            *out_in_root = in_root;
            *out_dir_cluster = current_dir_cluster;
            return 1;
        }

        uint8_t fat_name[11];
        to_fat_name(component, fat_name);
        struct fat16_dirent entry;
        int found = in_root ? find_in_root(v, fat_name, &entry, NULL, NULL)
                             : find_in_directory_cluster(v, current_dir_cluster, fat_name, &entry, NULL, NULL);
        if (!found) {
            return -ENOENT;
        }
        if (!(entry.attr & FAT_ATTR_DIRECTORY)) {
            return -ENOTDIR;
        }
        current_dir_cluster = entry.first_cluster_low;
        in_root = 0;

        path += i + 1; // skip the '/'
    }
}

int fat16_create_file(const char *path, uint32_t *out_dir_lba, uint16_t *out_dir_offset) {
    struct fat_volume *v = &legacy_volume;
    int in_root;
    uint16_t dir_cluster;
    uint8_t fat_name[11];
    int result = resolve_parent(v, path, &in_root, &dir_cluster, fat_name);
    if (result < 0) {
        return result;
    }

    struct fat16_dirent existing;
    int already_exists = in_root ? find_in_root(v, fat_name, &existing, NULL, NULL)
                                   : find_in_directory_cluster(v, dir_cluster, fat_name, &existing, NULL, NULL);
    if (already_exists) {
        return -EEXIST;
    }

    int created = create_entry_in_directory(v, dir_cluster, in_root, fat_name, 0, 0, 0, out_dir_lba, out_dir_offset);
    return created > 0 ? 0 : created;
}

int fat16_mkdir(const char *path) {
    struct fat_volume *v = &legacy_volume;
    int in_root;
    uint16_t dir_cluster;
    uint8_t fat_name[11];
    int result = resolve_parent(v, path, &in_root, &dir_cluster, fat_name);
    if (result < 0) {
        return result;
    }

    struct fat16_dirent existing;
    int already_exists = in_root ? find_in_root(v, fat_name, &existing, NULL, NULL)
                                   : find_in_directory_cluster(v, dir_cluster, fat_name, &existing, NULL, NULL);
    if (already_exists) {
        return -EEXIST;
    }

    uint16_t new_cluster = fat16_alloc_cluster(v);
    if (new_cluster == 0) {
        return -ENOSPC;
    }

    uint8_t zero_sector[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        zero_sector[i] = 0;
    }
    uint32_t lba = cluster_to_lba(v, new_cluster);
    for (uint8_t s = 0; s < v->sectors_per_cluster; s++) {
        ata_write_sectors(v->drive, lba + s, 1, zero_sector);
    }

    uint32_t dir_lba;
    uint16_t dir_offset;
    int created = create_entry_in_directory(v, dir_cluster, in_root, fat_name, FAT_ATTR_DIRECTORY, new_cluster, 0, &dir_lba, &dir_offset);
    if (created <= 0) {
        fat16_free_chain(v, new_cluster);
        return created;
    }
    return 0;
}

int fat16_delete_entry(const char *path) {
    struct fat_volume *v = &legacy_volume;
    uint16_t cluster;
    uint32_t size;
    uint32_t dir_lba;
    uint16_t dir_offset;
    if (!fat16_find(path, &cluster, &size, &dir_lba, &dir_offset)) {
        return -ENOENT;
    }

    uint8_t sector[SECTOR_SIZE];
    ata_read_sectors(v->drive, dir_lba, 1, sector);
    struct fat16_dirent *entry = (struct fat16_dirent *)(sector + dir_offset);
    if (entry->attr & FAT_ATTR_DIRECTORY) {
        return -EISDIR;
    }

    if (cluster != 0) {
        fat16_free_chain(v, cluster);
    }
    entry->name[0] = 0xE5;
    ata_write_sectors(v->drive, dir_lba, 1, sector);
    return 0;
}

void fat16_update_entry_size(uint32_t dir_lba, uint16_t dir_offset, uint16_t first_cluster, uint32_t size) {
    struct fat_volume *v = &legacy_volume;
    uint8_t sector[SECTOR_SIZE];
    ata_read_sectors(v->drive, dir_lba, 1, sector);
    struct fat16_dirent *entry = (struct fat16_dirent *)(sector + dir_offset);
    entry->first_cluster_low = first_cluster;
    entry->file_size = size;
    ata_write_sectors(v->drive, dir_lba, 1, sector);
}

int fat16_find(const char *path, uint16_t *out_cluster, uint32_t *out_size,
               uint32_t *out_dir_lba, uint16_t *out_dir_offset) {
    struct fat_volume *v = &legacy_volume;
    if (path[0] == '/') {
        path++;
    }
    if (*path == '\0') {
        return 0; // empty path (or just "/") is not a valid file lookup
    }

    struct fat16_dirent entry;
    int in_root = 1;
    uint16_t current_dir_cluster = 0;
    uint32_t dir_lba = 0;
    uint16_t dir_offset = 0;

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

        int found = in_root ? find_in_root(v, fat_name, &entry, &dir_lba, &dir_offset)
                             : find_in_directory_cluster(v, current_dir_cluster, fat_name, &entry, &dir_lba, &dir_offset);
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
    if (out_dir_lba) {
        *out_dir_lba = dir_lba;
    }
    if (out_dir_offset) {
        *out_dir_offset = dir_offset;
    }
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

    if (!fat16_find("/HELLO.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] selftest FAILED: /HELLO.TXT not found\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size ||
        !buffer_equals_string(buffer, size, "Hello from NeoOS FAT16!\n")) {
        serial_write_string("[fat16] selftest FAILED: /HELLO.TXT contents mismatch\n");
        return;
    }

    if (!fat16_find("/BIGFILE.TXT", &cluster, &size, NULL, NULL) || size != 8192) {
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

    if (!fat16_find("/DIR/NESTED.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] selftest FAILED: /DIR/NESTED.TXT not found\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size ||
        !buffer_equals_string(buffer, size, "nested file contents\n")) {
        serial_write_string("[fat16] selftest FAILED: /DIR/NESTED.TXT contents mismatch\n");
        return;
    }

    if (fat16_find("/DIR/MISSING.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] selftest FAILED: /DIR/MISSING.TXT should not be found\n");
        return;
    }

    kfree(buffer);
    serial_write_string("[fat16] selftest passed\n");
}

void fat16_write_selftest(void) {
    struct fat_volume *v = &legacy_volume;
    uint16_t cluster = fat16_alloc_cluster(v);
    if (cluster == 0) {
        serial_write_string("[fat16] write selftest FAILED: alloc_cluster returned 0\n");
        return;
    }

    uint8_t write_buf[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }
    uint32_t lba = cluster_to_lba(v, cluster);
    if (!ata_write_sectors(v->drive, lba, 1, write_buf)) {
        serial_write_string("[fat16] write selftest FAILED: ata_write_sectors failed\n");
        return;
    }

    uint8_t read_buf[SECTOR_SIZE];
    if (!ata_read_sectors(v->drive, lba, 1, read_buf)) {
        serial_write_string("[fat16] write selftest FAILED: ata_read_sectors failed\n");
        return;
    }
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        if (read_buf[i] != write_buf[i]) {
            serial_write_string("[fat16] write selftest FAILED: byte mismatch at offset ");
            serial_write_hex64(i);
            serial_write_string("\n");
            return;
        }
    }

    if (fat16_next_cluster(v, cluster) < FAT16_EOC_MIN) {
        serial_write_string("[fat16] write selftest FAILED: newly allocated cluster not marked EOC\n");
        return;
    }

    fat16_free_chain(v, cluster);
    if (fat16_next_cluster(v, cluster) != 0x0000) {
        serial_write_string("[fat16] write selftest FAILED: freed cluster not zeroed in FAT\n");
        return;
    }

    uint32_t size;
    if (fat16_find("/NEWFILE.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] write selftest FAILED: /NEWFILE.TXT already exists before creation\n");
        return;
    }
    if (fat16_create_file("/NEWFILE.TXT", NULL, NULL) != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_create_file(/NEWFILE.TXT) failed\n");
        return;
    }
    if (!fat16_find("/NEWFILE.TXT", &cluster, &size, NULL, NULL) || cluster != 0 || size != 0) {
        serial_write_string("[fat16] write selftest FAILED: /NEWFILE.TXT not found or not empty after creation\n");
        return;
    }
    if (fat16_create_file("/NEWFILE.TXT", NULL, NULL) != -EEXIST) {
        serial_write_string("[fat16] write selftest FAILED: creating /NEWFILE.TXT again did not return -EEXIST\n");
        return;
    }
    if (fat16_delete_entry("/NEWFILE.TXT") != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_delete_entry(/NEWFILE.TXT) failed\n");
        return;
    }
    if (fat16_find("/NEWFILE.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] write selftest FAILED: /NEWFILE.TXT still found after deletion\n");
        return;
    }
    if (fat16_delete_entry("/NEWFILE.TXT") != -ENOENT) {
        serial_write_string("[fat16] write selftest FAILED: deleting /NEWFILE.TXT again did not return -ENOENT\n");
        return;
    }

    if (fat16_mkdir("/NEWDIR") != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_mkdir(/NEWDIR) failed\n");
        return;
    }
    if (fat16_create_file("/NEWDIR/INNER.TXT", NULL, NULL) != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_create_file(/NEWDIR/INNER.TXT) failed\n");
        return;
    }
    if (!fat16_find("/NEWDIR/INNER.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] write selftest FAILED: /NEWDIR/INNER.TXT not found after creation\n");
        return;
    }

    uint32_t dir_lba;
    uint16_t dir_offset;
    if (fat16_create_file("/WDATA.TXT", &dir_lba, &dir_offset) != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_create_file(/WDATA.TXT) failed\n");
        return;
    }
    const char *phrase = "Hello, written file!"; // NOTE: 20 bytes, see the next line
    uint32_t phrase_len = 20;
    uint16_t new_cluster;
    uint32_t new_size;
    int written = fat16_write_file(0, 0, 0, phrase, phrase_len, &new_cluster, &new_size);
    if (written != (int)phrase_len || new_size != phrase_len) {
        serial_write_string("[fat16] write selftest FAILED: fat16_write_file(/WDATA.TXT) wrote wrong length\n");
        return;
    }
    fat16_update_entry_size(dir_lba, dir_offset, new_cluster, new_size);

    uint8_t readback[64];
    fat16_read_at(new_cluster, 0, readback, phrase_len);
    for (uint32_t i = 0; i < phrase_len; i++) {
        if (readback[i] != (uint8_t)phrase[i]) {
            serial_write_string("[fat16] write selftest FAILED: /WDATA.TXT readback mismatch\n");
            return;
        }
    }

    // Random-access overwrite mid-file: "written" (position 7) -> "WRITTEN".
    const char *patch = "WRITTEN";
    written = fat16_write_file(new_cluster, new_size, 7, patch, 7, &new_cluster, &new_size);
    if (written != 7) {
        serial_write_string("[fat16] write selftest FAILED: mid-file overwrite failed\n");
        return;
    }
    fat16_update_entry_size(dir_lba, dir_offset, new_cluster, new_size);
    fat16_read_at(new_cluster, 0, readback, new_size);
    if (!buffer_equals_string(readback, new_size, "Hello, WRITTEN file!")) {
        serial_write_string("[fat16] write selftest FAILED: mid-file overwrite readback mismatch\n");
        return;
    }

    // Write past current EOF (position 25, current size 20): the
    // 5-byte gap [20,25) must be zero-filled.
    written = fat16_write_file(new_cluster, new_size, new_size + 5, "END", 3, &new_cluster, &new_size);
    if (written != 3) {
        serial_write_string("[fat16] write selftest FAILED: past-EOF write failed\n");
        return;
    }
    fat16_update_entry_size(dir_lba, dir_offset, new_cluster, new_size);
    fat16_read_at(new_cluster, 0, readback, new_size);
    for (uint32_t i = 20; i < 25; i++) {
        if (readback[i] != 0) {
            serial_write_string("[fat16] write selftest FAILED: gap not zero-filled\n");
            return;
        }
    }
    if (readback[25] != 'E' || readback[26] != 'N' || readback[27] != 'D') {
        serial_write_string("[fat16] write selftest FAILED: past-EOF write data mismatch\n");
        return;
    }

    fat16_delete_entry("/WDATA.TXT");

    serial_write_string("[fat16] write selftest passed\n");
}
