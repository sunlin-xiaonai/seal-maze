#include "web.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#if __has_include("mdns.h")
#include "mdns.h"
#define WEB_MDNS 1
#endif

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define LEVEL_MAX 2048
#define STATE_MAX 320
#define MAX_PAGES 4

static httpd_handle_t s_server;
static SemaphoreHandle_t s_mu;
static char s_level[LEVEL_MAX], s_state[STATE_MAX];
static volatile bool s_state_queued, s_level_new;
static volatile int s_clients;
static volatile uint32_t s_sent, s_dropped, s_send_us_max;  // web_stats()

static esp_err_t index_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);  // EMBED_TXTFILES adds a NUL
}

static esp_err_t send_text(httpd_req_t *req, int fd, const char *text) {
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text), .final = true};
    return req ? httpd_ws_send_frame(req, &f) : httpd_ws_send_frame_async(s_server, fd, &f);
}

static esp_err_t send_level(httpd_req_t *req) {
    char *level = malloc(LEVEL_MAX);
    if (!level) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strcpy(level, s_level);
    xSemaphoreGive(s_mu);
    esp_err_t err = level[0] ? send_text(req, 0, level) : ESP_OK;
    free(level);
    return err;
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {  // the handshake
        ESP_LOGI(TAG, "page connected");
        return ESP_OK;
    }
    // A page says "level" once it is open and gets the level back; anything else is read and dropped.
    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK || f.len == 0 || f.len > 256) return err;
    uint8_t buf[257];
    f.payload = buf;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err != ESP_OK) return err;
    buf[f.len] = 0;
    if (f.type == HTTPD_WS_TYPE_TEXT && strstr((const char *)buf, "\"level\"")) return send_level(req);
    return ESP_OK;
}

/** In the server's task: the latest state (and a new level) to every page. */
static void send_work(void *arg) {
    (void)arg;
    static char state[STATE_MAX];
    char *level = NULL;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strcpy(state, s_state);
    if (s_level_new && (level = malloc(LEVEL_MAX))) strcpy(level, s_level), s_level_new = false;
    xSemaphoreGive(s_mu);
    int fds[MAX_PAGES + 2];
    size_t n = MAX_PAGES + 2;
    int pages = 0;
    if (httpd_get_client_list(s_server, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
            if (level) send_text(NULL, fds[i], level);
            const int64_t t0 = esp_timer_get_time();
            if (state[0] && send_text(NULL, fds[i], state) != ESP_OK) continue;
            const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            if (us > s_send_us_max) s_send_us_max = us;
            s_sent++;
            pages++;
        }
    }
    free(level);
    s_clients = pages;
    s_state_queued = false;
}

static void queue_send(void) {
    if (!s_server) return;
    if (s_state_queued) {
        s_dropped++;
        return;
    }
    s_state_queued = true;
    if (httpd_queue_work(s_server, send_work, NULL) != ESP_OK) s_state_queued = false;
}

/** Every connection: no Nagle, so each short state goes out at once instead of waiting to be batched. */
static esp_err_t on_open(httpd_handle_t hd, int fd) {
    (void)hd;
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
}

esp_err_t web_start(void) {
    if (s_server) return ESP_OK;
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = MAX_PAGES;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.open_fn = on_open;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "server: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }
    const httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_get};
    const httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    httpd_register_uri_handler(s_server, &index);
    httpd_register_uri_handler(s_server, &ws);
#ifdef WEB_MDNS
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("maze");
        mdns_instance_name_set("Seal Maze");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
#endif
    ESP_LOGI(TAG, "serving the big screen");
    return ESP_OK;
}

bool web_running(void) { return s_server != NULL; }

void web_set_level(const char *json) {
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strlcpy(s_level, json, sizeof(s_level));
    s_level_new = true;
    xSemaphoreGive(s_mu);
    queue_send();
}

void web_publish(const char *json) {
    if (!s_server) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strlcpy(s_state, json, sizeof(s_state));
    xSemaphoreGive(s_mu);
    queue_send();
}

int web_clients(void) { return s_clients; }

void web_stats(uint32_t *sent, uint32_t *dropped, uint32_t *send_us_max) {
    *sent = s_sent, *dropped = s_dropped, *send_us_max = s_send_us_max;
    s_send_us_max = 0;
}
