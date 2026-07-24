#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include "esp_camera.h"
#include "driver/gpio.h"

// 有線UARTおよびネットワーク(UDP)通信に必要なヘッダーファイルをインクルード
#include "driver/uart.h"
#include "lwip/sockets.h"

static const char *TAG = "xiao_camera_server";

// 有線通信(UART)で使用するポート番号、ピン配置、およびバッファサイズの設定
#define EX_UART_NUM           UART_NUM_1
#define UART_TX_PIN           (43)  // XIAOのD6ピン (メイン制御側のRXピンへ接続)
#define UART_RX_PIN           (44)  // XIAOのD7ピン (メイン制御側のTXピンから接続)
#define BUF_SIZE              (1024)
#define ANDROID_UDP_PORT      (8888) // Androidアプリ側でデータを受け取るためのUDPポート番号

// XIAO ESP32 S3 Sense ボードに内蔵されているカメラモジュールのピンアサイン定義
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

// MJPEG形式のHTTPストリーミング配信で使用するマルチパートの境界値およびヘッダー形式の定義
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/**
 * @brief XIAO用カメラモジュールのハードウェアおよびドライバの初期化を行う関数
 * @return esp_err_t 初期化の成否（成功時はESP_OK）
 */
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
    
    // XIAOに搭載されているOV2640カメラセンサーが安定して動作する20MHzにクロックを設定
    config.xclk_freq_hz = 20000000; 

    // OV2640内蔵のハードウェアJPEGエンコーダ機能を有効化し、処理を高速化
    config.pixel_format = PIXFORMAT_JPEG; 
    config.frame_size = FRAMESIZE_VGA;      // 解像度をVGA (640x480) に設定
    config.jpeg_quality = 10;               // JPEGの圧縮画質を設定 (0〜63の間で、値が小さいほど高画質)
    config.fb_count = 2;                    // ダブルバッファ構成にして描画をスムーズにする
    config.fb_location = CAMERA_FB_IN_PSRAM; // 画像のフレームバッファに外部PSRAMの領域を割り当て

    // 設定構造体をもとにカメラドライバを初期化・起動
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "Camera init success!");
    return ESP_OK;
}

/**
 * @brief 接続されたクライアントへMJPEG形式のカメラ映像を連続配信するHTTPリクエストハンドラ関数
 * @param req HTTPサーバーのリクエスト情報を格納した構造体へのポインタ
 * @return esp_err_t 送信処理の成否
 */
esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];

    // レスポンスのコンテンツタイプをMJPEGストリーム用に設定
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    ESP_LOGI(TAG, "Client connected to stream.");

    // クライアントとの接続が維持されている間、無限ループでフレームを送信し続ける
    while (true) {
        // カメラから1フレーム分の画像データ（バッファ）を取得
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        // マルチパート用の境界線データをクライアントへ送信
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            goto send_error;
        }

        // 送信するJPEGデータのサイズ情報を付与したパートヘッダーを作成し、送信
        size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            goto send_error;
        }

        // 実際のJPEG画像本体のバイナリデータを送信
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb); // 送信が完了したため、フレームバッファを速やかに解放・返却
        if (res != ESP_OK) goto send_error;
    }

    return res;

send_error:
    ESP_LOGI(TAG, "Client disconnected or send error.");
    return res;
}

/**
 * @brief Wi-FiのSoftAP（アクセスポイント）モードを初期化し、自らWi-Fi親機として動作させる関数
 */
void wifi_init_softap() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // アクセスポイントの設定（SSIDやパスワード、接続台数の制限など）
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "XIAO-ESP32S3-CRAWLER",
            .ssid_len = strlen("XIAO-ESP32S3-CRAWLER"),
            .channel = 1,                 // メイン制御側のESP-NOW通信チャンネルと同一チャンネルに固定
            .password = "12345678",
            .max_connection = 4,          // 最大同時接続クライアント数
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi SoftAP started. SSID: XIAO-ESP32S3-CRAWLER");
}

/**
 * @brief HTTP Webサーバーを起動し、カメラ映像配信用パス（/stream）のルーティングを登録する関数
 */
