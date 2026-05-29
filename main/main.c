#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "esp_camera.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "nvs.h"

#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define NVS_NAMESPACE "settings"
#define DEFAULT_AP_SSID "ESP32CAM-Setup"
#define DEFAULT_AP_PASS "esp32cam"
#define DEFAULT_ENDPOINT "https://connect.prusa3d.com/c/snapshot"
#define MDNS_HOSTNAME "esp32cam"

static const char *TAG = "esp32cam-prusa";
static EventGroupHandle_t wifi_events;
static httpd_handle_t server;
static char ip_text[16] = "192.168.4.1";
static bool wifi_configured;
static char page_notice[192];
static char last_upload_summary[192] = "No upload attempted yet.";
static int64_t last_upload_ms;

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char prusa_token[96];
    char prusa_fingerprint[96];
    char prusa_endpoint[160];
    uint32_t upload_interval_s;
    uint8_t framesize;
    uint8_t jpeg_quality;
    int8_t brightness;
    int8_t contrast;
    int8_t saturation;
    uint8_t special_effect;
    uint8_t wb_mode;
    uint8_t awb;
    uint8_t awb_gain;
    uint8_t aec;
    uint8_t aec2;
    int8_t ae_level;
    uint16_t aec_value;
    uint8_t agc;
    uint8_t agc_gain;
    uint8_t gainceiling;
    uint8_t bpc;
    uint8_t wpc;
    uint8_t raw_gma;
    uint8_t lenc;
    uint8_t hmirror;
    uint8_t vflip;
    uint8_t dcw;
    uint8_t colorbar;
} app_settings_t;

static app_settings_t settings;

static void settings_defaults(void)
{
    memset(&settings, 0, sizeof(settings));
    strncpy(settings.prusa_endpoint, DEFAULT_ENDPOINT, sizeof(settings.prusa_endpoint) - 1);
    settings.upload_interval_s = 30;
    settings.framesize = FRAMESIZE_VGA;
    settings.jpeg_quality = 12;
    settings.awb = 1;
    settings.awb_gain = 1;
    settings.aec = 1;
    settings.aec2 = 0;
    settings.aec_value = 300;
    settings.agc = 1;
    settings.gainceiling = GAINCEILING_2X;
    settings.bpc = 0;
    settings.wpc = 1;
    settings.raw_gma = 1;
    settings.lenc = 1;
    settings.dcw = 1;
}

static void settings_load(void)
{
    settings_defaults();
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t size = sizeof(settings);
    if (nvs_get_blob(nvs, "app", &settings, &size) != ESP_OK || size != sizeof(settings)) {
        settings_defaults();
    }
    nvs_close(nvs);

    if (settings.upload_interval_s < 10) {
        settings.upload_interval_s = 10;
    }
    if (settings.prusa_endpoint[0] == '\0') {
        strncpy(settings.prusa_endpoint, DEFAULT_ENDPOINT, sizeof(settings.prusa_endpoint) - 1);
    }
}

static esp_err_t settings_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, "app", &settings, sizeof(settings));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t settings_reset_saved(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, "app");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void delayed_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
}

static void schedule_restart(void)
{
    xTaskCreate(delayed_restart_task, "restart", 2048, NULL, 5, NULL);
}

static const char *framesize_name(framesize_t size)
{
    switch (size) {
    case FRAMESIZE_QQVGA: return "QQVGA 160x120";
    case FRAMESIZE_QVGA: return "QVGA 320x240";
    case FRAMESIZE_VGA: return "VGA 640x480";
    case FRAMESIZE_SVGA: return "SVGA 800x600";
    case FRAMESIZE_XGA: return "XGA 1024x768";
    case FRAMESIZE_SXGA: return "SXGA 1280x1024";
    case FRAMESIZE_UXGA: return "UXGA 1600x1200";
    default: return "Other";
    }
}

