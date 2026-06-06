#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

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
// durations[0] = first active pulse (LOW on receiver)
// durations[1] = first inactive pulse (HIGH on receiver)
// durations[2] = second active pulse, etc.
static uint32_t s_durations[MAX_TRANSITIONS];
static uint32_t s_transition_count = 0;
static app_state_t s_state = STATE_BOOT;

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
    // For longer delays, we yield using vTaskDelay to be RTOS-friendly
    if (duration_us > 2000) {
        vTaskDelay(pdMS_TO_TICKS(duration_us / 1000 - 1));
    }
    // Busy wait for the remaining time to keep microsecond accuracy
    while ((esp_timer_get_time() - start) < duration_us) {
        esp_rom_delay_us(1);
    }
}

// Configure GPIO pins
static void init_gpios(void) {
    // 1. Configure VISLED as Output
    gpio_config_t io_conf_visled = {
        .pin_bit_mask = (1ULL << VISLED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_visled);
    set_visled(VISLED_OFF);

    // 2. Configure IRLED as Output
    gpio_config_t io_conf_irled = {
        .pin_bit_mask = (1ULL << IRLED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_irled);
    set_irled(IRLED_OFF);

    // 3. Configure BUTTON_ACTION as Input with Pull-Up
    gpio_config_t io_conf_btn = {
        .pin_bit_mask = (1ULL << BUTTON_ACTION),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_btn);

    // 4. Configure REGION_MODE as Input with Pull-Up
    gpio_config_t io_conf_region = {
        .pin_bit_mask = (1ULL << REGION_MODE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_region);

    // 5. Configure IR_RCVR as Input with Pull-Up
    gpio_config_t io_conf_rcvr = {
        .pin_bit_mask = (1ULL << IR_RCVR),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_rcvr);

    ESP_LOGI(TAG, "GPIO Initialization completed.");
    
    // Read and log region mode
    int region = gpio_get_level(REGION_MODE);
    ESP_LOGI(TAG, "Region Mode: %s (GPIO10 Level: %d)", region ? "NA" : "EU", region);
}

// Task to handle the state machine and IR capture/replay
static void cloner_task(void *pvParameters) {
    init_gpios();
    s_state = STATE_WAITING_FOR_IR;
    
    uint32_t button_press_start_time = 0;
    bool button_was_pressed = false;

    while (1) {
        // Read button state
        bool button_pressed = (gpio_get_level(BUTTON_ACTION) == BUTTON_IS_PRESSED);
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Button press detection (Short and Long Press)
        if (button_pressed) {
            if (!button_was_pressed) {
                button_was_pressed = true;
                button_press_start_time = now_ms;
                ESP_LOGD(TAG, "Button pressed");
            } else {
                // Check for long press reset
                if (s_state == STATE_READY_TO_PLAY && (now_ms - button_press_start_time) >= LONG_PRESS_TIME_MS) {
                    ESP_LOGI(TAG, "Long press detected! Clearing recorded code and resetting...");
                    // Visual feedback: blink VISLED quickly
                    for (int i = 0; i < 6; i++) {
                        set_visled(i % 2);
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    set_visled(VISLED_OFF);
                    s_transition_count = 0;
                    s_state = STATE_WAITING_FOR_IR;
                    button_was_pressed = false; // Reset button state to prevent repeat triggers
                }
            }
        } else {
            if (button_was_pressed) {
                uint32_t duration = now_ms - button_press_start_time;
                button_was_pressed = false;
                ESP_LOGD(TAG, "Button released. Duration: %d ms", duration);

                if (duration < LONG_PRESS_TIME_MS) {
                    // Short press action
                    if (s_state == STATE_READY_TO_PLAY) {
                        s_state = STATE_PLAYING;
                    } else if (s_state == STATE_WAITING_FOR_IR) {
                        ESP_LOGW(TAG, "No IR code recorded yet. Point remote at receiver and press button.");
                        // Blink VISLED to show error/warning
                        set_visled(VISLED_ON);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        set_visled(VISLED_OFF);
                    }
                }
            }
        }

        // State Machine
        switch (s_state) {
            case STATE_WAITING_FOR_IR: {
                // Poll IR receiver. TSOP is active low.
                // If it goes LOW, a transmission has started.
                if (gpio_get_level(IR_RCVR) == 0) {
                    ESP_LOGI(TAG, "IR signal detected! Recording started...");
                    s_state = STATE_RECORDING;
                    // VISLED starts OFF since the initial signal is LOW
                    set_visled(VISLED_OFF);
                }
                break;
            }

            case STATE_RECORDING: {
                s_transition_count = 0;
                uint64_t start_time = esp_timer_get_time();
                uint64_t last_time = start_time;
                int current_level = 0; // The signal just went LOW

                while (1) {
                    int level = gpio_get_level(IR_RCVR);
                    set_visled(level); // HIGH (no carrier) -> VISLED ON, LOW (carrier detected) -> VISLED OFF
                    uint64_t now = esp_timer_get_time();

                    // If level changed, record duration of previous level
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

                    // Check for timeout (signal has been HIGH/idle for too long)
                    if (current_level == 1 && (now - last_time) > IR_TIMEOUT_US) {
                        ESP_LOGI(TAG, "Inactivity timeout. Recording finished.");
                        break;
                    }

                    // Check for maximum record duration safety limit
                    if ((now - start_time) > MAX_RECORD_TIME_US) {
                        ESP_LOGI(TAG, "Maximum recording duration reached. Recording finished.");
                        break;
                    }

                    // Small delay to prevent lockup and watchdog triggers
                    esp_rom_delay_us(5);
                }

                set_visled(VISLED_OFF); // Turn off recording LED when done

                if (s_transition_count > 0) {
                    ESP_LOGI(TAG, "Successfully recorded %d transitions.", s_transition_count);
                    for (int i = 0; i < (s_transition_count > 10 ? 10 : s_transition_count); i++) {
                        ESP_LOGD(TAG, "  Duration[%d]: %d us", i, s_durations[i]);
                    }
                    s_state = STATE_READY_TO_PLAY;
                } else {
                    ESP_LOGE(TAG, "Recording failed (no transitions detected).");
                    s_state = STATE_WAITING_FOR_IR;
                }
                break;
            }

            case STATE_READY_TO_PLAY:
                // VISLED stays OFF while waiting for button action
                set_visled(VISLED_OFF);
                break;

            case STATE_PLAYING: {
                ESP_LOGI(TAG, "Playing back recorded IR code...");
                set_visled(VISLED_OFF);
                
                // Playback transitions
                for (uint32_t i = 0; i < s_transition_count; i++) {
                    // Even indices are marks (emit 38kHz carrier -> VISLED ON)
                    // Odd indices are spaces (emit silence -> VISLED OFF)
                    if (i % 2 == 0) {
                        set_visled(VISLED_ON);
                        play_carrier(s_durations[i]);
                    } else {
                        set_visled(VISLED_OFF);
                        play_silence(s_durations[i]);
                    }
                }
                
                // Ensure IR LED and VISLED are completely off after transmission
                set_irled(IRLED_OFF);
                set_visled(VISLED_OFF);
                ESP_LOGI(TAG, "Playback completed.");

                s_state = STATE_READY_TO_PLAY;
                break;
            }

            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // 10ms loop delay for button polling and idle cycles
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP32-C3 IR Cloner Firmware");
    ESP_LOGI(TAG, "Firmware Version: 1.01");
    ESP_LOGI(TAG, "Release Date: June 5th, 2026");
    ESP_LOGI(TAG, "Author: Mike Zhao (EtonTech)");
    
    // Create the cloner task with higher priority to ensure microsecond timing accuracy
    xTaskCreate(cloner_task, "cloner_task", 4096, NULL, 10, NULL);
}
