#include "patchy.h"
#include "../../common/common.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct
{
    uint64_t remote_fd; // Remote file descriptor to be patched AFTER installation
    int local_fd; // Local fd to patch
    size_t local_size;
}
patch_entry_t;

void
free_patch_entries(afc_client_t client, patch_entry_t ***patch_entries)
{
    if (NULL == *patch_entries)
        return;

    patch_entry_t **entry = *patch_entries;
    do
    {
        if (0 != entry[0]->local_fd)
            close(entry[0]->local_fd);

        if (0 != entry[0]->remote_fd)
            afc_file_close(client, entry[0]->remote_fd);

        free(entry[0]);
        entry[0] = NULL;

        entry++;
    } while (*entry != NULL);

    free(*entry);
    *patch_entries = NULL;
}

patch_entry_t **
prepare_container_patches(afc_client_t client, const char *install_path, const char *patch_path)
{
#define check_result(cond, error, ...) if (cond) { log_error(error, ##__VA_ARGS__); return EXIT_FAILURE; }

    int ret = EXIT_FAILURE;
    __block patch_entry_t **patch_entries = NULL;
    __block char *path = NULL;
    __block void *tmp = NULL;
    __block int i = 0;
    size_t patch_path_len = 0;

    if (NULL == (path = malloc(PATH_MAX * sizeof(char))))
    {
        log_error("malloc() failed: %s", strerror(errno));
        return NULL;
    }

    patch_path_len = strlen(patch_path);

    ret = fs_visit((char *)patch_path, FS_ENTRY_TYPE_FILE, ^int(FTSENT *entry, enum FS_ENTRY_TYPE entry_type) {
        /* Prepare patch_entries */
        check_result(
                     NULL == patch_entries && NULL == (patch_entries = calloc(i + 1, sizeof(patch_entry_t *))),
                     "calloc() failed: %s", strerror(errno)
                     );

        check_result(
                     i != 0 && NULL == (tmp = realloc(patch_entries, (i + 1) * sizeof(patch_entry_t *))),
                     "realloc() failed: %s", strerror(errno)
                     );

        if (NULL != tmp)
        {
            patch_entries = tmp;
            patch_entries[i + 1] = NULL;
            log_debug("patch_entries[%d]: %p", i + 1, patch_entries[i + 1]);
            tmp = NULL;
        }

        check_result(
                     NULL == (patch_entries[i] = calloc(1, sizeof(patch_entry_t))),
                     "calloc() failed: %s", strerror(errno)
                     );

        /* Initialize patch entry */
        patch_entries[i]->local_size = entry->fts_statp->st_size;

        snprintf(path, PATH_MAX, "%s/%s", entry->fts_path, entry->fts_name);

        check_result(
                     0 >= (patch_entries[i]->local_fd = open(path, O_RDONLY)),
                     "Failed to open local file %s: %s", path, strerror(errno)
                     );

        snprintf(path, PATH_MAX, "%s%s%s", install_path, &entry->fts_path[patch_path_len], entry->fts_name);
        log_debug("Openning remote file %s", path);
        check_result(
                     AFC_E_SUCCESS != afc_file_open(client, path, AFC_FOPEN_RW, &patch_entries[i]->remote_fd),
                     "Failed to open remote file: %s", path
                     );

        i++;
        return EXIT_SUCCESS;
    });

    if (EXIT_SUCCESS != ret)
        free_patch_entries(client, &patch_entries);

    free(path);
    return patch_entries;
#undef check_result
}

int
apply_patch_entries(afc_client_t client, patch_entry_t **patch_entries)
{
    patch_entry_t **entry = patch_entries;
    char *buffer = NULL;
    uint32_t written = 0;

    do
    {
        log_debug("Reading %lu bytes from local file", entry[0]->local_size);
        if (MAP_FAILED == (buffer = mmap(NULL, entry[0]->local_size, PROT_READ, MAP_PRIVATE, entry[0]->local_fd, 0)))
        {
            log_error("mmap() failed: %s", strerror(errno));
            return EXIT_FAILURE;
        }

        log_debug("Writing %lu bytes to remote file", entry[0]->local_size);
        if (AFC_E_SUCCESS != afc_file_write(client, entry[0]->remote_fd, buffer, (uint32_t)entry[0]->local_size, &written))
        {
            log_error("afc_file_write() failed");
            return EXIT_FAILURE;
        }

        if (written != entry[0]->local_size)
        {
            log_error("afc_file_write() failed: Mismatched size (%u/%lu)", written, entry[0]->local_size);
            return EXIT_FAILURE;
        }

        entry++;
    } while (*entry != NULL);

    return EXIT_SUCCESS;
}

int
install_and_patch(idevice_t device, const char *app_path, const char *patch_path)
{
    return get_afc_client(device, ^int(afc_client_t client) {
        patch_entry_t **patches = NULL;

        log_info("Uploading app contents");
        if (EXIT_SUCCESS != afc_upload_dir(client, app_path, "PublicStaging/Payload/app.app"))
        {
            log_error("Failed to upload app contents to device");
            return EXIT_FAILURE;
        }

        log_info("Preparing patch environment");
        if (NULL == (patches = prepare_container_patches(client, "PublicStaging/Payload/app.app", patch_path)))
        {
            log_error("Failed to prepare patch environment");
            return EXIT_FAILURE;
        }

        log_info("Installing app");
        if (EXIT_SUCCESS != install_bundles(device, "PublicStaging"))
        {
            log_error("Failed to install app, bailing");
            free_patch_entries(client, &patches);
            return EXIT_FAILURE;
        }

        log_info("Applying container patches");
        if (EXIT_SUCCESS != apply_patch_entries(client, patches))
        {
            log_error("Failed to apply container patches");
            free_patch_entries(client, &patches);
            return EXIT_FAILURE;
        }

        free_patch_entries(client, &patches);
        return EXIT_SUCCESS;
    });
}