static void apply_camera_settings(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return;
    }
    s->set_framesize(s, settings.framesize);
    s->set_quality(s, settings.jpeg_quality);
    s->set_brightness(s, settings.brightness);
    s->set_contrast(s, settings.contrast);
    s->set_saturation(s, settings.saturation);
    s->set_special_effect(s, settings.special_effect);
    s->set_whitebal(s, settings.awb);
    s->set_awb_gain(s, settings.awb_gain);
    s->set_wb_mode(s, settings.wb_mode);
    s->set_exposure_ctrl(s, settings.aec);
    s->set_aec2(s, settings.aec2);
    s->set_ae_level(s, settings.ae_level);
    s->set_aec_value(s, settings.aec_value);
    s->set_gain_ctrl(s, settings.agc);
    s->set_agc_gain(s, settings.agc_gain);
    s->set_gainceiling(s, (gainceiling_t)settings.gainceiling);
    s->set_bpc(s, settings.bpc);
    s->set_wpc(s, settings.wpc);
    s->set_raw_gma(s, settings.raw_gma);
    s->set_lenc(s, settings.lenc);
    s->set_hmirror(s, settings.hmirror);
    s->set_vflip(s, settings.vflip);
    s->set_dcw(s, settings.dcw);
    s->set_colorbar(s, settings.colorbar);
}

static esp_err_t camera_init(void)
{
    bool psram_ready = esp_psram_is_initialized();
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = settings.framesize,
        .jpeg_quality = settings.jpeg_quality,
        .fb_count = psram_ready ? 2 : 1,
        .grab_mode = CAMERA_GRAB_LATEST,
        .fb_location = psram_ready ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err == ESP_OK) {
        apply_camera_settings();
    }
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (wifi_configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        if (wifi_configured) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Open http://%s", ip_text);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_configured = settings.wifi_ssid[0] != '\0';
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    if (wifi_configured) {
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, settings.wifi_ssid, sizeof(sta.sta.ssid));
        strncpy((char *)sta.sta.password, settings.wifi_pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, DEFAULT_AP_SSID, sizeof(ap.ap.ssid));
    strncpy((char *)ap.ap.password, DEFAULT_AP_PASS, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(DEFAULT_AP_SSID);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Setup AP: http://192.168.4.1");
}

static void mdns_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("ESP32-CAM Prusa Connect");
    mdns_service_add("ESP32-CAM Web", "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local", MDNS_HOSTNAME);
}

static void html_escape(char *out, size_t out_len, const char *in)
{
    size_t pos = 0;
    for (; *in && pos + 6 < out_len; in++) {
        if (*in == '&') pos += snprintf(out + pos, out_len - pos, "&amp;");
        else if (*in == '<') pos += snprintf(out + pos, out_len - pos, "&lt;");
        else if (*in == '>') pos += snprintf(out + pos, out_len - pos, "&gt;");
        else if (*in == '"') pos += snprintf(out + pos, out_len - pos, "&quot;");
        else out[pos++] = *in;
    }
    out[pos] = '\0';
}

static void send_chunk(httpd_req_t *req, const char *fmt, ...)
{
    char chunk[768];
    va_list args;
    va_start(args, fmt);
    vsnprintf(chunk, sizeof(chunk), fmt, args);
    va_end(args);
    httpd_resp_sendstr_chunk(req, chunk);
}

static void upload_status_set(esp_err_t err, int http_status, size_t bytes, const char *source)
{
    last_upload_ms = esp_timer_get_time() / 1000;
    if (err == ESP_OK && (http_status == 200 || http_status == 204)) {
        snprintf(last_upload_summary, sizeof(last_upload_summary),
                 "%s upload OK. HTTP %d, %u bytes sent.", source, http_status, (unsigned)bytes);
    } else if (err == ESP_ERR_INVALID_STATE) {
        snprintf(last_upload_summary, sizeof(last_upload_summary),
                 "%s upload skipped: Prusa token or fingerprint is missing.", source);
    } else if (http_status > 0) {
        snprintf(last_upload_summary, sizeof(last_upload_summary),
                 "%s upload failed. HTTP %d, error %s.", source, http_status, esp_err_to_name(err));
    } else {
        snprintf(last_upload_summary, sizeof(last_upload_summary),
                 "%s upload failed: %s.", source, esp_err_to_name(err));
    }
}

