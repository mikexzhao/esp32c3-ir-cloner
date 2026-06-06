#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "dns_server.h"

// GPIO Pin Definitions
#define VISLED           7    // GPIO7 (Out)
#define IRLED            3    // GPIO3 (Out)
#define REGION_MODE      10   // NA is 1, EU is 0 (Input)
#define BUTTON_ACTION    2    // Action button on GPIO2 (input)
#define IR_RCVR          6    // IR Receiver on GPIO6 (input)

#define VISLED_OFF            0
#define VISLED_ON             1
#define IRLED_OFF             0
#define IRLED_ON              1
#define BUTTON_IS_PRESSED     0       // The value for the pressed Action button

// Configuration constants
#define MAX_TRANSITIONS       1024
#define IR_TIMEOUT_US         200000  // 200ms of inactivity ends recording
#define MAX_RECORD_TIME_US    3000000 // 3 seconds maximum record time
#define LONG_PRESS_TIME_MS    2000    // 2 seconds for long press reset

static const char *TAG = "IR_CLONER";

// Application states
typedef enum {
    STATE_BOOT,
    STATE_WAITING_FOR_IR,
    STATE_RECORDING,
    STATE_READY_TO_PLAY,
    STATE_PLAYING
} app_state_t;

// RAM Buffer to store the captured transitions (durations in microseconds)
static uint32_t s_durations[MAX_TRANSITIONS];
static uint32_t s_transition_count = 0;
static app_state_t s_state = STATE_BOOT;

// Handles for network and server tasks
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_timer_handle_t s_inactivity_timer = NULL;
static dns_server_handle_t s_dns_server = NULL;
static httpd_handle_t s_http_server = NULL;
static TaskHandle_t s_ota_task_handle = NULL;

// Buffers for OTA credentials
static char s_ota_ssid[64];
static char s_ota_pass[64];
static char s_ota_url[256];

// Captive Portal HTML Content
static const char ota_html[] = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ESP32-C3 Firmware Update</title>"
"<style>"
"body { background: linear-gradient(135deg, #0f172a 0%, #1e1b4b 100%); color: #f8fafc; font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, sans-serif; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; }"
".card { background: rgba(30, 41, 59, 0.7); backdrop-filter: blur(10px); border: 1px solid rgba(255, 255, 255, 0.1); border-radius: 16px; padding: 32px; width: 90%; max-width: 400px; box-shadow: 0 10px 25px -5px rgba(0,0,0,0.3); }"
"h2 { margin-top: 0; font-size: 24px; font-weight: 600; background: linear-gradient(to right, #38bdf8, #818cf8); -webkit-background-clip: text; -webkit-text-fill-color: transparent; text-align: center; }"
".input-group { margin-bottom: 20px; }"
"label { display: block; font-size: 14px; font-weight: 500; margin-bottom: 6px; color: #94a3b8; }"
"input[type=\"text\"], input[type=\"password\"] { width: 100%; padding: 12px; border: 1px solid rgba(255, 255, 255, 0.1); border-radius: 8px; background: rgba(15, 23, 42, 0.6); color: #fff; box-sizing: border-box; font-size: 14px; transition: border-color 0.2s; }"
"input:focus { outline: none; border-color: #6366f1; }"
"button { width: 100%; padding: 12px; background: linear-gradient(to right, #4f46e5, #6366f1); border: none; border-radius: 8px; color: #fff; font-size: 16px; font-weight: 600; cursor: pointer; box-shadow: 0 4px 6px -1px rgba(79, 70, 229, 0.2); transition: transform 0.1s, box-shadow 0.1s; }"
"button:active { transform: scale(0.98); }"
".footer { text-align: center; margin-top: 24px; font-size: 12px; color: #64748b; }"
"</style>"
"</head>"
"<body>"
"<div class=\"card\">"
"    <h2>Firmware Update</h2>"
"    <form action=\"/callback\" method=\"POST\">"
"        <div class=\"input-group\">"
"            <label for=\"ssid\">Target Wi-Fi SSID</label>"
"            <input type=\"text\" id=\"ssid\" name=\"ssid\" placeholder=\"Enter local Wi-Fi SSID\" required>"
"        </div>"
"        <div class=\"input-group\">"
"            <label for=\"password\">Wi-Fi Password</label>"
"            <input type=\"password\" id=\"password\" name=\"password\" placeholder=\"Enter Wi-Fi Password\">"
"        </div>"
"        <div class=\"input-group\">"
"            <label for=\"url\">OTA Binary URL</label>"
"            <input type=\"text\" id=\"url\" name=\"url\" placeholder=\"http://192.168.x.x/firmware.bin\" value=\"http://192.168.4.2:8000/build/esp32c3_ir_cloner.bin\" required>"
"        </div>"
"        <button type=\"submit\">Start OTA Update</button>"
"    </form>"
"    <div class=\"footer\">EtonTech Cloner v1.1.0</div>"
"</div>"
"</body>"
"</html>";

