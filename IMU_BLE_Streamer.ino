/*
 * IMU BLE Streamer for Waveshare ESP32-S3-Touch-LCD-1.46
 *
 * - Streams QMI8658 IMU data (200 Hz) over BLE via NimBLE in batches
 * - Saves timestamped data to TF card during recording sessions
 * - Displays IST on LCD; battery, BLE / record status
 * - Time synced from browser via BLE (fallback: on-board PCF85063 RTC)
 *
 * Dependencies:
 *   - NimBLE-Arduino v2.5.x (Arduino Library Manager)
 *   - LVGL v8.3.10 (from Waveshare demo package)
 *   - esp32 board package v3.0.2+
 *
 * Settings:
 *   Board: ESP32S3 Dev Module
 *   Flash Size: 16MB
 *   USB CDC On Boot: Enabled
 *   Partition: 16M Flash (3MB APP/9.9MB FATFS)
 *   Core Debug Level: Info
 *
 * Architecture (two fan-out rings so BLE and SD both get every sample):
 *   IMU task (Core 1, P5) -> ringBuf (BLE)  -> BLE notify task (Core 0, P4)
 *                        -> sdRing (SD)     -> SD write task (Core 0, P1)
 */

#include <NimBLEDevice.h>
#include "Display_SPD2010.h"
#include "Gyro_QMI8658.h"
#include "LVGL_Driver.h"
#include "RTC_PCF85063.h"
#include "SD_Card.h"
#include "BAT_Driver.h"
#include "PWR_Key.h"

// ─── BLE Configuration ───────────────────────────────────────────
#define DEVICE_NAME           "ESP32S3-IMU"
#define SERVICE_UUID          "19b10000-e8f2-537e-4f6c-d104768a1214"
#define IMU_CHAR_UUID         "19b10001-e8f2-537e-4f6c-d104768a1214"
#define CMD_CHAR_UUID         "19b10002-e8f2-537e-4f6c-d104768a1214"
#define STATUS_CHAR_UUID      "19b10003-e8f2-537e-4f6c-d104768a1214"

// Command bytes
#define CMD_START_RECORD      0x01
#define CMD_STOP_RECORD       0x02
#define CMD_SYNC_TIME         0x03

// ─── Packet Format ───────────────────────────────────────────────
#define IMU_SAMPLES_PER_PACKET  8
#define PACKET_MAGIC           0xBEEF
#define PKT_TYPE_IMU           0x01

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t seq;
    uint8_t  pkt_type;
    uint8_t  reserved;
    uint32_t ts_ms;
    struct __attribute__((packed)) {
        int16_t ax, ay, az;
        int16_t gx, gy, gz;
    } samples[IMU_SAMPLES_PER_PACKET];
} ImuPacket;

// ─── Shared Sample ───────────────────────────────────────────────
typedef struct {
    int16_t ax, ay, az, gx, gy, gz;
    uint32_t ts_ms;
    uint32_t epoch_ms;
} SampleEntry;

// ─── Ring Buffer (DROP_OLDEST, never blocks producer) ───────────
#define RING_SIZE 1024

