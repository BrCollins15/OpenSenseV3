#include "web_server.h"
#include "opensense.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "sd_card.h"
#include "port_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "esp_wifi.h"
#include <stdio.h>

#define TAG          "WEB_SERVER"
#define RECV_BUF_MAX 4096

/* Embedded static assets — injected by the linker from main/web/ */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t style_css_gz_start[] asm("_binary_style_css_gz_start");
extern const uint8_t style_css_gz_end[]   asm("_binary_style_css_gz_end");
extern const uint8_t app_js_gz_start[]    asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[]      asm("_binary_app_js_gz_end");

/* PNG images — place left.png and right.png in main/web/ before building */
extern const uint8_t left_png_start[]   asm("_binary_left_png_start");
extern const uint8_t left_png_end[]     asm("_binary_left_png_end");
extern const uint8_t right_png_start[] asm("_binary_right_png_start");
extern const uint8_t right_png_end[]   asm("_binary_right_png_end");

static char *recv_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > RECV_BUF_MAX) return NULL;
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf + received, total - received);
        if (n <= 0) { free(buf); return NULL; }
        received += n;
    }
    buf[total] = '\0';
    return buf;
}

static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
}

static void send_error_json(httpd_req_t *req, int code, const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error");
    send_json(req, buf);
}

/* ── Static asset handlers ──────────────────────────────────────────────── */
static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}
static esp_err_t handler_style_css(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    httpd_resp_send(req, (const char *)style_css_gz_start, style_css_gz_end - style_css_gz_start);
    return ESP_OK;
}
static esp_err_t handler_app_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    httpd_resp_send(req, (const char *)app_js_gz_start, app_js_gz_end - app_js_gz_start);
    return ESP_OK;
}
static esp_err_t handler_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── PNG image handlers ─────────────────────────────────────────────────── */
static esp_err_t handler_left_png(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, (const char *)left_png_start,
                    left_png_end - left_png_start);
    return ESP_OK;
}
static esp_err_t handler_right_png(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, (const char *)right_png_start,
                    right_png_end - right_png_start);
    return ESP_OK;
}

/* ── GET /api/config ────────────────────────────────────────────────────── */
static esp_err_t handler_get_config(httpd_req_t *req)
{
    sensor_config_t cfgs[MAX_SENSORS];
    if (xSemaphoreTake(g_sensor_state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        send_error_json(req, 500, "mutex timeout"); return ESP_OK;
    }
    memcpy(cfgs, g_sensor_state.configs, sizeof(cfgs));
    xSemaphoreGive(g_sensor_state.mutex);

    cJSON *root = cJSON_CreateObject();
    cJSON *sensors = cJSON_AddArrayToObject(root, "sensors");

    for (int p = 0; p < MAX_SENSORS; p++) {
        const sensor_config_t *c = &cfgs[p];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "port",        p);
        cJSON_AddBoolToObject  (obj, "enabled",     c->enabled);
        cJSON_AddNumberToObject(obj, "sensor_type", (int)c->sensor_type);
        cJSON_AddNumberToObject(obj, "i2c_addr",    c->i2c_addr);
        cJSON_AddBoolToObject  (obj, "voltage_5v",  c->voltage_5v);
        cJSON_AddStringToObject(obj, "name",        c->name);
        cJSON_AddNumberToObject(obj, "num_regs",    c->num_regs);

        cJSON *regs_arr = cJSON_AddArrayToObject(obj, "regs");
        for (int r = 0; r < MAX_REGS_PER_SENSOR; r++) {
            const reg_config_t *rc = &c->regs[r];
            cJSON *reg = cJSON_CreateObject();
            cJSON_AddNumberToObject(reg, "reg_addr",          rc->reg_addr);
            cJSON_AddNumberToObject(reg, "byte_len",          rc->byte_len);
            cJSON_AddBoolToObject  (reg, "is_signed",         rc->is_signed);
            cJSON_AddBoolToObject  (reg, "big_endian",        rc->big_endian);
            cJSON_AddNumberToObject(reg, "scale",             (double)rc->scale);
            cJSON_AddNumberToObject(reg, "offset",            (double)rc->offset);
            cJSON_AddStringToObject(reg, "expr",              rc->expr);
            cJSON_AddStringToObject(reg, "label",             rc->label);
            cJSON_AddStringToObject(reg, "unit",              rc->unit);
            cJSON_AddBoolToObject  (reg, "write_before_read", rc->write_before_read);
            cJSON_AddNumberToObject(reg, "write_len",         rc->write_len);
            cJSON *wb = cJSON_CreateArray();
            for (int wi = 0; wi < 4; wi++) cJSON_AddItemToArray(wb, cJSON_CreateNumber(rc->write_buf[wi]));
            cJSON_AddItemToObject  (reg, "write_buf",         wb);
            cJSON_AddNumberToObject(reg, "delay_us",          rc->delay_us);
            cJSON_AddNumberToObject(reg, "data_shift",        rc->data_shift);
            cJSON_AddNumberToObject(reg, "data_mask",         rc->data_mask);
            cJSON_AddNumberToObject(reg, "valid_bits",        rc->valid_bits);
            cJSON_AddBoolToObject  (reg, "direct_read",       rc->direct_read);
            cJSON_AddNumberToObject(reg, "merge_with",         rc->merge_with);
            cJSON_AddNumberToObject(reg, "merge_shift",        rc->merge_shift);
            cJSON_AddItemToArray(regs_arr, reg);
        }
        cJSON_AddItemToArray(sensors, obj);
    }

    char *json = cJSON_PrintUnformatted(root);
    send_json(req, json);
    cJSON_free(json); cJSON_Delete(root);
    return ESP_OK;
}

