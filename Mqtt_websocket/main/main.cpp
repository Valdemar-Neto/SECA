#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#define WIFI_SSID "SUAREDE"
#define WIFI_PASS "SUASENHA"
#define MQTT_BROKER_URL "mqtt://test.mosquitto.org"
#define MQTT_TOPIC "s3/potenciometro"
#define LED_GPIO GPIO_NUM_38

#define ADC_WIDTH ADC_WIDTH_BIT_12
#define ADC_ATTEN ADC_ATTEN_DB_11
#define ADC_CHANNEL ADC1_CHANNEL_0 // GPIO_NUM1

//incluindo um char para verificar a tag de mqtt com websocket

static const char *TAG = "MQTT_LED";
static EventGroupHandle_t s_wifi_event_group;

static int estado_led = 0;
//inicializacao do led

static esp_adc_cal_characteristics_t *adc_chars;
static adc_oneshot_unit_handle_t adc_handle;

static int valor_lido;

// INicializando o led
static void init_led(){
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, estado_led);
}

static void set_led(int estado){

    estado_led = estado;
    gpio_set_level(LED_GPIO, estado);
}

//Inicializacao do nvs (Non Volatile Storage)
static void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Se o NVS estiver corrompido ou cheio, apaga e re-inicializa
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI("NVS", "NVS inicializado com sucesso!");
}


// Inicializando o ADC

static void adc_init(void){
    adc_oneshot_unit_init_cfg_t init_config_adc {
        .unit_id = ADC_UNIT_1
    }; // configurando o adc


    adc_oneshot_chan_cfg_t channel_config {
        .atten =  ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .channel = ADC_CHANNEL,
    };

    adc_oneshot_new_unit(&init_config_adc, &adc_handle)

    //configarando o canal

    adc_oneshot_config_channel(adc_handle, &channel_config);

}

// leitura do adc

static uint32_t adc_read_mv(void){
    int vm;
    adc_oneshot_read(adc_handle, ADC_CHANNEL, &valor_lido);
    return esp_adc_cal_raw_to_voltage(valor_lido, adc_chars);

}o
//inicializando o handle do wifi

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data){
    if(event_base ==  WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
        esp_wifi_connect();
    } else if( event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
        esp_wifi_connected();
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

//Inicializando O Wifi

static void wifi_init(void){
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    //instanciando os eventos que serao utilizados pelo wifi

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    // registrando os eventos

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL< &instance_got_ip);

    // condigurando o wifi

    wifi_config_t wifi_config = {
        .sta{
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        }
    }

    //setando o wifi como station mode

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();


    // fazendo o evento

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
}

// iniciando um handle da funcao para escrever os evetns destinados 

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t eventd_id, void *event_data){
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch(event -> event_id){
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "CONECTADO AO BROKER MQTT");
            esp_mqtt_client_publish(client, MQTT_TOPIC, "online" 0, 0, 0);
            ESP_LOGI(TAG, "ESCRITO NO TOPICO %s: ", MQTT_TOPIC);
            break;
        case MQTT_EVENT_DATA:
            printf("Mensagem no tópico: %.*s", event->topic_len, event->topic);
            printf("Conteudo: %.*s", event->data_len, event->data);

            if(strncmp(event-> data, "ON", event->data_len) == 0){
                set_led(1);
            } else if(strncmp(event->data, "OFF", event->data_len) ==0){
                set_led(0);
            }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;     
    }
}

//iniciando ao start do mqtt
static void mqtt_start(void){
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL, 
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg); //inicializando o cliente 
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL); // registra o evento mqtt
    esp_mqtt_client_start(client);
}

// task do ADC

static void adc_task(void *pvParameters) {
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)pvParameters;
    char msg[32];

    while (1) {
        uint32_t adc_value = adc_read_mv();
        snprintf(msg, sizeof(msg), "%d", adc_value);
        esp_mqtt_client_publish(client, MQTT_TOPIC, msg, 0, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


// Inicializando o ADC

void app_main(void){
    init_nvs();
    init_led();
    wifi_init();
    adc_init();
    mqtt_start();
}