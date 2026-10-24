#include <SPIFFS.h>
#include <WiFiManager.h>
#include "local_secrets.h"
#include <Arduino.h>
#include <WiFi.h>
#include "board_config.h"
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "hid_actions.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
// #include <ArduinoOTA.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "ui.h"
#include "touch_esp32.h"
#include "ambient_esp32.h"
#include "touch_live_config.h"
#include "touch_spi_diag.h"
#include "encoder_esp32.h"
#include "hid_output_config.h"
#include <math.h>

static_assert(sizeof(NEXUS_WIFI_AP_PASSWORD) >= 9 && sizeof(NEXUS_WIFI_AP_PASSWORD) <= 64,
              "Set a local provisioning AP password of 8..63 bytes in include/local_secrets.h");

typedef enum {
    SYS_MODE_NORMAL,
    SYS_MODE_CODING,
    SYS_MODE_BROWSER,
    SYS_MODE_MEDIA
} SystemMode_t;

// 状态机消息队列
QueueHandle_t modeQueue;

TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[320 * 10];

WiFiManager wm;
Adafruit_SHT31 SHT30;
USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl; // Registers a distinct Consumer report before USB.begin().
SemaphoreHandle_t dataMutex;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Protected by dataMutex. Only loop() applies this snapshot to the UI.
struct RuntimeData {
    float temp = 0, humi = 0;
    bool environment_valid = false;
    float cpu = 0, mem = 0, cpu_temp = 0;
    int mode = SYS_MODE_NORMAL;
    bool mqtt_connected = false;
    bool pc_seen = false, cpu_valid = false, mem_valid = false, cpu_temp_valid = false;
    uint32_t pc_updated_at = 0;
#if NEXUS_ENCODER_PREVIEW
    // A telemetry-only frame must not be mistaken for an actual NORMAL status.
    bool pc_scene_seen = false;
    uint32_t pc_scene_updated_at = 0, pc_scene_since = 0;
#endif
};
RuntimeData runtimeData;

static constexpr uint32_t PC_TIMEOUT_MS = 5000;
static constexpr size_t PC_FRAME_LIMIT = 512;

static bool pcOnline(const RuntimeData &data, uint32_t now){
    return data.pc_seen && (uint32_t)(now - data.pc_updated_at) < PC_TIMEOUT_MS;
}

// Track consumption so a valid JSON prefix followed by garbage is rejected.
struct PcJsonReader {
    const char *text;
    size_t length, position;
    int read(){ return position < length ? (unsigned char)text[position++] : -1; }
    size_t readBytes(char *buffer, size_t count){
        size_t copied = 0;
        while (copied < count && position < length) buffer[copied++] = text[position++];
        return copied;
    }
};

static bool applyPcFrame(const char *line, size_t length, uint32_t now){
    JsonDocument doc;
    PcJsonReader reader{line, length, 0};
    if (deserializeJson(doc, reader, DeserializationOption::NestingLimit(8)) ||
        !doc.is<JsonObject>()) return false;
    for (size_t i = reader.position; i < length; ++i){
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') return false;
    }
    JsonVariantConst cpu = doc["cpu"], mem = doc["mem"];
    JsonVariantConst temperature = doc["cpu_temp"], status = doc["status"];
    bool hasCpu = !cpu.isUnbound(), hasMem = !mem.isUnbound();
    bool hasTemperature = !temperature.isUnbound(), hasStatus = !status.isUnbound();
    if (!hasCpu && !hasMem && !hasTemperature && !hasStatus) return false;
    float cpuValue = 0, memValue = 0, temperatureValue = 0;
    if (hasCpu){
        if (!cpu.is<float>()) return false;
        cpuValue = cpu.as<float>();
        if (!isfinite(cpuValue) || cpuValue < 0 || cpuValue > 100) return false;
    }
    if (hasMem){
        if (!mem.is<float>()) return false;
        memValue = mem.as<float>();
        if (!isfinite(memValue) || memValue < 0 || memValue > 100) return false;
    }
    bool temperatureValid = hasTemperature && !temperature.isNull();
    if (temperatureValid){
        if (!temperature.is<float>()) return false;
        temperatureValue = temperature.as<float>();
        if (!isfinite(temperatureValue)) return false;
    }
    SystemMode_t mode = SYS_MODE_NORMAL;
    if (hasStatus){
        if (!status.is<const char*>()) return false;
        const char *name = status.as<const char*>();
        if (!strcmp(name, "coding")) mode = SYS_MODE_CODING;
        else if (!strcmp(name, "browser")) mode = SYS_MODE_BROWSER;
        else if (!strcmp(name, "media")) mode = SYS_MODE_MEDIA;
        else if (strcmp(name, "normal")) return false;
    }
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    if (hasCpu){ runtimeData.cpu = cpuValue; runtimeData.cpu_valid = true; }
    if (hasMem){ runtimeData.mem = memValue; runtimeData.mem_valid = true; }
    if (hasTemperature){
        if (temperatureValid) runtimeData.cpu_temp = temperatureValue;
        runtimeData.cpu_temp_valid = temperatureValid;
    }
    if (hasStatus){
#if NEXUS_ENCODER_PREVIEW
        if (!runtimeData.pc_scene_seen || runtimeData.mode != (int)mode ||
            uint32_t(now - runtimeData.pc_scene_updated_at) >= PC_TIMEOUT_MS)
            runtimeData.pc_scene_since = now;
        runtimeData.pc_scene_seen = true;
        runtimeData.pc_scene_updated_at = now;
#endif
        runtimeData.mode = (int)mode;
    }
    runtimeData.pc_seen = true;
    runtimeData.pc_updated_at = now;
    xSemaphoreGive(dataMutex);
    return true;
}