static void send_select(httpd_req_t *req, const char *name, int current)
{
    const framesize_t sizes[] = {
        FRAMESIZE_QQVGA, FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA,
        FRAMESIZE_XGA, FRAMESIZE_SXGA, FRAMESIZE_UXGA
    };
    send_chunk(req, "<label><span class=\"label-row\">Resolution <span class=\"info\" title=\"Photo size. Smaller values like QQVGA are fast and light; larger values like UXGA give more detail but upload slower and use more memory.\">i</span></span><select name=\"%s\">", name);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        send_chunk(req, "<option value=\"%d\"%s>%s</option>", sizes[i],
                   current == sizes[i] ? " selected" : "", framesize_name(sizes[i]));
    }
    httpd_resp_sendstr_chunk(req, "</select></label>");
}

static esp_err_t index_handler(httpd_req_t *req)
{
    char ssid[80], token[140], fingerprint[140], endpoint[220], notice[260], upload_summary[260];
    html_escape(ssid, sizeof(ssid), settings.wifi_ssid);
    html_escape(token, sizeof(token), settings.prusa_token);
    html_escape(fingerprint, sizeof(fingerprint), settings.prusa_fingerprint);
    html_escape(endpoint, sizeof(endpoint), settings.prusa_endpoint);
    html_escape(notice, sizeof(notice), page_notice);
    html_escape(upload_summary, sizeof(upload_summary), last_upload_summary);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32-CAM Prusa Connect</title><style>"
        "body{font-family:system-ui,Segoe UI,sans-serif;margin:0;background:#f6f7f9;color:#17202a}"
        "main{max-width:1040px;margin:auto;padding:20px}.top{display:flex;gap:16px;align-items:flex-end;justify-content:space-between;flex-wrap:wrap}"
        "h1{font-size:24px;margin:0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:14px;margin-top:16px}"
        "section{background:white;border:1px solid #d8dee8;border-radius:8px;padding:16px}label{display:grid;gap:6px;margin:10px 0;font-size:14px}"
        ".label-row{display:flex;align-items:center;gap:6px}.info{display:inline-grid;place-items:center;width:16px;height:16px;border-radius:50%;background:#e7edf5;color:#2d4b66;font-size:11px;font-weight:700;cursor:help}"
        "input,select{font:inherit;padding:9px;border:1px solid #b8c0cc;border-radius:6px;box-sizing:border-box;width:100%}"
        "button,a.btn{font:inherit;display:inline-block;padding:10px 13px;border:0;border-radius:6px;background:#d94b2b;color:white;text-decoration:none;cursor:pointer}"
        "a.secondary{background:#596779}a.danger{background:#9b2f2f}"
        ".status{margin-top:16px;padding:10px 12px;border:1px solid #c8d3e1;border-radius:8px;background:#fff;color:#24364a}"
        ".notice{margin-top:16px;padding:10px 12px;border:1px solid #e2c45f;border-radius:8px;background:#fff7d6;color:#4d3a00}"
        ".top-actions{display:flex;gap:10px;align-items:flex-start;flex-wrap:wrap}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:16px}"
        "a.snapshot{background:#2f6f8f}a.stream{background:#28785f}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
        ".wide{grid-column:1/-1}"
        ".sensor-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:0 10px}.sensor-grid label{margin:8px 0}"
        ".preview{width:100%;margin-top:16px;background:#222;border-radius:8px}small{color:#596779}@media(max-width:760px){.sensor-grid,.row{grid-template-columns:1fr}}"
        "</style></head><body><main><div class=\"top\"><div><h1>ESP32-CAM Prusa Connect</h1>");
    send_chunk(req, "<small>Device IP: %s | mDNS: http://%s.local</small><form method=\"post\" action=\"/save\"><div class=\"actions\"><button type=\"submit\">Save settings</button> <a class=\"btn\" href=\"/send\">Test upload now</a> <a class=\"btn secondary\" href=\"/reboot\">Reboot</a> <a class=\"btn danger\" href=\"/reset\" onclick=\"return confirm('Clear all saved settings and reboot?')\">Factory reset</a></div></div><div class=\"top-actions\"><a class=\"btn snapshot\" href=\"/jpg\">Snapshot</a> <a class=\"btn stream\" href=\"/stream\">Stream</a></div></div>", ip_text, MDNS_HOSTNAME);
    if (page_notice[0]) {
        send_chunk(req, "<div class=\"notice\">%s</div>", notice);
        page_notice[0] = '\0';
    }
    int64_t age_s = last_upload_ms ? ((esp_timer_get_time() / 1000) - last_upload_ms) / 1000 : -1;
    if (age_s >= 0) {
        send_chunk(req, "<div class=\"status\">Last upload: %s (%lld seconds ago)</div>", upload_summary, age_s);
    } else {
        send_chunk(req, "<div class=\"status\">Last upload: %s</div>", upload_summary);
    }
    httpd_resp_sendstr_chunk(req, "<img class=\"preview\" src=\"/jpg\" alt=\"camera preview\"><div class=\"grid\"><section><h2>Network</h2>");
    send_chunk(req, "<label><span class=\"label-row\">Wi-Fi SSID <span class=\"info\" title=\"The name of the Wi-Fi network the camera should join. Pick a scanned network or type a hidden network name manually.\">i</span></span><input name=\"wifi_ssid\" list=\"wifi_networks\" maxlength=\"32\" value=\"%s\"></label><datalist id=\"wifi_networks\">", ssid);

    wifi_ap_record_t aps[20] = {0};
    uint16_t ap_count = 20;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK &&
        esp_wifi_scan_get_ap_records(&ap_count, aps) == ESP_OK) {
        for (int i = 0; i < ap_count; i++) {
            char scanned_ssid[80];
            html_escape(scanned_ssid, sizeof(scanned_ssid), (const char *)aps[i].ssid);
            send_chunk(req, "<option value=\"%s\">%d dBm%s</option>",
                       scanned_ssid, aps[i].rssi,
                       aps[i].authmode == WIFI_AUTH_OPEN ? " open" : "");
        }
    }
    httpd_resp_sendstr_chunk(req, "</datalist><label><span class=\"label-row\">Wi-Fi password <span class=\"info\" title=\"The Wi-Fi password. Leave blank for open networks; saving blank clears any stored password.\">i</span></span><input name=\"wifi_pass\" type=\"password\" maxlength=\"64\" placeholder=\"Blank for open Wi-Fi\"></label></section><section><h2>Prusa Connect</h2>");
    send_chunk(req, "<label><span class=\"label-row\">Token <span class=\"info\" title=\"The camera token from Prusa Connect. It authorizes this ESP32-CAM to upload snapshots to your Prusa Connect camera.\">i</span></span><input name=\"prusa_token\" type=\"password\" maxlength=\"95\" value=\"%s\"></label>", token);
    send_chunk(req, "<label><span class=\"label-row\">Fingerprint <span class=\"info\" title=\"The camera fingerprint from Prusa Connect. It identifies this camera alongside the token.\">i</span></span><input name=\"prusa_fingerprint\" type=\"password\" maxlength=\"95\" value=\"%s\"></label>", fingerprint);
    send_chunk(req, "<label><span class=\"label-row\">Endpoint <span class=\"info\" title=\"The Prusa Connect camera upload URL. The default is normally correct unless Prusa changes the API or you use a proxy.\">i</span></span><input name=\"prusa_endpoint\" maxlength=\"159\" value=\"%s\"></label>", endpoint);
    send_chunk(req, "<label><span class=\"label-row\">Upload interval seconds <span class=\"info\" title=\"How often the ESP32-CAM sends a snapshot to Prusa Connect. Shorter intervals feel more live but use more bandwidth and power.\">i</span></span><input name=\"upload_interval_s\" type=\"number\" min=\"10\" max=\"3600\" value=\"%lu\"></label></section>", (unsigned long)settings.upload_interval_s);
    httpd_resp_sendstr_chunk(req, "<section><h2>Image</h2>");
    send_select(req, "framesize", settings.framesize);
    send_chunk(req, "<label><span class=\"label-row\">JPEG quality 4-63 <span class=\"info\" title=\"JPG compression. 4 is highest quality and largest file; 10-15 is a good balance; 63 is smallest file and lowest quality.\">i</span></span><input name=\"jpeg_quality\" type=\"number\" min=\"4\" max=\"63\" value=\"%u\"></label>", settings.jpeg_quality);
    send_chunk(req, "<div class=\"row\"><label><span class=\"label-row\">Brightness <span class=\"info\" title=\"Overall lightness. -2 is darkest; 0 is normal; 2 is brightest.\">i</span></span><input name=\"brightness\" type=\"number\" min=\"-2\" max=\"2\" value=\"%d\"></label>", settings.brightness);
    send_chunk(req, "<label><span class=\"label-row\">Contrast <span class=\"info\" title=\"Dark-to-light separation. -2 is flatter and softer; 0 is normal; 2 is punchier but can lose detail.\">i</span></span><input name=\"contrast\" type=\"number\" min=\"-2\" max=\"2\" value=\"%d\"></label></div>", settings.contrast);
    send_chunk(req, "<div class=\"row\"><label><span class=\"label-row\">Saturation <span class=\"info\" title=\"Color strength. -2 is muted; 0 is normal; 2 is very colorful and can look unnatural.\">i</span></span><input name=\"saturation\" type=\"number\" min=\"-2\" max=\"2\" value=\"%d\"></label>", settings.saturation);
    send_chunk(req, "<label><span class=\"label-row\">Effect <span class=\"info\" title=\"Built-in effect. 0 normal; 1 negative; 2 black and white; 3 reddish; 4 greenish; 5 blue; 6 retro.\">i</span></span><input name=\"special_effect\" type=\"number\" min=\"0\" max=\"6\" value=\"%u\"></label></div></section>", settings.special_effect);
    httpd_resp_sendstr_chunk(req, "<section class=\"wide\"><h2>Sensor</h2><div class=\"sensor-grid\">");
