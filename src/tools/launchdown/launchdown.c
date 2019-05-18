#include "launchdown.h"
#include "../../common/common.h"
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

plist_t
create_service_agent_plist(service_arguments_t *args)
{
    plist_t service = plist_new_dict();
    plist_t program_arguments = NULL;

    plist_dict_set_item(service, "AllowByProxy", plist_new_bool(1));
    plist_dict_set_item(service, "AllowUnactivatedService", plist_new_bool(1));
    plist_dict_set_item(service, "UserName", plist_new_string((args->username) ? args->username : "root"));

    if (NULL != args->label)
        plist_dict_set_item(service, "Label", plist_new_string(args->label));

    if (NULL != args->xpc_service)
        plist_dict_set_item(service, "XPCServiceName", plist_new_string(args->xpc_service));

    if (args->argc)
    {
        program_arguments = plist_new_array();

        for (int i = 0; i < args->argc; i++)
            plist_array_append_item(program_arguments, plist_new_string(args->argv[i]));

        plist_dict_set_item(service, "ProgramArguments", program_arguments);
    }

    return service;
}

char *
get_service_agent_label(plist_t service)
{
    char *label = NULL;
    plist_t label_node = NULL;

    if (NULL != (label_node = plist_dict_get_item(service, "Label")))
        plist_get_string_val(label_node, &label);

    if (NULL == label)
    {
        label = get_uuid();
        plist_dict_set_item(service, "Label", plist_new_string(label));
    }

    return label;
}

void
print_launchdown_helper_compilation_reminder()
{
    fprintf(stdout, "\e[31m");
    fprintf(stdout, "\t\t-------------------------------------------------------------------\n");
    fprintf(stdout, "\t\t-- Make sure launchdown-helper app is compiled & properly signed --\n");
    fprintf(stdout, "\t\t-------------------------------------------------------------------\n");
    fprintf(stdout, "\e[0m");
}

