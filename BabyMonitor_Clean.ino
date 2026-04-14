/* Baby Vital Monitor — ESP32-S3  Firmware
==========================================
    Peter Hogg | Edinburgh Napier University | BEng (Honours) Project

    // Hardware //
        ESP32-S3 Freenove WROOM N8R8 (8 MB Flash, 8 MB OPI PSRAM)
        MR60BHA2: 60 GHz FMCW radar, UART - GPIO47(RX)/GPIO21(TX)
        MLX90614: IR temperature, I²C - GPIO42(SDA)/GPIO41(SCL)
        INMP441: MEMS microphone, I²S - GPIO1(WS)/GPIO14(SCK)/GPIO2(SD)
        OV3660: DVP camera - see lines 80-95
 
    // Cloud pipelines //
        Sensor data -> MQTT -> HiveMQ Cloud -> Render.com Node.js -> browser

        Audio PCM -> WSS /audio -> Render.com -> browser Web Audio API

        Video JPEG -> WSS /video -> Render.com -> browser <img> tag

        Alerts -> SMTPS -> Brevo -> email inbox

    // Arduino IDE settings //
        Board: ESP32S3 Dev Module
        Flash Size: 8MB (64Mb)
        Partition: 8M with spiffs (3MB APP / 1.5MB SPIFFS)
        PSRAM: OPI PSRAM
        USB CDC On Boot: Disabled
*/

// Libraries Required // 
    #include <Arduino.h>
    #include <Wire.h>
    #include <WiFi.h>
    #include <WiFiClientSecure.h>
    #include <time.h>
    #include <Adafruit_MLX90614.h>
    #include <driver/i2s.h>
    #include <SEEED_MR60BHA2.h>
    #include "esp_camera.h"
    #include <PubSubClient.h>

// Credentials //
    #define WIFI_SSID           "PLUSNET-5CC3QH"
    #define WIFI_PASSWORD       "JoggNetwork2024"

// Alerts
    #define SMTP_HOST           "smtp-relay.brevo.com"
    #define SMTP_PORT           465
    #define SMTP_USERNAME       "a68c4e001@smtp-brevo.com"
    #define SENDER_APP_PASSWORD "YzBDJ8ZvCdTk7bwP"
    #define SENDER_EMAIL        "pjhogg2014@gmail.com"
    #define RECIPIENT_EMAIL     "pjhogg2014@gmail.com"
    #define RECIPIENT_NAME      "Parent"

// Server Setup 
    #define MQTT_HOST           "55bf30b3bf6c4d7388f56f89e23855bd.s1.eu.hivemq.cloud"
    #define MQTT_PORT           8883
    #define MQTT_USER           "BabyVitalsSystem"
    #define MQTT_PASS           "Password1234"
    #define MQTT_CLIENT         "BabyMonitor_ESP32"
    #define TOPIC_TEMPERATURE   "babymonitor/temperature"
    #define TOPIC_RADAR         "babymonitor/radar"
    #define TOPIC_ALERT         "babymonitor/alerts"
    #define TOPIC_STATUS        "babymonitor/status"

// Data Streaming 
    #define AUDIO_HOST          "baby-monitor-server-0st8.onrender.com"
    #define VIDEO_HOST          "baby-monitor-server-0st8.onrender.com"
    #define WS_PORT             443
    #define AUDIO_PATH          "/audio"
    #define VIDEO_PATH          "/video"

// Pin assignments
    #define I2C_SDA     42
    #define I2C_SCL     41
    #define RADAR_RX    47
    #define RADAR_TX    21
    #define I2S_WS       1 
    #define I2S_SCK     14
    #define I2S_SD       2 //Short Circuit on DevBoard Prototype between I2S_SD and VDD (5V). See section 
    #define CAM_PIN_PWDN    -1
    #define CAM_PIN_RESET   -1
    #define CAM_PIN_XCLK    15
    #define CAM_PIN_SIOD     4
    #define CAM_PIN_SIOC     5
    #define CAM_PIN_D7      16
    #define CAM_PIN_D6      17
    #define CAM_PIN_D5      18
    #define CAM_PIN_D4      12
    #define CAM_PIN_D3      10
    #define CAM_PIN_D2       8
    #define CAM_PIN_D1       9
    #define CAM_PIN_D0      11
    #define CAM_PIN_VSYNC    6
    #define CAM_PIN_HREF     7
    #define CAM_PIN_PCLK    13