void start_web_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80; // 標準のHTTPポート80番を使用

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        // アクセスされたURLに応じたハンドラ関数の割り当て設定
        httpd_uri_t stream_uri = {
            .uri      = "/stream",
            .method   = HTTP_GET,
            .handler  = stream_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stream_uri);
        ESP_LOGI(TAG, "HTTP Server started. URL: http://192.168.4.1/stream");
    }
}

/**
 * @brief メイン制御側から有線UART経由で送られてくるデータを受信し、AndroidアプリへUDPで転送するバックグラウンドタスク関数
 * @param pvParameters FreeRTOSタスクへ渡す引数（本関数では使用しないためNULL）
 */
static void uart_to_android_rx_task(void *pvParameters) {
    // UART1の通信仕様（ボーレートやデータビット数など）を定義
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    QueueHandle_t uart_queue;
    
    // UARTドライバのインストール（送受信バッファの確保とイベントキューの登録）
    uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);
    uart_set_pin(EX_UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // 改行コード '\n' をハードウェアレベルで検知するパターン割り込み機能を有効化
    uart_enable_pattern_det_baud_intr(EX_UART_NUM, '\n', 1, 9, 0, 0);
    uart_pattern_queue_reset(EX_UART_NUM, 20);

    // 受信データを一時的に格納するためのメモリ領域をヒープ上に確保
    uint8_t* data = (uint8_t*) malloc(BUF_SIZE);
    
    // Androidアプリへブロードキャスト送信を行うためのUDPソケットを作成
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    int broadcastPermission = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastPermission, sizeof(broadcastPermission));

    // 送信先アドレス構造体（ブロードキャスト用IPと指定ポート）の設定
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(ANDROID_UDP_PORT);

    ESP_LOGI(TAG, "有線通信監視タスク（パターン検出有効）が起動しました。");

    uart_event_t event;

    // 無限ループでUARTからのイベント発生を常時監視
    while (1) {
        // キューからUARTイベントを受信するまでタスクをブロック（CPU負荷ゼロで待機）
        if (xQueueReceive(uart_queue, (void *)&event, portMAX_DELAY)) {
            
            // 改行コードが検出された、またはデータ受信イベントが発生した場合
            if (event.type == UART_PATTERN_DET || event.type == UART_DATA) {
                
                size_t buffered_size;
                uart_get_buffered_data_len(EX_UART_NUM, &buffered_size);
                
                if (buffered_size > 0) {
                    if (buffered_size > BUF_SIZE - 1) buffered_size = BUF_SIZE - 1;
                    
                    // バッファから文字列データを安全に読み出し
                    int len = uart_read_bytes(EX_UART_NUM, data, buffered_size, pdMS_TO_TICKS(10));
                    if (len > 0) {
                        data[len] = '\0'; // 文字列の終端処理
                        
                        // 読み込んだデータをUDPブロードキャストによりネットワーク経由でAndroid端末へ送信
                        sendto(sock, data, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                        ESP_LOGI(TAG, "有線受信 ➔ Android転送: %s", data);
                    }
                }
            }
            
            // 受信バッファが溢れた場合のエラー処理（バッファのクリアを実施）
            if (event.type == UART_BUFFER_FULL) {
                ESP_LOGW(TAG, "UARTバッファフル検出");
                uart_flush_input(EX_UART_NUM);
            }
        }
    }
    
    // 終了時のクリーンアップ処理（通常このループは終了しない）
    close(sock);
    free(data);
    vTaskDelete(NULL);
}

/**
 * @brief プログラム全体の初期化および各機能の起動を行うメインエントリーポイント
 */
void app_main(void) {
    // 不揮発性メモリ(NVS)の初期化（Wi-Fi設定等の保存に必須）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // カメラの初期化が正常に完了した場合のみ、ネットワークおよび通信タスクを起動
    if (init_camera() == ESP_OK) {
        wifi_init_softap();
        start_web_server();

        // メイン制御側からの有線UARTデータをAndroidへ中継するバックグラウンドタスクを生成
        xTaskCreate(uart_to_android_rx_task, "uart_to_android_task", 4096, NULL, 5, NULL);
    }
}