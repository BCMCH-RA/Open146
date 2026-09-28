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
 *
 * Data-integrity notes (do not change without updating index.html too):
 *   - Every sample carries its OWN timestamp. A packet only carries the
 *     timestamp of its first sample; the rest are 16-bit ms offsets from it.
 *     Reconstructing per-sample times as ts + i*period is wrong the moment a
 *     single sample is dropped, and the drop is invisible to the client.
 *   - The header carries a cumulative drop counter so the client can see
 *     samples lost inside the ring, which sequence numbers cannot reveal.
 *   - Ring pushes report failure; failures are counted, never silently eaten.
 */

#include <NimBLEDevice.h>
#include <stdarg.h>
#include <time.h>
#include "Display_SPD2010.h"
#include "Gyro_QMI8658.h"
#include "LVGL_Driver.h"
#include "Tremor_Metric.h"
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
#define FILE_CHAR_UUID        "19b10004-e8f2-537e-4f6c-d104768a1214"

// Command bytes
#define CMD_START_RECORD      0x01
#define CMD_STOP_RECORD       0x02
#define CMD_SYNC_TIME         0x03
#define CMD_LIST_SESSIONS     0x04
#define CMD_DOWNLOAD_SESSION  0x05
#define CMD_DOWNLOAD_ABORT    0x06

// SD→BLE transfer sizing. DL_CHUNK_MAX is only an upper bound: the real chunk
// is additionally clamped to the negotiated ATT payload so downloads also work
// against peers that only accept MTU 185 (Android/Chrome default).
#define DL_CHUNK_MAX          160
#define DL_PKT_MAX            244
#define DL_TEXT_MAX           96
#define SD_PATH_MAX           40
#define SD_SESSION_SCAN_MAX   250

// ─── Packet Format (little-endian, packed) ───────────────────────
//  byte 0..1  magic      0xBEEF
//  byte 2..3  seq        packet counter, wraps at 65536
//  byte 4..7  ts_ms      millis() at the FIRST sample in this packet
//  byte 8..9  dropped    cumulative samples dropped from the BLE ring
//  byte 10    n_samples  samples carried by this packet
//  byte 11    pkt_type
//  byte 12..  samples, IMU_SAMPLE_BYTES each
// 16 samples per notification rather than 8: at 200 Hz that is 12.5
// notifications/s instead of 25, and each queued notification costs a NimBLE
// host buffer. Halving the notification rate halves the peak buffer demand,
// which is what "BLE_INIT: Malloc failed" on a phone was actually reporting.
// 12 + 16*14 = 236 bytes, still inside a 247-byte MTU.
#define IMU_SAMPLES_PER_PACKET  16
#define PACKET_MAGIC           0xBEEF
#define PKT_TYPE_IMU           0x01
#define STATUS_MAGIC           0xBEE5
#define PKT_HDR_BYTES          12
#define IMU_SAMPLE_BYTES       14
#define IMU_PKT_MAX_BYTES      (PKT_HDR_BYTES + IMU_SAMPLES_PER_PACKET * IMU_SAMPLE_BYTES)

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t seq;
    uint32_t ts_ms;
    uint16_t dropped;
    uint8_t  n_samples;
    uint8_t  pkt_type;
} ImuHdr;

typedef struct __attribute__((packed)) {
    int16_t  ax, ay, az;
    int16_t  gx, gy, gz;
    uint16_t rel_ms;      // ms since ImuHdr.ts_ms (wraps at 65.5 s, far beyond
                          // the 80 ms a single packet can ever span)
} ImuSample;

// Compile-time proof that the JS side's hard-coded offsets stay valid.
static_assert(sizeof(ImuHdr) == PKT_HDR_BYTES, "ImuHdr must be exactly 12 bytes");
static_assert(sizeof(ImuSample) == IMU_SAMPLE_BYTES, "ImuSample must be exactly 14 bytes");
static_assert(IMU_PKT_MAX_BYTES <= 244, "IMU packet must fit a 247-byte MTU");

// STATUS characteristic payload (16 bytes, refreshed at 2 Hz)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t flags;      // bit0: epoch_s/millis are meaningful
    uint32_t epoch_s;    // UTC epoch seconds
    uint32_t millis;     // device millis() at the moment epoch_s was valid
    uint32_t total;      // samples successfully pushed since boot
} StatusPayload;

static_assert(sizeof(StatusPayload) == 16, "StatusPayload must be exactly 16 bytes");

// ─── Shared Sample ───────────────────────────────────────────────
typedef struct {
    int16_t ax, ay, az, gx, gy, gz;
    uint32_t ts_ms;       // milliseconds since boot
    uint32_t epoch_s;     // UTC epoch SECONDS (never ms: ms overflows uint32)
} SampleEntry;

// ─── Ring Buffer (DROP_OLDEST, never blocks producer) ───────────
// 512 entries = 2.6 s of headroom at 200 Hz, which is still 30x the 80 ms
// BLE drain interval and absorbs SD card stalls. This was 2048 (10.2 s),
// which cost 2048*20*2 = 80 KB of DRAM across the two rings. That is the
// heap NimBLE needs for its own host buffers, and starving it showed up as
// "BLE_INIT: Malloc failed", which in turn failed every ATT write and
// silently dropped queued notifications.
#define RING_SIZE 512
#define RING_MUTEX_WAIT_MS 5

