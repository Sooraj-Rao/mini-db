#include "storage_engine.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int seen_records = 0;
static int callback(uint8_t operation, const char *key, size_t key_length,
                    const char *value, size_t value_length, void *user_data) {
    (void)user_data;
    if (operation == 1 && key_length == 5 && memcmp(key, "hello", 5) == 0 &&
        value_length == 5 && memcmp(value, "world", 5) == 0) {
        ++seen_records;
        return 1;
    }
    return 0;
}

int main(void) {
    const char *path = "minidb_c_api_test.db";
    char error[256] = {0};
    mdb_store *store = NULL;
    mdb_record record;
    FILE *cleanup;

    cleanup = fopen(path, "wb");
    if (!cleanup) return 1;
    fclose(cleanup);
    cleanup = fopen("minidb_c_api_test.db.wal", "wb");
    if (!cleanup) return 1;
    fclose(cleanup);

    if (!mdb_store_open(&store, path, error, sizeof(error))) {
        fprintf(stderr, "C API open failed: %s\n", error);
        return 1;
    }
    record.operation = 1;
    record.key = "hello";
    record.key_length = 5;
    record.value = "world";
    record.value_length = 5;
    if (!mdb_store_append_batch(store, &record, 1, error, sizeof(error))) {
        fprintf(stderr, "C API append failed: %s\n", error);
        mdb_store_close(store);
        return 1;
    }
    if (!mdb_store_scan(store, callback, NULL, error, sizeof(error)) || seen_records != 1) {
        fprintf(stderr, "C API scan failed: %s\n", error);
        mdb_store_close(store);
        return 1;
    }
    mdb_store_close(store);
    remove(path);
    remove("minidb_c_api_test.db.wal");
    puts("C storage engine API test passed.");
    return 0;
}
