# Lockdown Playground
Lockdown related research, tools and POCs.
All POCs should work fine on iOS 12.2 and below.

It was a fun research project and I hope you enjoy the short writeup.

*Note that I omitted some details about existing underlaying issues.*

## FAQ
- __Is this a jailbreak? NO__
- __Will this be used in a jailbreak? Maybe, but probably not__
- __Will this lead to other useful tools and discoveries? Maybe__

#### For any other questions you may contact me at:
- @DanyL931 - Twitter
- @DanyL - GitHub


## Compiling
The project was developed and tested under `macOS Mojave`, but should work fine on older macOS versions as well.
Other than the iOS app, the code should be quite easy to port and support other operating systems.

### Dependencies:
- libimobiledevice (HEAD)
- libplist (HEAD)
- libzip (for `system_app_man`)
- libcurl (for `system_app_man`)

### macOS:
* Set a valid signing identity for the `launchdown_helper` target.
* Make sure `LDFLAGS` are configured correctly.
* Build `All` scheme using Xcode, or just run:

> xcodebuild -scheme All -project lockdownd_playground.xcodeproj build

* Compiled binaries will result in `build/release` or `build/debug`, depending on the build type used.


## launchdown
Exploits CVE-2019-8637 to achieve a launchd job submission primitive and implements a shell-like CLI interface on top of it.

This can be used for research purposes, altering system state and triggering additional vulnerabilities.


>
	Usage: 	launchdown [OPTIONS] COMMAND
		launchdown [-v] [-u] [--user <USER>] [-l] exec <PROGRAM> [ARG1, ...]
		launchdown [-v] [-u] [-l] xpc <XPC_SERVICE_NAME>
		launchdown [-v] [-u] [-l] raw <SERVICE_PLIST>
		launchdown [-v] [-u] [--user <USER>] [-l] test

	Options:
		--user	- Specifies the user to run the job as. (defaults to 'root' unless command 'test' is used)
 		--label - Specifies a unique job identifier to lockdownd.
	 	-v	- Set verbosity level (Incremental)
 		-u	- Target specific device by UDID
	 	-h	- Prints usage information

	Commands:
	 	exec <command>	Executes the given command
	 	xpc  <label>	Spawn the given XPC Service. (Only services reachable to lockdownd)
	 	test		Executes launchdown-helper in test mode which prints username, uid and gid to syslog.
 				* Sets user to '_networkd'.
 				  Because of sandbox restrictions containermanagerd is unable to
 				  create containers for users other than 'mobile' and '_networkd'.
 				  (Use '--user' option to override)

 			-------------------------------------------------------------------
 			-- Make sure launchdown-helper app is compiled & properly signed --
 			-------------------------------------------------------------------

### `MobileStorageMounter` strikes again! or was it?
After mounting a `DeveloperDiskImage` and loading new Launch Daemons, `MobileStorageMounter` will use the `distributed notification center` to inform other system services about the new disk image mount and it will send its type and mount point inside a user info dictionary.

A little help from `grep -r` revealed that the observers of this notification are `installd` and `lockdownd`, which will load new applications and lockdown agents (respectively) __upon a relative path to the mount point sent by `MobileStorageMounter`__.

By spoofing the above notification from a 3rd party application, __I was able to control the path from which `installd` and `lockdownd` are loading their resources__. While `installd` complained about insufficient permissions, I was able to fool `lockdownd` into loading and launching my own agents from `/var/mobile/Media/Library/Lockdown/ServiceAgents`.

*Note that the vulnerability in `installd` is still present on iOS 12.3*

#### CVE-2019-8637 (`MobileLockdown`):
	Available for: iPhone 5s and later, iPad Air and later, and iPod touch 6th generation
	Impact: A malicious application may be able to gain root privileges
	Description: An input validation issue was addressed with improved input validation.
	CVE-2019-8637: Dany Lisiansky (@DanyL931)

#### New Mitigations:
- `lockdownd` now verifies the mount point path against a list of mount points retrieved by using the `MobileStorageCopyDevices` API.


## patchy
Exploits some of the mitigations patched by `CVE-2019-8593` and the `MobileInstallation` bugs described below to form a TOCTTOU attack against `AppleFileConduit` and `MobileInstallation` to allow modification of protected application bundles right after installation, and therefore bypassing package inspection.

This may be used to corrupt applications and other installable bundles (like removable system apps, Swift Playgrounds, Carrier bundles, etc) and exploit other parts of the system which otherwise were inaccessible.

>
 	Usage: patchy [-v] [-h] [-u <udid>] <application> <patch>

 	Options:
 		-v - Set verbosity level (Incremental)
 		-u - Target specific device by UDID
 		-h - Prints usage information

