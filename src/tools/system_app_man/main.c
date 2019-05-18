#include "../../common/common.h"
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system_application_assets.h"


void
print_catalogue(system_app_asset_t **catalogue) {
    system_app_asset_t **asset = catalogue;
    do
    {
        log_info("%s", asset[0]->appid);
        asset++;
    } while (*asset != NULL);
}

void
print_usage(const char *progname)
{
    fprintf(stdout, "Usage: %s [-v] [-h] [-u <udid>] [<url | list | download | install> [appid] [dest]]\n\n", progname);
    fprintf(stdout, "Options:\n");
    fprintf(stdout, "\t-v - Set verbosity level (Incremental)\n");
    fprintf(stdout, "\t-u - Target specific device by UDID\n");
    fprintf(stdout, "\t-h - Prints usage information\n\n");
    fprintf(stdout, "Commands:\n");
    fprintf(stdout, "\turl\t\t\t\tPrint catalogue url\n");
    fprintf(stdout, "\tlist\t\t\t\tPrint available applications\n");
    fprintf(stdout, "\tdownload <appid> <dest>\t\tDownload <appid> to <dest>\n");
    fprintf(stdout, "\tinstall <appid>\t\t\tDownload & Install <appid>\n\n");
}

enum CMD {
    CMD_INVALID,
    CMD_URL,
    CMD_LIST,
    CMD_DOWNLOAD,
    CMD_INSTALL
};

int
main(int argc, char *argv[]) {
    char *progname = NULL;
    enum CMD cmd = CMD_INVALID;
    char *udid = NULL;
    char *appid = NULL;
    __block char *tmp;
    __block char *download_dest = NULL;
    __block int ret = EXIT_FAILURE;
    __block char *url = NULL;
    __block system_app_asset_t **catalogue = NULL;
    __block system_app_asset_t *asset = NULL;

#define arg_error(err) do { \
    log_error("%s\n", err); \
    print_usage(progname); \
    return EXIT_FAILURE; \
} while (0);

    progname = basename(*argv);
    if (2 > argc)
        arg_error("Missing arguments");

    for (int i = 1; argc > i; i++)
    {
        switch (*argv[i])
        {
            case '-':
                switch (argv[i][1])
                {
                    case 'v':
                        g_log_verbosity = (int)strlen(&argv[i][1]);
                        break;
                    case 'h':
                        print_usage(progname);
                        return EXIT_SUCCESS;
                    case 'u':
                        udid = argv[++i];
                        continue;
                    default:
                        break;
                }
                break;
            case 'u':
                cmd = CMD_URL;
                continue;
            case 'l':
                cmd = CMD_LIST;
                continue;
            case 'd':
                if (3 > argc - i)
                    arg_error("Missing arguments");

                cmd = CMD_DOWNLOAD;
                appid = argv[++i];
                download_dest = argv[++i];
                continue;
            case 'i':
                if (2 > argc - i)
                    arg_error("Missing arguments");

                cmd = CMD_INSTALL;
                appid = argv[++i];
                continue;
            default:
                break;
        }
    }

    if (CMD_INVALID == cmd)
        arg_error("Invalid arguments");

#undef arg_error

    return get_device(udid, ^(idevice_t device)
    {
        // Update depth counter to ommit get_device function and the block
        g_log_depth--;
        g_log_depth--;

        if (CMD_URL == cmd)
        {
            url = system_app_asset_catalogue_url(device);
            if (NULL == url)
            {
                log_error("Could not build catalogue url");
                return EXIT_FAILURE;
            }

            fprintf(stdout, "%s\n", url);
            free(url);
            return EXIT_SUCCESS;
        }

        log_info("Downloading asset catalogue");
        catalogue = system_app_asset_catalogue(device);
        if (NULL == catalogue)
        {
            log_error("Failed to query system app catalogue");
            return EXIT_FAILURE;
        }

        switch (cmd) {
            case CMD_LIST:
                log_info("Available applications:");
                print_catalogue(catalogue);
                ret = EXIT_SUCCESS;
                break;
            case CMD_INSTALL:
                tmp = get_uuid();
                download_dest = malloc(PATH_MAX);
                snprintf(download_dest, PATH_MAX, "/tmp/%s", tmp);
                free(tmp);
                tmp = NULL;
            case CMD_DOWNLOAD:
                asset = system_app_asset_by_id(catalogue, appid);
                if (NULL == asset)
                {
                    log_error("Could not find appid: '%s'", appid);
                    break;
                }

                log_info("Downloading %s (%lu bytes)", asset->appid, asset->range_end - asset->range_start);
                if (EXIT_SUCCESS != (ret = system_app_asset_download(asset, download_dest)))
                    log_error("Download failed");

                if (CMD_DOWNLOAD == cmd)
                    break;

                // CMD_INSTALL
                log_info("Uploading asset data");
                ret = get_afc_client(device, ^int(afc_client_t client) {
                    return afc_upload_dir(client, download_dest, "PublicStaging/Payload");
                });

                if (EXIT_SUCCESS != ret)
                {
                    log_error("Upload failed");
                    free(download_dest);
                    download_dest = NULL;
                    break;
                }

                free(download_dest);
                download_dest = NULL;

                log_info("Installing %s", asset->appid);
                if (EXIT_FAILURE == (ret = install_bundles(device, "PublicStaging")))
                    log_error("Installation failed");
                else
                    log_info("Done!");
                
                break;
            default:
                break;
        }

        system_app_asset_free_catalogue(&catalogue);
        return ret;
    });
}
