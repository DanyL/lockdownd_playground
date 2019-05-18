#include "device.h"
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <libimobiledevice/debugserver.h>
#include <libimobiledevice/diagnostics_relay.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/notification_proxy.h>
#include <libimobiledevice/house_arrest.h>
#include <limits.h>
#include "log.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "utils.h"

#include <usbmuxd.h>

#define LOCKDOWN_SERVICE_DIAGNOSTICS_RELAY      "com.apple.mobile.diagnostics_relay"
#define LOCKDOWN_SERVICE_DIAGNOSTICS_RELAY_OLD  "com.apple.iosdiagnostics.relay"
#define LOCKDOWN_SERVICE_AFC                    "com.apple.afc"
#define LOCKDOWN_SERVICE_HOUSE_ARREST           "com.apple.mobile.house_arrest"
#define LOCKDOWN_SERVICE_NOTIFICATION_PROXY     "com.apple.mobile.notification_proxy"

#define APPLiCATION_INSTALLED_NOTIFICATION      "com.apple.mobile.application_installed"

enum connection_type {
    CONNECTION_USBMUXD = 1
};

struct idevice_private {
    char *udid;
    uint32_t mux_id;
    enum connection_type conn_type;
    void *conn_data;
    int version;
};


/*
 Partly support wifi connections
 Reference: https://github.com/libimobiledevice/libimobiledevice/issues/757
 */
int
idevice_new_wifi(idevice_t * device, const char *udid)
{
    usbmuxd_device_info_t muxdev;
    int res = usbmuxd_get_device(udid, &muxdev, DEVICE_LOOKUP_USBMUX | DEVICE_LOOKUP_NETWORK);
    if (res > 0) {
        idevice_t dev = (idevice_t) malloc(sizeof(struct idevice_private));
        dev->udid = strdup(muxdev.udid);
        dev->mux_id = muxdev.handle;
        dev->conn_type = CONNECTION_USBMUXD;
        dev->conn_data = NULL;
        dev->version = 0;
        *device = dev;
        return IDEVICE_E_SUCCESS;
    }
    /* other connection types could follow here */

    return IDEVICE_E_NO_DEVICE;
}

int
get_device(const char *udid, int (^block)(idevice_t device))
{
    int ret = EXIT_FAILURE;
    idevice_error_t device_error;
    idevice_t device = NULL;

    if (NULL == block)
        return EXIT_FAILURE;

    device_error = idevice_new_wifi(&device, udid);

    if (IDEVICE_E_NO_DEVICE == device_error)
    {
        log_error("Could not find device");
        return EXIT_FAILURE;
    }
    else if (IDEVICE_E_SUCCESS != device_error)
    {
        log_error("Could not connect to device");
        return EXIT_FAILURE;
    }

    if (NULL != device)
    {
        ret = block(device);
        idevice_free(device);
    }

    return ret;
}

plist_t
mobile_gestalt_query(idevice_t device, const char *key)
{
    lockdownd_client_t lockdown_client = NULL;
    lockdownd_service_descriptor_t service = NULL;
    diagnostics_relay_client_t diagnostics_client = NULL;
    plist_t keys = NULL;
    plist_t result = NULL;

    if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(device, &lockdown_client, "idevicediagnostics"))
    {
        log_error("Failed to connect lockdownd client");
        return NULL;
    }

    if (LOCKDOWN_E_SUCCESS != lockdownd_start_service(lockdown_client, LOCKDOWN_SERVICE_DIAGNOSTICS_RELAY, &service))
        lockdownd_start_service(lockdown_client, LOCKDOWN_SERVICE_DIAGNOSTICS_RELAY_OLD, &service);

    lockdownd_client_free(lockdown_client);

    if (NULL == service || service->port <= 0)
    {
        log_error("Failed to start diagnostics_relay service");
        return NULL;
    }

    if (DIAGNOSTICS_RELAY_E_SUCCESS != diagnostics_relay_client_new(device, service, &diagnostics_client))
    {
        lockdownd_service_descriptor_free(service);
        log_error("Could not connect to diagnostics_relay service");
        return NULL;
    }

    keys = plist_new_array();
    plist_array_append_item(keys, plist_new_string(key));

    if (DIAGNOSTICS_RELAY_E_SUCCESS != diagnostics_relay_query_mobilegestalt(diagnostics_client, keys, &result))
        log_error("Failed to query MobileGestalt key: %s", key);

    plist_free(keys);
    lockdownd_service_descriptor_free(service);
    diagnostics_relay_client_free(diagnostics_client);

    return result;
}

