#ifndef device_h
#define device_h

#include <libimobiledevice/afc.h>
#include <libimobiledevice/libimobiledevice.h>
#include <plist/plist.h>
#include <stdio.h>

int
get_device(const char *udid, int (^block)(idevice_t device));

plist_t
mobile_gestalt_query(idevice_t device, const char *key);

char *
mobile_gestalt_get_string_value(idevice_t device, const char *key);

int
get_afc_app_client(idevice_t device, const char *bundle_identifier, int (^block)(afc_client_t client));

int
get_afc_client(idevice_t device, int (^block)(afc_client_t client));

int
afc_upload_dir(afc_client_t client, const char *src, const char *dst);

int
observe_notification_with_timeout(idevice_t device, const char *notification, int timeout, int(^operation)(void));

int
install_bundles(idevice_t device, char *path);

char *
get_app_executable(idevice_t device, char *bundle_identifier);

int
debugserver_launch_app_executable(idevice_t device, char *path);

#endif /* device_h */