// Alert thresholds //
    #define FEVER_THRESHOLD         38.0f   // °C
    #define HYPO_THRESHOLD          34.0f   // °C
    #define BRADYPNOEA_THRESHOLD    16.0f   // bpm
    #define TACHYPNOEA_THRESHOLD    50.0f   // bpm
    #define BRADYCARDIA_THRESHOLD   70.0f   // bpm
    #define TACHYCARDIA_THRESHOLD  160.0f   // bpm
    #define NO_DETECT_TIMEOUT    20000UL    // ms — triggers apnea alert
    #define READING_INTERVAL      2000UL    // ms between MLX90614 reads
    #define ALERT_COOLDOWN      300000UL    // ms — 5-minute repeat suppression
    #define INIT_GRACE_PERIOD    70000UL    // ms — radar settle time after boot

// Audio config //
    /*
  16 kHz satisfies the Nyquist criterion for the INMP441's useful
  frequency range (60 Hz – 15 kHz per datasheet), while keeping the bitrate at 256 kbps. manageable alongside MQTT and video.
  The DMA buffer length of 512 32-bit words gives 32 ms of audio
  per WebSocket frame, which is enough for the browser's Web Audio
  API to schedule seamless playback without gaps. [Ref: Espressif
  Systems, "ESP32-S3 Technical Reference Manual", I2S chapter;
  Perkins, C., "RTP: Audio and Video for the Internet", 2003]
 */
    #define SAMPLE_RATE     16000
    #define AUDIO_BUF_LEN   512

// ── Objects ───────────────────────────────────────────────────────
    HardwareSerial    mmWaveSerial(1);
    SEEED_MR60BHA2    mmWave;
    Adafruit_MLX90614 mlx;
    WiFiClientSecure  mqttTls, smtpTls, audioTls, videoTls;
    PubSubClient      mqttClient(mqttTls);

// ── State ─────────────────────────────────────────────────────────
    float         currentBreathing  = 0.0f;
    float         currentHeartRate  = 0.0f;
    float         currentDistance   = 0.0f;
    bool          radarDetected     = false;
    unsigned long lastDetectedTime  = 0;
    unsigned long lastReadTime      = 0;

    unsigned long lastTempAlertTime   = 0;
    unsigned long lastBradypnoeaTime  = 0;
    unsigned long lastTachypnoeaTime  = 0;
    unsigned long lastBradycardiaTime = 0;
    unsigned long lastTachycardiaTime = 0;
    unsigned long lastApneaTime       = 0;

    bool          audioConnected    = false;
    bool          videoConnected    = false;
    unsigned long lastAudioRetry    = 0;
    unsigned long lastVideoRetry    = 0;
    unsigned long lastVideoFrame    = 0;

    #define STREAM_RETRY_MS   10000UL
    #define VIDEO_FPS_MS        200UL

