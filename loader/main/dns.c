#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "dns.h"

static const char *TAG = "dns";
static volatile bool s_running;
static volatile TaskHandle_t s_task;

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "cannot open port 53");
        if (sock >= 0) close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    static const uint8_t answer[] = {
        0xC0, 0x0C,             // name: pointer to the question
        0x00, 0x01, 0x00, 0x01, // type A, class IN
        0x00, 0x00, 0x00, 0x3C, // TTL 60 s
        0x00, 0x04,             // 4 bytes of address
        192, 168, 4, 1,
    };
    uint8_t buf[512];

    while (s_running) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf) - sizeof(answer), 0, (struct sockaddr *)&src, &slen);
        if (len < 12 || (buf[2] & 0x80)) {
            continue;  // timeout, garbage, or not a query
        }
        int p = 12;
        while (p < len && buf[p] != 0) {
            p += buf[p] + 1;
        }
        p += 5;  // terminating zero + QTYPE + QCLASS
        if (p > len) {
            continue;
        }
        uint16_t qtype = (uint16_t)((buf[p - 4] << 8) | buf[p - 3]);

        buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
        buf[3] = 0x00;                    // no error
        buf[4] = 0; buf[5] = 1;           // one question (we drop any extras)
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0;
        memset(buf + 8, 0, 4);
        int n = p;
        if (qtype == 1) {
            memcpy(buf + n, answer, sizeof(answer));
            n += sizeof(answer);
        }
        sendto(sock, buf, n, 0, (struct sockaddr *)&src, slen);
    }

    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

void otaf_dns_start(void)
{
    if (s_task) {
        return;
    }
    s_running = true;
    TaskHandle_t t;
    xTaskCreate(dns_task, "otaf_dns", 3072, NULL, 5, &t);
    s_task = t;
}

void otaf_dns_stop(void)
{
    s_running = false;
    for (int i = 0; i < 40 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