typedef struct {
    SampleEntry buf[RING_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint16_t count;
    volatile uint32_t drops;      // cumulative, never reset by ringClear
    SemaphoreHandle_t mutex;
} RingBuffer;

static RingBuffer bleRing;
static RingBuffer sdRing;

static bool ringInit(RingBuffer *r) {
    r->head = 0; r->tail = 0; r->count = 0; r->drops = 0;
    r->mutex = xSemaphoreCreateMutex();
    return r->mutex != NULL;
}

static bool ringPush(RingBuffer *r, const SampleEntry *s) {
    if (xSemaphoreTake(r->mutex, pdMS_TO_TICKS(RING_MUTEX_WAIT_MS)) != pdTRUE) {
        r->drops++;                       // contended: count it, do not eat it
        return false;
    }
    if (r->count >= RING_SIZE) {
        r->tail = (r->tail + 1) % RING_SIZE;   // drop oldest
        r->count--;
        r->drops++;
    }
    r->buf[r->head] = *s;
    r->head = (r->head + 1) % RING_SIZE;
    r->count++;
    xSemaphoreGive(r->mutex);
    return true;
}

// Must actually clear: a silent no-op here would let pre-session samples leak
// into a new recording, so a contended attempt is retried rather than ignored.
static bool ringClear(RingBuffer *r) {
    for (int attempt = 0; attempt < 10; attempt++) {
        if (xSemaphoreTake(r->mutex, pdMS_TO_TICKS(RING_MUTEX_WAIT_MS)) == pdTRUE) {
            r->head = 0; r->tail = 0; r->count = 0;
            xSemaphoreGive(r->mutex);
            return true;
        }
    }
    printf("ringClear: mutex unavailable\r\n");
    return false;
}

static bool ringPop(RingBuffer *r, SampleEntry *s) {
    if (xSemaphoreTake(r->mutex, pdMS_TO_TICKS(RING_MUTEX_WAIT_MS)) != pdTRUE) return false;
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
static NimBLECharacteristic *pFileChar = NULL;

typedef struct {
    uint8_t cmd;
    uint8_t sessionIdx;
} DlRequest;
static QueueHandle_t dlQueue = NULL;
static QueueHandle_t ctrlQueue = NULL;   // record start/stop, keeps FAT I/O
                                         // out of the NimBLE host task
static volatile bool dlActive = false;
static volatile bool dlAbort = false;

static void postRecordCmd(uint8_t cmd);

static volatile bool bleConnected = false;
static volatile uint16_t bleConnHandle = 0;
static volatile bool recording = false;

// Set when a client enables notifications on the IMU characteristic. The
// previous design required CMD_START as well, which meant a client that
// connected, subscribed and synced time — but never sent 0x01 — sat connected
// forever receiving nothing, with no error to point at. Subscribing is the
// unambiguous "I want the stream" signal, so it is the only one needed.
// CMD_START still works and additionally opens the SD log.
static volatile bool streamArmed = false;
static bool sdReady = false;
static uint16_t bleSeq = 0;
static volatile uint32_t totalSamples = 0;

// notify() reports whether the stack actually queued the packet. Ignoring it
// made "packets produced" indistinguishable from "packets sent", which is the
// difference between a client that is not subscribed and a link that is
// dropping notifications.
static volatile uint32_t blePktSent = 0;
static volatile uint32_t bleNotifyFail = 0;
static volatile uint32_t bleRingDrops = 0;
static bool wireDumped = false;   // one hex dump per session

static uint32_t bleTimeOffset = 0;     // UTC epoch seconds
static uint32_t bleTimeMillis = 0;
static bool timeSet = false;

// RTC epoch is refreshed once per second: PCF85063_GetEpoch() does a full I2C
// read plus mktime(), which is far too heavy for the 200 Hz IMU path.
static uint32_t rtcEpochCache = RTC_EPOCH_UNSET;
static uint32_t rtcEpochMillis = 0;
// Epoch requested by CMD_SYNC_TIME, applied by displayTask. 0xFFFFFFFF is the
// RTC_EPOCH_UNSET sentinel, so it doubles as "nothing pending".
static volatile uint32_t rtcSetEpoch = RTC_EPOCH_UNSET;

// Display update
static char dispIST[32] = "IST: --:--:--";
static char dispDate[32] = "---- -- --";
static char dispBLE[32] = "BLE: Disconnected";
static char dispRec[32] = "Recording: OFF";
static char dispSev[16] = "CALM";
static uint32_t dispSevColor = 0x00E676;
static float tremorRms = 0.0f;
static float batVolts = 0.0f;
static uint8_t batteryPercent = 0;
static volatile bool batteryLow = false;

// ─── IMU scaling (matches acc_scale=ACC_RANGE_4G, gyro_scale=GYR_RANGE_512DPS
//     configured in Gyro_QMI8658.cpp) ─────────────────────────────
#define ACCEL_LSB_PER_G    (32768.0f / 4.0f)     // 4G range
#define GYRO_LSB_PER_DPS   (32768.0f / 512.0f)   // 512 dps range

// ─── Time ────────────────────────────────────────────────────────
static uint32_t getCurrentEpoch(void) {
    if (timeSet && bleTimeOffset > 0) {
        return bleTimeOffset + (millis() - bleTimeMillis) / 1000;
    }
    if (rtcEpochCache != RTC_EPOCH_UNSET) {
        return rtcEpochCache + (millis() - rtcEpochMillis) / 1000;
    }
    return RTC_EPOCH_UNSET;
}

static void formatIST(char *buf, size_t len) {
    uint32_t epoch = getCurrentEpoch();
    if (epoch == RTC_EPOCH_UNSET || epoch < 1000000000) {   // not in Unix-time era
        snprintf(buf, len, "IST: --:--:--");
        return;
    }
    uint32_t istEpoch = epoch + 19800;  // UTC + 5:30
    time_t t = (time_t)istEpoch;
    struct tm *tm_info = localtime(&t);
    if (!tm_info) { snprintf(buf, len, "IST: --:--:--"); return; }
    static const char *dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    snprintf(buf, len, "IST: %s %02d:%02d:%02d",
             dayNames[tm_info->tm_wday],
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

// IST date for the elderly display: "SUN 18 SEP 2026" (big, readable)
static void formatISTDate(char *buf, size_t len) {
    uint32_t epoch = getCurrentEpoch();
    if (epoch == RTC_EPOCH_UNSET || epoch < 1000000000) {
        snprintf(buf, len, "-- -- ----");
        return;
    }
    uint32_t istEpoch = epoch + 19800;  // UTC + 5:30
    time_t t = (time_t)istEpoch;
    struct tm *tm_info = localtime(&t);
    if (!tm_info) { snprintf(buf, len, "-- -- ----"); return; }
    static const char *dayNames[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *monNames[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                     "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    snprintf(buf, len, "%s %02d %s %04d",
             dayNames[tm_info->tm_wday], tm_info->tm_mday,
             monNames[tm_info->tm_mon], tm_info->tm_year + 1900);
}

// ─── BLE Callbacks ───────────────────────────────────────────────
// Defined further down with the other MTU helpers; needed here so onConnect
// can report the per-packet sample budget the link actually allows.
static uint8_t imuSamplesPerPacket(void);

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s, NimBLEConnInfo &cinfo) override {
        bleConnHandle = cinfo.getConnHandle();
        bleConnected = true;
        strlcpy(dispBLE, "BLE: Connected", sizeof(dispBLE));
        printf("BLE connected, requesting CI\r\n");
        // Do NOT pin min == max. Asking for a 7.5 ms interval leaves the phone
        // no room to service the link, and a peer that cannot keep up turns
        // into a host-buffer backlog ("BLE_INIT: Malloc failed") rather than
        // into slower delivery. A wide range lets the phone choose, and a
        // 10 s supervision timeout tolerates a brief stall instead of
        // dropping the connection.
        s->updateConnParams(cinfo.getConnHandle(), 8, 40, 0, 1000);
    }
    // onConnect fires before the MTU exchange completes, so the MTU read
    // there is always the 23-byte default. This is the only place the real
    // negotiated value is known.
    void onMTUChange(uint16_t mtu, NimBLEConnInfo &connInfo) override {
        (void)connInfo;
        printf("MTU negotiated: %u (ATT payload %u, %u samples/packet)\r\n",
               (unsigned)mtu,
               (unsigned)((mtu > 23) ? (mtu - 3) : 20),
               (unsigned)imuSamplesPerPacket());
    }
    void onDisconnect(NimBLEServer *s, NimBLEConnInfo &cinfo, int reason) override {
        (void)s; (void)cinfo;
        bleConnected = false;
        // The reason is the whole diagnosis for a link that drops a few
        // seconds after connecting, and it was being thrown away. 0x08 is a
        // supervision timeout (the phone stopped hearing us, usually a
        // sustained notification flood); 0x13 is the client hanging up on its
        // own, which is what an app does when it waits for data that never
        // arrives.
        printf("BLE disconnected: reason 0x%02x | sent=%lu notify_fail=%lu "
               "ring_drops=%lu\n", (unsigned)reason,
               (unsigned long)blePktSent, (unsigned long)bleNotifyFail,
               (unsigned long)bleRingDrops);
        blePktSent = 0; bleNotifyFail = 0;
        streamArmed = false;
        // Queue the stop instead of doing SD I/O in the BLE host task.
        // Queueing also covers the browser simply closing the tab.
        postRecordCmd(CMD_STOP_RECORD);
        strlcpy(dispBLE, "BLE: Disconnected", sizeof(dispBLE));
        NimBLEDevice::startAdvertising();
    }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
        (void)connInfo;
        std::string value = pCharacteristic->getValue();
        // Log every command byte that actually arrives. "The app says it sent
        // the epoch" and "the app's write reached this handler" are different
        // claims, and only one of them was being assumed.
        printf("CMD rx: 0x%02X len=%u\r\n",
               value.length() ? (unsigned)(uint8_t)value[0] : 0u,
               (unsigned)value.length());
        if (value.length() < 1) return;
        uint8_t cmd = (uint8_t)value[0];
        switch (cmd) {
            case CMD_START_RECORD:
            case CMD_STOP_RECORD:
                // Cheap in-memory work is done by sdTask together with the SD
                // open/close so the ring reset and the seq reset are atomic
                // with respect to recording.
                postRecordCmd(cmd);
                break;
            case CMD_SYNC_TIME:
                if (value.length() >= 5) {
                    uint32_t epoch = (uint8_t)value[1]
                                   | ((uint32_t)(uint8_t)value[2] << 8)
                                   | ((uint32_t)(uint8_t)value[3] << 16)
                                   | ((uint32_t)(uint8_t)value[4] << 24);
                    if (epoch != RTC_EPOCH_UNSET) {
                        bleTimeOffset = epoch;
                        bleTimeMillis = millis();
                        timeSet = true;
                        // Do NOT touch I2C here. This callback runs in the
                        // NimBLE host task, and PCF85063_SetFromEpoch takes the
                        // shared I2C mutex with a 100 ms timeout, so a busy
                        // bus stalls the BLE stack itself — which stops
                        // notifications being queued and expires the
                        // supervision timer. Hand it to displayTask instead.
                        rtcSetEpoch = epoch;
                        printf("Time synced: %lu (RTC write deferred)\r\n",
                               (unsigned long)epoch);
                    }
                }
                break;
            case CMD_LIST_SESSIONS:
                if (dlQueue) {
                    DlRequest r = { CMD_LIST_SESSIONS, 0 };
                    xQueueSend(dlQueue, &r, 0);
                }
                break;
            case CMD_DOWNLOAD_SESSION:
                if (dlQueue && value.length() >= 2) {
                    DlRequest r = { CMD_DOWNLOAD_SESSION, (uint8_t)value[1] };
                    xQueueSend(dlQueue, &r, 0);
                }
                break;
            case CMD_DOWNLOAD_ABORT:
                dlAbort = true;
                break;
            default:
                // An app written against a different command set will land
                // here; the name of the byte is more useful than silence.
                printf("CMD rx: unknown command 0x%02X\r\n", (unsigned)cmd);
                break;
        }
    }
};

static ServerCallbacks serverCB;
static CmdCallbacks cmdCB;

// "Connected but no data" is almost always one of three things, and none of
// them are visible from the phone: the client never subscribed, the client
// never sent CMD_START, or the negotiated MTU is too small. Log the first
// (here) and the MTU (on connect) so the serial console names the cause
// instead of leaving it to guesswork.
class ImuCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic *pCharacteristic,
                     NimBLEConnInfo &connInfo, uint16_t subValue) override {
        (void)pCharacteristic; (void)connInfo;
        const char *mode = (subValue & 1) ? "notify"
                          : (subValue & 2) ? "indicate" : "DISABLED";
        printf("IMU characteristic subscription: %s (0x%04x)\r\n", mode, subValue);
        if (subValue & 3) {
            streamArmed = true;
            // Start a fresh session at the subscribe edge. CMD_START does the
            // same, so a client that also sends 0x01 is unaffected, but this
            // keeps the stream running for a client that only subscribes.
            ringClear(&bleRing);
            bleSeq = 0;
            wireDumped = false;
            printf("IMU stream armed by subscription\r\n");
        } else {
            streamArmed = false;
            printf("IMU stream disarmed\r\n");
        }
    }
};
static ImuCallbacks imuCB;

