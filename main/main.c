#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include "esp_camera.h"
#include "driver/gpio.h"

static const char *TAG = "xiao_camera_server";

// 💡 XIAO ESP32 S3 Sense 専用のカメラピン定義（基板内部で配線されているピンです）
#define CAM_PIN_PWDN    (-1)
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK    (10)
#define CAM_PIN_SIOD    (40)
#define CAM_PIN_SIOC    (39)
#define CAM_PIN_D7      (48)
#define CAM_PIN_D6      (11)
#define CAM_PIN_D5      (12)
#define CAM_PIN_D4      (14)
#define CAM_PIN_D3      (16)
#define CAM_PIN_D2      (18)
#define CAM_PIN_D1      (17)
#define CAM_PIN_D0      (15)
#define CAM_PIN_VSYNC   (38)
#define CAM_PIN_HREF    (47)
#define CAM_PIN_PCLK    (13)

// MJPEGストリーム用のHTTPヘッダー定義
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// XIAO用 カメラ初期化関数
esp_err_t init_camera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_pclk = CAM_PIN_PCLK;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;
    
    // XIAOのOV2640は20MHzで安定動作します
    config.xclk_freq_hz = 20000000; 

    // 💡 OV2640のハードウェアJPEGエンコーダを使用（超高速化）
    config.pixel_format = PIXFORMAT_JPEG; 
    config.frame_size = FRAMESIZE_VGA;      // 最初はVGA(640x480)でテスト。重ければFRAMESIZE_QVGAに変更
    config.jpeg_quality = 10;               // 画質設定 (0-63、数字が小さいほど高画質)
    config.fb_count = 2;                    // ダブルバッファで滑らかに
    config.fb_location = CAMERA_FB_IN_PSRAM; // XIAO搭載の外部PSRAMを活用

    // カメラドライバの起動
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Camera init success!");
    return ESP_OK;
}

// ストリーミング配信ハンドラ（不要なエンコード処理を排除し最適化）
esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    ESP_LOGI(TAG, "Client connected to stream.");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        // 💡 カメラが元からJPEGで撮ってくれているので、そのまま送信データとして使えます
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            goto send_error;
        }

        size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            goto send_error;
        }

        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb); // 送信後すぐにバッファを返却
        if (res != ESP_OK) goto send_error;
    }

    return res;

send_error:
    ESP_LOGI(TAG, "Client disconnected or send error.");
    return res;
}

// Wi-Fi (SoftAP) 起動関数
void wifi_init_softap() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "XIAO-ESP32S3-CRAWLER", // クローラー用のSSIDに変更
            .ssid_len = strlen("XIAO-ESP32S3-CRAWLER"),
            .channel = 1,                 // ⚠️駆動部のESP-NOWと同じチャンネルに固定してください
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi SoftAP started. SSID: XIAO-ESP32S3-CRAWLER");
}

// HTTP Webサーバー起動関数
void start_web_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t stream_uri = {
            .uri       = "/stream",
            .method    = HTTP_GET,
            .handler   = stream_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &stream_uri);
        ESP_LOGI(TAG, "HTTP Server started. URL: http://192.168.4.1/stream");
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (init_camera() == ESP_OK) {
        wifi_init_softap();
        start_web_server();
    }
}