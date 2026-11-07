/* Finds AirPlay 1 (RAOP) speakers by browsing mDNS for _raop._tcp. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "airplay_sender.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "airplay_discover";

#define QUERY_MAX_RESULTS 32

/* The value of TXT key, or NULL. DNS-SD keys compare without case. */
static const char *txt_value(const mdns_result_t *result, const char *key)
{
    for (size_t i = 0; i < result->txt_count; ++i) {
        if (result->txt[i].key != NULL && strcasecmp(result->txt[i].key, key) == 0) {
            return result->txt[i].value != NULL ? result->txt[i].value : "";
        }
    }
    return NULL;
}

/* Whether a comma-separated TXT list such as "0,3,5" holds item. */
static bool list_has(const char *list, const char *item)
{
    if (list == NULL) {
        return false;
    }
    size_t length = strlen(item);
    for (const char *p = list; *p != '\0';) {
        while (*p == ' ' || *p == ',') {
            ++p;
        }
        const char *end = p;
        while (*end != '\0' && *end != ',') {
            ++end;
        }
        size_t token = (size_t)(end - p);
        while (token > 0 && p[token - 1] == ' ') {
            --token;
        }
        if (token == length && strncasecmp(p, item, length) == 0) {
            return true;
        }
        p = end;
    }
    return false;
}

static void fill_device(airplay_device_t *device, const mdns_result_t *result)
{
    memset(device, 0, sizeof(*device));
    /* RAOP instance names are "<MAC>@<name>". */
    const char *instance = result->instance_name != NULL ? result->instance_name : "";
    const char *at = strchr(instance, '@');
    snprintf(device->name, sizeof(device->name), "%s", at != NULL ? at + 1 : instance);
    snprintf(device->hostname, sizeof(device->hostname), "%s",
             result->hostname != NULL ? result->hostname : "");
    device->port = result->port;
    for (const mdns_ip_addr_t *addr = result->addr; addr != NULL; addr = addr->next) {
        if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
            device->ip = addr->addr.u_addr.ip4;
            break;
        }
    }

    const char *model = txt_value(result, "am");
    snprintf(device->model, sizeof(device->model), "%s", model != NULL ? model : "");
    const char *password = txt_value(result, "pw");
    device->password = password != NULL && strcasecmp(password, "true") == 0;
    const char *encryption = txt_value(result, "et");
    /* A speaker that lists no encryption types takes plain audio. */
    device->plain_audio = encryption == NULL || list_has(encryption, "0");
    const char *codecs = txt_value(result, "cn");
    device->alac = codecs == NULL || list_has(codecs, "1");
    const char *transport = txt_value(result, "tp");
    device->udp = transport == NULL || list_has(transport, "UDP");
    /* Apple's AirPlay 2 speakers (Apple TV, HomePod) want an /auth-setup
       exchange before they accept a RAOP session; et 4 marks them too. */
    device->auth_setup = strncmp(device->model, "AppleTV", 7) == 0 ||
                         strncmp(device->model, "AudioAccessory", 14) == 0 ||
                         list_has(encryption, "4");
    device->supported = device->ip.addr != 0 && device->port != 0 && !device->password &&
                        device->plain_audio && device->alac && device->udp;

    ESP_LOGI(TAG, "%s (%s.local) " IPSTR ":%u model %s et %s cn %s pw %s tp %s -> %s",
             device->name, device->hostname, IP2STR(&device->ip), device->port,
             device->model[0] ? device->model : "?", encryption ? encryption : "-",
             codecs ? codecs : "-", password ? password : "-", transport ? transport : "-",
             device->supported ? "supported" : "not supported");
}

static int compare_devices(const void *a, const void *b)
{
    const airplay_device_t *first = a;
    const airplay_device_t *second = b;
    if (first->supported != second->supported) {
        return first->supported ? -1 : 1;
    }
    return strcasecmp(first->name, second->name);
}

esp_err_t airplay_discover(uint32_t timeout_ms, airplay_device_t *devices, size_t max,
                           size_t *count)
{
    if (devices == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    esp_err_t error = mdns_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }

    mdns_result_t *results = NULL;
    error = mdns_query_ptr("_raop", "_tcp", timeout_ms, QUERY_MAX_RESULTS, &results);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "_raop._tcp query failed: %s", esp_err_to_name(error));
        return error;
    }
    for (const mdns_result_t *result = results; result != NULL && *count < max;
         result = result->next) {
        airplay_device_t device;
        fill_device(&device, result);
        /* The same speaker answers once per interface and IP version. */
        bool duplicate = false;
        for (size_t i = 0; i < *count; ++i) {
            if (strcasecmp(devices[i].name, device.name) == 0 &&
                strcasecmp(devices[i].hostname, device.hostname) == 0) {
                if (devices[i].ip.addr == 0) {
                    devices[i] = device;
                }
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            devices[(*count)++] = device;
        }
    }
    mdns_query_results_free(results);
    qsort(devices, *count, sizeof(devices[0]), compare_devices);
    ESP_LOGI(TAG, "%u AirPlay speakers", (unsigned)*count);
    return ESP_OK;
}
