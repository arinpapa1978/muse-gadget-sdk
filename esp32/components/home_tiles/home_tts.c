// 우리 집 목소리 — Muse 답변 글자를 Worker(/api/gadget/tts)에서 16 kHz 음성으로 받아 옴
//   Muse 는 SDK 기기에 음성 답변을 보내지 않음 (글자만). 그래서 Muse 대화 작업(muse_chat_session.cpp)의
//   start_tts 자리에서 home_tts_start() 로 받기 시작하고, home_tts_read() 로 꺼내 스피커로 보냄
//   받기는 이 파일의 별도 작업이 함 (대화 작업이 네트워크를 기다리지 않게)
#include "home_tiles.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"

static const char *TAG = "home_tts";
#define SB_BYTES (96 * 1024)          /* 받아 둔 음성 (16 kHz 16bit: 약 3초) */
#define TEXT_CAP 2048

static StreamBufferHandle_t s_sb;
static TaskHandle_t s_task;
static char *s_text;
static volatile uint32_t s_job;       /* 새로 시작하거나 취소하면 1 늘어남 → 받던 것은 버림 */
static volatile bool s_busy;
static volatile int s_state = HOME_TTS_IDLE;
static bool s_skip_hdr;               /* 처음 4바이트가 RIFF 면 WAV 머리 44바이트를 건너뜀 */
static size_t s_hdr_seen;

static bool load(char *url, size_t ucap, char *key, size_t kcap)
{
    snprintf(url, ucap, "%s", CONFIG_HOME_TILES_URL);
    key[0] = 0;
    nvs_handle_t n;
    if (nvs_open("home", NVS_READONLY, &n) != ESP_OK) return false;
    size_t l = kcap; if (nvs_get_str(n, "key", key, &l) != ESP_OK) key[0] = 0;
    l = ucap; if (nvs_get_str(n, "url", url, &l) != ESP_OK) snprintf(url, ucap, "%s", CONFIG_HOME_TILES_URL);
    nvs_close(n);
    return key[0] != 0;
}

/* 지금 일감이면 음성을 버퍼에 넣음 (가득 차면 조금씩 기다림). 일감이 바뀌면 false */
static bool put(uint32_t job, const uint8_t *p, size_t n)
{
    while (n) {
        if (job != s_job) return false;
        size_t w = xStreamBufferSend(s_sb, p, n, pdMS_TO_TICKS(50));
        p += w; n -= w;
    }
    return true;
}

static void fetch(uint32_t job)
{
    char url[320], key[96];
    if (!load(url, sizeof url, key, sizeof key)) { s_state = HOME_TTS_FAILED; return; }
    char full[448]; snprintf(full, sizeof full, "%s/api/gadget/tts?k=%s", url, key);
    esp_http_client_config_t cfg = { .url = full, .method = HTTP_METHOD_POST, .timeout_ms = 20000,
                                     .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 4608, .buffer_size_tx = 4608 };   /* 4 KB 넘게: PSRAM 에 잡힘 */
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { s_state = HOME_TTS_FAILED; return; }
    esp_http_client_set_header(h, "Content-Type", "text/plain; charset=utf-8");
    int len = strlen(s_text), status = 0; size_t total = 0;
    uint8_t *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    bool ok = false;
    if (buf && esp_http_client_open(h, len) == ESP_OK && esp_http_client_write(h, s_text, len) == len) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        if (status == 200) {
            uint8_t carry = 0; bool has_carry = false; int r;
            ok = true;
            while ((r = esp_http_client_read(h, (char *)buf + 1, 2047)) > 0) {
                uint8_t *p = buf + 1; size_t n = r;
                if (s_hdr_seen < 44) {                               /* WAV 머리 확인·건너뛰기 */
                    if (s_hdr_seen == 0 && n >= 4 && !memcmp(p, "RIFF", 4)) s_skip_hdr = true;
                    if (s_skip_hdr) {
                        size_t k = 44 - s_hdr_seen; if (k > n) k = n;
                        s_hdr_seen += k; p += k; n -= k;
                    } else s_hdr_seen = 44;
                }
                if (has_carry && n) { *--p = carry; n++; has_carry = false; }   /* 16bit 짝 맞추기 */
                if (n & 1) { carry = p[n - 1]; has_carry = true; n--; }
                total += n;
                if (!put(job, p, n)) { ok = false; break; }
            }
        }
    }
    esp_http_client_close(h); esp_http_client_cleanup(h);
    free(buf);
    if (job != s_job) return;
    ESP_LOGI(TAG, "status %d, %u bytes of speech", status, (unsigned)total);
    s_state = ok && total ? HOME_TTS_DONE : HOME_TTS_FAILED;
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_busy = true;
        fetch(s_job);
        s_busy = false;
    }
}

static bool ensure(void)
{
    if (s_task) return true;
    s_text = heap_caps_malloc(TEXT_CAP, MALLOC_CAP_SPIRAM);
    s_sb = xStreamBufferCreateWithCaps(SB_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!s_text || !s_sb) return false;
    return xTaskCreatePinnedToCoreWithCaps(task, "home_tts", 10 * 1024, NULL, 4, &s_task, tskNO_AFFINITY,
                                           MALLOC_CAP_SPIRAM) == pdPASS;
}

static void drain(void)
{
    uint8_t junk[256];
    while (xStreamBufferReceive(s_sb, junk, sizeof junk, 0)) {}
}

bool home_tts_start(const char *text)
{
    char url[320], key[96];
    if (!text || !text[0] || !load(url, sizeof url, key, sizeof key) || !ensure()) return false;
    s_job++;                                             /* 받던 것이 있으면 멈추게 */
    for (int i = 0; s_busy && i < 40; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_busy) return false;                            /* 앞의 받기가 아직 안 끝남 → 이번엔 글자로만 */
    drain();
    snprintf(s_text, TEXT_CAP, "%s", text);
    s_skip_hdr = false; s_hdr_seen = 0;
    s_state = HOME_TTS_RUNNING;
    xTaskNotifyGive(s_task);
    return true;
}

size_t home_tts_read(uint8_t *out, size_t cap)
{
    if (!s_sb) return 0;
    return xStreamBufferReceive(s_sb, out, cap & ~(size_t)1, 0);
}

int home_tts_state(void)
{
    if (s_state == HOME_TTS_RUNNING) return HOME_TTS_RUNNING;
    if (s_sb && xStreamBufferBytesAvailable(s_sb)) return HOME_TTS_RUNNING;   /* 남은 것을 다 꺼낼 때까지 */
    return s_state;
}

void home_tts_cancel(void)
{
    if (!s_task) return;
    s_job++;
    s_state = HOME_TTS_IDLE;
    drain();
}