static void postRecordCmd(uint8_t cmd) {
    if (!ctrlQueue) {
        printf("ctrlQueue missing, ignoring cmd %u\r\n", cmd);
        return;
    }
    // Small queue: coalesce repeats, but never block the BLE host task.
    if (xQueueSend(ctrlQueue, &cmd, 0) != pdTRUE) {
        printf("ctrlQueue full, cmd %u dropped\r\n", cmd);
    }
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
    pImuChar->setCallbacks(&imuCB);
    // NimBLE 2.x does not auto-create the CCCD; without this the browser's
    // startNotifications() has nothing to write to and no data ever arrives.
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

    pFileChar = pService->createCharacteristic(
        FILE_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );
    pFileChar->createDescriptor("2902", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);

    pService->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->enableScanResponse(true);
    adv->setPreferredParams(0x06, 0x0C);   // 7.5-15 ms connection interval
    NimBLEDevice::startAdvertising();

    printf("BLE started as '%s'\r\n", DEVICE_NAME);
}

// ─── SD → BLE session download task (Core 0) ─────────────────────
static void dlNotify(const char *fmt, ...) {
    if (!pFileChar) return;
    char buf[DL_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    pFileChar->setValue((const uint8_t *)buf, (size_t)n);
    pFileChar->notify();
}

// Payload bytes available in one notification, derived from the MTU the peer
// actually agreed to (not the 247 we asked for). This has to be honoured by
// EVERY notification path, not just downloads: clients negotiate anywhere from
// 23 to 247, and a notification larger than the link MTU is silently dropped
// rather than truncated, which looks exactly like "connected but no data".
static size_t attPayload(void) {
    size_t mtu = 247;
    if (pServer && bleConnected) {
        uint16_t peer = pServer->getPeerMTU(bleConnHandle);
        if (peer > 0) mtu = peer;
    }
    return (mtu > 23) ? (mtu - 3) : 20;   // 3-byte ATT header
}

static size_t dlMaxPayload(void) {
    size_t payload = attPayload();
    if (payload > DL_PKT_MAX) payload = DL_PKT_MAX;
    return payload;
}

// Samples that fit in one IMU notification on the peer's MTU: 8 on a normal
// link, fewer on a small-MTU client, 0 if not even one sample fits. A smaller
// packet costs a little throughput on a chatty link but keeps the stream
// working, which matters far more than the 25 packets/s difference.
static uint8_t imuSamplesPerPacket(void) {
    const size_t avail = attPayload() - PKT_HDR_BYTES;
    if (avail < IMU_SAMPLE_BYTES) return 0;
    uint8_t n = (uint8_t)(avail / IMU_SAMPLE_BYTES);
    return (n > IMU_SAMPLES_PER_PACKET) ? IMU_SAMPLES_PER_PACKET : n;
}

// "D," + raw file bytes. The client strips the 2-byte ASCII tag and appends
// the rest as raw bytes, so CSV content is never round-tripped through UTF-8.
static void dlSendChunk(const char *buf, size_t len) {
    if (!pFileChar) return;
    char out[DL_PKT_MAX];
    size_t headLen = 2;                        // "D,"
    size_t avail = dlMaxPayload() - headLen;
    if (avail < 16) return;                    // peer MTU too small to be useful
    if (len > avail) len = avail;
    out[0] = 'D'; out[1] = ',';
    memcpy(out + headLen, buf, len);
    pFileChar->setValue((const uint8_t *)out, headLen + len);
    pFileChar->notify();
}

static void dlStreamSession(uint8_t idx) {
    char path[SD_PATH_MAX];
    snprintf(path, sizeof(path), "/imu_data/session_%03u.csv", idx);
    if (!SD_MMC.exists(path)) {
        dlNotify("ERR,NOSUCH,%u", idx);
        return;
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        dlNotify("ERR,OPEN,%u", idx);
        return;
    }
    dlAbort = false;
    size_t fileSize = f.size();
    uint32_t chunks = 0;
    static char chBuf[DL_CHUNK_MAX];
    dlNotify("DFILE_START,%u,%s,%u", idx, (const char *)path + 10, (unsigned)fileSize);
    while (f.available() && !dlAbort) {
        size_t n = f.read((uint8_t *)chBuf, sizeof(chBuf));
        if (n == 0) break;
        dlSendChunk(chBuf, n);
        chunks++;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    f.close();
    if (dlAbort) {
        dlAbort = false;
        dlNotify("ABORT,%u,%u", idx, chunks);
    } else {
        dlNotify("DFILE_END,%u,%u", idx, chunks);
    }
}

static void dlListSessions(void) {
    char path[SD_PATH_MAX];
    uint16_t count = 0;
    for (uint16_t i = 0; i < SD_SESSION_SCAN_MAX; i++) {
        snprintf(path, sizeof(path), "/imu_data/session_%03u.csv", (unsigned)i);
        if (SD_MMC.exists(path)) count++;
    }
    dlNotify("DLIST_START,%u", count);
    for (uint16_t i = 0; i < SD_SESSION_SCAN_MAX; i++) {
        snprintf(path, sizeof(path), "/imu_data/session_%03u.csv", (unsigned)i);
        if (SD_MMC.exists(path)) {
            File sf = SD_MMC.open(path, FILE_READ);
            size_t sz = sf ? sf.size() : 0;
            if (sf) sf.close();
            dlNotify("FSESS,%u,session_%03u.csv,%u", (unsigned)i, (unsigned)i, (unsigned)sz);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    dlNotify("DLIST_END");
}

static void dlTask(void *pvParameters) {
    (void)pvParameters;
    printf("DL task on core %d\r\n", xPortGetCoreID());
    DlRequest req;
    while (1) {
        if (xQueueReceive(dlQueue, &req, portMAX_DELAY) == pdTRUE) {
            if (req.cmd == CMD_DOWNLOAD_ABORT) {
                dlAbort = true;
                continue;
            }
            if (dlActive) {
                dlNotify("ERR,BUSY");
                continue;
            }
            if (!bleConnected) continue;
            dlActive = true;
            if (req.cmd == CMD_LIST_SESSIONS) {
                dlListSessions();
            } else if (req.cmd == CMD_DOWNLOAD_SESSION) {
                dlStreamSession(req.sessionIdx);
            }
            dlActive = false;
        }
    }
}

// ─── Task: IMU Reader (Core 1, highest priority) ────────────────
static void imuTask(void *pvParameters) {
    (void)pvParameters;
    printf("IMU task on core %d\r\n", xPortGetCoreID());
    TickType_t xLastWake = xTaskGetTickCount();
    int16_t ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;

    while (1) {
        QMI8658_BurstRead(&ax, &ay, &az, &gx, &gy, &gz);

        Tremor_Metric_Feed((float)gx / GYRO_LSB_PER_DPS,
                           (float)gy / GYRO_LSB_PER_DPS,
                           (float)gz / GYRO_LSB_PER_DPS);

        uint32_t now_ms = millis();
        uint32_t epoch_s = 0;
        if (timeSet && bleTimeOffset > 0) {
            epoch_s = bleTimeOffset + (now_ms - bleTimeMillis) / 1000;
        } else if (rtcEpochCache != RTC_EPOCH_UNSET) {
            // No browser sync: fall back to the cached RTC so the SD file still
            // carries absolute time instead of a column of zeros.
            epoch_s = rtcEpochCache + (now_ms - rtcEpochMillis) / 1000;
        }

        SampleEntry entry;
        entry.ax = ax; entry.ay = ay; entry.az = az;
        entry.gx = gx; entry.gy = gy; entry.gz = gz;
        entry.ts_ms = now_ms;
        entry.epoch_s = epoch_s;

        if (ringPush(&bleRing, &entry)) totalSamples++;
        if (recording) ringPush(&sdRing, &entry);

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(5)); // 200 Hz
    }
}

// ─── Task: BLE Notify (Core 0, high priority) ───────────────────
static void sendImuPacket(const SampleEntry *batch, uint8_t count) {
    uint8_t pkt[IMU_PKT_MAX_BYTES];
    ImuHdr hdr;
    hdr.magic     = PACKET_MAGIC;
    hdr.seq       = bleSeq++;
    hdr.ts_ms     = batch[0].ts_ms;
    hdr.dropped   = (uint16_t)bleRing.drops;
    hdr.n_samples = count;
    hdr.pkt_type  = PKT_TYPE_IMU;
    memcpy(pkt, &hdr, PKT_HDR_BYTES);

    for (uint8_t i = 0; i < count; i++) {
        ImuSample s;
        s.ax = batch[i].ax; s.ay = batch[i].ay; s.az = batch[i].az;
        s.gx = batch[i].gx; s.gy = batch[i].gy; s.gz = batch[i].gz;
        s.rel_ms = (uint16_t)(batch[i].ts_ms - batch[0].ts_ms);
        memcpy(pkt + PKT_HDR_BYTES + (size_t)i * IMU_SAMPLE_BYTES, &s, IMU_SAMPLE_BYTES);
    }
    size_t len = PKT_HDR_BYTES + (size_t)count * IMU_SAMPLE_BYTES;
    pImuChar->setValue(pkt, len);
    // Dump the first packet of a session. The firmware is known to be
    // delivering, so if a client still shows nothing the fault is in its
    // parser — and this gives the exact bytes to compare it against instead
    // of inferring the layout from the source.
    if (!wireDumped) {
        wireDumped = true;
        printf("WIRE first pkt (%u B):", (unsigned)len);
        for (size_t i = 0; i < len && i < 48; i++) printf(" %02X", pkt[i]);
        printf("\r\nWIRE offsets: magic 0-1 | seq 2-3 | ts_ms 4-7 | dropped 8-9"
               " | n_samples 10 | type 11 | samples 12+\r\n");
    }
    if (pImuChar->notify()) blePktSent++;
    else                     bleNotifyFail++;
}

static void bleTask(void *pvParameters) {
    (void)pvParameters;
    printf("BLE task on core %d\r\n", xPortGetCoreID());
    static SampleEntry batch[IMU_SAMPLES_PER_PACKET];
    TickType_t xLastWake = xTaskGetTickCount();
    uint8_t sinceStats = 0;

    while (1) {
        if (bleConnected && (streamArmed || recording) && pImuChar) {
            // Drain fully and emit as many packets as the backlog needs.
            // Capping at one packet per 40 ms tick would exactly match the
            // 200 Hz production rate with zero headroom, so any jitter would
            // push the ring into drop-oldest.
            const uint8_t perPkt = imuSamplesPerPacket();
            if (perPkt == 0) {
                // Peer MTU cannot even carry one sample. Nothing we send will
                // arrive, so say so once instead of silently spinning.
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    printf("IMU stream blocked: peer MTU %u too small "
                           "(need %u, have %u)\r\n",
                           (unsigned)pServer->getPeerMTU(bleConnHandle),
                           (unsigned)(PKT_HDR_BYTES + IMU_SAMPLE_BYTES),
                           (unsigned)attPayload());
                }
            } else {
                // Rate cap: a client with a small MTU may only fit 1-2 samples
                // per notification, which at 200 Hz would mean 100-200
                // notifications/s. That is past what a phone can drain and the
                // link supervision timer expires, dropping the connection
                // after a few seconds. Capping packets per tick keeps the link
                // healthy and costs sample rate on that client only; the
                // shortfall shows up honestly in the header's dropped counter
                // and the SD log still records the full 200 Hz.
                const uint8_t maxPkt = 2;   // <= 50 notifications/s
                uint8_t count = 0, sent = 0;
                while (ringPop(&bleRing, &batch[count])) {
                    if (++count == perPkt) {
                        sendImuPacket(batch, count);
                        count = 0;
                        if (++sent == maxPkt) break;   // keep the backlog
                    }
                }
                if (count > 0 && sent < maxPkt) sendImuPacket(batch, count);
            }
        }

        // 80 ms, not 40 ms: the ring only refills at 200 Hz, so a 40 ms tick
    // could never accumulate the 16 samples a full packet needs and the
    // device was emitting ~34 notifications/s instead of 12.5. Over-producing
    // notifications is what exhausted the NimBLE host buffer pool, so pace
    // the drain to the packet size rather than assuming batching happens.
    vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(80));
        // One self-contained line per second. A single pasted line answers the
        // whole question: armed=0 means the CCCD was never written, rec=1 means
        // the app sent 0x01, pkt_sent=0 means notifications are not being
        // queued, and a climbing notify_fail means the link is discarding them.
        if (++sinceStats >= 12) {   // 12 * 80 ms ~= 1 s
            sinceStats = 0;
            if (bleConnected) {
                bleRingDrops = bleRing.drops;
                printf("STREAM conn=1 armed=%d rec=%d mtu=%u per_pkt=%u "
                       "pkt_sent=%lu notify_fail=%lu ring_drops=%lu\r\n",
                       (int)streamArmed, (int)recording,
                       (unsigned)pServer->getPeerMTU(bleConnHandle),
                       (unsigned)imuSamplesPerPacket(),
                       (unsigned long)blePktSent, (unsigned long)bleNotifyFail,
                       (unsigned long)bleRingDrops);
            } else {
                printf("STREAM conn=0 armed=0 rec=%d\r\n", (int)recording);
            }
        }
    }
}

// ─── Task: SD Logger + record control (Core 0, low priority) ────
static void sdTask(void *pvParameters) {
    (void)pvParameters;
    printf("SD task on core %d\r\n", xPortGetCoreID());
    SampleEntry entry;
    TickType_t xLastWake = xTaskGetTickCount();
    TickType_t xLastFlush = xTaskGetTickCount();

    while (1) {
        uint8_t cmd;
        while (ctrlQueue && xQueueReceive(ctrlQueue, &cmd, 0) == pdTRUE) {
            if (cmd == CMD_START_RECORD) {
                ringClear(&bleRing);      // drop pre-session stale samples
                ringClear(&sdRing);
                bleSeq = 0;               // client tracks loss from seq 0
                if (sdReady) {
                    if (!SD_Logger_Open()) {
                        printf("SD Logger unavailable, recording to BLE only\r\n");
                    }
                }
                recording = true;
                strlcpy(dispRec, "Recording: ON", sizeof(dispRec));
                printf("CMD: Start recording\r\n");
            } else if (cmd == CMD_STOP_RECORD) {
                recording = false;
                if (SD_Logger_IsOpen()) {
                    SD_Logger_Flush();
                    SD_Logger_Close();
                }
                strlcpy(dispRec, "Recording: OFF", sizeof(dispRec));
                printf("CMD: Stop recording, SD flushed\r\n");
            }
            xLastFlush = xTaskGetTickCount();
        }

        if (recording && sdReady && SD_Logger_IsOpen()) {
            while (ringPop(&sdRing, &entry)) {
                char line[128];
                snprintf(line, sizeof(line), "%lu,%lu,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f",
                         (unsigned long)entry.ts_ms,
                         (unsigned long)entry.epoch_s,
                         (float)entry.ax / ACCEL_LSB_PER_G,
                         (float)entry.ay / ACCEL_LSB_PER_G,
                         (float)entry.az / ACCEL_LSB_PER_G,
                         (float)entry.gx / GYRO_LSB_PER_DPS,
                         (float)entry.gy / GYRO_LSB_PER_DPS,
                         (float)entry.gz / GYRO_LSB_PER_DPS);
                SD_Logger_Write(line);
            }
        }

        // Periodic flush every 2 s to protect against power loss
        if (SD_Logger_IsOpen() &&
            (xTaskGetTickCount() - xLastFlush) >= pdMS_TO_TICKS(2000)) {
            SD_Logger_Flush();
            xLastFlush = xTaskGetTickCount();
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(100));
    }
}

// ─── Task: Display + STATUS (Core 0, medium priority) ───────────
// Runs at 10 Hz so the tremor band and its numeric reading track the DSP
// instead of stepping twice a second. The slow peripherals stay on their
// original rates — RTC I2C read 1 Hz, calibrated battery ADC 1 Hz, BLE status
// 2 Hz — so raising the tremor rate costs nothing extra on the bus or ADC.
static void displayTask(void *pvParameters) {
    (void)pvParameters;
    printf("Display task on core %d\r\n", xPortGetCoreID());
    static uint32_t lastRtc = 0;
    static uint32_t lastClock = 0;
    static uint32_t lastBat = 0;
    static uint32_t lastStatus = 0;
    static uint32_t lastTremorTrace = 0;
    while (1) {
        const uint32_t now = millis();

        if (now - lastRtc >= 1000) {
            lastRtc = now;
            // Deferred RTC write requested by a BLE CMD_SYNC_TIME. The I2C
            // bus is shared with the gyro and touch driver, so this must not
            // run in the NimBLE host task.
            if (rtcSetEpoch != RTC_EPOCH_UNSET) {
                uint32_t want = rtcSetEpoch;
                rtcSetEpoch = RTC_EPOCH_UNSET;
                PCF85063_SetFromEpoch(want);
                printf("RTC written from BLE sync: %lu\r\n", (unsigned long)want);
            }
            if (!timeSet) {
                rtcEpochCache = PCF85063_GetEpoch();
                rtcEpochMillis = now;
            }
        }
        // Formatting is pure string work off the cached epoch, so it stays at
        // 2 Hz to keep the displayed seconds stepping smoothly. Only the I2C
        // read above is expensive, and that is what the 1 Hz gate is for.
        if (now - lastClock >= 500) {
            lastClock = now;
            formatIST(dispIST, sizeof(dispIST));
            formatISTDate(dispDate, sizeof(dispDate));
        }

        TremorLevel_t lvl;
        float rms = 0.0f;
        float narrow = 1.0f;
        if (Tremor_Metric_Get(&lvl, &rms)) {
            tremorRms = rms;
            strlcpy(dispSev, Tremor_Level_Text(lvl), sizeof(dispSev));
            dispSevColor = Tremor_Level_Color(lvl);
        }
        // Separate call on purpose: Tremor_Metric_Get and
        // Tremor_Metric_GetNarrowband each track their own sample stamp, so
        // asking for both does not make either of them report stale data.
        Tremor_Metric_GetNarrowband(&narrow);

        // 1 Hz trace of the measured severity. The on-screen "RMS: x.x dps"
        // only shows one decimal of a smoothed value; this is what to watch
        // when re-deriving the band edges against real subjects, since it
        // shows the actual number driving the CALM/MILD/MODERATE/SEVERE call.
        //
        // nb is the narrowband ratio and the flag is the movement gate
        // suppressing escalation. Logging both is what lets TREMOR_NARROWBAND_MIN
        // be re-derived from real walks instead of guessed: sit still, then
        // deliberately swing the arm, then hold a tremor, and compare. Real
        // tremor has been measured at 0.86-0.94 and a large arm swing at
        // 0.19-0.34, so the floor sits between them with room on both sides.
        if (now - lastTremorTrace >= 1000) {
            lastTremorTrace = now;
            printf("TREMOR %.2f dps %s nb=%.2f%s\r\n",
                   tremorRms, dispSev, narrow,
                   narrow < TREMOR_NARROWBAND_MIN ? " (movement)" : "");
        }

        // BAT_Get_Volts() goes through the ADC calibration path, so keep it at
        // 1 Hz rather than dragging it up to the tremor refresh rate.
        if (now - lastBat >= 1000) {
            lastBat = now;
            batVolts = BAT_Get_Volts();
            batteryPercent = BAT_Get_Percent();
            batteryLow = (batVolts <= BAT_LOW_VOLTS);
        }

        // Publish device time so the client can convert ts_ms to wall clock
        // instead of showing its own (possibly wrong) clock.
        if (pStatusChar && (now - lastStatus) >= 500) {
            lastStatus = now;
            StatusPayload st;
            uint32_t ep = getCurrentEpoch();
            bool timeValid = (ep != RTC_EPOCH_UNSET && ep > 1000000000u);
            st.magic   = STATUS_MAGIC;
            st.flags   = timeValid ? 0x0001 : 0x0000;
            st.epoch_s = timeValid ? ep : 0;
            st.millis  = now;
            st.total   = totalSamples;
            pStatusChar->setValue((const uint8_t *)&st, sizeof(st));
        }

        vTaskDelay(pdMS_TO_TICKS(100));
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

static bool startTask(TaskFunction_t fn, const char *name, uint32_t stack,
                      UBaseType_t prio, BaseType_t core) {
    if (xTaskCreatePinnedToCore(fn, name, stack, NULL, prio, NULL, core) != pdPASS) {
        printf("FATAL: task '%s' failed to create\r\n", name);
        return false;
    }
    return true;
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

    if (!ringInit(&bleRing) || !ringInit(&sdRing)) {
        printf("FATAL: ring mutex alloc failed\r\n");
        return;
    }

    // Queues must exist before advertising starts, otherwise a client can
    // connect and issue a command during the window where they do not.
    dlQueue   = xQueueCreate(8, sizeof(DlRequest));
    ctrlQueue = xQueueCreate(4, sizeof(uint8_t));
    if (!dlQueue || !ctrlQueue) {
        printf("FATAL: queue alloc failed\r\n");
        return;
    }

    Tremor_Metric_Init();

    BLE_Init();

    if (!startTask(imuTask,     "IMU", 4096, 5, 1) ||   // Core 1, real-time
        !startTask(bleTask,     "BLE", 8192, 4, 0) ||
        !startTask(sdTask,      "SD",  8192, 1, 0) ||
        !startTask(displayTask, "LCD", 4096, 2, 0) ||
        !startTask(dlTask,      "DL",  8192, 3, 0)) {
        printf("FATAL: not all tasks running, data will be lost\r\n");
    }

    printf("All tasks started\r\n");
}

// ─── Loop (LVGL handler, single task owns all LVGL calls) ───────
#define BACKLIGHT_IDLE_MS   10000
#define BACKLIGHT_ACTIVE    50

void loop() {
    static uint32_t lastDispUpdate = 0;
    static uint32_t lastPwrCheck = 0;
    static uint32_t lastBkltCheck = 0;
    static bool backlightOn = true;
    uint32_t now = millis();

    Lvgl_Loop();

    // Update labels ~10 Hz; avoids constant label invalidations at loop rate
    if (now - lastDispUpdate >= 100) {
        lastDispUpdate = now;
        Lvgl_Update_Display(dispIST, dispDate, dispBLE, dispRec, totalSamples, batVolts, batteryPercent, dispSev, dispSevColor, tremorRms);
        Lvgl_Set_LowBat(batteryLow, batteryPercent);
    }

    // Power key state machine (timings scale with this 100 ms period)
    if (now - lastPwrCheck >= 100) {
        lastPwrCheck = now;
        PWR_Loop();
    }

    // Battery watchdog: auto-save recording on critical battery, then shut down
    static uint32_t lastBatWatch = 0;
    static uint32_t critSince = 0;
    static bool lowBatSaved = false;
    if (now - lastBatWatch >= 500) {
        lastBatWatch = now;
        if (batVolts > 0.1f) {
            if (batVolts <= BAT_CRITICAL_VOLTS) {
                if (critSince == 0) critSince = now;
                if (!lowBatSaved) {
                    lowBatSaved = true;
                    if (recording) {
                        printf("BAT: critical, saving recording\r\n");
                        postRecordCmd(CMD_STOP_RECORD);
                    }
                }
                // Grace period so the LOW BATTERY warning is visible before power cut
                if (now - critSince >= 10000) {
                    printf("BAT: %.2fV shutting down\r\n", batVolts);
                    Shutdown();
                }
            } else {
                critSince = 0;
                if (batVolts > BAT_LOW_VOLTS) lowBatSaved = false;
            }
        }
    }

    // Backlight auto-off: off after 10 s idle, back on for 10 s after touch
    if (now - lastBkltCheck >= 100) {
        lastBkltCheck = now;
        if (now - Lvgl_Get_LastTouchMs() >= BACKLIGHT_IDLE_MS) {
            if (backlightOn) {
                backlightOn = false;
                Set_Backlight(0);
            }
        } else {
            if (!backlightOn) {
                backlightOn = true;
                Set_Backlight(BACKLIGHT_ACTIVE);
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
}