static void receivePcByte(char byte, uint32_t now){
    static char line[PC_FRAME_LIMIT + 1];
    static size_t length = 0;
    static bool discard = false;
    if (byte == '\n'){
        if (!discard && length){
            line[length] = '\0';
            applyPcFrame(line, length, now);
        }
        length = 0;
        discard = false;
    } else if (!discard){
        if (byte == '\0' || length == PC_FRAME_LIMIT){
            discard = true; // Drop the whole oversized/binary line, up to LF.
            length = 0;
        } else {
            line[length++] = byte;
        }
    }
}

#if NEXUS_ENCODER_PREVIEW
// UI task only. One boot-time demo unlock; ordinary scene navigation stays intact.
static void servicePreviewScene(const RuntimeData &data, uint32_t now){
    static bool unlocked = false;
    if (unlocked || !pcOnline(data, now) || !data.pc_scene_seen ||
        uint32_t(now - data.pc_scene_updated_at) >= PC_TIMEOUT_MS ||
        uint32_t(now - data.pc_scene_since) < nexus::Navigation::debounce_ms) return;
    unlocked = ui_preview_unlock_scene(data.mode);
}
#endif

// Macro requests may come from LVGL or MQTT; only loop() sends HID reports.
static USBHID macroHid;
static portMUX_TYPE macroMux = portMUX_INITIALIZER_UNLOCKED;
static bool macroBusy = false, macroPending = false;
static uint8_t requestedMacro = 0;

bool executeMacro(uint8_t id){
    if (!nexus::hidAction(id)) return false;
    portENTER_CRITICAL(&macroMux);
    bool accepted = !macroBusy;
    if (accepted){
        requestedMacro = id;
        macroBusy = macroPending = true;
    }
    portEXIT_CRITICAL(&macroMux);
    return accepted;
}

static void onMacroRequested(int id){
    if (!nexus::hidAction(id)){
        ui_set_macro_status("Invalid macro");
    } else if (executeMacro((uint8_t)id)){
        char message[32];
        snprintf(message, sizeof(message), "Queued: %s", nexus::hidAction(id)->name);
        ui_set_macro_status(message);
    } else {
        ui_set_macro_status("Macro busy");
    }
}

static bool sendActionReport(uint8_t id,bool release) {
#if NEXUS_TOUCH_LIVE_PREVIEW || NEXUS_HID_OUTPUT_DISABLED
    (void)id;(void)release;
    return false; // No keyboard/Consumer report is compiled into this preview sender.
#else
    const auto* action=nexus::hidAction(id);
    if(!action) return false;
    if(action->kind==nexus::HidKind::Consumer) {
        uint16_t usage=release ? 0 : action->usage;
        uint8_t payload[2]={uint8_t(usage),uint8_t(usage>>8)};
        return macroHid.SendReport(HID_REPORT_ID_CONSUMER_CONTROL,payload,sizeof(payload),10);
    }
    KeyReport report={};
    if(!release) { report.modifiers=action->modifiers; report.keys[0]=action->key; }
    return macroHid.SendReport(HID_REPORT_ID_KEYBOARD,&report,sizeof(report),10);
#endif
}