int
install_launchdown_helper(idevice_t device)
{
    char *path = NULL;
    char *tmp = NULL;
    DIR *dir = NULL;

    if (NULL == (path = get_executable_path()))
        return EXIT_FAILURE;

    if (NULL == (tmp = realloc(path, PATH_MAX)))
    {
        log_error("realloc() failed: %s", strerror(errno));
        free(path);
        return EXIT_FAILURE;
    }

    path = tmp;

    if (NULL == (tmp = strrchr(path, '/')))
    {
        free(path);
        return EXIT_FAILURE;
    }

    memcpy(tmp, "/launchdown-helper.app", 22 + 1);

    if (NULL == (dir = opendir(path)))
    {
        log_error("%s: %s", path, strerror(errno));
        print_launchdown_helper_compilation_reminder();

        free(path);
        return EXIT_FAILURE;
    }

    closedir(dir);

    return get_afc_client(device, ^int(afc_client_t client) {
        log_info("Uplaoding launchdown-helper.app contents");
        if (EXIT_SUCCESS != afc_upload_dir(client, path, "PublicStaging/Payload/launchdown-helper.app"))
        {
            log_error("Failed to upload launchdown-helper.app contents");
            free(path);
            return EXIT_FAILURE;
        }

        free(path);

        log_info("Installing launchdown-helper");
        if (EXIT_SUCCESS != install_bundles(device, "PublicStaging"))
        {
            log_error("Failed to install launchdown-helper");
            print_launchdown_helper_compilation_reminder();

            return  EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    });
}

char *
get_launchdown_helper_path(idevice_t device)
{
    char *path = get_app_executable(device, "com.danyl.launchdown-helper");

    if (NULL == path && EXIT_SUCCESS == install_launchdown_helper(device))
        path = get_app_executable(device, "com.danyl.launchdown-helper");

    return path;
}

int
reload_service_agents(idevice_t device)
{
    int ret = EXIT_FAILURE;
    char *launcdown_helper_exec_path = NULL;

    if (NULL == (launcdown_helper_exec_path = get_launchdown_helper_path(device)))
        return EXIT_FAILURE;

    ret = observe_notification_with_timeout(device, "com.apple.mobile.new_service_available", 30, ^int{
        log_debug("Waiting for app to post mount notification");
        if (EXIT_SUCCESS != debugserver_launch_app_executable(device, launcdown_helper_exec_path))
        {
            log_error("Failed to launch launchdown-helper app");
            return EXIT_FAILURE; // abort
        }
        log_debug("Done");
        return EXIT_SUCCESS;
    });

    free(launcdown_helper_exec_path);
    return ret;
}

int
upload_service_agent(idevice_t device, plist_t service)
{
    int ret = EXIT_FAILURE;
    char *buffer = NULL;
    uint32_t size = 0;

    plist_to_xml(service, &buffer, &size);

    if (NULL == buffer || 0 == size)
    {
        log_error("Failed to convert service to bplist format");

        if (buffer)
            free(buffer);
        return EXIT_FAILURE;
    }

    log_debug("Service:\n%s", buffer);

    ret = get_afc_client(device, ^int(afc_client_t client) {
        afc_error_t error;
        uint64_t service_fd = 0;
        uint32_t written = 0;

#define DEST_DIR "/Library/Lockdown/ServiceAgents"

        error = afc_make_directory(client, DEST_DIR);
        if (AFC_E_SUCCESS != error && AFC_E_OBJECT_EXISTS != error)
        {
            log_error("AFC failed to create directory: " DEST_DIR);
            return EXIT_FAILURE;
        }

        if (AFC_E_SUCCESS != afc_file_open(client, DEST_DIR "/service.plist", AFC_FOPEN_WRONLY, &service_fd))
        {
            log_error("Failed to open remote file: " DEST_DIR "/service.plist");
            return EXIT_FAILURE;
        }

        error = afc_file_write(client, service_fd, buffer, size, &written);
        afc_file_close(client, service_fd);

        if (AFC_E_SUCCESS != error)
        {
            log_error("Failed to write service data to " DEST_DIR "/service.plist");
            return EXIT_FAILURE;
        }

#undef DEST_DIR

        if (written != size)
        {
            log_error("Service upload failed: Mismatched size (%lu/%lu)", written, size);
            return EXIT_FAILURE;
        }

        return AFC_E_SUCCESS;
    });

    free(buffer);
    return ret;
}

int
launch_service_agent(idevice_t device, const char *label)
{
    lockdownd_client_t lockdown_client = NULL;
    lockdownd_service_descriptor_t service = NULL;
    lockdownd_error_t error;

    if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(device, &lockdown_client, label))
    {
        log_info("Failed to connect lockdownd client");
        return EXIT_FAILURE;
    }

    error = lockdownd_start_service(lockdown_client, label, &service);

    if (service)
        lockdownd_service_descriptor_free(service);
    lockdownd_client_free(lockdown_client);

    return (LOCKDOWN_E_SUCCESS != error) ? EXIT_FAILURE : EXIT_SUCCESS;
}

int
run_service_agent(idevice_t device, plist_t service)
{
#define check_error(ret, fmt, ...) if (EXIT_SUCCESS != ret) {log_error(fmt, ##__VA_ARGS__); free(label); return EXIT_FAILURE;}

    char *label = get_service_agent_label(service);

    log_info("Uploading service agent: %s", label);
    check_error(
        upload_service_agent(device, service),
        "Failed to upload service agent to device"
    );

    log_info("Reloading lockdownd service agents");
    check_error(
        reload_service_agents(device),
        "Failed to reload lockdownd service agents"
    );

    log_info("Launching lockdownd service agent: %s", label);
    check_error(
        launch_service_agent(device, label),
        "Failed to launch lockdownd service agents: %s", label
    );

    free(label);
    return EXIT_SUCCESS;
#undef check_error
}
