// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define fsync _commit
#define lstat stat
#define mkdir(path, mode) _mkdir(path)
#endif

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
uint32_t get_file_mode(const char *path);

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

static int ensure_index_parent_dir(void) {
    if (mkdir(PES_DIR, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int path_is_tracked(const Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return 1;
    }
    return 0;
}

static int should_skip_untracked_name(const char *name) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 1;
    if (strcmp(name, ".pes") == 0) return 1;
    if (strcmp(name, "pes") == 0 || strcmp(name, "pes.exe") == 0) return 1;
    if (strcmp(name, "test_objects") == 0 || strcmp(name, "test_objects.exe") == 0) return 1;
    if (strcmp(name, "test_tree") == 0 || strcmp(name, "test_tree.exe") == 0) return 1;
    return strstr(name, ".o") != NULL;
}

static int print_untracked_recursive(const Index *index, const char *dir_path, const char *prefix) {
    DIR *dir = opendir(dir_path);
    if (!dir)
        return 0;

    int untracked_count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (should_skip_untracked_name(ent->d_name))
            continue;

        char rel_path[1024];
        if (prefix[0] == '\0')
            snprintf(rel_path, sizeof(rel_path), "%s", ent->d_name);
        else
            snprintf(rel_path, sizeof(rel_path), "%s/%s", prefix, ent->d_name);

        char full_path[1024];
        if (strcmp(dir_path, ".") == 0)
            snprintf(full_path, sizeof(full_path), "%s", ent->d_name);
        else
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (lstat(full_path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            untracked_count += print_untracked_recursive(index, full_path, rel_path);
            continue;
        }

        if (!S_ISREG(st.st_mode))
            continue;

        if (!path_is_tracked(index, rel_path)) {
            printf("  untracked:  %s\n", rel_path);
            untracked_count++;
        }
    }

    closedir(dir);
    return untracked_count;
}

// Windows rename semantics differ from POSIX, so use a helper that
// preserves the "replace existing file" behavior expected by the lab.
static int replace_path(const char *src, const char *dst) {
#ifdef _WIN32
    return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(src, dst);
#endif
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = print_untracked_recursive(index, ".", "");
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f)
        return errno == ENOENT ? 0 : -1;

    char hash_hex[HASH_HEX_SIZE + 1];
    IndexEntry entry;
    while (fscanf(f, "%o %64s %" SCNu64 " %u %511[^\n]\n",
                  &entry.mode, hash_hex, &entry.mtime_sec, &entry.size, entry.path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES || hex_to_hash(hash_hex, &entry.hash) != 0) {
            fclose(f);
            return -1;
        }
        index->entries[index->count++] = entry;
    }

    if (!feof(f)) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    Index *sorted = malloc(sizeof(Index));
    if (!sorted)
        return -1;
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_index_entries);

    if (ensure_index_parent_dir() != 0)
        goto fail;

    const char *tmp_path = INDEX_FILE ".tmp";
    FILE *f = fopen(tmp_path, "wb");
    if (!f) goto fail;

    for (int i = 0; i < sorted->count; i++) {
        char hash_hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted->entries[i].hash, hash_hex);
        if (fprintf(f, "%o %s %" PRIu64 " %u %s\n",
                    sorted->entries[i].mode, hash_hex, sorted->entries[i].mtime_sec,
                    sorted->entries[i].size, sorted->entries[i].path) < 0) {
            fclose(f);
            unlink(tmp_path);
            goto fail;
        }
    }

    fflush(f);
    if (fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp_path);
        goto fail;
    }

    if (fclose(f) != 0) {
        unlink(tmp_path);
        goto fail;
    }

    if (replace_path(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        goto fail;
    }

    int dir_fd = open(PES_DIR, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    free(sorted);
    return 0;

fail:
    free(sorted);
    return -1;
}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t *data = malloc((size_t)st.st_size ? (size_t)st.st_size : 1);
    if (!data) {
        fclose(f);
        return -1;
    }

    if (st.st_size > 0 && fread(data, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID blob_id;
    if (object_write(OBJ_BLOB, data, (size_t)st.st_size, &blob_id) != 0) {
        free(data);
        return -1;
    }
    free(data);

    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
    }

    entry->mode = get_file_mode(path);
    entry->hash = blob_id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;
    if (snprintf(entry->path, sizeof(entry->path), "%s", path) >= (int)sizeof(entry->path))
        return -1;

    return index_save(index);
}