static void serviceMacro(){
    enum Phase { IDLE, HOLDING, RELEASING };
    static Phase phase = IDLE;
    static uint8_t id = 0;
    static uint32_t pressedAt = 0, lastReleaseAttempt = 0;
    static bool downSent = false;

    if (phase == IDLE){
        portENTER_CRITICAL(&macroMux);
        bool pending = macroPending;
        if (pending){
            id = requestedMacro;
            macroPending = false;
        }
        portEXIT_CRITICAL(&macroMux);
        if (!pending) return;
#if NEXUS_TOUCH_LIVE_PREVIEW || NEXUS_HID_OUTPUT_DISABLED
        char preview[48];
        snprintf(preview,sizeof(preview),"PREVIEW HID OFF: %s",nexus::hidAction(id)->name);
        ui_set_macro_status(preview);
        portENTER_CRITICAL(&macroMux);macroBusy=false;portEXIT_CRITICAL(&macroMux);
        return; // Applies to both LVGL and existing MQTT executeMacro() requests.
#endif
        if (!USB || !macroHid.ready()){
            ui_set_macro_status("USB not ready");
            portENTER_CRITICAL(&macroMux);
            macroBusy = false;
            portEXIT_CRITICAL(&macroMux);
            return; // Never replay a key-down when USB later connects.
        }
        downSent = sendActionReport(id,false);
        pressedAt = millis();
        // A timeout can occur after submitting a report: always attempt key-up.
        phase = downSent ? HOLDING : RELEASING;
        lastReleaseAttempt = pressedAt - 20U;
        if (!downSent) ui_set_macro_status("HID send failed");
        return;
    }

    if (!USB){
        ui_set_macro_status("USB disconnected");
    } else {
        uint32_t now = millis();
        if (phase == HOLDING){
            if (now - pressedAt < 30U) return;
            phase = RELEASING;
        }
        if (now - lastReleaseAttempt < 20U) return;
        lastReleaseAttempt = now;
        if (!macroHid.ready() || !sendActionReport(id,true)){
            ui_set_macro_status("Key-up pending");
            return; // Keep busy; retry only the all-released report.
        }
        char message[32];
        snprintf(message, sizeof(message), "%s: %s", downSent ? "Sent" : "Failed", nexus::hidAction(id)->name);
        ui_set_macro_status(message); // USB report result, not a PC action ACK.
    }
    phase = IDLE;
    portENTER_CRITICAL(&macroMux);
    macroBusy = false;
    portEXIT_CRITICAL(&macroMux);
}

// void broadcastData(){
//     double temp, humi, cpu, mem;
//     temp = random(0, 100);
//     humi = random(0, 100);
//     cpu = random(0, 100);
//     mem = random(0, 100);
//     String jsonData = "{\"temp\":" + String(temp) + ", \"humi\":" + String(humi) + ", \"cpu\":" + String(cpu) + ", \"mem\":" + String(mem) + "}";
//     ws.textAll(jsonData);
// }

