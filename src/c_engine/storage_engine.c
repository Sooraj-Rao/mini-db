#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "storage_engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#include <sys/types.h>
#endif

#define MDB_MAX_FIELD_SIZE (16u * 1024u * 1024u)
#define MDB_ERROR_SIZE 256

struct mdb_store {
    char *database_path;
    char *wal_path;
};

static void set_error(char *buffer, size_t size, const char *message) {
    if (buffer && size) {
        snprintf(buffer, size, "%s", message ? message : "Unknown storage error");
    }
}

static char *duplicate_string(const char *value) {
    size_t n;
    char *copy;
    if (!value) return NULL;
    n = strlen(value) + 1;
    copy = (char *)malloc(n);
    if (copy) memcpy(copy, value, n);
    return copy;
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char b[4];
    b[0] = (unsigned char)(value & 0xffu);
    b[1] = (unsigned char)((value >> 8) & 0xffu);
    b[2] = (unsigned char)((value >> 16) & 0xffu);
    b[3] = (unsigned char)((value >> 24) & 0xffu);
    return fwrite(b, 1, 4, file) == 4;
}

static int read_u32(FILE *file, uint32_t *value, int *incomplete) {
    unsigned char b[4];
    size_t n = fread(b, 1, 4, file);
    if (n != 4) {
        if (ferror(file)) return 0;
        *incomplete = 1;
        return 1;
    }
    *value = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
             ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}

static int write_record(FILE *file, const mdb_record *record) {
    if (fputc(record->operation, file) == EOF) return 0;
    if (!write_u32(file, (uint32_t)record->key_length) ||
        !write_u32(file, (uint32_t)record->value_length)) return 0;
    if (record->key_length && fwrite(record->key, 1, record->key_length, file) != record->key_length) return 0;
    if (record->value_length && fwrite(record->value, 1, record->value_length, file) != record->value_length) return 0;
    return 1;
}

