#include "../../common/common.h"
#include <errno.h>
#include <fcntl.h>
#include "launchdown.h"
#include <libgen.h>
#include <plist/plist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void print_usage(const char *progname)
{
    fprintf(stdout, "Usage:\t%s [OPTIONS] COMMAND\n", progname);
    fprintf(stdout, "\t%s [-v] [-u] [--user <USER>] [-l] exec <PROGRAM> [ARG1, ...]\n", progname);
    fprintf(stdout, "\t%s [-v] [-u] [-l] xpc <XPC_SERVICE_NAME>\n", progname);
    fprintf(stdout, "\t%s [-v] [-u] [-l] raw <SERVICE_PLIST>\n", progname);
    fprintf(stdout, "\t%s [-v] [-u] [--user <USER>] [-l] test\n\n", progname);

    fprintf(stdout, "Options:\n");
    fprintf(stdout, "\t--user\t- Specifies the user to run the job as. (defaults to 'root' unless command 'test' is used)\n");
    fprintf(stdout, "\t--label\t- Specifies a unique job identifier to lockdownd.\n");
    fprintf(stdout, "\t-v\t- Set verbosity level (Incremental)\n");
    fprintf(stdout, "\t-u\t- Target specific device by UDID\n");
    fprintf(stdout, "\t-h\t- Prints usage information\n\n");

    fprintf(stdout, "Commands:\n");
    fprintf(stdout, "\texec <command>\tExecutes the given command\n");
    fprintf(stdout, "\txpc  <label>\tSpawn the given XPC Service. (Only services reachable to lockdownd)\n");
    fprintf(stdout, "\ttest\t\tExecutes launchdown-helper in test mode which prints username, uid and gid to syslog.\n"
                      "\t\t\t* Sets user to '_networkd'.\n"
                      "\t\t\t  Because of sandbox restrictions containermanagerd is unable to\n"
                      "\t\t\t  create containers for users other than 'mobile' and '_networkd'.\n"
                      "\t\t\t  (Use '--user' option to override)\n\n"
    );

    print_launchdown_helper_compilation_reminder();
}

int main(int argc, char *argv[])
{
#define check_missing_arguments() if (0 == argc - parsed_args) {log_error("Missing arguments\n"); print_usage(progname); return EXIT_FAILURE;}

    char *progname = NULL;
    int parsed_args = 1;
    char *udid = NULL;
    __block service_arguments_t service_args;
    __block plist_t service = NULL;
    __block int run_test = 0;
    
    progname = basename(*argv);
    check_missing_arguments()

    memset(&service_args, 0, sizeof(service_arguments_t));
    for (; argc > parsed_args; parsed_args++)
    {
        if (*argv[parsed_args] != '-') break;

        switch (argv[parsed_args][1])
        {
            case 'v':
                g_log_verbosity = (int)strlen(&argv[parsed_args][1]);
                break;
            case 'h':
                print_usage(progname);
                return EXIT_SUCCESS;
            case 'u':
                udid = argv[++parsed_args];
                break;
            case '-':
                if (!strncmp(&argv[parsed_args][2], "user", 4))
                {
                    service_args.username = argv[++parsed_args];
                    break;
                }
                else if (!strncmp(&argv[parsed_args][2], "label", 5))
                {
                    service_args.label = argv[++parsed_args];
                    break;
                }
            default:
                log_error("Unknown option: %s\n", argv[parsed_args]);
                print_usage(progname);
                return EXIT_FAILURE;
                break;
        }
    }

    check_missing_arguments();

    if (!strncmp(argv[parsed_args], "exec", 4))
    {
        parsed_args++;
        check_missing_arguments();

        service_args.argc = argc - parsed_args;
        service_args.argv = &argv[parsed_args];
    }
    else if (!strncmp(argv[parsed_args], "xpc", 3))
    {
        parsed_args++;
        check_missing_arguments();

        service_args.xpc_service = argv[parsed_args];
    }
    else if (!strncmp(argv[parsed_args], "test", 4))
    {
        run_test = 1;
    }
    else if (!strncmp(argv[parsed_args], "raw", 3))
    {
        char *plist_path = NULL;
        struct stat plist_stat;
        int fd = 0;
        char *buffer = NULL;

        parsed_args++;
        check_missing_arguments();

        plist_path = argv[parsed_args];
        if (0 != stat(plist_path, &plist_stat))
        {
            log_error("Could not stat file: %s (%s)", plist_path, strerror(errno));
            return EXIT_FAILURE;
        }

        if ((plist_stat.st_flags & O_DIRECTORY) & 0)
        {
            log_error("Not a file: %s", plist_path);
            return EXIT_FAILURE;
        }

        if (0 == (fd = open(plist_path, O_RDONLY)))
        {
            log_error("Could not open file: %s (%s)", plist_path, strerror(errno));
            return EXIT_FAILURE;
        }

        if (MAP_FAILED == (buffer = mmap(NULL, plist_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0)))
        {
            log_error("Could not read file: %s (%s)", plist_path, strerror(errno));
            close(fd);
            return EXIT_FAILURE;
        }

        plist_from_memory(buffer, (uint32_t)plist_stat.st_size, &service);
        close(fd);

        if (NULL == service)
        {
            log_error("Failed to desrialize plist", plist_path);
            return EXIT_FAILURE;
        }
    }
    else
    {
        log_error("Unknown command: %s\n", argv[parsed_args]);
        print_usage(progname);
        return EXIT_FAILURE;
    }

    return get_device(udid, ^int(idevice_t device) {
        if (NULL == service)
        {
            if (run_test)
            {
                char *test_args[2] = {NULL, "--test"};

                if (NULL == (test_args[0] = get_launchdown_helper_path(device)))
                    return EXIT_FAILURE;

                service_args.username = (service_args.username) ? service_args.username : "_networkd";
                service_args.argc = 2;
                service_args.argv = test_args;
            }

            if (NULL == (service = create_service_agent_plist(&service_args)))
            {
                log_error("Failed to construct service");
                return EXIT_FAILURE;
            }

            if (run_test && service_args.argv[0])
                free(service_args.argv[0]);
        }

        if (EXIT_SUCCESS == run_service_agent(device, service))
        {
            log_info("Successfully launched job, use syslog/ps to verify.");
            return EXIT_SUCCESS;
        }
        return EXIT_FAILURE;
    });
#undef check_missing_arguments
}