void Task_SampleData(void *pvParameters){
    while(1){
        float temp = SHT30.readTemperature();
        float humi = SHT30.readHumidity();
        if (xSemaphoreTake(dataMutex, 100 / portTICK_PERIOD_MS) == pdTRUE){
            runtimeData.temp = temp;
            runtimeData.humi = humi;
            runtimeData.environment_valid = isfinite(temp) && isfinite(humi);
            xSemaphoreGive(dataMutex);
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
        String receivedMessage = "";
        for (size_t i = 0; i < length; i++){
            receivedMessage += (char) payload[i];
        }
        Serial.print("收到网页 JSON: ");
        Serial.println(receivedMessage);
        if (receivedMessage.indexOf("emergency") >= 0){
            Serial.println(executeMacro(3) ? "Win+D macro queued" : "Macro busy");
        }
        else if (receivedMessage.indexOf("A") >= 0){
            Serial.println("收到A指令");
        }
        else if (receivedMessage.indexOf("C") >= 0){
            Serial.println("收到C指令");
        }
        else if (receivedMessage.indexOf("reset_wifi") >= 0){
            Serial.println("清除 WiFi 记忆并重启");
            wm.resetSettings();
            delay(1000);
            ESP.restart();
        }
}

// One owner for WiFiManager and PubSubClient. Their synchronous connection/
// packet waits must never run in loop(), the sampler, or the USB RX task.
void Task_Network(void *pvParameters){
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    wm.setConfigPortalBlocking(false);
    wm.setConnectTimeout(5);
    wm.setSaveConnectTimeout(5);
    wm.autoConnect("Nexus_S1 WiFi", NEXUS_WIFI_AP_PASSWORD);

    espClient.setTimeout(1);       // Arduino ESP32 WiFiClient: seconds
    mqttClient.setSocketTimeout(1);
    mqttClient.setServer(NEXUS_MQTT_HOST, NEXUS_MQTT_PORT);
    mqttClient.setCallback(mqttCallback);

    uint32_t lastWifiAttempt = millis();
    uint32_t lastMqttAttempt = millis() - 5000U;
    uint32_t lastPublish = millis();
    while(1){
        wm.process();
        uint32_t now = millis();
        if (!WiFi.isConnected()){
            if (mqttClient.connected()) mqttClient.disconnect();
            if (!wm.getConfigPortalActive() &&
                now - lastWifiAttempt >= 10000U){
                lastWifiAttempt = now;
                WiFi.reconnect();
            }
        } else {
            if (!mqttClient.connected() && now - lastMqttAttempt >= 5000U){
                if (mqttClient.connect("ESP32_Nexus_Client")){
                    mqttClient.subscribe("nexus/esp32/command");
                }
                // Wait between completed attempts, without a long delay().
                lastMqttAttempt = millis();
            }
            if (mqttClient.connected()) mqttClient.loop();

            now = millis();
            if (mqttClient.connected() && now - lastPublish >= 1000U){
                RuntimeData snapshot;
                if (xSemaphoreTake(dataMutex, 0) == pdTRUE){
                    snapshot = runtimeData;
                    xSemaphoreGive(dataMutex);
                    JsonDocument doc;
                    doc["temp"] = snapshot.temp;
                    doc["humi"] = snapshot.humi;
                    doc["cpu"] = snapshot.cpu;
                    doc["mem"] = snapshot.mem;
                    doc["cpu_temp"] = snapshot.cpu_temp;
                    String jsonData;
                    serializeJson(doc, jsonData);
                    mqttClient.publish("nexus/esp32/sensor", jsonData.c_str());
                    lastPublish = millis();
                }
            }
        }
        bool mqttConnected = mqttClient.connected();
        if (xSemaphoreTake(dataMutex, 0) == pdTRUE){
            runtimeData.mqtt_connected = mqttConnected;
            xSemaphoreGive(dataMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void Task_SerialRead(void *pvParameters){
    while(1){
        // Bounded work per pass; never wait for the rest of an incomplete line.
        for (size_t i = 0; i < 128 && Serial.available() > 0; ++i){
            int byte = Serial.read();
            if (byte >= 0) receivePcByte((char)byte, millis());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// D0/D1 display gate: keep this actual flush path covered by host LVGL tests.
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
TouchSpiDisplayDiagnostic& nexusTouchDisplayDiagnostic() {
    static TouchSpiDisplayDiagnostic diagnostic;
    return diagnostic;
}
#endif
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p){
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
    auto& diagnostic=nexusTouchDisplayDiagnostic();
    // flush_ready clears flushing_last in LVGL 8.3.11: sample it first.
    const bool last=lv_disp_flush_is_last(disp);
    if(!diagnostic.frozen) {
#endif
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
    }
    diagnostic.flushed(area->x1,area->y1,area->x2,area->y2,last,!diagnostic.frozen);
#endif
    lv_disp_flush_ready(disp);
}
// End D0/D1 display gate.

void setup(){
    modeQueue = xQueueCreate(5, sizeof(SystemMode_t));

    // 唤醒 USBHID
    Keyboard.begin();
    ConsumerControl.begin();
    USB.begin();
    delay(500);

    // 唤醒单片机 + 配置波特率
    Serial.begin(115200);
    delay(2000);

    // 唤醒物理屏幕
    prepareNexusTouchPins(); // No GPIO access unless the touch configuration is valid.
    tft.begin();
    // 设置横屏
    tft.setRotation(1);
    // 唤醒 LVGL 引擎总控
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, 320 * 10);

    static lv_disp_drv_t disp_drv;

    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res = 320;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    ui_init();
    ui_set_macro_callback(onMacroRequested);
    initNexusTouch(tft); // Temporary raw diagnostics only; touch cannot operate the UI.
#if NEXUS_ENCODER_PREVIEW
    initNexusEncoder();
#endif

    // 初始化I2C
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    // 启动SHT30
    if (SHT30.begin(0x44)){
        Serial.println("SHT30启动成功");
    }
    else{
        Serial.println("SHT30启动失败");
    }

    // Mutex 互斥锁
    dataMutex = xSemaphoreCreateMutex();

    // 挂载判断
    if (!SPIFFS.begin(true)){
        Serial.println("SPIFFS 挂载失败");
        // return;
    }
    else{
        Serial.println("SPIFFS 挂载成功");
        File root = SPIFFS.open("/");
        File file = root.openNextFile();
        Serial.println("硬盘文件清单开始");
        while(file){
            Serial.print("文件名:");
            Serial.println(file.name());
            file = root.openNextFile();
        }
        Serial.println("硬盘文件清单结束");
    }

    // // OTA
    // ArduinoOTA.setHostname("Nexus_S1");
// OTA stays disabled; any future credential must also be configured locally.
    // ArduinoOTA.onStart([](){
    //     Serial.println("OTA开始");
    // });
    // ArduinoOTA.onEnd([](){
    //     Serial.println("OTA结束");
    // });
    // ArduinoOTA.onProgress([](unsigned int progress, unsigned int total){
    //     Serial.printf("OTA进度: %u%%\r", (progress / (total / 100)));
    // });
    // ArduinoOTA.onError([](ota_error_t error){
    //     Serial.printf("OTA出错: %u\n", error);
    // });

    // ArduinoOTA.begin();

    xTaskCreate(Task_SampleData, "Task_Data", 4096, NULL, 1, NULL);

    xTaskCreate(Task_SerialRead, "Task_Serial", 4096, NULL, 2, NULL);

    // Render locally before starting any optional network activity.
    lv_timer_handler();
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
    nexusTouchDisplayDiagnostic().afterHandler();
#endif
    if (xTaskCreate(Task_Network, "Task_Network", 6144, NULL, 1, NULL) != pdPASS){
        Serial.println("网络任务启动失败，本地 UI 继续运行");
    }

    // ()()启动！
    Serial.println("Nexus_S1系统启动!");
}

void loop(){
    serviceNexusTouchDiagnostics(); // Drain short CDC chunks; no waiting for host/SPI/LVGL.
    RuntimeData snapshot;
    if (xSemaphoreTake(dataMutex, 0) == pdTRUE){
        snapshot = runtimeData;
        xSemaphoreGive(dataMutex);
        ui_set_pc_state(pcOnline(snapshot, millis()), snapshot.cpu_valid,
                        snapshot.mem_valid, snapshot.cpu_temp_valid);
        ui_update_sensor(snapshot.temp, snapshot.humi, snapshot.cpu_temp,
                         snapshot.cpu, snapshot.mem);
        ui_set_environment_valid(snapshot.environment_valid);
        ui_set_mode(snapshot.mode);
#if NEXUS_ENCODER_PREVIEW
        servicePreviewScene(snapshot, millis());
#endif
        ui_set_mqtt_status(snapshot.mqtt_connected);
    }
    ui_set_wifi_status(WiFi.isConnected(), WiFi.localIP().toString().c_str());
    serviceAmbientClock(WiFi.isConnected());
    serviceMacro();
#if NEXUS_ENCODER_PREVIEW
    serviceNexusEncoder();
#endif
    lv_timer_handler();
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
    nexusTouchDisplayDiagnostic().afterHandler();
#endif
    delay(5);
    // ArduinoOTA.handle();
}