typedef struct {
    SampleEntry buf[RING_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint16_t count;
    SemaphoreHandle_t mutex;
} RingBuffer;

static RingBuffer bleRing;
static RingBuffer sdRing;

static void ringInit(RingBuffer *r) {
    r->head = 0; r->tail = 0; r->count = 0;
    r->mutex = xSemaphoreCreateMutex();
}

static bool ringPush(RingBuffer *r, SampleEntry *s) {
    if (xSemaphoreTake(r->mutex, 0) != pdTRUE) return false;
    if (r->count >= RING_SIZE) {
        r->tail = (r->tail + 1) % RING_SIZE;   // drop oldest
        r->count--;
    }
    r->buf[r->head] = *s;
    r->head = (r->head + 1) % RING_SIZE;
    r->count++;
    xSemaphoreGive(r->mutex);
    return true;
}

static void ringClear(RingBuffer *r) {
    if (xSemaphoreTake(r->mutex, 0) != pdTRUE) return;
    r->head = 0; r->tail = 0; r->count = 0;
    xSemaphoreGive(r->mutex);
}

static bool ringPop(RingBuffer *r, SampleEntry *s) {
    if (xSemaphoreTake(r->mutex, 0) != pdTRUE) return false;
    if (r->count == 0) {
        xSemaphoreGive(r->mutex);
        return false;
    }
    *s = r->buf[r->tail];
    r->tail = (r->tail + 1) % RING_SIZE;
    r->count--;
    xSemaphoreGive(r->mutex);
    return true;
}

// ─── Global State ────────────────────────────────────────────────
static NimBLEServer *pServer = NULL;
static NimBLECharacteristic *pImuChar = NULL;
static NimBLECharacteristic *pCmdChar = NULL;
static NimBLECharacteristic *pStatusChar = NULL;

static void startRecordingFromBLE(void);
static void stopRecordingFromBLE(void);

static volatile bool bleConnected = false;
static volatile bool recording = false;
static bool sdReady = false;
static uint16_t bleSeq = 0;
static volatile uint32_t totalSamples = 0;

static uint32_t bleTimeOffset = 0;
static uint32_t bleTimeMillis = 0;
static bool timeSet = false;

// Display update
static char dispIST[32] = "IST: --:--:--";
static char dispBLE[32] = "BLE: Disconnected";
static char dispRec[32] = "Recording: OFF";

// ─── IMU scaling (match Gyro_QMI8658.cpp defaults) ───────────────
#define ACCEL_LSB_PER_G    (32768.0f / 4.0f)     // 4G range
#define GYRO_LSB_PER_DPS   (32768.0f / 512.0f)   // 512 dps range

// ─── IST Time (UTC+5:30) ─────────────────────────────────────────
static uint32_t getCurrentEpoch(void) {
    if (timeSet && bleTimeOffset > 0) {
        return bleTimeOffset + (millis() - bleTimeMillis) / 1000;
    }
    return PCF85063_GetEpoch();
}

static void formatIST(char *buf, size_t len) {
    uint32_t epoch = getCurrentEpoch();
    if (epoch < 1000000000) {           // not yet in Unix-time era
        snprintf(buf, len, "IST: --:--:--");
        return;
    }
    uint32_t istEpoch = epoch + 19800;  // UTC + 5:30
    time_t t = (time_t)istEpoch;
    struct tm *tm_info = localtime(&t);
    static const char *dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    snprintf(buf, len, "IST: %s %02d:%02d:%02d",
             dayNames[tm_info->tm_wday],
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

// ─── BLE Callbacks ───────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s, NimBLEConnInfo &cinfo) override {
        bleConnected = true;
        strlcpy(dispBLE, "BLE: Connected", sizeof(dispBLE));
        printf("BLE connected, requesting fast CI\r\n");
        s->updateConnParams(cinfo.getConnHandle(), 6, 6, 0, 500);
    }
    void onDisconnect(NimBLEServer *s, NimBLEConnInfo &cinfo, int reason) override {
        bleConnected = false;
        if (recording) stopRecordingFromBLE();
        strlcpy(dispBLE, "BLE: Disconnected", sizeof(dispBLE));
        NimBLEDevice::startAdvertising();
        printf("BLE disconnected, restarting adv\r\n");
    }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() < 1) return;
        uint8_t cmd = value[0];
        switch (cmd) {
            case CMD_START_RECORD: startRecordingFromBLE(); break;
            case CMD_STOP_RECORD:  stopRecordingFromBLE();  break;
            case CMD_SYNC_TIME:
                if (value.length() >= 5) {
                    uint32_t epoch;
                    memcpy(&epoch, &value[1], 4);
                    bleTimeOffset = epoch;
                    bleTimeMillis = millis();
                    timeSet = true;
                    PCF85063_SetFromEpoch(epoch);
                    printf("Time synced: %lu\r\n", (unsigned long)epoch);
                }
                break;
        }
    }
};

static ServerCallbacks serverCB;
static CmdCallbacks cmdCB;

static void startRecordingFromBLE(void) {
    if (recording) return;
    ringClear(&bleRing);         // drop pre-session stale samples
    ringClear(&sdRing);
    bleSeq = 0;                  // browser tracks loss from seq 0
    if (sdReady) {
        if (!SD_Logger_Open()) {
            printf("SD Logger unavailable, recording to BLE only\r\n");
        }
    }
    recording = true;
    strlcpy(dispRec, "Recording: ON", sizeof(dispRec));
    printf("CMD: Start recording\r\n");
}

static void stopRecordingFromBLE(void) {
    if (!recording) return;
    recording = false;
    if (SD_Logger_IsOpen()) {
        SD_Logger_Flush();
        SD_Logger_Close();
    }
    strlcpy(dispRec, "Recording: OFF", sizeof(dispRec));
    printf("CMD: Stop recording, SD flushed\r\n");
}

// ─── BLE Init ────────────────────────────────────────────────────
static void BLE_Init(void) {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setMTU(247);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(&serverCB);

    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    pImuChar = pService->createCharacteristic(
        IMU_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );
    pImuChar->createDescriptor("2902", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);

    pCmdChar = pService->createCharacteristic(
        CMD_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pCmdChar->setCallbacks(&cmdCB);

    pStatusChar = pService->createCharacteristic(
        STATUS_CHAR_UUID,
        NIMBLE_PROPERTY::READ
    );

    pService->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->enableScanResponse(true);
    adv->setPreferredParams(0x06, 0x0C);   // 7.5-15 ms connection interval
    NimBLEDevice::startAdvertising();

    printf("BLE started as '%s'\r\n", DEVICE_NAME);
}

// ─── Task: IMU Reader (Core 1, highest priority) ────────────────
static void imuTask(void *pvParameters) {
    printf("IMU task on core %d\r\n", xPortGetCoreID());
    TickType_t xLastWake = xTaskGetTickCount();
    int16_t ax, ay, az, gx, gy, gz;

    while (1) {
        QMI8658_BurstRead(&ax, &ay, &az, &gx, &gy, &gz);

        uint32_t now_ms = millis();
        uint32_t epoch_ms = 0;
        if (timeSet && bleTimeOffset > 0) {
            epoch_ms = (bleTimeOffset * 1000) + (now_ms - bleTimeMillis);
        }

        SampleEntry entry;
        entry.ax = ax; entry.ay = ay; entry.az = az;
        entry.gx = gx; entry.gy = gy; entry.gz = gz;
        entry.ts_ms = now_ms;
        entry.epoch_ms = epoch_ms;

        ringPush(&bleRing, &entry);
        if (recording) ringPush(&sdRing, &entry);
        totalSamples++;

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(5)); // 200 Hz
    }
}

