#include "../../common/common.h"
#include <libgen.h>
#include "patchy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
print_usage(const char *progname)
{
    fprintf(stdout, "Usage: %s [-v] [-h] [-u <udid>] <application> <patch>\n\n", progname);
    fprintf(stdout, "Options:\n");
    fprintf(stdout, "\t-v - Set verbosity level (Incremental)\n");
    fprintf(stdout, "\t-u - Target specific device by UDID\n");
    fprintf(stdout, "\t-h - Prints usage information\n\n");
}

int
main(int argc, char *argv[]) {
    char *progname = NULL;
    char *udid = NULL;
    char *app_path = NULL;
    char *patch_path = NULL;
    int parsed_args = 1;

    progname = basename(*argv);

    for (int i = 1; argc > i; i++)
    {
        if (*argv[i] != '-') continue;

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
                break;
            default:
                log_error("Unknown option: %s\n", argv[i]);
                print_usage(progname);
                return EXIT_FAILURE;
                break;
        }

        parsed_args++;
    }

    if (2 > argc - parsed_args)
    {
        log_error("Missing arguments\n");
        print_usage(progname);
        return EXIT_FAILURE;
    }

    app_path = argv[parsed_args++];
    patch_path = argv[parsed_args++];

    return get_device(udid, ^(idevice_t device)
    {
        return install_and_patch(device, app_path, patch_path);
    });
}
