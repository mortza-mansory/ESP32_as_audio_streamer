/*
 * Wi-Fi to Bluetooth A2DP Audio Bridge (Interactive TUI Setup)
 *
 * This version provides a step-by-step setup interface in the serial monitor.
 * This version includes a fix for a crash when discovering nameless Bluetooth devices.
 *
 * FRAMEWORK: ESP-IDF
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h" 
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

// --- Globals & Definitions ---
static const char *TAG = "AUDIO_BRIDGE_TUI";

// State machine for the setup process
typedef enum {
    APP_STATE_INIT,
    APP_STATE_BT_DISCOVERY,
    APP_STATE_BT_DEVICE_SELECTION,
    APP_STATE_BT_CONNECTING,
    APP_STATE_WIFI_SCANNING,
    APP_STATE_WIFI_NETWORK_SELECTION,
    APP_STATE_WIFI_PASSWORD_INPUT,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_RUNNING
} app_state_t;

static app_state_t s_app_state = APP_STATE_INIT;

// For storing discovered devices
#define MAX_DISCOVERED_DEVICES 20
static esp_bt_gap_cb_param_t s_bt_devices[MAX_DISCOVERED_DEVICES];
static uint8_t s_bt_device_count = 0;

static wifi_ap_record_t s_wifi_aps[MAX_DISCOVERED_DEVICES];
static uint16_t s_wifi_ap_count = 0;
static wifi_config_t s_wifi_config; // Store selected Wi-Fi config

// Event group to signal completion of async operations like scans
static EventGroupHandle_t s_app_event_group;
#define BT_DISCOVERY_DONE_BIT   BIT0
#define WIFI_SCAN_DONE_BIT      BIT1
#define BT_CONNECTED_BIT        BIT2
#define WIFI_CONNECTED_BIT      BIT3

// --- Audio Streaming Components ---
#define TCP_PORT              8080
#define STREAM_BUFFER_SIZE    (16 * 1024)
static StreamBufferHandle_t s_audio_stream_buffer; // MODIFIED: Changed from RingbufHandle_t
static int client_socket = -1;

// --- Function Prototypes ---
void app_main(void);
void setup_task(void *pvParameters);
void tcp_server_task(void *pvParameters);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static int32_t a2d_data_cb(uint8_t *data, int32_t len);
static void bt_app_av_sm_hdlr(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static void get_user_input(char* buffer, int len);
static char* get_bt_device_name(esp_bt_gap_cb_param_t *param);


// --- Bluetooth Callback ---
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            // Check if device already in list
            bool found = false;
            for (int i = 0; i < s_bt_device_count; i++) {
                if (memcmp(param->disc_res.bda, s_bt_devices[i].disc_res.bda, ESP_BD_ADDR_LEN) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found && s_bt_device_count < MAX_DISCOVERED_DEVICES) {
                // Add all devices regardless of having a name or not
                memcpy(&s_bt_devices[s_bt_device_count], param, sizeof(esp_bt_gap_cb_param_t));
                s_bt_device_count++;
            }
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                ESP_LOGI(TAG, "Bluetooth scan stopped.");
                xEventGroupSetBits(s_app_event_group, BT_DISCOVERY_DONE_BIT);
            } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
                ESP_LOGI(TAG, "Bluetooth scan started.");
            }
            break;
        }
        default:
            break;
    }
}


// --- A2DP Callback ---
static void bt_app_av_sm_hdlr(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP connected.");
                xEventGroupSetBits(s_app_event_group, BT_CONNECTED_BIT);
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGW(TAG, "A2DP disconnected. Please restart the device to reconnect.");
                xEventGroupClearBits(s_app_event_group, BT_CONNECTED_BIT);
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "A2DP audio streaming started.");
            }
            break;
        }
        default:
            break;
    }
}

// --- Wi-Fi Event Handler ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_app_event_group, WIFI_SCAN_DONE_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_app_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE(TAG, "Wi-Fi disconnected. Retrying...");
        esp_wifi_connect();
    }
}

// --- Main Setup Task ---
void setup_task(void *pvParameters) {
    char input_buffer[64];
    int choice;

    while (1) {
        switch (s_app_state) {
            case APP_STATE_INIT:
                printf("\n\n--- Step 1: Bluetooth Setup ---\n");
                s_bt_device_count = 0;
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 15, 0);
                s_app_state = APP_STATE_BT_DISCOVERY;
                break;

            case APP_STATE_BT_DISCOVERY:
                printf("Scanning for Bluetooth devices...\n");
                xEventGroupWaitBits(s_app_event_group, BT_DISCOVERY_DONE_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
                printf("Scan complete. Found %d devices:\n", s_bt_device_count);
                for (int i = 0; i < s_bt_device_count; i++) {
                    char *name = get_bt_device_name(&s_bt_devices[i]);
                    printf("  %d: %s\n", i + 1, name ? name : "[No Name]");
                }
                s_app_state = APP_STATE_BT_DEVICE_SELECTION;
                break;

            case APP_STATE_BT_DEVICE_SELECTION:
                printf("Enter the number of the device to connect to: ");
                get_user_input(input_buffer, sizeof(input_buffer));
                choice = atoi(input_buffer);
                if (choice > 0 && choice <= s_bt_device_count) {
                    esp_a2d_source_connect(s_bt_devices[choice - 1].disc_res.bda);
                    s_app_state = APP_STATE_BT_CONNECTING;
                } else {
                    printf("Invalid choice. Please try again.\n");
                }
                break;

            case APP_STATE_BT_CONNECTING:
                printf("Connecting to Bluetooth device...\n");
                xEventGroupWaitBits(s_app_event_group, BT_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
                printf("\n--- Step 2: Wi-Fi Setup ---\n");
                esp_wifi_scan_start(NULL, true);
                s_app_state = APP_STATE_WIFI_SCANNING;
                break;

            case APP_STATE_WIFI_SCANNING:
                printf("Scanning for Wi-Fi networks...\n");
                xEventGroupWaitBits(s_app_event_group, WIFI_SCAN_DONE_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
                s_wifi_ap_count = MAX_DISCOVERED_DEVICES;
                esp_wifi_scan_get_ap_records(&s_wifi_ap_count, s_wifi_aps);
                printf("Scan complete. Found %d networks:\n", s_wifi_ap_count);
                for (int i = 0; i < s_wifi_ap_count; i++) {
                    printf("  %d: %s (%d)\n", i + 1, s_wifi_aps[i].ssid, s_wifi_aps[i].rssi);
                }
                s_app_state = APP_STATE_WIFI_NETWORK_SELECTION;
                break;

            case APP_STATE_WIFI_NETWORK_SELECTION:
                printf("Enter the number of the Wi-Fi network: ");
                get_user_input(input_buffer, sizeof(input_buffer));
                choice = atoi(input_buffer);
                if (choice > 0 && choice <= s_wifi_ap_count) {
                    memset(&s_wifi_config, 0, sizeof(wifi_config_t));
                    strcpy((char *)s_wifi_config.sta.ssid, (char *)s_wifi_aps[choice - 1].ssid);
                    s_app_state = APP_STATE_WIFI_PASSWORD_INPUT;
                } else {
                    printf("Invalid choice. Please try again.\n");
                }
                break;
            
            case APP_STATE_WIFI_PASSWORD_INPUT:
                printf("Enter password for %s: ", s_wifi_config.sta.ssid);
                get_user_input((char *)s_wifi_config.sta.password, sizeof(s_wifi_config.sta.password));
                
                esp_wifi_set_config(WIFI_IF_STA, &s_wifi_config);
                esp_wifi_connect();
                s_app_state = APP_STATE_WIFI_CONNECTING;
                break;

            case APP_STATE_WIFI_CONNECTING:
                printf("Connecting to Wi-Fi...\n");
                xEventGroupWaitBits(s_app_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
                printf("\n--- Setup Complete! ---\n");
                printf("Audio bridge is now active. Connect your app to the ESP32.\n");

                // MODIFIED: The task is now created with higher priority and pinned to Core 1
                xTaskCreatePinnedToCore(
                    tcp_server_task,    // Function to implement the task
                    "tcp_server",       // Name of the task
                    4096,               // Stack size in words
                    NULL,               // Task input parameter
                    10,                 // Priority of the task (increased from 5)
                    NULL,               // Task handle
                    1                   // Core where the task should run (APP_CPU_NUM)
                );

                s_app_state = APP_STATE_RUNNING;
                break;

            case APP_STATE_RUNNING:
                // This task is no longer needed, so it deletes itself.
                vTaskDelete(NULL);
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
// --- Main Entry Point ---
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_app_event_group = xEventGroupCreate();
    // MODIFIED: Create a Stream Buffer instead of a Ring Buffer.
    // The second argument '1' is the trigger level.
    s_audio_stream_buffer = xStreamBufferCreate(STREAM_BUFFER_SIZE, 1);

    // --- Wi-Fi Init ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // --- Bluetooth Init ---
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_app_gap_cb));
    ESP_ERROR_CHECK(esp_a2d_register_callback(bt_app_av_sm_hdlr));
    ESP_ERROR_CHECK(esp_a2d_source_init());
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(a2d_data_cb));
    esp_bt_dev_set_device_name("ESP_A2DP_BRIDGE");

    xTaskCreate(setup_task, "setup_task", 4096, NULL, 5, NULL);
}

// --- Helper function to get user input from serial monitor ---
static void get_user_input(char* buffer, int len) {
    memset(buffer, 0, len);
    int i = 0;
    while (i < len - 1) {
        int c = fgetc(stdin);
        if (c >= 0) {
            if (c == '\n' || c == '\r') {
                break;
            }
            buffer[i++] = c;
            printf("%c", c);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("\n");
}

// --- Helper function to safely get a device's name ---
static char* get_bt_device_name(esp_bt_gap_cb_param_t *param) {
    char *name = NULL;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME) {
            name = (char *)param->disc_res.prop[i].val;
            return name;
        }
    }
    return NULL;
}

// --- Audio Streaming Code ---

// MODIFIED: This is the new, safe data callback function
static int32_t a2d_data_cb(uint8_t *data, int32_t len) {
    if (len < 0 || data == NULL) {
        return 0;
    }

    // Read the exact number of bytes the BT stack is asking for ('len')
    // We use a small timeout so it doesn't wait forever if the network is slow.
    size_t bytes_read = xStreamBufferReceive(s_audio_stream_buffer, data, len, pdMS_TO_TICKS(20));

    // If we received less data than requested (i.e., the buffer was partially empty),
    // fill the rest of the audio buffer with silence (zeros).
    if (bytes_read < len) {
        memset(data + bytes_read, 0, len - bytes_read);
    }

    // The A2DP stack needs to be told that we have filled its entire buffer.
    // So, we always return the originally requested length ('len').
    return len;
}

void tcp_server_task(void *pvParameters) {
    char addr_str[128];
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(TCP_PORT);
    bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    listen(listen_sock, 1);

    while (1) {
        ESP_LOGI(TAG, "TCP server listening on port %d", TCP_PORT);
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        client_socket = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (client_socket < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Accepted connection from %s", addr_str);
        
        uint8_t rx_buffer[1024];
        int len;
        do {
            len = recv(client_socket, rx_buffer, sizeof(rx_buffer), 0);
            if (len > 0) {
                // MODIFIED: Send data to the stream buffer
                xStreamBufferSend(s_audio_stream_buffer, rx_buffer, len, portMAX_DELAY);
            }
        } while (len > 0);

        ESP_LOGI(TAG, "Client disconnected.");
        shutdown(client_socket, 0);
        close(client_socket);
        client_socket = -1;
    }
    close(listen_sock);
    vTaskDelete(NULL);
}