#define FIELD_U(name, label, min, max, help) send_chunk(req, "<label><span class=\"label-row\">" label " <span class=\"info\" title=\"" help "\">i</span></span><input name=\"" #name "\" type=\"number\" min=\"" #min "\" max=\"" #max "\" value=\"%u\"></label>", settings.name)
#define FIELD_I(name, label, min, max, help) send_chunk(req, "<label><span class=\"label-row\">" label " <span class=\"info\" title=\"" help "\">i</span></span><input name=\"" #name "\" type=\"number\" min=\"" #min "\" max=\"" #max "\" value=\"%d\"></label>", settings.name)
    FIELD_U(hmirror, "Horizontal mirror", 0, 1, "0 off; 1 on. Flips the image left-to-right.");
    FIELD_U(vflip, "Vertical flip", 0, 1, "0 off; 1 on. Flips the image upside down.");
    FIELD_U(awb, "Auto white balance", 0, 1, "0 manual color balance; 1 automatic color balance. Usually leave on.");
    FIELD_U(awb_gain, "AWB gain", 0, 1, "0 fixed white-balance gain; 1 lets auto white balance adjust red and blue gain. Usually leave on with AWB.");
    FIELD_U(wb_mode, "WB mode", 0, 4, "White balance preset when manual color is used. 0 auto/default; 1 sunny; 2 cloudy; 3 office; 4 home.");
    FIELD_U(aec, "Auto exposure", 0, 1, "0 manual exposure using AEC value; 1 automatic exposure. Usually leave on unless lighting is fixed.");
    FIELD_U(aec2, "AEC DSP", 0, 1, "0 off; 1 on. Extra digital auto-exposure processing; try toggling if exposure pulses or looks wrong.");
    FIELD_I(ae_level, "AE level", -2, 2, "Auto-exposure brightness bias. -2 darker; 0 normal; 2 brighter.");
    FIELD_U(aec_value, "AEC value", 0, 1200, "Manual exposure when Auto exposure is 0. 0 darkest/fastest; 1200 brightest/slowest and may blur.");
    FIELD_U(agc, "Auto gain", 0, 1, "0 manual gain using AGC gain; 1 automatic gain. Gain brightens dark scenes but adds speckled noise.");
    FIELD_U(agc_gain, "AGC gain", 0, 30, "Manual gain when Auto gain is 0. 0 lowest gain and least noise; 30 brightest and most noise.");
    FIELD_U(gainceiling, "Gain ceiling", 0, 6, "Maximum auto gain. 0=2x least noise; 1=4x; 2=8x; 3=16x; 4=32x; 5=64x; 6=128x brightest/noisiest.");
    FIELD_U(bpc, "Black pixel correction", 0, 1, "0 off; 1 on. Fixes stuck dark pixels.");
    FIELD_U(wpc, "White pixel correction", 0, 1, "0 off; 1 on. Fixes stuck bright pixels.");
    FIELD_U(raw_gma, "Gamma correction", 0, 1, "0 off; 1 on. Brightens/darkens mid-tones for a more natural image.");
    FIELD_U(lenc, "Lens correction", 0, 1, "0 off; 1 on. Helps compensate for darker corners from the small lens.");
    FIELD_U(dcw, "Downsize enable", 0, 1, "0 off; 1 on. Uses sensor downsampling at smaller resolutions; usually leave on for cleaner resized images.");
    FIELD_U(colorbar, "Color bar test", 0, 1, "0 normal camera image; 1 color test pattern for checking the sensor.");