/* ── POST /api/config ───────────────────────────────────────────────────── */
static esp_err_t handler_post_config(httpd_req_t *req)
{
    char *body = recv_body(req);
    if (!body) { send_error_json(req, 400, "body too large or empty"); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { send_error_json(req, 400, "invalid JSON"); return ESP_OK; }

    cJSON *port_j = cJSON_GetObjectItem(root, "port");
    if (!cJSON_IsNumber(port_j) || port_j->valueint < 0 || port_j->valueint >= MAX_SENSORS) {
        cJSON_Delete(root); send_error_json(req, 400, "missing or invalid port"); return ESP_OK;
    }
    int port = port_j->valueint;

    sensor_config_t cfg = {0};
    cJSON *enabled  = cJSON_GetObjectItem(root, "enabled");
    cJSON *stype    = cJSON_GetObjectItem(root, "sensor_type");
    cJSON *addr     = cJSON_GetObjectItem(root, "i2c_addr");
    cJSON *volt5    = cJSON_GetObjectItem(root, "voltage_5v");
    cJSON *name     = cJSON_GetObjectItem(root, "name");
    cJSON *num_regs = cJSON_GetObjectItem(root, "num_regs");
    cJSON *regs_arr = cJSON_GetObjectItem(root, "regs");

    cfg.enabled     = cJSON_IsTrue(enabled);
    cfg.sensor_type = cJSON_IsNumber(stype) ? (sensor_type_t)stype->valueint : SENSOR_TYPE_I2C;
    cfg.i2c_addr    = cJSON_IsNumber(addr)  ? (uint8_t)addr->valueint  : 0;
    cfg.voltage_5v  = cJSON_IsTrue(volt5);
    cfg.num_regs    = cJSON_IsNumber(num_regs) ? (uint8_t)num_regs->valueint : 0;
    if (cfg.num_regs > MAX_REGS_PER_SENSOR) cfg.num_regs = MAX_REGS_PER_SENSOR;
    if (cJSON_IsString(name) && name->valuestring)
        strncpy(cfg.name, name->valuestring, sizeof(cfg.name) - 1);

    if (cJSON_IsArray(regs_arr)) {
        int ri = 0; cJSON *reg;
        cJSON_ArrayForEach(reg, regs_arr) {
            if (ri >= MAX_REGS_PER_SENSOR) break;
            reg_config_t *rc = &cfg.regs[ri];
            cJSON *ra  = cJSON_GetObjectItem(reg, "reg_addr");
            cJSON *bl  = cJSON_GetObjectItem(reg, "byte_len");
            cJSON *sig = cJSON_GetObjectItem(reg, "is_signed");
            cJSON *be  = cJSON_GetObjectItem(reg, "big_endian");
            cJSON *sc  = cJSON_GetObjectItem(reg, "scale");
            cJSON *off = cJSON_GetObjectItem(reg, "offset");
            cJSON *ex  = cJSON_GetObjectItem(reg, "expr");
            cJSON *lbl = cJSON_GetObjectItem(reg, "label");
            cJSON *unt = cJSON_GetObjectItem(reg, "unit");
            cJSON *wbr = cJSON_GetObjectItem(reg, "write_before_read");
            cJSON *wln = cJSON_GetObjectItem(reg, "write_len");
            cJSON *wba = cJSON_GetObjectItem(reg, "write_buf");
            cJSON *dly = cJSON_GetObjectItem(reg, "delay_us");
            cJSON *dsh = cJSON_GetObjectItem(reg, "data_shift");
            cJSON *dmk = cJSON_GetObjectItem(reg, "data_mask");
            cJSON *vbt = cJSON_GetObjectItem(reg, "valid_bits");
            cJSON *dr  = cJSON_GetObjectItem(reg, "direct_read");

            rc->reg_addr          = cJSON_IsNumber(ra)  ? (uint8_t)ra->valueint   : 0;
            rc->byte_len          = cJSON_IsNumber(bl)  ? (uint8_t)bl->valueint   : 1;
            rc->is_signed         = cJSON_IsTrue(sig);
            rc->big_endian        = cJSON_IsTrue(be);
            rc->scale             = cJSON_IsNumber(sc)  ? (float)sc->valuedouble  : 1.0f;
            rc->offset            = cJSON_IsNumber(off) ? (float)off->valuedouble : 0.0f;
            rc->expr[0]           = '\0';
            if (cJSON_IsString(ex) && ex->valuestring)
                strncpy(rc->expr, ex->valuestring, sizeof(rc->expr) - 1);
            rc->write_before_read = cJSON_IsTrue(wbr);
            rc->write_len         = cJSON_IsNumber(wln) ? (uint8_t)wln->valueint  : 0;
            rc->delay_us          = cJSON_IsNumber(dly) ? (uint32_t)dly->valuedouble : 0;
            rc->data_shift        = cJSON_IsNumber(dsh) ? (uint8_t)dsh->valueint  : 0;
            rc->data_mask         = cJSON_IsNumber(dmk) ? (uint32_t)dmk->valuedouble : 0;
            rc->valid_bits        = cJSON_IsNumber(vbt) ? (uint8_t)vbt->valueint  : 0;
            rc->direct_read       = cJSON_IsTrue(dr);
            cJSON *mw  = cJSON_GetObjectItem(reg, "merge_with");
            cJSON *ms  = cJSON_GetObjectItem(reg, "merge_shift");
            rc->merge_with  = cJSON_IsNumber(mw) ? (uint8_t)mw->valueint  : 255;
            rc->merge_shift = cJSON_IsNumber(ms) ? (uint8_t)ms->valueint  : 0;
            if (cJSON_IsString(lbl) && lbl->valuestring)
                strncpy(rc->label, lbl->valuestring, sizeof(rc->label) - 1);
            if (cJSON_IsString(unt) && unt->valuestring)
                strncpy(rc->unit,  unt->valuestring, sizeof(rc->unit)  - 1);
            if (cJSON_IsArray(wba)) {
                int wi = 0; cJSON *wb_item;
                cJSON_ArrayForEach(wb_item, wba) {
                    if (wi >= 4) break;
                    rc->write_buf[wi++] = cJSON_IsNumber(wb_item) ? (uint8_t)wb_item->valueint : 0;
                }
            }
            ri++;
        }
    }
    cJSON_Delete(root);

    if (xSemaphoreTake(g_sensor_state.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_sensor_state.configs[port] = cfg;
        g_sensor_state.config_version++;
        xSemaphoreGive(g_sensor_state.mutex);
    }

    if (port >= 3 && port <= 4)
        port_manager_update((uint8_t)port, cfg.enabled, cfg.voltage_5v);

    esp_err_t ret = nvs_config_save_sensor((uint8_t)port, &cfg);
    if (ret == ESP_OK) send_json(req, "{\"ok\":true}");
    else               send_error_json(req, 500, "NVS write failed");
    return ESP_OK;
}

/* ── GET /api/wifi — AP status only ────────────────────────────────────── */
static esp_err_t handler_get_wifi(httpd_req_t *req)
{
    wifi_mgr_status_t st; wifi_mgr_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode",      "ap");
    cJSON_AddBoolToObject  (root, "connected", st.connected);
    cJSON_AddStringToObject(root, "ssid",      st.ssid);
    cJSON_AddStringToObject(root, "ip",        st.ip);
    char *json = cJSON_PrintUnformatted(root);
    send_json(req, json); cJSON_free(json); cJSON_Delete(root);
    return ESP_OK;
}

/* ── SD card handlers ───────────────────────────────────────────────────── */
static esp_err_t handler_get_sdcard(httpd_req_t *req)
{
    sd_status_t s; sd_card_get_status(&s);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject  (root, "mounted",      s.mounted);
    cJSON_AddBoolToObject  (root, "logging",      s.logging);
    cJSON_AddStringToObject(root, "log_path",     s.log_path);
    cJSON_AddNumberToObject(root, "rows_written", (double)s.rows_written);
    cJSON_AddNumberToObject(root, "card_size_mb", (double)s.card_size_mb);
    char *json = cJSON_PrintUnformatted(root);
    send_json(req, json); cJSON_free(json); cJSON_Delete(root);
    return ESP_OK;
}

#define DOWNLOAD_CHUNK 2048

static esp_err_t handler_download_log(httpd_req_t *req)
{
    sd_status_t s; sd_card_get_status(&s);
    if (!s.mounted)  { httpd_resp_set_status(req, "503 Service Unavailable"); httpd_resp_sendstr(req, "SD card not mounted"); return ESP_OK; }
    if (!s.logging || s.log_path[0] == '\0') { httpd_resp_set_status(req, "404 Not Found"); httpd_resp_sendstr(req, "No active log file"); return ESP_OK; }

    FILE *fp = fopen(s.log_path, "r");
    if (!fp) { httpd_resp_set_status(req, "500 Internal Server Error"); httpd_resp_sendstr(req, "Cannot open log"); return ESP_OK; }

    const char *filename = s.log_path;
    for (const char *p = s.log_path; *p; p++) if (*p == '/') filename = p + 1;

    char disp_hdr[256];
    snprintf(disp_hdr, sizeof(disp_hdr), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", disp_hdr);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    static char s_dl_buf[DOWNLOAD_CHUNK];
    size_t bytes;
    while ((bytes = fread(s_dl_buf, 1, DOWNLOAD_CHUNK, fp)) > 0) {
        if (httpd_resp_send_chunk(req, s_dl_buf, (ssize_t)bytes) != ESP_OK) {
            ESP_LOGW(TAG, "Download chunk failed"); break;
        }
    }
    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_erase_sdcard(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (!sd_card_is_mounted()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No SD card mounted\"}");
        return ESP_OK;
    }
    esp_err_t ret = sd_card_erase_and_reset();
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Erase failed\"}");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── WebSocket ──────────────────────────────────────────────────────────── */
static esp_err_t handler_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) { ESP_LOGI(TAG, "WebSocket client connected"); return ESP_OK; }
    uint8_t buf[64] = {0};
    httpd_ws_frame_t pkt = { .type = HTTPD_WS_TYPE_TEXT, .payload = buf, .len = sizeof(buf) };
    httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    return ESP_OK;
}

static esp_err_t httpd_open_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    int nodelay = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0)
        ESP_LOGW("WEB", "TCP_NODELAY failed on fd=%d: %d", sockfd, errno);
    return ESP_OK;
}

