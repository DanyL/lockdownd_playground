#pragma once

#include <fts.h>
#include <stdio.h>
#include <zip.h>

enum FS_ENTRY_TYPE {
    FS_ENTRY_TYPE_FILE = (1 << 0),
    FS_ENTRY_TYPE_DIR  = (1 << 1),
    FS_ENTRY_TYPE_ALL  = FS_ENTRY_TYPE_FILE | FS_ENTRY_TYPE_DIR
};

char *
get_uuid(void);

char *
get_executable_path(void);

int
fs_visit(char *path, enum FS_ENTRY_TYPE visit_type, int (^block)(FTSENT *, enum FS_ENTRY_TYPE));

int
mkdir_p(const char *path);

void
download_remote_data(const char *url, char **headers, char **buffer, size_t *len);

zip_t *
zip_open_from_memory(char *buffer, size_t len);

int
zip_extract_entry(zip_t *za, const char *target, const char *dst);

int
zip_extract_entry_from_memory(char *buffer, size_t len, const char *target, const char *dst);