### The unmentioned `MobileInstallation` bugs:
When `MobileInstallation` installs an unpacked bundle, it will use `link`/`unlink` to move files and __will fail to verify whether there are additional links present or if the files were opened by another process before placing them inside a protected container__. While the permissions of the additional links will change, __the owners of file descriptors__ on the other hand __are able to keep operating on them as usual, with the same permissions they were opened with__.

I advised Apple to use `clonefile` instead, which is essentially the same as `link` but applies a CoW operation to prevent modifications to the cloned file by modifying the original one, and vice versa.

Because the above vulnerabilities could have been resolved in different locations and I don't have any iOS 12.3 primitive which can directly verify this, Its still hard for me to tell whether they were addressed or not, but by the lack of a CVE I doubt it and assume only the exploited attack vectors (`CVE-2019-8593`, `CVE-2019-8568`) were addressed.

#### CVE-2019-8593 (`AppleFileConduit`):
Note that while `AppleFileConduit` was abused by `patchy` and the same issues I reported about `MobileInstallation` were also found in `AppleFileConduit`, I did not find or disclosed any issues in `AppleFileConduit`. I believe that while addressing the issues a use case where they can be used to cause a memory corruption was found.

Similarly to `MobileInstallation` (and other than the memory corruption), `AppleFileConduit` was missing validation of hard links and mitigations against the usage of `link`/`unlink` between the calls to `AFCFileRefOpen` and other `AFCFileRef*` APIs (e.g: `AFCFileRefRead`/`AFCFileRefWrite`).

I'm still at the process of reversing all changes and I'll report back if I was able to spot the memory corruption.

	Available for: iPhone 5s and later, iPad Air and later, and iPod touch 6th generation
	Impact: An application may be able to execute arbitrary code with system privileges
	Description: A memory corruption issue was addressed with improved memory handling.

	CVE-2019-8593: Dany Lisiansky (@DanyL931)

#### New Mitigations:
From what I can tell by now, there are 2 additional checks added in `libafc.dylib` before each operation is done:

- File descriptors are validated to make sure only 1 link is present.
- The location of the file is retrieved using `fcntl([...], F_GETPATH)` to make sure it remained within `AppleFileConduit` root directory boundaries.


## system\_app\_man
A tool to download and install iOS system applications, which are tied to the device's image & build id.
*It was primarily used in conjunction with `patchy` during research*

>
	Usage: system_app_man [-v] [-h] [-u <udid>] [<url | list | download | install> [appid] [dest]]

	Options:
		-v - Set verbosity level (Incremental)
		-u - Target specific device by UDID
		-h - Prints usage information

	Commands:
		url				Print catalogue url
		list				Print available applications
		download <appid> <dest>		Download <appid> to <dest>
		install <appid>			Download & Install <appid>


## `mobile_installation_proxy` and `streaming_zip_conduit`
Though I did not include a POC to demonstrate this, the bug can be used to exploit the other `MobileInstallation` issues I mentioned (see `patchy` section) by using `mobile_installation_proxy` to install unpacked bundles from an application data container.

#### CVE-2019-8593 (`MobileInstallation`, `StreamingZip`):
Note that the advisory wrongly described the CVE as a symlink validation issue while it was in fact a path traversal validation issue.

*It looks like the issue wasn't patched in `streaming_zip_conduit`, which may indicate that another issue existed and was resolved instead.*

	Available for: iPhone 5s and later, iPad Air and later, and iPod touch 6th generation
	Impact: A local user may be able to modify protected parts of the file system
	Description: A validation issue existed in the handling of symlinks. This issue was addressed with improved validation of symlinks.
	CVE-2019-8568: Dany Lisiansky (@DanyL931)


### The faulty path resolving:
Both `mobile_installation_proxy` and `streaming_zip_conduit` implemented the following faulty method to resolve user controlled paths:

```
NSString *user_controlled_path = “../../“
NSString *media_path = [NSHomeDirectory() stringByAppendingPathComponent: @”Media”];
NSString *dest_path = [media_path stringByAppendingPathComponent: user_controlled_path];
NSString *resolved_dest_path = [dest_path stringByResolvingSymlinksInPath]; // <---- 'stringByResolvingSymlinksInPath' resolves only symlinks

If (![resolved_dest_path hasPrefix: media_path]) // <---- This only validate that the resolved path begins with `/var/mobile/Media'
{
	// Invalid
}
```

As can be seen from the pseudo code above, the resolved path may contain path traversals and still pass validation.

#### New Mitigations:
- `mobile_installation_proxy` now uses `realpath` to resolve both symlinks and path traversals.

## References
- https://support.apple.com/kb/HT201222
- https://github.com/libimobiledevice
- https://github.com/emonti/afcclient

## Credits
- Nikias Bassen (@pimskeks) and all `libimobiledevice` project contributors - For providing a very well written, cross-platform protocol library to communicate with iOS devices.
- Eric Monti - For `afcclient`, which was used as a reference for the `AFC` APIs of `libimobiledevice`.
