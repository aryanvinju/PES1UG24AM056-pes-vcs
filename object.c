// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/sha.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define fsync _commit
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#define mkdir(path, mode) _mkdir(path)
#endif

static int ensure_object_dirs(void) {
    if (mkdir(PES_DIR, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(OBJECTS_DIR, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    SHA256((const unsigned char *)data, len, id_out->hash);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str = NULL;
    switch (type) {
        case OBJ_BLOB: type_str = "blob"; break;
        case OBJ_TREE: type_str = "tree"; break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (header_len < 0 || (size_t)header_len + 1 > sizeof(header)) return -1;

    size_t full_len = (size_t)header_len + 1 + len;
    uint8_t *full = malloc(full_len > 0 ? full_len : 1);
    if (!full) return -1;

    memcpy(full, header, (size_t)header_len);
    full[header_len] = '\0';
    if (len > 0 && data) memcpy(full + header_len + 1, data, len);

    ObjectID id;
    compute_hash(full, full_len, &id);
    if (id_out) *id_out = id;

    if (object_exists(&id)) {
        free(full);
        return 0;
    }

    char final_path[512];
    char dir_path[512];
    char tmp_path[544];
    char hex[HASH_HEX_SIZE + 1];
    object_path(&id, final_path, sizeof(final_path));
    hash_to_hex(&id, hex);
    snprintf(dir_path, sizeof(dir_path), "%s/%.2s", OBJECTS_DIR, hex);
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp-%s", dir_path, hex + 2);

    if (ensure_object_dirs() != 0) {
        free(full);
        return -1;
    }

    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        free(full);
        return -1;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        free(full);
        return -1;
    }

    if (full_len > 0 && fwrite(full, 1, full_len, f) != full_len) {
        fclose(f);
        unlink(tmp_path);
        free(full);
        return -1;
    }

    if (fflush(f) != 0) {
        fclose(f);
        unlink(tmp_path);
        free(full);
        return -1;
    }

    if (fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp_path);
        free(full);
        return -1;
    }

    if (fclose(f) != 0) {
        unlink(tmp_path);
        free(full);
        return -1;
    }

    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        free(full);
        return -1;
    }

    free(full);
    return 0;
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }

    long file_size = ftell(f);
    if (file_size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    uint8_t *full = malloc((size_t)file_size > 0 ? (size_t)file_size : 1);
    if (!full) {
        fclose(f);
        return -1;
    }

    if ((size_t)file_size > 0 && fread(full, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(full);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID actual;
    compute_hash(full, (size_t)file_size, &actual);
    if (memcmp(actual.hash, id->hash, HASH_SIZE) != 0) {
        free(full);
        return -1;
    }

    uint8_t *null_byte = memchr(full, '\0', (size_t)file_size);
    if (!null_byte) {
        free(full);
        return -1;
    }

    size_t header_len = (size_t)(null_byte - full);
    char header[64];
    if (header_len >= sizeof(header)) {
        free(full);
        return -1;
    }
    memcpy(header, full, header_len);
    header[header_len] = '\0';

    char type_str[16];
    size_t declared_len;
    if (sscanf(header, "%15s %zu", type_str, &declared_len) != 2) {
        free(full);
        return -1;
    }

    ObjectType type;
    if (strcmp(type_str, "blob") == 0) type = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) type = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) type = OBJ_COMMIT;
    else {
        free(full);
        return -1;
    }

    size_t actual_len = (size_t)file_size - header_len - 1;
    if (declared_len != actual_len) {
        free(full);
        return -1;
    }

    uint8_t *data = malloc(actual_len + 1);
    if (!data) {
        free(full);
        return -1;
    }

    if (actual_len > 0) memcpy(data, null_byte + 1, actual_len);
    data[actual_len] = '\0';

    *type_out = type;
    *data_out = data;
    *len_out = actual_len;
    free(full);
    return 0;
}