#undef FIELD_U
#undef FIELD_I
    httpd_resp_sendstr_chunk(req, "</div></section></div></form></main></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++, w++) {
        if (*r == '+') {
            *w = ' ';
        } else if (*r == '%' && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
            *w = (char)((hexval(r[1]) << 4) | hexval(r[2]));
            r += 2;
        } else {
            *w = *r;
        }
    }
    *w = '\0';
}

static void copy_value(char *dst, size_t dst_len, const char *src)
{
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static int clamp_int(int value, int min, int max)
{
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static void form_apply_pair(char *key, char *value)
{
    url_decode(key);
    url_decode(value);
    if (!strcmp(key, "wifi_ssid")) copy_value(settings.wifi_ssid, sizeof(settings.wifi_ssid), value);
    else if (!strcmp(key, "wifi_pass")) copy_value(settings.wifi_pass, sizeof(settings.wifi_pass), value);
    else if (!strcmp(key, "prusa_token")) copy_value(settings.prusa_token, sizeof(settings.prusa_token), value);
    else if (!strcmp(key, "prusa_fingerprint")) copy_value(settings.prusa_fingerprint, sizeof(settings.prusa_fingerprint), value);
    else if (!strcmp(key, "prusa_endpoint")) copy_value(settings.prusa_endpoint, sizeof(settings.prusa_endpoint), value);
    else if (!strcmp(key, "upload_interval_s")) settings.upload_interval_s = clamp_int(atoi(value), 10, 3600);
    else if (!strcmp(key, "framesize")) settings.framesize = clamp_int(atoi(value), FRAMESIZE_QQVGA, FRAMESIZE_UXGA);
    else if (!strcmp(key, "jpeg_quality")) settings.jpeg_quality = clamp_int(atoi(value), 4, 63);
    else if (!strcmp(key, "brightness")) settings.brightness = clamp_int(atoi(value), -2, 2);
    else if (!strcmp(key, "contrast")) settings.contrast = clamp_int(atoi(value), -2, 2);
    else if (!strcmp(key, "saturation")) settings.saturation = clamp_int(atoi(value), -2, 2);
    else if (!strcmp(key, "special_effect")) settings.special_effect = clamp_int(atoi(value), 0, 6);
    else if (!strcmp(key, "wb_mode")) settings.wb_mode = clamp_int(atoi(value), 0, 4);
    else if (!strcmp(key, "awb")) settings.awb = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "awb_gain")) settings.awb_gain = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "aec")) settings.aec = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "aec2")) settings.aec2 = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "ae_level")) settings.ae_level = clamp_int(atoi(value), -2, 2);
    else if (!strcmp(key, "aec_value")) settings.aec_value = clamp_int(atoi(value), 0, 1200);
    else if (!strcmp(key, "agc")) settings.agc = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "agc_gain")) settings.agc_gain = clamp_int(atoi(value), 0, 30);
    else if (!strcmp(key, "gainceiling")) settings.gainceiling = clamp_int(atoi(value), 0, 6);
    else if (!strcmp(key, "bpc")) settings.bpc = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "wpc")) settings.wpc = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "raw_gma")) settings.raw_gma = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "lenc")) settings.lenc = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "hmirror")) settings.hmirror = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "vflip")) settings.vflip = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "dcw")) settings.dcw = clamp_int(atoi(value), 0, 1);
    else if (!strcmp(key, "colorbar")) settings.colorbar = clamp_int(atoi(value), 0, 1);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form length");
        return ESP_FAIL;
    }
    char *body = calloc(1, len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read failed");
        return ESP_FAIL;
    }

    char *saveptr = NULL;
    for (char *pair = strtok_r(body, "&", &saveptr); pair; pair = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            form_apply_pair(pair, eq + 1);
        }
    }
    free(body);
    settings_save();
    apply_camera_settings();

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Saved");
    return ESP_OK;
}

