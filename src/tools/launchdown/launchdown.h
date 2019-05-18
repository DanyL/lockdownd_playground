#pragma once

#include <libimobiledevice/libimobiledevice.h>
#include <plist/plist.h>

typedef struct
{
    char *label;
    char *username;
    char *xpc_service;
    int argc;
    char **argv;
}
service_arguments_t;


plist_t
create_service_agent_plist(service_arguments_t *args);

char *
get_service_agent_label(plist_t service);

void
print_launchdown_helper_compilation_reminder(void);

int
install_launchdown_helper(idevice_t device);

char *
get_launchdown_helper_path(idevice_t device);

int
reload_service_agents(idevice_t device);

int
upload_service_agent(idevice_t device, plist_t service);

int
launch_service_agent(idevice_t device, const char *label);

int
run_service_agent(idevice_t device, plist_t service);
