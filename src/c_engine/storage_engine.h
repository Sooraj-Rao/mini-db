#ifndef MINIDB_STORAGE_ENGINE_H
#define MINIDB_STORAGE_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdb_store mdb_store;

typedef struct mdb_record {
    uint8_t operation; /* 1 = SET, 2 = DELETE */
    const char *key;
    size_t key_length;
    const char *value;
    size_t value_length;
} mdb_record;

typedef int (*mdb_record_callback)(uint8_t operation,
                                   const char *key, size_t key_length,
                                   const char *value, size_t value_length,
                                   void *user_data);

/* Returns 0 on success. Error text is written to error_buffer when supplied. */
int mdb_store_open(mdb_store **out_store, const char *database_path,
                   char *error_buffer, size_t error_buffer_size);
void mdb_store_close(mdb_store *store);
int mdb_store_append_batch(mdb_store *store, const mdb_record *records,
                           size_t count, char *error_buffer, size_t error_buffer_size);
int mdb_store_scan(mdb_store *store, mdb_record_callback callback, void *user_data,
                   char *error_buffer, size_t error_buffer_size);
int mdb_store_compact(mdb_store *store, const mdb_record *records, size_t count,
                      char *error_buffer, size_t error_buffer_size);
int mdb_store_file_size(mdb_store *store, uint64_t *size_out);
int mdb_store_wal_size(mdb_store *store, uint64_t *size_out);

#ifdef __cplusplus
}
#endif
#endif
