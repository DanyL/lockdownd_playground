#include "utils.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libgen.h>
#include <limits.h>
#include "log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uuid/uuid.h>

#if __APPLE__
#include <mach-o/dyld.h>
#elif __linux__
#include <unistd.h>
#endif

#pragma mark - misc

char *
get_uuid()
{
    uuid_t uuid;
    uuid_string_t uuid_string;

    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_string);

    return strdup((char *)uuid_string);
}

char *
get_executable_path()
{
    uint32_t size = PATH_MAX;
    char path[size + 1];
    memset(path, 0, size + 1);

#if __APPLE__
    if (0 > _NSGetExecutablePath(path, &size))
#elif __linux__
    if (0 > readlink("/proc/self/exe", path, size))
#endif
    {
        log_error("Failed to get executable path (%s)", strerror(errno));
        return NULL;
    }

    return strdup(path);
}

#pragma mark - fs

int
fs_visit(char *path, enum FS_ENTRY_TYPE visit_type, int (^block)(FTSENT *, enum FS_ENTRY_TYPE))
{
    int ret = EXIT_FAILURE;
    char *_path[2] = {path, NULL};
    FTS *fs = NULL;
    FTSENT *parent = NULL;
    FTSENT *child = NULL;

    if (NULL == (fs = fts_open(_path, FTS_COMFOLLOW | FTS_NOCHDIR, NULL)))
    {
        log_error("fts_open() failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    while (NULL != (parent = fts_read(fs)))
    {
        child = fts_children(fs, 0);
        if (0 != errno)
        {
            log_error("fts_children() failed: %s", strerror(errno));
            goto cleanup;
        }

        if (NULL == child)
            continue;

        do
        {
            if (child->fts_statp->st_mode & S_IFDIR)
            {
                if (visit_type & FS_ENTRY_TYPE_DIR && block(child, FS_ENTRY_TYPE_DIR))
                    goto cleanup;
            }
            else if (visit_type & FS_ENTRY_TYPE_FILE && block(child, FS_ENTRY_TYPE_FILE))
                goto cleanup;

            child = child->fts_link;
        } while (NULL != child);
    }

    ret = EXIT_SUCCESS;

cleanup:
    fts_close(fs);
    return ret;
}

int
mkdir_p(const char *path)
{
    char _path[PATH_MAX];

    if (NULL == strncpy(_path, path, PATH_MAX))
        return EXIT_FAILURE;

    for (char *p = strchr(&_path[1], '/');; p = strchr(++p, '/'))
    {
        if (p) *p = '\0';

        log_debug("mkdir(%s)", _path);
        if (0 != mkdir(_path, S_IRWXU | S_IRWXG | S_IRWXO) && errno != EEXIST)
        {
            log_error("mkdir() failed (%s)", strerror(errno));
            return EXIT_FAILURE;
        }

        if (p) *p = '/';
        else break;
    }

    return EXIT_SUCCESS;
}


#pragma mark - curl stuff

typedef struct
{
    char *memory;
    size_t size;
}
memory_struct_t;

static size_t
curl_write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    memory_struct_t *mem = (memory_struct_t *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        log_error("not enough memory (%s)", strerror(errno));
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

__attribute__((no_instrument_function)) static int
curl_progress_func(void *userp, double dltotal, double dlnow, double ultotal, double ulnow)
{
    int *percentage = (int *)userp;
    if (0.0 >= dltotal)
        return 0;

    // For some reason the callback get called multiple times for compleation
    if (100 == *percentage)
        return 0;

    *percentage = dlnow / dltotal * 100;
    log_progress(*percentage, NULL, 0);

    return 0;
}

void
download_remote_data(const char *url, char **headers, char **buffer, size_t *len)
{
    CURL *curl_handle = NULL;
    struct curl_slist *header_list = NULL;
    memory_struct_t data;
    int percentage = 0;

    log_debug("url = %s", url);

    if (NULL != headers)
    {
        log_debug("headers:");
        do
        {
            log_debug("%s", *headers);
            header_list = curl_slist_append(header_list, *headers);
            headers++;
        } while (NULL != *headers);
    }

    data.memory = malloc(1);
    data.size = 0;

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, !isatty(fileno(stdout)));
    curl_easy_setopt(curl_handle, CURLOPT_PROGRESSFUNCTION, curl_progress_func);
    curl_easy_setopt(curl_handle, CURLOPT_PROGRESSDATA, &percentage);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_write_memory_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &data);

    if (CURLE_OK == curl_easy_perform(curl_handle))
    {
        *buffer = data.memory;
        *len = data.size;

        log_debug("Download done; buffer: %p len: %lu", *buffer, *len);
    }

    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();
}


#pragma mark - zip

zip_t *
zip_open_from_memory(char *buffer, size_t len)
{
    zip_source_t *src = NULL;
    zip_t *za = NULL;
    zip_error_t error;

    log_debug("buffer: %p len: %lu", buffer, len);

    if (NULL == buffer || !len)
        return NULL;

    zip_error_init(&error);
    if ((src = zip_source_buffer_create(buffer, len, 0, &error)) == NULL) {
        log_error("can't create source (%s)", zip_error_strerror(&error));
        free(buffer);
        zip_error_fini(&error);
        return NULL;
    }

    if ((za = zip_open_from_source(src, 0, &error)) == NULL) {
        log_error("can't open zip from source (%s)", zip_error_strerror(&error));
        zip_source_free(src);
        zip_error_fini(&error);
        return NULL;
    }

    zip_error_fini(&error);

    return za;
}

int
zip_extract_entry(zip_t *za, const char *target, const char *dst)
{
    struct zip_stat sb;
    struct zip_file *zf = NULL;
    zip_error_t error;
    char tmp[PATH_MAX];
    size_t target_len = 0, len = 0, sum = 0;
    int fd = 0;

    if (NULL == za || NULL == target || NULL == dst)
        return EXIT_FAILURE;

    target_len = strlen(target);
    zip_error_init(&error);

    for (int i = 0; i < zip_get_num_entries(za, 0); i++)
    {
        if (0 != zip_stat_index(za, i, 0, &sb))
            continue;

        len = strlen(sb.name);
        if (sb.name != strnstr(sb.name, target, len))
        {
            log_debug("Skipping \"%s\"", sb.name);
            continue;
        }

        if (0 >= snprintf(tmp, PATH_MAX, "%s/%s", dst, &sb.name[target_len]))
        {
            log_error("Failed to construct destination path (%s)", strerror(errno));
            zip_error_fini(&error);
            return EXIT_FAILURE;
        }

        if (sb.name[len - 1] == '/')
        {
            log_debug("mkdir_p(%s)", tmp);
            if (EXIT_SUCCESS != mkdir_p(tmp))
            {
                log_debug("mkdir() failed");
                zip_error_fini(&error);
                return EXIT_FAILURE;
            }
            continue;
        }

        log_debug("Openning %s", tmp);
        if (0 >= (fd = open(tmp, O_WRONLY | O_TRUNC | O_CREAT, 0666)))
        {
            log_debug("Open failed, trying to mkdir(%s)", dirname(tmp));
            if (EXIT_SUCCESS != mkdir_p(dirname(tmp)))
            {
                log_debug("mkdir() failed");
                zip_error_fini(&error);
                return EXIT_FAILURE;
            }

            log_debug("mkdir() success, reopenning %s", tmp);
            if (0 >= (fd = open(tmp, O_WRONLY | O_TRUNC | O_CREAT)))
            {
                log_error("Failed to open \"%s\" (%s)", tmp, strerror(errno));
                zip_error_fini(&error);
                return EXIT_FAILURE;
            }
        }

        log_debug("Openning zip entry (name: %s, size: %lu)", sb.name, sb.size);
        if (NULL == (zf = zip_fopen_index(za, i, 0)))
        {
            log_error("Can't open zip entry (%s)", zip_error_strerror(&error));
            close(fd);
            zip_error_fini(&error);
            return EXIT_FAILURE;
        }

        sum = 0;
        while (sum != sb.size)
        {
            if (0 > (len = zip_fread(zf, tmp, PATH_MAX)))
            {
                log_error("Can't read zip entry (%s)", zip_error_strerror(&error));
                close(fd);
                zip_fclose(zf);
                zip_error_fini(&error);
                return EXIT_FAILURE;
            }

            sum += write(fd, tmp, len);
        }
        log_debug("Written %d bytes", sum);

        close(fd);
        zip_fclose(zf);
        continue;
    }

    zip_error_fini(&error);
    log_debug("Done");
    return EXIT_SUCCESS;
}

int
zip_extract_entry_from_memory(char *buffer, size_t len, const char *target, const char *dst)
{
    int ret = EXIT_FAILURE;
    zip_t *za = zip_open_from_memory(buffer, len);
    if (NULL == za)
    {
        log_error("Failed to open archive");
        return ret;
    }

    ret = zip_extract_entry(za, target, dst);

    zip_close(za);
    return ret;
}