// ─── Task: BLE Notify (Core 0, high priority) ───────────────────
static void bleTask(void *pvParameters) {
    printf("BLE task on core %d\r\n", xPortGetCoreID());
    ImuPacket pkt;
    SampleEntry batch[IMU_SAMPLES_PER_PACKET];
    TickType_t xLastWake = xTaskGetTickCount();

    while (1) {
        if (bleConnected && recording) {
            uint8_t count = 0;
            while (count < IMU_SAMPLES_PER_PACKET && ringPop(&bleRing, &batch[count])) {
                count++;
            }
            if (count > 0) {
                pkt.magic = PACKET_MAGIC;
                pkt.seq = bleSeq++;
                pkt.pkt_type = PKT_TYPE_IMU;
                pkt.reserved = 0;
                pkt.ts_ms = batch[0].ts_ms;

                for (uint8_t i = 0; i < count; i++) {
                    pkt.samples[i].ax = batch[i].ax;
                    pkt.samples[i].ay = batch[i].ay;
                    pkt.samples[i].az = batch[i].az;
                    pkt.samples[i].gx = batch[i].gx;
                    pkt.samples[i].gy = batch[i].gy;
                    pkt.samples[i].gz = batch[i].gz;
                }

                pImuChar->setValue((uint8_t *)&pkt, 8 + (size_t)count * 12);
                pImuChar->notify();
            }
        }

        // ~25 notifications/s (8 samples each = full 200 Hz)
        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(40));
    }
}

// ─── Task: SD Logger (Core 0, low priority) ─────────────────────
static void sdTask(void *pvParameters) {
    printf("SD task on core %d\r\n", xPortGetCoreID());
    SampleEntry entry;
    TickType_t xLastWake = xTaskGetTickCount();
    TickType_t xLastFlush = xTaskGetTickCount();

    while (1) {
        if (recording && sdReady) {
            while (ringPop(&sdRing, &entry)) {
                char line[128];
                snprintf(line, sizeof(line), "%lu,%lu,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f",
                         (unsigned long)entry.ts_ms,
                         (unsigned long)(entry.epoch_ms / 1000),
                         (float)entry.ax / ACCEL_LSB_PER_G,
                         (float)entry.ay / ACCEL_LSB_PER_G,
                         (float)entry.az / ACCEL_LSB_PER_G,
                         (float)entry.gx / GYRO_LSB_PER_DPS,
                         (float)entry.gy / GYRO_LSB_PER_DPS,
                         (float)entry.gz / GYRO_LSB_PER_DPS);
                SD_Logger_Write(line);
            }
        }

        // Periodic flush every 10 s to protect against power loss
        if (SD_Logger_IsOpen() &&
            (xTaskGetTickCount() - xLastFlush) >= pdMS_TO_TICKS(10000)) {
            SD_Logger_Flush();
            xLastFlush = xTaskGetTickCount();
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(100));
    }
}

// ─── Task: Display Update (Core 0, medium priority) ──────────────
static void displayTask(void *pvParameters) {
    printf("Display task on core %d\r\n", xPortGetCoreID());
    while (1) {
        formatIST(dispIST, sizeof(dispIST));
        float batV = BAT_Get_Volts();
        Lvgl_Update_Display(dispIST, dispBLE, dispRec, totalSamples, batV);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ─── Hardware init (Waveshare driver stack) ──────────────────────
void Driver_Init()
{
    Flash_test();
    PWR_Init();
    BAT_Init();
    I2C_Init();
    TCA9554PWR_Init(0x00);
    Backlight_Init();
    Set_Backlight(50);
    PCF85063_Init();
    QMI8658_Init();
}

// ─── Setup ───────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    printf("\r\n=== IMU BLE Streamer ===\r\n");

    Driver_Init();

    SD_Init();
    sdReady = (SD_MMC.cardType() != CARD_NONE);
    if (!sdReady) {
        printf("SD card not found (recordings will be BLE-only)\r\n");
    }

    LCD_Init();
    Lvgl_Init();

    ringInit(&bleRing);
    ringInit(&sdRing);

    BLE_Init();

    xTaskCreatePinnedToCore(imuTask,    "IMU", 4096, NULL, 5, NULL, 1);  // Core 1, real-time
    xTaskCreatePinnedToCore(bleTask,    "BLE", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(sdTask,     "SD",  8192, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(displayTask,"LCD", 4096, NULL, 2, NULL, 0);

    printf("All tasks started\r\n");
}

// ─── Loop (LVGL handler) ────────────────────────────────────────
void loop() {
    Lvgl_Loop();
    vTaskDelay(pdMS_TO_TICKS(5));
}