// ─────────────────────────────────────────────────────────────────
// I²S — INMP441 microphone
/*The INMP441 outputs 24-bit PCM MSB-aligned inside a 32-bit I²S
 frame. Reading with I2S_BITS_PER_SAMPLE_16BIT only captures the
 upper 16 bits of the 32-bit slot, which on this microphone are
 the zero-padding bits — producing silence. The fix is to read
 32-bit words and right-shift by 8 to extract the 24-bit value,
 then truncate to int16_t. The lowest 8 bits of the 24-bit value
 are below the noise floor and discarding them causes no audible
 quality loss. [Ref: esp32.com forum, "Inputting audio to an
 ESP32 from an INMP441 I2S microphone: success", 2020;
 circuitlabs.net, "I2S Digital Microphone Integration", 2025]*/ 
    void setupI2S()
    {
        i2s_config_t cfg = 
        {
            .mode                = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
            .sample_rate         = SAMPLE_RATE,
            .bits_per_sample     = I2S_BITS_PER_SAMPLE_32BIT,
            .channel_format      = I2S_CHANNEL_FMT_ONLY_LEFT,  // L/R pin tied to GND
            .communication_format= (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
            .intr_alloc_flags    = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count       = 4,
            .dma_buf_len         = AUDIO_BUF_LEN,
            .use_apll            = false,
            .tx_desc_auto_clear  = false,
            .fixed_mclk          = 0
        };
        i2s_pin_config_t pins = 
        {
            .bck_io_num   = I2S_SCK,
            .ws_io_num    = I2S_WS,
            .data_out_num = I2S_PIN_NO_CHANGE,
            .data_in_num  = I2S_SD
        };
        i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
        i2s_set_pin(I2S_NUM_1, &pins);
        i2s_zero_dma_buffer(I2S_NUM_1);
        Serial.println("I2S ready.");
    }

/* WebSocket helpers: key generation and masked frame sending
 RFC 6455 §5.3 requires every client-to-server frame to be masked
 with a 4-byte XOR key. The Node.js ws library enforces this and
 closes the connection with code 1002 on any unmasked frame.
 esp_random() provides hardware RNG output on the ESP32-S3, which
 satisfies the RFC 6455 §10.3 requirement for an unpredictable key. 
 [Ref: Fette, I. and Melnikov, A., "The WebSocket Protocol",
    RFC 6455, IETF, 2011]*/
    static void b64Encode16(const uint8_t* key, char out[25]) 
    {
        static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
        for (int i = 0, j = 0; i < 15; i += 3, j += 4) 
        {
            uint32_t v = ((uint32_t)key[i]<<16) | ((uint32_t)key[i+1]<<8) | key[i+2];
            out[j]   = t[(v>>18)&0x3F]; out[j+1] = t[(v>>12)&0x3F];
            out[j+2] = t[(v>> 6)&0x3F]; out[j+3] = t[ v     &0x3F];
        }
        out[20] = t[(key[15]>>2)&0x3F];
        out[21] = t[((key[15]&0x03)<<4)&0x3F];
        out[22] = '='; out[23] = '='; out[24] = '\0';
    }

static void wsSendBinary(WiFiClientSecure& tls, const uint8_t* data, size_t len) 
    {
        if (!tls.connected()) 
        return;
        uint32_t mw = esp_random();
        uint8_t  mask[4];
        memcpy(mask, &mw, 4);
        uint8_t hdr[8];
        int hlen;
        hdr[0] = 0x82;// FIN + binary opcode
                       
        if (len <= 125) 
        {
            hdr[1] = 0x80 | (uint8_t)len;
            hlen   = 2;
        } 
        else 
        {
            hdr[1] = 0x80 | 126;
            hdr[2] = (len >> 8) & 0xFF;
            hdr[3] = len & 0xFF;
            hlen   = 4;
        }
        memcpy(hdr + hlen, mask, 4);
        tls.write(hdr, hlen + 4);

        uint8_t chunk[128];
        for (size_t sent = 0; sent < len; ) 
        {
            size_t sz = min(len - sent, sizeof(chunk));
            for (size_t i = 0; i < sz; i++)
            chunk[i] = data[sent + i] ^ mask[(sent + i) & 3];
            tls.write(chunk, sz);
            sent += sz;
        }
    }

static bool wsConnect(WiFiClientSecure& tls, const char* host, const char* path) 
    {
        tls.stop();
        tls.setInsecure();
        if (!tls.connect(host, WS_PORT)) 
        return false;

        uint8_t kb[16];
        for (auto& b : kb) b = random(0, 256);
        char key[25];
        b64Encode16(kb, key);

        tls.printf
            ("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            path, host, key);

        String resp;
            unsigned long t = millis();
            while (millis() - t < 3000) 
            {
            if (tls.available()) 
                {
                    String ln = tls.readStringUntil('\n');
                    resp += ln;
                    if (ln == "\r") break;
                }
                delay(1);
            }
            return resp.indexOf("101") > 0;
    }

/*Camera initialisation
The camera DMA allocator needs one large contiguous PSRAM block.
mbedTLS fragments the heap during TLS handshakes, so the camera
must be initialised before any network connection is opened.
Calling esp_camera_deinit() after a failed init is unsafe because
the driver creates FreeRTOS tasks and semaphores before the DMA
setup that fails; destroying partially-initialised objects causes
a NULL semaphore assertion on the next init or reboot.
[Ref: Espressif GitHub, "esp32-camera" driver source; ESP-IDF
FreeRTOS documentation on xQueueSemaphoreTake]*/ 
void initCamera() 
    {
        camera_config_t cfg = {};
        cfg.ledc_channel = LEDC_CHANNEL_0;
        cfg.ledc_timer   = LEDC_TIMER_0;
        cfg.pin_d0 = CAM_PIN_D0; cfg.pin_d1 = CAM_PIN_D1;
        cfg.pin_d2 = CAM_PIN_D2; cfg.pin_d3 = CAM_PIN_D3;
        cfg.pin_d4 = CAM_PIN_D4; cfg.pin_d5 = CAM_PIN_D5;
        cfg.pin_d6 = CAM_PIN_D6; cfg.pin_d7 = CAM_PIN_D7;
        cfg.pin_xclk     = CAM_PIN_XCLK;  cfg.pin_pclk  = CAM_PIN_PCLK;
        cfg.pin_vsync    = CAM_PIN_VSYNC;  cfg.pin_href  = CAM_PIN_HREF;
        cfg.pin_sccb_sda = CAM_PIN_SIOD;   cfg.pin_sccb_scl = CAM_PIN_SIOC;
        cfg.pin_pwdn     = CAM_PIN_PWDN;   cfg.pin_reset = CAM_PIN_RESET;
        cfg.xclk_freq_hz = 20000000;
        cfg.pixel_format = PIXFORMAT_JPEG;
        cfg.frame_size   = FRAMESIZE_QVGA;  // 320×240, ~10 KB per frame
        cfg.jpeg_quality = 12;
        cfg.fb_count     = 1;
        cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
        cfg.fb_location  = CAMERA_FB_IN_PSRAM;

        if (esp_camera_init(&cfg) != ESP_OK) 
            {
            Serial.println("[CAM] Init failed — check Flash Size = 8MB.");
            return;
            }
            sensor_t* s = esp_camera_sensor_get();
        if (s) 
            {  
            s->set_brightness(s, 1); s->set_vflip(s, 1); 
            }
        Serial.println("[CAM] Ready.");
    }

/*SMTP — direct implementation over SMTPS (port 465)
 ReadyMail 0.3.8 is incompatible with ESP32 Arduino core 3.3.7:
 the library's internal utility functions (rd_cast, rd_b64_enc,
 rd_free) are not resolved at link time under this core version.
 SMTP is straightforward to implement directly over WiFiClientSecure.
 Port 465 is SMTPS, TLS is established before any SMTP dialogue,
 unlike port 587 (STARTTLS) where plaintext starts first.
 [Ref: Klensin, J., "Simple Mail Transfer Protocol", RFC 5321,
 IETF, 2008; Brevo SMTP relay documentation]*/  
 
static const char B64T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String smtpB64(const String& s) 
    {
        String out;
        const uint8_t* b = (const uint8_t*)s.c_str();
        int len = s.length();
        for (int i = 0; i < len; i += 3) 
            {
            uint32_t v = (uint32_t)b[i] << 16;
            if (i+1 < len) v |= (uint32_t)b[i+1] << 8;
            if (i+2 < len) v |= b[i+2];
            out += B64T[(v>>18)&0x3F]; out += B64T[(v>>12)&0x3F];
            out += (i+1<len) ? B64T[(v>>6)&0x3F] : '=';
            out += (i+2<len) ? B64T[v&0x3F]      : '=';
            }
        return out;
    }

static int smtpRead(WiFiClientSecure& c) 
    {
        int code = 0;
        unsigned long t = millis();
        while (millis() - t < 6000) 
        {
            if (c.available()) 
                {
                String ln = c.readStringUntil('\n');
                ln.trim();
                code = ln.substring(0, 3).toInt();
                if (ln.length() < 4 || ln[3] != '-') break;
                t = millis();
                }
            delay(5);
        }
        return code;
    }

static bool smtpCmd(const String& cmd, int expected) 
    {
        smtpTls.println(cmd);
        return smtpRead(smtpTls) == expected;
    }

static bool sendEmail(const String& subject, const String& txt, const String& html) 
    {
        smtpTls.stop();
        smtpTls.setInsecure();
        if (!smtpTls.connect(SMTP_HOST, SMTP_PORT) || smtpRead(smtpTls) != 220) 
            return false;
        if (!smtpCmd("EHLO babymonitor", 250)) 
            return false;

        smtpTls.println("AUTH LOGIN");
        smtpRead(smtpTls);
        smtpTls.println(smtpB64(SMTP_USERNAME));
        smtpRead(smtpTls);
        smtpTls.println(smtpB64(SENDER_APP_PASSWORD));
        if (smtpRead(smtpTls) != 235) 
        { 
            smtpTls.stop(); 
            return false; 
        }

        if (!smtpCmd("MAIL FROM:<" + String(SENDER_EMAIL) + ">", 250)) 
        { 
            smtpTls.stop();
            return false; 
        }
        if (!smtpCmd("RCPT TO:<"  + String(RECIPIENT_EMAIL) + ">", 250)) 
        { 
            smtpTls.stop(); 
            return false; 
        }

        smtpTls.println("DATA");
        if (smtpRead(smtpTls) != 354) 
        { 
            smtpTls.stop(); 
            return false; 
        }

        smtpTls.println("From: Baby Monitor <" + String(SENDER_EMAIL) + ">");
        smtpTls.println("To: "      + String(RECIPIENT_NAME) + " <" + String(RECIPIENT_EMAIL) + ">");
        smtpTls.println("Subject: Baby Monitor Alert: " + subject);
        smtpTls.println("MIME-Version: 1.0");
        smtpTls.println("Content-Type: multipart/alternative; boundary=\"b\"");
        smtpTls.println();
        smtpTls.println("--b\r\nContent-Type: text/plain; charset=UTF-8\r\n");
        smtpTls.println(txt);
        smtpTls.println("--b\r\nContent-Type: text/html; charset=UTF-8\r\n");
        smtpTls.println(html);
        smtpTls.println("--b--\r\n.");

        bool ok = (smtpRead(smtpTls) == 250);
        smtpCmd("QUIT", 221);
        smtpTls.stop();
        if (ok) Serial.println("[SMTP] Sent.");
        return ok;
    }

// ── Radar ─────────────────────────────────────────────────────────
void updateRadar() 
    {
        if (mmWave.update(100)) 
        {
            radarDetected    = true;
            lastDetectedTime = millis();
            float v;
            if (mmWave.getBreathRate(v)) currentBreathing = v;
            if (mmWave.getHeartRate(v))  currentHeartRate  = v;
            if (mmWave.getDistance(v))   currentDistance   = v;
            Serial.printf("[RADAR] BR:%.1f HR:%.1f D:%.2f\n",
                      currentBreathing, currentHeartRate, currentDistance);
        } 
        else 
        {
            radarDetected = false;
        }
    }

// Alert evaluation 
void sendAlert(float temp, const String& condition) 
    {
        Serial.println("[ALERT] " + condition);

        char buf[256];
        snprintf(buf, sizeof(buf),
        "{\"condition\":\"%s\",\"temperature\":%.1f,"
        "\"breathing\":%.0f,\"heartrate\":%.0f,"
        "\"detected\":%s,\"ts\":%lu}",
        condition.c_str(), temp, currentBreathing, currentHeartRate,
        radarDetected ? "true" : "false", (unsigned long)time(nullptr));
        mqttClient.publish(TOPIC_ALERT, buf);

        String txt =
            "Baby Monitor Alert\n\nCondition: " + condition +
            "\nTemperature: " + String(temp, 1) + " C" +
            "\nBreathing: "   + String(currentBreathing, 0) + " bpm" +
            "\nHeart rate: "  + String(currentHeartRate, 0) + " bpm" +
            "\nDetected: "    + String(radarDetected ? "Yes" : "No") +
            "\n\nPlease check on the infant.\n-- Baby Monitor";

        String html =
            "<html><body style='font-family:sans-serif;max-width:520px'>"
            "<h2 style='color:#c0392b'>Baby Monitor Alert</h2>"
            "<p><b>" + condition + "</b></p>"
            "<table><tr><td>Temperature</td><td style='color:#c0392b'>"
            + String(temp, 1) + " &deg;C</td></tr>"
            "<tr><td>Breathing</td><td>" + String(currentBreathing, 0) + " bpm</td></tr>"
            "<tr><td>Heart rate</td><td>" + String(currentHeartRate, 0) + " bpm</td></tr>"
            "<tr><td>Detected</td><td>" + String(radarDetected ? "Yes" : "No") + "</td></tr>"
            "</table><p>Please check on the infant.</p></body></html>";
        sendEmail(condition, txt, html);
    }

void evaluateAlerts(float temp) 
    {
        unsigned long now = millis();

        if (temp > FEVER_THRESHOLD || (temp < HYPO_THRESHOLD && temp > 20.0f)) 
        {
            if (now - lastTempAlertTime >= ALERT_COOLDOWN) 
            {
                sendAlert(temp, temp > FEVER_THRESHOLD
                ? "Fever: " + String(temp, 1) + " C"
                : "Low temperature: " + String(temp, 1) + " C");
                lastTempAlertTime = now;
            }
        }

        if (now <= INIT_GRACE_PERIOD) 
        return;  // wait for radar to settle

        if (!radarDetected && now - lastDetectedTime >= NO_DETECT_TIMEOUT) 
        {
            if (now - lastApneaTime >= ALERT_COOLDOWN) 
            {
                sendAlert(temp, "No breath detected — possible apnea");
                lastApneaTime = now;
            }
        }

        if (radarDetected && currentBreathing > 0) 
        {
            if (currentBreathing < BRADYPNOEA_THRESHOLD && now - lastBradypnoeaTime >= ALERT_COOLDOWN) 
            {
                sendAlert(temp, "Low breathing: " + String(currentBreathing, 0) + " bpm");
                lastBradypnoeaTime = now;
            } 
            else if (currentBreathing > TACHYPNOEA_THRESHOLD && now - lastTachypnoeaTime >= ALERT_COOLDOWN) 
            {
                sendAlert(temp, "High breathing: " + String(currentBreathing, 0) + " bpm");
                lastTachypnoeaTime = now;
            }
        }

        if (radarDetected && currentHeartRate > 0) 
        {
            if (currentHeartRate < BRADYCARDIA_THRESHOLD && now - lastBradycardiaTime >= ALERT_COOLDOWN) 
            {
                sendAlert(temp, "Low heart rate: " + String(currentHeartRate, 0) + " bpm");
                lastBradycardiaTime = now;
            } 
            else if (currentHeartRate > TACHYCARDIA_THRESHOLD && now - lastTachycardiaTime >= ALERT_COOLDOWN) 
            {
                sendAlert(temp, "High heart rate: " + String(currentHeartRate, 0) + " bpm");
                lastTachycardiaTime = now;
            }
        }
    }

// ── MQTT publish ──────────────────────────────────────────────────
void publishSensors(float objectTemp, float ambientTemp) 
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
        "{\"object\":%.1f,\"ambient\":%.1f,\"ts\":%lu}",
        objectTemp, ambientTemp, (unsigned long)time(nullptr));
        mqttClient.publish(TOPIC_TEMPERATURE, buf);

        snprintf(buf, sizeof(buf),
        "{\"breathing\":%.0f,\"heartrate\":%.0f,\"distance\":%.2f,"
        "\"detected\":%s,\"ts\":%lu}",
        currentBreathing, currentHeartRate, currentDistance,
        radarDetected ? "true" : "false", (unsigned long)time(nullptr));
        mqttClient.publish(TOPIC_RADAR, buf);
    }