char *
mobile_gestalt_get_string_value(idevice_t device, const char *key)
{
    plist_t query_node = NULL;
    plist_t status_node = NULL;
    plist_t value_node = NULL;
    char *status = NULL;
    char *value = NULL;

    query_node = mobile_gestalt_query(device, key);

    if (NULL == query_node)
        return NULL;

    if (NULL != (status_node = plist_access_path(query_node, 2, "MobileGestalt", "Status")))
    {
        plist_get_string_val(status_node, &status);
        if (NULL != status)
        {
            if (!strncmp(status, "Succ", 4))
            {
                if (NULL != (value_node = plist_access_path(query_node, 2, "MobileGestalt", key)))
                    plist_get_string_val(value_node, &value);
            }
            free(status);
        }
    }

    plist_free(query_node);
    return value;
}

int
get_afc_app_client(idevice_t device, const char *bundle_identifier, int (^block)(afc_client_t client))
{
    int ret = EXIT_FAILURE;
    lockdownd_client_t lockdownd_client = NULL;
    lockdownd_service_descriptor_t service = NULL;
    house_arrest_client_t ha_client = NULL;
    plist_t ha_result = NULL;
    plist_t ha_error_node = NULL;
    char *ha_error = NULL;
    afc_client_t afc_client = NULL;

    if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(device, &lockdownd_client, "mobile_house_arrest"))
    {
        log_error("Could not connect to lockdownd");
        return EXIT_FAILURE;
    }

    if (LOCKDOWN_E_SUCCESS != lockdownd_start_service(lockdownd_client, LOCKDOWN_SERVICE_HOUSE_ARREST, &service))
    {
        log_error("Could not start house arrest service");
        goto cleanup;
    }

    if (HOUSE_ARREST_E_SUCCESS != house_arrest_client_new(device, service, &ha_client))
    {
        log_error("Could not house arrest client client");
        goto cleanup;
    }

    if (HOUSE_ARREST_E_SUCCESS != house_arrest_send_command(ha_client, "VendContainer", bundle_identifier))
    {
        log_error("Failed to set house arrest container");
        goto cleanup;
    }

    if (HOUSE_ARREST_E_SUCCESS != house_arrest_get_result(ha_client, &ha_result))
    {
        log_error("Failed to get result from house arrest service");
        goto cleanup;
    }

    if (NULL != (ha_error_node = plist_dict_get_item(ha_result, "Error")))
    {
        plist_get_string_val(ha_error_node, &ha_error);
        log_error("Failed to set house arrest container (%s)", ha_error);
        goto cleanup;
    }

    if (AFC_E_SUCCESS != afc_client_new_from_house_arrest_client(ha_client, &afc_client))
    {
        log_error("Could not create afc client");
        goto cleanup;
    }

    ret = block(afc_client);

cleanup:
    if (afc_client)
        afc_client_free(afc_client);

    if (ha_error)
        free(ha_error);

    if (ha_error_node)
        plist_free(ha_error_node);

    if (ha_result)
        plist_free(ha_result);

    if (ha_client)
        house_arrest_client_free(ha_client);

    if (service)
        lockdownd_service_descriptor_free(service);

    if (lockdownd_client)
        lockdownd_client_free(lockdownd_client);

    return ret;
}

int
get_afc_client(idevice_t device, int (^block)(afc_client_t client))
{
    int ret = EXIT_FAILURE;
    lockdownd_client_t lockdownd_client = NULL;
    lockdownd_service_descriptor_t service = NULL;
    afc_client_t afc_client = NULL;

    if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(device, &lockdownd_client, "apple_file_conduit"))
    {
        log_error("Could not connect to lockdownd");
        return EXIT_FAILURE;
    }

    if (LOCKDOWN_E_SUCCESS != lockdownd_start_service(lockdownd_client, LOCKDOWN_SERVICE_AFC, &service))
    {
        log_error("Could not start afc service");
        goto cleanup;
    }

    if (AFC_E_SUCCESS != afc_client_new(device, service, &afc_client))
    {
        log_error("Could not create afc client");
        goto cleanup;
    }

    ret = block(afc_client);

cleanup:
    if (afc_client)
        afc_client_free(afc_client);

    if (service)
        lockdownd_service_descriptor_free(service);

    if (lockdownd_client)
        lockdownd_client_free(lockdownd_client);

    return ret;
}