// Helper to set VISLED state
static void set_visled(int level) {
    gpio_set_level(VISLED, level);
}

// Helper to set IRLED state
static void set_irled(int level) {
    gpio_set_level(IRLED, level);
}

// Generate 38kHz carrier wave on IRLED for specified duration in microseconds
static void play_carrier(uint32_t duration_us) {
    uint64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < duration_us) {
        set_irled(IRLED_ON);
        esp_rom_delay_us(13); // 38kHz carrier -> 26.3us period (~13us HIGH / ~13us LOW)
        set_irled(IRLED_OFF);
        esp_rom_delay_us(13);
    }
}

// Play silence (LOW state) on IRLED for specified duration in microseconds
static void play_silence(uint32_t duration_us) {
    set_irled(IRLED_OFF);
    uint64_t start = esp_timer_get_time();
    if (duration_us > 2000) {
        vTaskDelay(pdMS_TO_TICKS(duration_us / 1000 - 1));
    }
    while ((esp_timer_get_time() - start) < duration_us) {
        esp_rom_delay_us(1);
    }
}

// Configure GPIO pins
static void init_gpios(void) {
    gpio_config_t io_conf_visled = {
        .pin_bit_mask = (1ULL << VISLED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_visled);
    set_visled(VISLED_OFF);

    gpio_config_t io_conf_irled = {
        .pin_bit_mask = (1ULL << IRLED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_irled);
    set_irled(IRLED_OFF);

    gpio_config_t io_conf_btn = {
        .pin_bit_mask = (1ULL << BUTTON_ACTION),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_btn);

    gpio_config_t io_conf_region = {
        .pin_bit_mask = (1ULL << REGION_MODE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_region);

    gpio_config_t io_conf_rcvr = {
        .pin_bit_mask = (1ULL << IR_RCVR),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_rcvr);

    ESP_LOGI(TAG, "GPIO Initialization completed.");
    int region = gpio_get_level(REGION_MODE);
    ESP_LOGI(TAG, "Region Mode: %s (GPIO10 Level: %d)", region ? "NA" : "EU", region);
}

// Reset/feed inactivity timer on user web activity
static void feed_inactivity_timer(void) {
    if (s_inactivity_timer) {
        esp_timer_stop(s_inactivity_timer);
        esp_timer_start_once(s_inactivity_timer, 60000000); // Reset to 60s
        ESP_LOGI(TAG, "Inactivity timer fed/extended by 60 seconds.");
    }
}

// HTTP handlers for Captive Portal
static esp_err_t get_handler(httpd_req_t *req) {
    feed_inactivity_timer();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ota_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t redirect_handler(httpd_req_t *req) {
    feed_inactivity_timer();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Form urlencoded decoding helper functions
static esp_err_t url_decode(const char *src, char *dest, size_t dest_len) {
    size_t i = 0, j = 0;
    while (src[i] && j < dest_len - 1) {
        if (src[i] == '%') {
            if (src[i+1] && src[i+2]) {
                char hex[3] = { src[i+1], src[i+2], 0 };
                dest[j++] = (char)strtol(hex, NULL, 16);
                i += 3;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (src[i] == '+') {
            dest[j++] = ' ';
            i++;
        } else {
            dest[j++] = src[i];
            i++;
        }
    }
    dest[j] = '\0';
    return ESP_OK;
}

static esp_err_t get_form_value(const char *buf, const char *key, char *val, size_t val_len) {
    char *pos = strstr(buf, key);
    if (!pos) return ESP_ERR_NOT_FOUND;
    pos += strlen(key);
    if (*pos != '=') return ESP_ERR_INVALID_ARG;
    pos++;
    char temp[256];
    size_t idx = 0;
    while (*pos && *pos != '&' && idx < sizeof(temp) - 1) {
        temp[idx++] = *pos++;
    }
    temp[idx] = '\0';
    return url_decode(temp, val, val_len);
}

static esp_err_t post_handler(httpd_req_t *req) {
    char buf[512];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    if (get_form_value(buf, "ssid", s_ota_ssid, sizeof(s_ota_ssid)) == ESP_OK &&
        get_form_value(buf, "url", s_ota_url, sizeof(s_ota_url)) == ESP_OK) {
        
        if (get_form_value(buf, "password", s_ota_pass, sizeof(s_ota_pass)) != ESP_OK) {
            s_ota_pass[0] = '\0';
        }

        ESP_LOGI(TAG, "Parsed form payload: SSID=%s, URL=%s", s_ota_ssid, s_ota_url);

        const char *resp = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{background:#0f172a;color:#fff;font-family:sans-serif;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}div{text-align:center;padding:24px;border:1px solid rgba(255,255,255,0.1);border-radius:12px;background:rgba(30,41,59,0.8);}</style></head><body><div><h2>Update Initialized</h2><p>Connecting to Wi-Fi to download firmware...</p><p>Check serial monitor for details.</p></div></body></html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

        if (s_ota_task_handle) {
            xTaskNotifyGive(s_ota_task_handle);
        }
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing required fields");
    return ESP_FAIL;
}

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t get_uri = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &get_uri);

        httpd_uri_t post_uri = {
            .uri      = "/callback",
            .method   = HTTP_POST,
            .handler  = post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &post_uri);

        httpd_uri_t default_get_uri = {
            .uri      = "*",
            .method   = HTTP_GET,
            .handler  = redirect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &default_get_uri);
    }
    return server;
}

// Wi-Fi AP initial configuration
static void wifi_init_softap(void) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32C3-IR-Cloner-AP",
            .ssid_len = strlen("ESP32C3-IR-Cloner-AP"),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Low Wi-Fi TX Power (approx. 2dBm output) to save battery power during portal configuration
    esp_wifi_set_max_tx_power(8);
    ESP_LOGI(TAG, "SoftAP started. SSID: ESP32C3-IR-Cloner-AP");
}

// Wi-Fi events handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == STATE_BOOT) { // Retry connect during setup connect phases
            ESP_LOGI(TAG, "STA disconnected. Re-trying connection...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[32];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "STA obtained IP address: %s", ip_str);
    }
}

// Main cloner tasks (state machine)
static void cloner_task(void *pvParameters) {
    init_gpios();
    s_state = STATE_WAITING_FOR_IR;
    
    uint32_t button_press_start_time = 0;
    bool button_was_pressed = false;

    while (1) {
        bool button_pressed = (gpio_get_level(BUTTON_ACTION) == BUTTON_IS_PRESSED);
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (button_pressed) {
            if (!button_was_pressed) {
                button_was_pressed = true;
                button_press_start_time = now_ms;
                ESP_LOGD(TAG, "Button pressed");
            } else {
                if (s_state == STATE_READY_TO_PLAY && (now_ms - button_press_start_time) >= LONG_PRESS_TIME_MS) {
                    ESP_LOGI(TAG, "Long press detected! Clearing RAM and restarting to enter OTA mode...");
                    for (int i = 0; i < 6; i++) {
                        set_visled(i % 2);
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    set_visled(VISLED_OFF);
                    s_transition_count = 0;
                    button_was_pressed = false;
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart(); // Reboot system so it goes back to OTA configuration window
                }
            }
        } else {
            if (button_was_pressed) {
                uint32_t duration = now_ms - button_press_start_time;
                button_was_pressed = false;
                ESP_LOGD(TAG, "Button released. Duration: %d ms", duration);

                if (duration < LONG_PRESS_TIME_MS) {
                    if (s_state == STATE_READY_TO_PLAY) {
                        s_state = STATE_PLAYING;
                    } else if (s_state == STATE_WAITING_FOR_IR) {
                        ESP_LOGW(TAG, "No IR code recorded yet. Point remote at receiver and press button.");
                        set_visled(VISLED_ON);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        set_visled(VISLED_OFF);
                    }
                }
            }
        }

        switch (s_state) {
            case STATE_WAITING_FOR_IR: {
                if (gpio_get_level(IR_RCVR) == 0) {
                    ESP_LOGI(TAG, "IR signal detected! Recording started...");
                    s_state = STATE_RECORDING;
                    set_visled(VISLED_OFF);
                }
                break;
            }

            case STATE_RECORDING: {
                s_transition_count = 0;
                uint64_t start_time = esp_timer_get_time();
                uint64_t last_time = start_time;
                int current_level = 0;

                while (1) {
                    int level = gpio_get_level(IR_RCVR);
                    set_visled(level); // HIGH (no carrier) -> VISLED ON, LOW (carrier detected) -> VISLED OFF
                    uint64_t now = esp_timer_get_time();

                    if (level != current_level) {
                        uint32_t diff = (uint32_t)(now - last_time);
                        if (s_transition_count < MAX_TRANSITIONS) {
                            s_durations[s_transition_count++] = diff;
                        } else {
                            ESP_LOGW(TAG, "Buffer full, stopping record.");
                            break;
                        }
                        current_level = level;
                        last_time = now;
                    }

                    if (current_level == 1 && (now - last_time) > IR_TIMEOUT_US) {
                        ESP_LOGI(TAG, "Inactivity timeout. Recording finished.");
                        break;
                    }

                    if ((now - start_time) > MAX_RECORD_TIME_US) {
                        ESP_LOGI(TAG, "Maximum recording duration reached. Recording finished.");
                        break;
                    }

                    esp_rom_delay_us(5);
                }

                set_visled(VISLED_OFF);

                if (s_transition_count > 0) {
                    ESP_LOGI(TAG, "Successfully recorded %d transitions.", s_transition_count);
                    s_state = STATE_READY_TO_PLAY;
                } else {
                    ESP_LOGE(TAG, "Recording failed (no transitions detected).");
                    s_state = STATE_WAITING_FOR_IR;
                }
                break;
            }

            case STATE_READY_TO_PLAY:
                set_visled(VISLED_OFF);
                break;

            case STATE_PLAYING: {
                ESP_LOGI(TAG, "Playing back recorded IR code...");
                set_visled(VISLED_OFF);
                
                for (uint32_t i = 0; i < s_transition_count; i++) {
                    if (i % 2 == 0) {
                        set_visled(VISLED_ON);
                        play_carrier(s_durations[i]);
                    } else {
                        set_visled(VISLED_OFF);
                        play_silence(s_durations[i]);
                    }
                }
                
                set_irled(IRLED_OFF);
                set_visled(VISLED_OFF);
                ESP_LOGI(TAG, "Playback completed.");

                s_state = STATE_READY_TO_PLAY;
                break;
            }

            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Wi-Fi teardown & load IR Cloner after 60-second inactivity timeout
static void ota_timeout_callback(void* arg) {
    ESP_LOGI(TAG, "OTA setup window timed out. Disabling Wi-Fi...");

    if (s_dns_server) {
        stop_dns_server(s_dns_server);
        s_dns_server = NULL;
    }
    
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    ESP_LOGI(TAG, "Wi-Fi radio completely powered off. Starting main application cloner...");
    
    xTaskCreate(cloner_task, "cloner_task", 4096, NULL, 10, NULL);
}

// OTA Background execution worker task
static void ota_task(void *pvParameters) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    
    ESP_LOGI(TAG, "OTA Task triggered! Dismantling AP and Web Servers...");

    if (s_inactivity_timer) {
        esp_timer_stop(s_inactivity_timer);
        esp_timer_delete(s_inactivity_timer);
        s_inactivity_timer = NULL;
    }

    if (s_dns_server) {
        stop_dns_server(s_dns_server);
        s_dns_server = NULL;
    }

    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    esp_wifi_stop();

    ESP_LOGI(TAG, "Switching Wi-Fi to STA Mode...");
    wifi_config_t sta_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)sta_config.sta.ssid, s_ota_ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, s_ota_pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s...", s_ota_ssid);
    esp_wifi_connect();

    // Poll for IP connection
    int retries = 0;
    bool connected = false;
    while (retries < 30) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ip_str[32];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Wi-Fi Connected! IP Address obtained: %s", ip_str);
            connected = true;
            break;
        }
        retries++;
    }

    if (!connected) {
        ESP_LOGE(TAG, "Failed to connect to router. Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // Execute esp_https_ota update
    ESP_LOGI(TAG, "Starting OTA update from: %s", s_ota_url);

    esp_http_client_config_t http_config = {
        .url = s_ota_url,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach, // Attach Root CAs for standard HTTPS
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t err = esp_https_ota(&ota_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA Update complete! Rebooting board...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Update failed with error code: %s. Rebooting...", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP32-C3 IR Cloner Firmware");
    ESP_LOGI(TAG, "Firmware Version: 1.1.0");
    ESP_LOGI(TAG, "Release Date: June 5th, 2026");
    ESP_LOGI(TAG, "Author: Mike Zhao (EtonTech)");

    // Initialize flash storage (necessary for wifi credentials)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Network and Event loops
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_init_softap();

    // Start local DNS redirector
    dns_server_config_t dns_cfg = {
        .num_of_entries = 1,
        .item = { { .name = "*", .ip = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) } } }
    };
    s_dns_server = start_dns_server(&dns_cfg);

    // Start Captive Portal Server
    s_http_server = start_webserver();

    // Spawn OTA task
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, &s_ota_task_handle);

    // Setup 60-second inactivity callback
    const esp_timer_create_args_t timer_args = {
        .callback = &ota_timeout_callback,
        .name = "ota_timeout"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_inactivity_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(s_inactivity_timer, 60000000));
    
    ESP_LOGI(TAG, "Inactivity timer set. Captive portal configuration active.");
}
