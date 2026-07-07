#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include "esp_camera.h"
#include "driver/gpio.h"

static const char *TAG = "xiao_camera_server"; // ログ出力時に使用する識別名

// XIAO ESP32 S3 Sense専用のカメラピン定義。基板内部で配線されているピンを指定
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

// 動画ストリーミング配信用に使用するHTTP通信用のヘッダー定義
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// 使用するカメラモジュールを初期化する関数
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
    
    // カメラの動作クロック周波数を20メガヘルツに設定して動作
    config.xclk_freq_hz = 20000000; 

    // カメラモジュールが持つハードウェア機能を利用してJPEG画像を作成する設定
    config.pixel_format = PIXFORMAT_JPEG; 
    config.frame_size = FRAMESIZE_VGA;      // 画像サイズを640x480ピクセルに設定
    config.jpeg_quality = 10;               // 画質の数値を設定する。小さいほど高画質になる
    config.fb_count = 2;                    // 画面の書き換えを滑らかにするためバッファを2個確保
    config.fb_location = CAMERA_FB_IN_PSRAM; // 搭載されている外部メモリを活用してデータを保存

    // 設定した内容でカメラの制御用ドライバを起動
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        // 初期化に失敗した場合はエラーログを出力して終了
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Camera init success!");
    return ESP_OK;
}

// 接続されたブラウザなどのクライアントに対してリアルタイムで映像を送信し続ける関数
esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];

    // 通信の形式を動画配信用のマルチパートデータに設定
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    ESP_LOGI(TAG, "Client connected to stream.");

    // 映像データを連続して送り続けるための無限ループ
    while (true) {
        // カメラから撮影した画像データを1フレーム分取得
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        // カメラが撮影したJPEGデータをそのまま送信用の区切りデータとともに配信
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res != ESP_OK) {
            esp_camera_fb_return(fb); // エラー時は確保したメモリを返却
            goto send_error;
        }

        // 送信する画像サイズを含めたヘッダー情報を構築して送信
        size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            goto send_error;
        }

        // 画像データの本体を送信
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb); // 送信完了後すぐに次の撮影のためにメモリを返却
        if (res != ESP_OK) goto send_error;
    }

    return res;

send_error:
    // 通信エラーが発生するか接続が切断された場合の処理
    ESP_LOGI(TAG, "Client disconnected or send error.");
    return res;
}

// マイコン自身がアクセスポイントとなる無線Wi-Fi機能を起動する関数
void wifi_init_softap() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // アクセスポイントの電波名やパスワードを設定
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "XIAO-ESP32S3-CRAWLER", // スマートフォンなどから接続するときの名前
            .ssid_len = strlen("XIAO-ESP32S3-CRAWLER"),
            .channel = 1,                 // モーター駆動側の通信規格と同じ無線チャンネルに固定
            .password = "12345678",       // 接続に必要なパスワード
            .max_connection = 4,          // 同時接続できる最大台数
            .authmode = WIFI_AUTH_WPA2_PSK // セキュリティ方式の設定
        },
    };

    // 親機モードとして設定を登録しWi-Fi電波の発信を開始する
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi SoftAP started. SSID: XIAO-ESP32S3-CRAWLER");
}

// 映像配信ページにアクセスできるようにするためのウェブサーバーを起動する関数
void start_web_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80; // 通常のウェブ閲覧で使用される80番ポートを使用

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        // 指定されたアドレスの後ろにスラッシュストリームを付けた際の応答ルールを定義
        httpd_uri_t stream_uri = {
            .uri       = "/stream",
            .method    = HTTP_GET,
            .handler   = stream_handler, // アクセスされたときに映像配信関数を実行
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &stream_uri);
        ESP_LOGI(TAG, "HTTP Server started. URL: http://192.168.4.1/stream");
    }
}

// プログラム起動時に最初に実行されるメイン関数
void app_main(void) {
    // 内部設定の保存領域であるフラッシュメモリを初期化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // カメラの初期化が正常に完了した場合のみ無線Wi-Fiとウェブサーバーを起動
    if (init_camera() == ESP_OK) {
        wifi_init_softap();
        start_web_server();
    }
}