int
afc_upload_dir(afc_client_t client, const char *src, const char *dst)
{
    int ret = EXIT_FAILURE;
    __block char *remote_path = NULL;
    __block char *local_path = NULL;
    __block char *buffer = NULL;
    __block afc_error_t afc_error;
    __block int local_fd = 0;
    __block uint64_t remote_fd = 0;
    __block uint32_t written = 0;

    size_t src_len = strlen(src);

    if (NULL == (remote_path = malloc(PATH_MAX * sizeof(char))))
    {
        log_error("malloc() failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    if (NULL == (local_path = malloc(PATH_MAX * sizeof(char))))
    {
        log_error("malloc() failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    ret = fs_visit((char *)src, FS_ENTRY_TYPE_ALL, ^int(FTSENT *entry, enum FS_ENTRY_TYPE entry_type) {
        snprintf(remote_path, PATH_MAX, "%s%s%s", dst, &entry->fts_path[src_len], entry->fts_name);
        log_debug("remote_path: %s", remote_path);

        if (entry_type & FS_ENTRY_TYPE_DIR)
        {
            log_debug("Creating remote dir: %s", remote_path);
            afc_error = afc_make_directory(client, remote_path);
            if (afc_error != AFC_E_SUCCESS && afc_error != AFC_E_OBJECT_EXISTS)
            {
                log_error("Failed to create dir at path: %s", remote_path);
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }

        snprintf(local_path, PATH_MAX, "%s%s", entry->fts_path, entry->fts_name);

        log_debug("Openning local file %s", local_path);
        if (0 >= (local_fd = open(local_path, O_RDONLY)))
        {
            log_error("Failed to open local file %s: %s", local_path, strerror(errno));
            return EXIT_FAILURE;
        }

        log_debug("Openning remote file %s", remote_path);
        if (AFC_E_SUCCESS != afc_file_open(client, remote_path, AFC_FOPEN_RW, &remote_fd))
        {
            log_error("Failed to open remote file: %s", remote_path);
            close(local_fd);
            return EXIT_FAILURE;
        }

        log_debug("Reading %lu bytes from local file: %s", entry->fts_statp->st_size, local_path);
        if (MAP_FAILED == (buffer = mmap(NULL, entry->fts_statp->st_size, PROT_READ, MAP_PRIVATE, local_fd, 0)))
        {
            log_error("mmap() %s: %s", local_path, strerror(errno));
            close(local_fd);
            afc_file_close(client, remote_fd);
            return EXIT_FAILURE;
        }

        log_debug("Writing %lu bytes to remote file: %s", entry->fts_statp->st_size, remote_path);
        afc_error = afc_file_write(client, remote_fd, buffer, (uint32_t)entry->fts_statp->st_size, &written);

        close(local_fd);
        afc_file_close(client, remote_fd);

        if (afc_error != AFC_E_SUCCESS)
        {
            log_error("Failed to write data to: %s", dst);
            return EXIT_FAILURE;
        }

        if (written != entry->fts_statp->st_size)
        {
            log_error("afc_file_write() failed: Mismatched size (%u/%lu)", written, entry->fts_statp->st_size);
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    });

    free(remote_path);
    free(local_path);
    return ret;
}

void
observe_notification_with_timeout_callback(const char *notification, void *userp)
{
    pthread_mutex_t *lock = (pthread_mutex_t *)userp;
    log_debug("Recevied notification: %s", notification);
    pthread_mutex_unlock(lock);
}

int
observe_notification_with_timeout(idevice_t device, const char *notification, int timeout, int(^operation)(void))
{
    int ret = EXIT_FAILURE;
    lockdownd_client_t lockdownd_client = NULL;
    lockdownd_service_descriptor_t service = NULL;
    np_client_t client = NULL;
    pthread_mutex_t lock;

    if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(device, &lockdownd_client, "notification_proxy"))
    {
        log_error("Could not connect to lockdownd");
        return ret;
    }

    if (LOCKDOWN_E_SUCCESS != lockdownd_start_service(
                                                      lockdownd_client,
                                                      LOCKDOWN_SERVICE_NOTIFICATION_PROXY,
                                                      &service
                                                      ) || (service->port == 0))
    {
        log_error("Could not start notification proxy service");
        goto cleanup;
    }

    if (NP_E_SUCCESS != np_client_new(device, service, &client))
    {
        log_error("Could not create notification proxy client");
        goto cleanup;
    }

    /*
     lock the mutex here and call pthread_mutex_trylock until success or timeout.
     The callback will unlock the mutex once the notification has been received.
     */
    pthread_mutex_init(&lock, NULL);
    pthread_mutex_lock(&lock);

    np_set_notify_callback(client, observe_notification_with_timeout_callback, &lock);

    if (NP_E_SUCCESS != np_observe_notification(client, notification))
    {
        log_error("Failed to set notification observer for: %s", notification);
        goto cleanup;
    }

    if (operation && EXIT_SUCCESS != operation())
        goto cleanup;

    for (int i = 0; i < timeout; i++)
    {
        if (0 == pthread_mutex_trylock(&lock))
        {
            ret = EXIT_SUCCESS;
            break;
        }

        sleep(1);
    }

    if (EXIT_SUCCESS != ret)
        log_debug("timed out");

cleanup:
    if (lockdownd_client)
        lockdownd_client_free(lockdownd_client);

    if (service)
        lockdownd_service_descriptor_free(service);

    if (client)
        np_client_free(client);

    pthread_mutex_destroy(&lock);

    return ret;
}

typedef struct
{
    pthread_mutex_t status;
    char *error_name;
    char *error_description;
} installation_status_t;

__attribute__((no_instrument_function)) void
install_bundles_callback(plist_t command, plist_t status, void *userp)
{
    installation_status_t *install_status = (installation_status_t *)userp;
    char *name = NULL;
    int percent = 0;

    instproxy_status_get_name(status, &name);
    if (NULL == name)
    {
        log_progress(percent, "Error", 1);
        instproxy_status_get_error(status, &install_status->error_name, &install_status->error_description, NULL);
    }
    else {
        if (!strcmp(name, "Complete"))
            percent = 100;
        else
            instproxy_status_get_percent_complete(status, &percent);

        log_progress(percent, name, 0);
        free(name);
    }

    if (NULL != install_status->error_name || NULL != install_status->error_description || percent == 100)
        pthread_mutex_unlock(&install_status->status);
}

int
install_bundles(idevice_t device, char *path) {
    int ret = EXIT_FAILURE;
    instproxy_client_t client = NULL;
    installation_status_t install_status;

    if (INSTPROXY_E_SUCCESS != instproxy_client_start_service(device, &client, "installation_proxy")) {
        log_error("Could not start installation proxy service");
        return ret;
    }

    plist_t options = plist_new_dict();

    memset(&install_status, 0, sizeof(install_status));
    pthread_mutex_init(&install_status.status, NULL);
    pthread_mutex_lock(&install_status.status);

    if (INSTPROXY_E_SUCCESS != instproxy_install(client, path, options, install_bundles_callback, &install_status))
    {
        log_error("Installation proxy failed");
        goto cleanup;
    }

    pthread_mutex_lock(&install_status.status);

    if (NULL != install_status.error_name || NULL != install_status.error_description)
    {
        log_error("%s (%s)", install_status.error_description, install_status.error_name);
        goto cleanup;
    }

    ret = EXIT_SUCCESS;

cleanup:
    plist_free(options);
    instproxy_client_free(client);

    pthread_mutex_destroy(&install_status.status);

    if (install_status.error_name)
        free(install_status.error_name);

    if (install_status.error_description)
        free(install_status.error_description);

    return ret;
}

char *
get_app_executable(idevice_t device, char *bundle_identifier)
{
    instproxy_client_t instproxy_client = NULL;
    char *path = NULL;

    if (INSTPROXY_E_SUCCESS != instproxy_client_start_service(device, &instproxy_client, "installation_proxy"))
    {
        log_error("Could not start installation proxy service");
        return NULL;
    }

    instproxy_client_get_path_for_bundle_identifier(instproxy_client, bundle_identifier, &path);
    instproxy_client_free(instproxy_client);

    if (!path)
        log_error("Could not get executable path for bundle identifier: %s", bundle_identifier);

    return path;
}

int
debugserver_launch_app_executable(idevice_t device, char *path)
{
    int ret = EXIT_FAILURE;
    debugserver_client_t debugserver_client = NULL;
    debugserver_command_t command = NULL;
    char *app_argv[2] = {path, NULL};
    char *response = NULL;

    if (DEBUGSERVER_E_SUCCESS != debugserver_client_start_service(device, &debugserver_client, "debugserver"))
    {
        log_error("Could not start debugserver service");
        return ret;
    }

    log_debug("Launching: %s", path);
    debugserver_client_set_argv(debugserver_client, 2, app_argv, NULL);
    debugserver_command_new("qLaunchSuccess", 0, NULL, &command);
    debugserver_client_send_command(debugserver_client, command, &response);

    log_debug("Got response: %s", response);

    debugserver_command_free(command);
    debugserver_client_free(debugserver_client);

    ret = (!response || strncmp(response, "OK", 2)) ? EXIT_FAILURE : EXIT_SUCCESS;

    if (response)
        free(response);

    if (EXIT_SUCCESS != ret)
        log_error("Failed to run app: %s", path);

    return ret;
}