esp_err_t web_server_start(httpd_handle_t *out_handle)
{
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15;   /* 12 routes + WS + 2 images */
    config.max_open_sockets = 9;
    config.lru_purge_enable = true;
    config.stack_size       = 12288;
    config.send_wait_timeout = 1;
    config.recv_wait_timeout = 2;
    config.keep_alive_enable   = true;
    config.keep_alive_idle     = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count    = 3;
    config.open_fn = httpd_open_fn;

    httpd_handle_t server;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret)); return ret; }

    static const httpd_uri_t uris[] = {
        { .uri="/",                    .method=HTTP_GET,  .handler=handler_root          },
        { .uri="/style.css",           .method=HTTP_GET,  .handler=handler_style_css     },
        { .uri="/app.js",              .method=HTTP_GET,  .handler=handler_app_js        },
        { .uri="/favicon.ico",         .method=HTTP_GET,  .handler=handler_favicon       },
        { .uri="/left.png",            .method=HTTP_GET,  .handler=handler_left_png      },
        { .uri="/right.png",          .method=HTTP_GET,  .handler=handler_right_png    },
        { .uri="/api/config",          .method=HTTP_GET,  .handler=handler_get_config    },
        { .uri="/api/config",          .method=HTTP_POST, .handler=handler_post_config   },
        { .uri="/api/wifi",            .method=HTTP_GET,  .handler=handler_get_wifi      },
        { .uri="/api/sdcard",          .method=HTTP_GET,  .handler=handler_get_sdcard    },
        { .uri="/api/sdcard/download", .method=HTTP_GET,  .handler=handler_download_log  },
        { .uri="/api/sdcard/erase",    .method=HTTP_POST, .handler=handler_erase_sdcard  },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);

    httpd_uri_t ws_uri = {
        .uri = "/ws", .method = HTTP_GET, .handler = handler_ws,
        .is_websocket = true, .handle_ws_control_frames = false,
    };
    httpd_register_uri_handler(server, &ws_uri);

    ESP_LOGI(TAG, "HTTP server started — dashboard at http://192.168.4.1/");
    *out_handle = server;
    return ESP_OK;
}

void web_server_stop(httpd_handle_t handle)
{
    if (handle) { httpd_stop(handle); ESP_LOGI(TAG, "HTTP server stopped"); }
}
