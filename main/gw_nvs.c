#include "gw_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

#define NVS_NS "gw_cfg"

static void copy_default(char *out, size_t out_sz, const char *def)
{
    if (!out || out_sz == 0) return;
    if (!def) { out[0] = 0; return; }
    strncpy(out, def, out_sz - 1);
    out[out_sz - 1] = 0;
}

esp_err_t gw_nvs_get_str(const char *key, char *out, size_t out_sz,
                         const char *def)
{
    if (!key || !out || out_sz == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        copy_default(out, out_sz, def);
        return ESP_OK;
    }
    size_t sz = out_sz;
    esp_err_t err = nvs_get_str(h, key, out, &sz);
    nvs_close(h);
    if (err != ESP_OK) copy_default(out, out_sz, def);
    return ESP_OK;
}

esp_err_t gw_nvs_set_str(const char *key, const char *val)
{
    if (!key || !val) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
