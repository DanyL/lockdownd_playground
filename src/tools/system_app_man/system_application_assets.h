#pragma once

#include <libimobiledevice/libimobiledevice.h>
#include <stdio.h>

typedef struct
{
    char *appid;
    char *url;
    size_t range_start;
    size_t range_end;
}
system_app_asset_t;

char *
system_app_asset_catalogue_url(idevice_t device);

system_app_asset_t **
system_app_asset_catalogue(idevice_t device);

void
system_app_asset_free_catalogue(system_app_asset_t ***catalogue);

system_app_asset_t *
system_app_asset_by_id(system_app_asset_t **catalogue, const char *appid);

int
system_app_asset_download(system_app_asset_t *asset, const char *dst);
