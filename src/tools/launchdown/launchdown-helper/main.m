#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <libgen.h>

extern CFNotificationCenterRef
CFNotificationCenterGetDistributedCenter(void);

void
print_proc_info()
{
    NSLog(@"login:\t%s", getlogin());
    NSLog(@"uid:\t%d", getuid());
    NSLog(@"gid:\t%d", getgid());
}

void
reload_services()
{
    NSDictionary *userinfo = @{
       @"DiskImageType"     : @"Developer",
       @"DiskImageMountPath": @"/var/mobile/Media/",
    };

    CFNotificationCenterPostNotification(
                                         CFNotificationCenterGetDistributedCenter(),
                                         (__bridge CFStringRef)@"com.apple.mobile.disk_image_mounted",
                                         (__bridge CFStringRef)@"MobileStorageMounter",
                                         (__bridge CFDictionaryRef)userinfo,
                                         true
                                         );
}

int
main(int argc, char *argv[]) {
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "--test"))
            print_proc_info();
        else
            reload_services();

        return 0;
    }
}
