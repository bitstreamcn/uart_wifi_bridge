#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/uart.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* ================= 按你的环境修改 ================= */
#define WIFI_SSID     "zlang_autopilot"
#define WIFI_PASS     "lizenglang"
#define SERVER_HOST   "192.168.4.1"   /* 支持 IP 或域名 */
#define SERVER_PORT   26413
#define UART_PORT     UART_NUM_1
#define UART_BAUD     115200
#define UART_TX_PIN   17
#define UART_RX_PIN   18
#define BUF_SIZE      2048
/* ================================================= */

static const char *TAG = "bridge";
static EventGroupHandle_t s_wifi_eg;
#define WIFI_GOT_IP_BIT  BIT0
static int s_sock = -1;
static SemaphoreHandle_t s_sock_mtx;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_eg, WIFI_GOT_IP_BIT);
        ESP_LOGW(TAG, "WiFi 断开，重连中...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_eg, WIFI_GOT_IP_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  /* 关省电，透传延迟更低 */
}

static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE * 2, BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

/* 连接维护 + 下行 TCP -> UART */
static void tcp_task(void *arg)
{
    char rxbuf[BUF_SIZE];
    while (1) {
        xEventGroupWaitBits(s_wifi_eg, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        struct sockaddr_in dst = { 0 };
        dst.sin_family = AF_INET;
        dst.sin_port = htons(SERVER_PORT);

        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        if (getaddrinfo(SERVER_HOST, NULL, &hints, &res) != 0 || res == NULL) {
            ESP_LOGE(TAG, "域名解析失败");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        dst.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        ESP_LOGI(TAG, "连接 %s:%d ...", SERVER_HOST, SERVER_PORT);
        if (sock < 0 || connect(sock, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
            ESP_LOGE(TAG, "connect 失败: errno %d", errno);
            if (sock >= 0) close(sock);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        xSemaphoreTake(s_sock_mtx, portMAX_DELAY);
        s_sock = sock;
        xSemaphoreGive(s_sock_mtx);
        ESP_LOGI(TAG, "透传已建立");

        while (1) {
            int len = recv(sock, rxbuf, sizeof(rxbuf), 0);
            if (len == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                continue;                       /* 空闲超时，继续等 */
            if (len <= 0) {
                ESP_LOGW(TAG, "连接断开 (len=%d errno=%d)", len, errno);
                break;
            }
            uart_write_bytes(UART_PORT, rxbuf, len);
        }

        xSemaphoreTake(s_sock_mtx, portMAX_DELAY);
        close(sock);
        s_sock = -1;
        xSemaphoreGive(s_sock_mtx);
        vTaskDelay(pdMS_TO_TICKS(3000));        /* 断线后自动重连 */
    }
}

/* 上行 UART -> TCP */
static void uart_task(void *arg)
{
    uint8_t buf[BUF_SIZE];
    while (1) {
        int len = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (len <= 0) continue;

        xSemaphoreTake(s_sock_mtx, portMAX_DELAY);
        int sock = s_sock;
        if (sock >= 0) {
            int sent = 0;
            while (sent < len) {                /* 循环发，保证写完 */
                int n = send(sock, buf + sent, len - sent, 0);
                if (n < 0) { ESP_LOGW(TAG, "send 失败: errno %d", errno); break; }
                sent += n;
            }
        } /* 没连上时丢弃；要缓存可在此改为入队列 */
        xSemaphoreGive(s_sock_mtx);
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_sock_mtx = xSemaphoreCreateMutex();
    uart_init();
    wifi_init();
    xTaskCreate(tcp_task, "tcp",  4096, NULL, 5, NULL);
    xTaskCreate(uart_task, "uart", 4096, NULL, 5, NULL);
}