// ── setup() ───────────────────────────────────────────────────────
void setup() 
{
    Serial.begin(115200);

    // Camera must come first — needs contiguous PSRAM before TLS
    // fragments the heap. [See initCamera() comment above]
    initCamera();

    Wire.begin(I2C_SDA, I2C_SCL);
    if (!mlx.begin()) 
        {
        Serial.println("ERROR: MLX90614 not found.");
        while (true) delay(1000);
        }
        Serial.println("MLX90614 ready.");
    mmWaveSerial.begin(115200, SERIAL_8N1, RADAR_RX, RADAR_TX);
    mmWave.begin(&mmWaveSerial);
    lastDetectedTime = millis();
    Serial.println("MR60BHA2 ready.");

    setupI2S();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting");
    while (WiFi.status() != WL_CONNECTED) 
    { 
        delay(500); Serial.print("."); 
    }
    Serial.println("\nIP: " + WiFi.localIP().toString());
    delay(500);

    // British Summer Time offset = 3600, GMT = 0. Update each October/March.
    configTime(3600, 0, "pool.ntp.org");
    time_t t = time(nullptr);
    while (t < 100000) 
    { 
        delay(200); t = time(nullptr); 
    }
    Serial.println("Time synced.");

    mqttTls.setInsecure();
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setKeepAlive(15);
    mqttClient.setBufferSize(512);
    mqttClient.setSocketTimeout(30);
    for (int i = 0; i < 10 && !mqttClient.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS); i++)
        delay(2000);
    Serial.println(mqttClient.connected() ? "MQTT connected." : "MQTT failed.");

    if (wsConnect(audioTls, AUDIO_HOST, AUDIO_PATH)) 
    {
        audioConnected = true;
        Serial.println("[AUDIO] Connected.");
    }
    
    if (wsConnect(videoTls, VIDEO_HOST, VIDEO_PATH)) 
    {
        videoConnected = true;
        Serial.println("[VIDEO] Connected.");
    }

    Serial.println("System ready.");
}

