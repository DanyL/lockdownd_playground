#pragma once

#include <libimobiledevice/libimobiledevice.h>

int
install_and_patch(idevice_t device, const char *app_path, const char *patch_path);