static esp_err_t jpg_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return err;
}

static esp_err_t upload_snapshot(const char *source)
{
    if (!settings.prusa_token[0] || !settings.prusa_fingerprint[0]) {
        upload_status_set(ESP_ERR_INVALID_STATE, 0, 0, source);
        return ESP_ERR_INVALID_STATE;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        upload_status_set(ESP_FAIL, 0, 0, source);
        return ESP_FAIL;
    }

    esp_http_client_config_t cfg = {
        .url = settings.prusa_endpoint,
        .method = HTTP_METHOD_PUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "image/jpg");
    esp_http_client_set_header(client, "token", settings.prusa_token);
    esp_http_client_set_header(client, "fingerprint", settings.prusa_fingerprint);
    esp_http_client_set_post_field(client, (const char *)fb->buf, fb->len);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Prusa snapshot upload: err=%s status=%d bytes=%u", esp_err_to_name(err), status, (unsigned)fb->len);
    upload_status_set(err, status, fb->len, source);
    esp_http_client_cleanup(client);
    esp_camera_fb_return(fb);
    return (err == ESP_OK && (status == 200 || status == 204)) ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_now_handler(httpd_req_t *req)
{
    esp_err_t err = upload_snapshot("Manual test");
    snprintf(page_notice, sizeof(page_notice), "%s", err == ESP_OK ? "Test upload completed." : "Test upload failed. See the upload status line below.");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Test complete");
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><meta http-equiv=\"refresh\" content=\"8;url=/\"><title>Rebooting</title></head><body><h1>Rebooting ESP32-CAM</h1><p>Reconnect to the camera in a few seconds.</p></body></html>");
    schedule_restart();
    return ESP_OK;
}