/* Scans a log; incomplete trailing records are truncated. */
static int scan_file(const char *path, mdb_record_callback callback, void *user_data,
                     char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    long valid_end = 0;
    if (!file) { set_error(error, error_size, "Unable to open storage log"); return 0; }
    for (;;) {
        int operation = fgetc(file);
        uint32_t key_len = 0, value_len = 0;
        int incomplete = 0;
        char *key = NULL, *value = NULL;
        size_t got;
        if (operation == EOF) {
            if (ferror(file)) { fclose(file); set_error(error, error_size, "Error reading storage log"); return 0; }
            break;
        }
        if (operation != 1 && operation != 2) {
            fclose(file); set_error(error, error_size, "Storage corruption: unknown operation code"); return 0;
        }
        if (!read_u32(file, &key_len, &incomplete) || !read_u32(file, &value_len, &incomplete)) {
            fclose(file); set_error(error, error_size, "Error reading record header"); return 0;
        }
        if (incomplete) break;
        if (!key_len || key_len > MDB_MAX_FIELD_SIZE || value_len > MDB_MAX_FIELD_SIZE) {
            fclose(file); set_error(error, error_size, "Storage corruption: invalid record field size"); return 0;
        }
        key = (char *)malloc((size_t)key_len);
        value = value_len ? (char *)malloc((size_t)value_len) : NULL;
        if (!key || (value_len && !value)) {
            free(key); free(value); fclose(file); set_error(error, error_size, "Out of memory while reading record"); return 0;
        }
        got = fread(key, 1, key_len, file);
        if (got != key_len) incomplete = 1;
        if (!incomplete && value_len && fread(value, 1, value_len, file) != value_len) incomplete = 1;
        if (incomplete) { free(key); free(value); break; }
        if (callback && !callback((uint8_t)operation, key, key_len, value, value_len, user_data)) {
            free(key); free(value); fclose(file); set_error(error, error_size, "Record callback failed"); return 0;
        }
        free(key); free(value);
        valid_end = ftell(file);
        if (valid_end < 0) { fclose(file); set_error(error, error_size, "Unable to determine log position"); return 0; }
    }
    if (fclose(file) != 0) { set_error(error, error_size, "Unable to close storage log"); return 0; }
    {
        FILE *check = fopen(path, "rb");
        long end;
        if (!check) { set_error(error, error_size, "Unable to reopen storage log"); return 0; }
        if (fseek(check, 0, SEEK_END) != 0 || (end = ftell(check)) < 0) {
            fclose(check); set_error(error, error_size, "Unable to inspect storage log"); return 0;
        }
        fclose(check);
        if (end != valid_end) {
            FILE *truncate_file = fopen(path, "rb+");
            if (!truncate_file) { set_error(error, error_size, "Unable to truncate incomplete storage tail"); return 0; }
#if defined(_WIN32)
            if (_chsize_s(_fileno(truncate_file), (long long)valid_end) != 0) {
#else
            if (ftruncate(fileno(truncate_file), (off_t)valid_end) != 0) {
#endif
                fclose(truncate_file); set_error(error, error_size, "Failed to truncate incomplete storage tail"); return 0;
            }
            fclose(truncate_file);
        }
    }
    return 1;
}

static int append_records_path(const char *path, const mdb_record *records, size_t count,
                               int truncate_first, char *error, size_t error_size) {
    FILE *file = fopen(path, truncate_first ? "wb" : "ab");
    size_t i;
    if (!file) { set_error(error, error_size, "Unable to open log for writing"); return 0; }
    for (i = 0; i < count; ++i) {
        if (!write_record(file, &records[i])) {
            fclose(file); set_error(error, error_size, "Failed while writing storage record"); return 0;
        }
    }
    if (fflush(file) != 0) { fclose(file); set_error(error, error_size, "Failed to flush storage log"); return 0; }
    if (fclose(file) != 0) { set_error(error, error_size, "Failed to close storage log"); return 0; }
    return 1;
}

static int clear_wal(mdb_store *store, char *error, size_t error_size) {
    FILE *file = fopen(store->wal_path, "wb");
    if (!file) { set_error(error, error_size, "Unable to clear WAL"); return 0; }
    if (fflush(file) != 0 || fclose(file) != 0) { set_error(error, error_size, "Failed to clear WAL"); return 0; }
    return 1;
}

int mdb_store_open(mdb_store **out_store, const char *database_path,
                   char *error_buffer, size_t error_buffer_size) {
    mdb_store *store;
    FILE *file;
    size_t path_len;
    if (!out_store || !database_path || !database_path[0]) { set_error(error_buffer, error_buffer_size, "Invalid database path"); return 0; }
    *out_store = NULL;
    store = (mdb_store *)calloc(1, sizeof(*store));
    if (!store) { set_error(error_buffer, error_buffer_size, "Out of memory"); return 0; }
    store->database_path = duplicate_string(database_path);
    path_len = strlen(database_path) + 5;
    store->wal_path = (char *)malloc(path_len);
    if (!store->database_path || !store->wal_path) { mdb_store_close(store); set_error(error_buffer, error_buffer_size, "Out of memory"); return 0; }
    snprintf(store->wal_path, path_len, "%s.wal", database_path);
    file = fopen(store->database_path, "ab");
    if (!file) { mdb_store_close(store); set_error(error_buffer, error_buffer_size, "Unable to create database file"); return 0; }
    fclose(file);
    file = fopen(store->wal_path, "ab");
    if (!file) { mdb_store_close(store); set_error(error_buffer, error_buffer_size, "Unable to create WAL file"); return 0; }
    fclose(file);
    /* Recover complete WAL records by replaying them to the main log. */
    {
        FILE *wal = fopen(store->wal_path, "rb");
        long wal_size;
        if (!wal || fseek(wal, 0, SEEK_END) != 0 || (wal_size = ftell(wal)) < 0) {
            if (wal) fclose(wal);
            mdb_store_close(store);
            set_error(error_buffer, error_buffer_size, "Unable to inspect WAL");
            return 0;
        }
        fclose(wal);
        if (wal_size > 0) {
            /* First normalize the WAL tail, then append its valid records. */
            if (!scan_file(store->wal_path, NULL, NULL, error_buffer, error_buffer_size)) { mdb_store_close(store); return 0; }
            /* WAL is replayed byte-for-byte; records have already been validated. */
            wal = fopen(store->wal_path, "rb");
            if (!wal) { mdb_store_close(store); set_error(error_buffer, error_buffer_size, "Unable to read WAL for replay"); return 0; }
            {
                FILE *db = fopen(store->database_path, "ab");
                char buffer[8192]; size_t n;
                if (!db) { fclose(wal); mdb_store_close(store); set_error(error_buffer, error_buffer_size, "Unable to open database for WAL replay"); return 0; }
                while ((n = fread(buffer, 1, sizeof(buffer), wal)) > 0) {
                    if (fwrite(buffer, 1, n, db) != n) { fclose(wal); fclose(db); mdb_store_close(store); set_error(error_buffer, error_buffer_size, "WAL replay failed"); return 0; }
                }
                if (ferror(wal) || fflush(db) != 0) { fclose(wal); fclose(db); mdb_store_close(store); set_error(error_buffer, error_buffer_size, "WAL replay failed"); return 0; }
                fclose(wal); fclose(db);
            }
            if (!clear_wal(store, error_buffer, error_buffer_size)) { mdb_store_close(store); return 0; }
        }
    }
    if (!scan_file(store->database_path, NULL, NULL, error_buffer, error_buffer_size)) { mdb_store_close(store); return 0; }
    *out_store = store;
    return 1;
}

void mdb_store_close(mdb_store *store) {
    if (!store) return;
    free(store->database_path);
    free(store->wal_path);
    free(store);
}

int mdb_store_append_batch(mdb_store *store, const mdb_record *records,
                           size_t count, char *error_buffer, size_t error_buffer_size) {
    size_t i;
    if (!store || (!records && count)) { set_error(error_buffer, error_buffer_size, "Invalid append arguments"); return 0; }
    for (i = 0; i < count; ++i) {
        if ((records[i].operation != 1 && records[i].operation != 2) || !records[i].key ||
            records[i].key_length == 0 || records[i].key_length > MDB_MAX_FIELD_SIZE ||
            records[i].value_length > MDB_MAX_FIELD_SIZE || (records[i].value_length && !records[i].value)) {
            set_error(error_buffer, error_buffer_size, "Invalid record supplied to C storage engine"); return 0;
        }
    }
    if (!count) return 1;
    if (!append_records_path(store->wal_path, records, count, 0, error_buffer, error_buffer_size)) return 0;
    if (!append_records_path(store->database_path, records, count, 0, error_buffer, error_buffer_size)) {
        set_error(error_buffer, error_buffer_size, "Database append failed; pending WAL will be replayed on restart"); return 0;
    }
    return clear_wal(store, error_buffer, error_buffer_size);
}

int mdb_store_scan(mdb_store *store, mdb_record_callback callback, void *user_data,
                   char *error_buffer, size_t error_buffer_size) {
    if (!store || !callback) { set_error(error_buffer, error_buffer_size, "Invalid scan arguments"); return 0; }
    return scan_file(store->database_path, callback, user_data, error_buffer, error_buffer_size);
}

int mdb_store_compact(mdb_store *store, const mdb_record *records, size_t count,
                      char *error_buffer, size_t error_buffer_size) {
    size_t path_len;
    char *temp, *backup;
    size_t i;
    if (!store || (!records && count)) { set_error(error_buffer, error_buffer_size, "Invalid compaction arguments"); return 0; }
    path_len = strlen(store->database_path) + 16;
    temp = (char *)malloc(path_len); backup = (char *)malloc(path_len);
    if (!temp || !backup) { free(temp); free(backup); set_error(error_buffer, error_buffer_size, "Out of memory"); return 0; }
    snprintf(temp, path_len, "%s.compact.tmp", store->database_path);
    snprintf(backup, path_len, "%s.compact.bak", store->database_path);
    remove(temp);
    for (i = 0; i < count; ++i) {
        if (!records[i].key || !records[i].key_length || records[i].key_length > MDB_MAX_FIELD_SIZE || records[i].value_length > MDB_MAX_FIELD_SIZE) {
            free(temp); free(backup); set_error(error_buffer, error_buffer_size, "Invalid compaction record"); return 0;
        }
    }
    if (!append_records_path(temp, records, count, 1, error_buffer, error_buffer_size)) { free(temp); free(backup); return 0; }
    remove(backup);
    if (rename(store->database_path, backup) != 0) { remove(temp); free(temp); free(backup); set_error(error_buffer, error_buffer_size, "Unable to create compaction backup"); return 0; }
    if (rename(temp, store->database_path) != 0) {
        rename(backup, store->database_path); remove(temp); free(temp); free(backup); set_error(error_buffer, error_buffer_size, "Unable to install compacted database"); return 0;
    }
    remove(backup);
    free(temp); free(backup);
    return 1;
}

static int get_size(const char *path, uint64_t *size_out) {
    FILE *file = fopen(path, "rb");
    long size;
    if (!file || fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0) { if (file) fclose(file); return 0; }
    fclose(file); *size_out = (uint64_t)size; return 1;
}
int mdb_store_file_size(mdb_store *store, uint64_t *size_out) { return store && size_out && get_size(store->database_path, size_out); }
int mdb_store_wal_size(mdb_store *store, uint64_t *size_out) { return store && size_out && get_size(store->wal_path, size_out); }