// ── loop() ────────────────────────────────────────────────────────
void loop() 
{
    // Rate-limited MQTT reconnect — a blocking connect() call on every
    // failed pass would stall audio and video for 30 s each time.
    if (!mqttClient.connected()) 
    {
        static unsigned long lastMqttRetry = 0;
        if (millis() - lastMqttRetry >= 5000UL) 
        {
            lastMqttRetry = millis();
            mqttClient.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS);
        }
    }
    mqttClient.loop();

    unsigned long now = millis();

    // Audio capture — read 32-bit I²S samples, shift right 8 to extract
    // the MSB-aligned 24-bit audio value, truncate to 16-bit for streaming.
    // Buffers are static to avoid a 3 KB stack allocation each iteration.
    if (audioConnected && audioTls.connected()) 
    {
        static int32_t rawBuf[AUDIO_BUF_LEN];
        static int16_t pcmBuf[AUDIO_BUF_LEN];
        size_t bytesRead = 0;
        i2s_read(I2S_NUM_1, rawBuf, sizeof(rawBuf), &bytesRead, 0);
        if (bytesRead > 0) 
        {
            int n = bytesRead / 4;
            for (int i = 0; i < n; i++) pcmBuf[i] = (int16_t)(rawBuf[i] >> 8);
            wsSendBinary(audioTls, (uint8_t*)pcmBuf, n * 2);
        }
    }

    if (!audioConnected || !audioTls.connected()) 
    {
        if (now - lastAudioRetry >= STREAM_RETRY_MS) 
        {
            lastAudioRetry = now;
            audioConnected = wsConnect(audioTls, AUDIO_HOST, AUDIO_PATH);
            Serial.println(audioConnected ? "[AUDIO] Reconnected." : "[AUDIO] Retry failed.");
        }
    }

    // Video — one JPEG frame per 200 ms (5 fps). esp_camera_fb_return()
    // must always be called to release the DMA buffer.
    if (videoConnected && videoTls.connected()) 
    {
        if (now - lastVideoFrame >= VIDEO_FPS_MS) 
        {
            lastVideoFrame = now;
            camera_fb_t* fb = esp_camera_fb_get();
            if (fb) 
            {
            wsSendBinary(videoTls, fb->buf, fb->len);
            esp_camera_fb_return(fb);
            }
        }
    }

    if (!videoConnected || !videoTls.connected()) 
    {
        if (now - lastVideoRetry >= STREAM_RETRY_MS) 
        {
            lastVideoRetry = now;
            videoConnected = wsConnect(videoTls, VIDEO_HOST, VIDEO_PATH);
            Serial.println(videoConnected ? "[VIDEO] Reconnected." : "[VIDEO] Retry failed.");
        }
    }

    updateRadar();

    if (now - lastReadTime < READING_INTERVAL) return;
    lastReadTime = now;

    float obj = mlx.readObjectTempC();
    float amb = mlx.readAmbientTempC();

    // Discard NaN and implausible values — MLX90614 returns 999.0 °C
    // on I²C fault. Guard prevents false fever alerts.
    if (isnan(obj) || isnan(amb) || obj > 50.0f || obj < 20.0f) 
    {
        Serial.println("[MLX] Invalid — skipped.");
        return;
    }

    Serial.printf("[MLX] %.1f C\n", obj);
    publishSensors(obj, amb);
    evaluateAlerts(obj);
}