static esp_err_t reset_handler(httpd_req_t *req)
{
    esp_err_t err = settings_reset_saved();
    httpd_resp_set_type(req, "text/html");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><meta http-equiv=\"refresh\" content=\"10;url=http://192.168.4.1/\"><title>Factory reset</title></head><body><h1>Settings cleared</h1><p>The ESP32-CAM is rebooting. Join Wi-Fi network ESP32CAM-Setup, then open http://192.168.4.1/.</p></body></html>");
        schedule_restart();
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not clear settings");
    }
    return ESP_OK;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    static const char *boundary = "\r\n--frame\r\n";
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            return ESP_FAIL;
        }
        char header[96];
        int header_len = snprintf(header, sizeof(header), "%sContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", boundary, (unsigned)fb->len);
        if (httpd_resp_send_chunk(req, header, header_len) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    return ESP_OK;
}

static void web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler};
    httpd_uri_t jpg = {.uri = "/jpg", .method = HTTP_GET, .handler = jpg_handler};
    httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler};
    httpd_uri_t send = {.uri = "/send", .method = HTTP_GET, .handler = send_now_handler};
    httpd_uri_t reboot = {.uri = "/reboot", .method = HTTP_GET, .handler = reboot_handler};
    httpd_uri_t reset = {.uri = "/reset", .method = HTTP_GET, .handler = reset_handler};
    httpd_register_uri_handler(server, &index);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &jpg);
    httpd_register_uri_handler(server, &stream);
    httpd_register_uri_handler(server, &send);
    httpd_register_uri_handler(server, &reboot);
    httpd_register_uri_handler(server, &reset);
}

static void uploader_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(settings.upload_interval_s * 1000));
        EventBits_t bits = xEventGroupGetBits(wifi_events);
        if ((bits & WIFI_CONNECTED_BIT) && settings.prusa_token[0] && settings.prusa_fingerprint[0]) {
            upload_snapshot("Automatic");
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    settings_load();
    ESP_ERROR_CHECK(camera_init());
    wifi_start();
    mdns_start();
    web_start();
    xTaskCreate(uploader_task, "prusa_uploader", 8192, NULL, 5, NULL);
}
