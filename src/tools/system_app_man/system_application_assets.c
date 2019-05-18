#include "system_application_assets.h"
#include "device.h"
#include <limits.h>
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include "utils.h"

#define CATALOGUE_URL_FORMAT "http://mesu.apple.com/systemassets/%s/%s/com_apple_MobileAsset_SystemApp/com_apple_MobileAsset_SystemApp.xml"

#define kMobileGestaltSystemImageIDKey  "4qfpxrvLtWillIHpIsVgMA"
#define kMobileGestaltBuildIDKey        "qwXfFvH5jPXPxrny0XuGtQ"

char *
system_app_asset_catalogue_url(idevice_t device)
{
    char *url = NULL;
    char *build_id = NULL;
    char *system_image_id = NULL;
    size_t len = 0;

    build_id = mobile_gestalt_get_string_value(device, kMobileGestaltBuildIDKey);
    system_image_id = mobile_gestalt_get_string_value(device, kMobileGestaltSystemImageIDKey);

    if (NULL == build_id || NULL == system_image_id)
        return NULL;

    len = strlen(CATALOGUE_URL_FORMAT);
    len += strlen(build_id);
    len += strlen(system_image_id);

    url = calloc(len, sizeof(char));
    if (url)
        snprintf(url, len + 1, CATALOGUE_URL_FORMAT, build_id, system_image_id);

    free(build_id);
    free(system_image_id);
    return url;
}

system_app_asset_t **
__system_app_asset_parse_raw_catalogue(plist_t raw_catalogue)
{
    system_app_asset_t **catalogue = NULL;
    plist_t assets_node = NULL;
    size_t asset_count = 0;

    assets_node = plist_dict_get_item(raw_catalogue, "Assets");
    asset_count = plist_array_get_size(assets_node);

    catalogue = calloc(asset_count + 1, sizeof(system_app_asset_t *));
    if (NULL == catalogue)
        return NULL;

    for (int i = 0; i < asset_count; i++)
    {
        system_app_asset_t *asset = calloc(1, sizeof(system_app_asset_t));

        plist_t asset_node          = plist_array_get_item(assets_node, i);
        plist_t appid_node          = plist_dict_get_item(asset_node, "AppBundleID");
        plist_t base_url_node       = plist_dict_get_item(asset_node, "__BaseURL");
        plist_t url_path_node       = plist_dict_get_item(asset_node, "__RelativePath");
        plist_t range_start_node    = plist_dict_get_item(asset_node, "_StartOfDataRange");
        plist_t range_len_node      = plist_dict_get_item(asset_node, "_LengthOfDataRange");

        if (appid_node)
            plist_get_string_val(appid_node, &asset->appid);

        if (base_url_node)
            plist_get_string_val(base_url_node, &asset->url);

        if (url_path_node)
        {
            char *tmp = NULL;

            plist_get_string_val(url_path_node, &tmp);
            if (tmp)
            {
                asset->url = realloc(asset->url, strlen(asset->url) + strlen(tmp) + 1);
                strncpy(&asset->url[strlen(asset->url)], tmp, strlen(tmp));
                free(tmp);
            }
        }

        if (range_start_node)
        {
            char *tmp = NULL;

            plist_get_string_val(range_start_node, &tmp);
            if (tmp)
            {
                asset->range_start = strtoll(tmp, NULL, 10);
                free(tmp);
            }
        }

        if (range_len_node)
        {
            char *tmp = NULL;
            size_t len = 0;

            plist_get_string_val(range_len_node, &tmp);
            if (tmp)
            {
                len = strtoll(tmp, NULL, 10);
                asset->range_end = asset->range_start + len;
                free(tmp);
            }
        }

        catalogue[i] = asset;
    }

    return catalogue;
}

system_app_asset_t **
system_app_asset_catalogue(idevice_t device)
{
    char *url = NULL;
    char *catalogue_buffer = NULL;
    size_t catalogue_buffer_len = 0;
    plist_t catalogue_node = NULL;
    system_app_asset_t **catalogue = NULL;


    url = system_app_asset_catalogue_url(device);
    if (NULL == url)
        return NULL;

    download_remote_data(url, NULL, &catalogue_buffer, &catalogue_buffer_len);
    free(url);

    if (NULL == catalogue_buffer)
        return NULL;

    if (catalogue_buffer_len)
        plist_from_memory(catalogue_buffer, (uint32_t)catalogue_buffer_len, &catalogue_node);
    free(catalogue_buffer);

    if (catalogue_node)
    {
        catalogue = __system_app_asset_parse_raw_catalogue(catalogue_node);
        plist_free(catalogue_node);
    }

    return catalogue;
}

void
system_app_asset_free_catalogue(system_app_asset_t ***catalogue) {
    system_app_asset_t **asset = *catalogue;
    do
    {
        if (asset[0]->appid) {
            free(asset[0]->appid);
            asset[0]->appid = NULL;
        }

        if (asset[0]->url) {
            free(asset[0]->url);
            asset[0]->url = NULL;
        }

        free(asset[0]);
        asset[0] = NULL;

        asset++;
    } while (*asset != NULL);

    free(*catalogue);
    *catalogue = NULL;
}

system_app_asset_t *
system_app_asset_by_id(system_app_asset_t **catalogue, const char *appid)
{
    system_app_asset_t **asset = catalogue;

    do
    {
        if (!strcmp(asset[0]->appid, appid))
            break;
        asset++;
    } while (*asset != NULL);

    return *asset;
}

int
system_app_asset_download(system_app_asset_t *asset, const char *dst)
{
    char range_header[PATH_MAX];
    char *buffer = NULL;
    size_t len = 0;
    char *headers[2] = {range_header, NULL};
    snprintf(range_header, PATH_MAX, "Range: bytes=%lu-%lu", asset->range_start, asset->range_end);

    download_remote_data(asset->url, headers, &buffer, &len);

    if (NULL == buffer)
    {
        log_error("Failed to download asset data");
        return EXIT_FAILURE;
    }

    log_info("Extracting asset data to %s", dst);
    if (EXIT_SUCCESS != zip_extract_entry_from_memory(buffer, len, "AssetData", dst))
    {
        log_error("Failed to extract asset archive");

        free(buffer);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
