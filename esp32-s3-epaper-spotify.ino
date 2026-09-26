// Spotify "spelas nu" för Waveshare ESP32-S3-ePaper-1.54
//   Visar låt, artist, albumomslag (dithrat till svartvitt) och förlopp från ditt Spotify-konto.
//   BOOT-knappen (GPIO0):  start / stopp
//   PWR-knappen  (GPIO18): nästa låt
// Kräver Spotify Premium för knapparna. Inloggning görs en gång med spotify_auth.py (se README).
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "secrets.h"

// ---- Pinnar på ESP32-S3-ePaper-1.54 ----
#define PIN_BOOT     0
#define PIN_PWR      18
#define PIN_VBAT     17   // batteriväg, aktiv hög
#define PIN_EPD_PWR  6    // e-papper ström, aktiv låg
#define PIN_AUD_PWR  42   // ljudström, aktiv låg (används inte här: stängs av)
#define PIN_PA       46
#define EPD_DC       10
#define EPD_CS       11
#define EPD_SCK      12
#define EPD_MOSI     13
#define EPD_RST      9
#define EPD_BUSY     8

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
U8G2_FOR_ADAFRUIT_GFX u8;
Preferences prefs;

// ---- Layout (200x200 efter rotation) ----
static const int COVER = 150, COVER_X = 25, COVER_Y = 2;   // 300 px omslag avkodas i halv storlek
static const int STRIP_Y = 184, STRIP_H = 16;               // rad med play/paus + förloppsstapel (delvis uppdatering)

// ---- Delat tillstånd ----
struct NowPlaying {
    char     id[48];
    char     title[160];
    char     artist[160];
    char     msg[72];
    uint32_t durMs, progMs, progAt;
    bool     playing, have, cover;
};
NowPlaying np = {};
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool needFull = true, needPartial = false;
volatile int  pendingCmd = 0;                 // 1 = start/stopp, 2 = nästa låt
uint8_t *gray = nullptr;                      // avkodat omslag, 8 bitar
uint8_t *coverBW[2] = {nullptr, nullptr};     // dithrat omslag, 1 byte/pixel (1 = svart)
volatile int coverCur = 0;

// ---- Token ----
String accessToken, refreshToken;
uint32_t tokenExpiresAt = 0;
uint32_t backoffUntil = 0;

static void setMsg(const char *m) {
    portENTER_CRITICAL(&mux);
    strlcpy(np.msg, m, sizeof(np.msg));
    portEXIT_CRITICAL(&mux);
}

// ================= Spotify API =================
static bool refreshAccessToken() {
    WiFiClientSecure c; c.setInsecure();
    HTTPClient http;
    if (!http.begin(c, "https://accounts.spotify.com/api/token")) return false;
    http.setTimeout(10000);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body = "grant_type=refresh_token&client_id=" SPOTIFY_CLIENT_ID "&refresh_token=" + refreshToken;
    int code = http.POST(body);
    String resp = http.getString();
    http.end();
    if (code != 200) {
        Serial.printf("Tokenförnyelse misslyckades: %d\n", code);
        if (code == 400 || code == 401) setMsg("Logga in igen: kor spotify_auth.py");
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return false;
    accessToken = doc["access_token"].as<String>();
    int exp = doc["expires_in"] | 3600;
    tokenExpiresAt = millis() + (uint32_t)(exp - 90) * 1000UL;
    const char *rt = doc["refresh_token"];
    if (rt && *rt && refreshToken != rt) { refreshToken = rt; prefs.putString("rt", refreshToken); }
    return true;
}

static bool ensureToken() {
    if (accessToken.length() && (int32_t)(tokenExpiresAt - millis()) > 0) return true;
    return refreshAccessToken();
}

// method: "GET" | "PUT" | "POST". Returnerar HTTP-kod (eller <0 vid nätverksfel).
static int apiCall(const char *method, const char *url, String *out = nullptr) {
    if (!ensureToken()) return -100;
    WiFiClientSecure c; c.setInsecure();
    HTTPClient http;
    if (!http.begin(c, url)) return -101;
    http.setTimeout(8000);
    http.addHeader("Authorization", "Bearer " + accessToken);
    const char *keys[] = {"Retry-After"};
    http.collectHeaders(keys, 1);
    int code;
    if (!strcmp(method, "GET"))       code = http.GET();
    else {                                            // Spotify svarar 411 utan Content-Length: skicka en tom JSON-kropp
        http.addHeader("Content-Type", "application/json");
        code = !strcmp(method, "PUT") ? http.PUT("{}") : http.POST("{}");
    }
    if (code == 200 && out) *out = http.getString();
    if (code == 429) {
        int s = http.header("Retry-After").toInt();
        backoffUntil = millis() + (uint32_t)(s > 0 ? s : 10) * 1000UL;
    }
    http.end();
    return code;
}

// ================= Omslag: hämta, avkoda, dithra =================
static int jpgDraw(JPEGDRAW *d) {
    const uint8_t *p = (const uint8_t *)d->pPixels;
    for (int r = 0; r < d->iHeight; ++r) {
        int y = d->y + r;
        if (y < 0 || y >= COVER) continue;
        for (int cx = 0; cx < d->iWidth; ++cx) {
            int x = d->x + cx;
            if (x < 0 || x >= COVER) continue;
            gray[y * COVER + x] = p[r * d->iWidth + cx];
        }
    }
    return 1;
}

static void ditherInto(uint8_t *dst) {
    // Kontraststräckning (2 %–98 %) + lätt uppljusning, sedan Floyd–Steinberg
    uint32_t hist[256] = {0};
    const int N = COVER * COVER;
    for (int i = 0; i < N; ++i) hist[gray[i]]++;
    uint32_t cum = 0; int lo = 0, hi = 255;
    for (int i = 0; i < 256; ++i) { cum += hist[i]; if (cum >= N * 2 / 100) { lo = i; break; } }
    cum = 0;
    for (int i = 0; i < 256; ++i) { cum += hist[i]; if (cum >= N * 98 / 100) { hi = i; break; } }
    if (hi - lo < 32) { lo = 0; hi = 255; }
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) {
        float v = (float)(i - lo) / (float)(hi - lo);
        v = v < 0 ? 0 : (v > 1 ? 1 : v);
        lut[i] = (uint8_t)(255.0f * powf(v, 0.85f));
    }
    static int16_t e0[COVER + 2], e1[COVER + 2];
    memset(e0, 0, sizeof(e0)); memset(e1, 0, sizeof(e1));
    int16_t *cur = e0, *nxt = e1;
    for (int y = 0; y < COVER; ++y) {
        memset(nxt, 0, sizeof(e0));
        for (int x = 0; x < COVER; ++x) {
            int v = lut[gray[y * COVER + x]] + cur[x + 1];
            int q = v < 128 ? 0 : 255;
            int err = v - q;
            dst[y * COVER + x] = q == 0 ? 1 : 0;
            cur[x + 2] += err * 7 / 16;
            nxt[x]     += err * 3 / 16;
            nxt[x + 1] += err * 5 / 16;
            nxt[x + 2] += err * 1 / 16;
        }
        int16_t *t = cur; cur = nxt; nxt = t;
    }
}

// Hämtar och förbereder omslaget i buffert `slot`. true = klart.
static bool loadCover(const char *url, int slot) {
    WiFiClientSecure c; c.setInsecure();
    HTTPClient http;
    if (!http.begin(c, url)) return false;
    http.setTimeout(10000);
    int code = http.GET();
    int len = http.getSize();
    if (code != 200 || len <= 0 || len > 250000) { http.end(); return false; }
    uint8_t *buf = (uint8_t *)ps_malloc(len);
    if (!buf) { http.end(); return false; }
    WiFiClient *s = http.getStreamPtr();
    int got = 0; uint32_t t = millis();
    while (http.connected() && got < len && millis() - t < 10000) {
        int n = s->available();
        if (n) { got += s->read(buf + got, min(n, len - got)); t = millis(); }
        else delay(2);
    }
    http.end();
    bool ok = false;
    if (got == len) {
        JPEGDEC jpeg;
        if (jpeg.openRAM(buf, len, jpgDraw)) {
            int w = jpeg.getWidth();
            if (w >= 250 && w <= 350) {                 // 300 px -> 150 px i halv storlek
                jpeg.setPixelType(EIGHT_BIT_GRAYSCALE);
                memset(gray, 255, COVER * COVER);
                ok = jpeg.decode(0, 0, JPEG_SCALE_HALF) != 0;
            }
            jpeg.close();
        }
    }
    free(buf);
    if (ok) ditherInto(coverBW[slot]);
    return ok;
}

// ================= Uppspelning: hämta status =================
static void pollPlayback() {
    String body;
    int code = apiCall("GET", "https://api.spotify.com/v1/me/player/currently-playing?additional_types=episode", &body);
    static int lastCode = 12345;
    if (code != lastCode) { Serial.printf("Spotify currently-playing -> %d\n", code); lastCode = code; }
    if (code == 200) {
        JsonDocument filter;
        filter["is_playing"] = true;
        filter["progress_ms"] = true;
        filter["currently_playing_type"] = true;
        JsonObject fi = filter["item"].to<JsonObject>();
        fi["id"] = true; fi["name"] = true; fi["duration_ms"] = true;
        fi["artists"][0]["name"] = true;
        fi["album"]["images"][0]["url"] = true; fi["album"]["images"][0]["width"] = true;
        fi["images"][0]["url"] = true; fi["images"][0]["width"] = true;
        fi["show"]["name"] = true;
        JsonDocument doc;
        if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return;

        const char *type = doc["currently_playing_type"] | "";
        JsonObject item = doc["item"];
        bool playing = doc["is_playing"] | false;
        uint32_t prog = doc["progress_ms"] | 0;

        char id[48] = "", title[160] = "", artist[160] = "";
        uint32_t dur = 0;
        String imgUrl;
        if (item.isNull()) {                                   // t.ex. reklam
            strlcpy(id, type, sizeof(id));
            strlcpy(title, !strcmp(type, "ad") ? "Reklam" : "Okand typ", sizeof(title));
        } else {
            strlcpy(id, item["id"] | (const char *)(item["name"] | ""), sizeof(id));
            strlcpy(title, item["name"] | "", sizeof(title));
            dur = item["duration_ms"] | 0;
            String a;
            for (JsonObject ar : item["artists"].as<JsonArray>()) {
                if (a.length()) a += ", ";
                a += (const char *)(ar["name"] | "");
            }
            if (!a.length()) a = (const char *)(item["show"]["name"] | "");
            strlcpy(artist, a.c_str(), sizeof(artist));
            JsonArray imgs = item["album"]["images"].is<JsonArray>() ? item["album"]["images"].as<JsonArray>()
                                                                     : item["images"].as<JsonArray>();
            int best = 1 << 30;
            for (JsonObject im : imgs) {
                int w = im["width"] | 0;
                int dlt = abs(w - 300);
                if (w && dlt < best) { best = dlt; imgUrl = (const char *)(im["url"] | ""); }
            }
        }

        bool newTrack = strcmp(id, np.id) != 0 || !np.have;
        if (newTrack) {
            int slot = 1 - coverCur;
            bool cov = imgUrl.length() && loadCover(imgUrl.c_str(), slot);
            portENTER_CRITICAL(&mux);
            strlcpy(np.id, id, sizeof(np.id));
            strlcpy(np.title, title, sizeof(np.title));
            strlcpy(np.artist, artist, sizeof(np.artist));
            np.durMs = dur; np.progMs = prog; np.progAt = millis();
            np.playing = playing; np.have = true; np.cover = cov; np.msg[0] = 0;
            if (cov) coverCur = slot;
            portEXIT_CRITICAL(&mux);
            needFull = true;
            Serial.printf("Ny lat: %s - %s (omslag: %s)\n", title, artist, cov ? "ja" : "nej");
        } else {
            bool changed = playing != np.playing;
            portENTER_CRITICAL(&mux);
            np.progMs = prog; np.progAt = millis(); np.playing = playing; np.durMs = dur ? dur : np.durMs; np.msg[0] = 0;
            portEXIT_CRITICAL(&mux);
            if (changed) needPartial = true;
        }
    } else if (code == 204) {                                  // inget aktivt
        if (np.have && np.playing) { np.playing = false; needPartial = true; }
        else if (!np.have && strcmp(np.msg, "Ingenting spelas") != 0) { setMsg("Ingenting spelas"); needFull = true; }
    } else if (code == 401) {
        tokenExpiresAt = 0;
    } else if (code < 0) {
        Serial.printf("Nätverksfel: %d\n", code);
    }
}

static void runCommand(int cmd) {
    if (cmd == 1) {
        bool was = np.playing;
        int code = apiCall("PUT", was ? "https://api.spotify.com/v1/me/player/pause" : "https://api.spotify.com/v1/me/player/play");
        if (code == 200 || code == 204) {
            portENTER_CRITICAL(&mux);
            np.playing = !was; np.progAt = millis(); np.msg[0] = 0;
            portEXIT_CRITICAL(&mux);
            needPartial = true;
        } else if (code == 404) { setMsg("Oppna Spotify pa en enhet"); needFull = true; }
        else if (code == 403) { setMsg("Kraver Spotify Premium"); needFull = true; }
        Serial.printf("start/stopp -> %d\n", code);
    } else if (cmd == 2) {
        int code = apiCall("POST", "https://api.spotify.com/v1/me/player/next");
        if (code == 404) { setMsg("Oppna Spotify pa en enhet"); needFull = true; }
        else if (code == 403) { setMsg("Kraver Spotify Premium"); needFull = true; }
        Serial.printf("nasta -> %d\n", code);
    }
}

static void netTask(void *) {
    uint32_t nextPoll = 0;
    for (;;) {
        if (pendingCmd) { int c = pendingCmd; pendingCmd = 0; runCommand(c); nextPoll = millis() + 900; }
        if ((int32_t)(millis() - nextPoll) >= 0 && (int32_t)(millis() - backoffUntil) >= 0) {
            pollPlayback();
            nextPoll = millis() + (np.playing ? 5000 : 10000);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ================= Ritning =================
// true om texten innehåller tecken utanför latin-1 (kinesiska, japanska, kyrilliska ...)
static bool needsWide(const char *s) {
    for (const uint8_t *p = (const uint8_t *)s; *p; ++p)
        if (*p >= 0xC4) return true;                 // UTF-8 ledbyte för tecken från U+0100 och uppåt
    return false;
}

static String fit(const char *s, int maxW) {
    String t = s;
    if (u8.getUTF8Width(t.c_str()) <= maxW) return t;
    while (t.length() > 1) {
        int i = t.length() - 1;
        while (i > 0 && (t[i] & 0xC0) == 0x80) --i;
        t.remove(i);
        String c = t + "...";
        if (u8.getUTF8Width(c.c_str()) <= maxW) return c;
    }
    return t;
}

static void drawStrip(const NowPlaying &n) {
    display.fillRect(0, STRIP_Y, 200, STRIP_H, GxEPD_WHITE);
    if (n.playing) {                                           // paus-ikon
        display.fillRect(6, STRIP_Y + 2, 3, 10, GxEPD_BLACK);
        display.fillRect(12, STRIP_Y + 2, 3, 10, GxEPD_BLACK);
    } else {                                                   // play-ikon
        display.fillTriangle(6, STRIP_Y + 2, 6, STRIP_Y + 12, 15, STRIP_Y + 7, GxEPD_BLACK);
    }
    display.drawRect(22, STRIP_Y + 3, 172, 8, GxEPD_BLACK);
    uint32_t prog = n.progMs + (n.playing ? millis() - n.progAt : 0);
    if (n.durMs && prog > n.durMs) prog = n.durMs;
    int w = n.durMs ? (int)((uint64_t)prog * 168 / n.durMs) : 0;
    if (w > 0) display.fillRect(24, STRIP_Y + 5, w, 4, GxEPD_BLACK);
}

static void drawIdle(const NowPlaying &n) {
    u8.setFontMode(1); u8.setForegroundColor(GxEPD_BLACK); u8.setBackgroundColor(GxEPD_WHITE);
    u8.setFont(u8g2_font_helvB18_tf);
    u8.setCursor((200 - u8.getUTF8Width("Spotify")) / 2, 70); u8.print("Spotify");
    u8.setFont(u8g2_font_helvR10_tf);
    const char *m = n.msg[0] ? n.msg : "Startar...";
    String f = fit(m, 190);
    u8.setCursor((200 - u8.getUTF8Width(f.c_str())) / 2, 110); u8.print(f);
    u8.setFont(u8g2_font_helvR08_tf);
    const char *h1 = "BOOT = start/stopp";
    const char *h2 = "PWR  = nasta lat";
    u8.setCursor((200 - u8.getUTF8Width(h1)) / 2, 160); u8.print(h1);
    u8.setCursor((200 - u8.getUTF8Width(h2)) / 2, 175); u8.print(h2);
}

static void drawFullScreen(const NowPlaying &n) {
    display.setRotation(1);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        if (!n.have) { drawIdle(n); continue; }
        if (n.cover) {
            const uint8_t *bw = coverBW[coverCur];
            for (int y = 0; y < COVER; ++y)
                for (int x = 0; x < COVER; ++x)
                    if (bw[y * COVER + x]) display.drawPixel(COVER_X + x, COVER_Y + y, GxEPD_BLACK);
        } else {
            display.drawRect(COVER_X, COVER_Y, COVER, COVER, GxEPD_BLACK);
        }
        u8.setFontMode(1); u8.setForegroundColor(GxEPD_BLACK); u8.setBackgroundColor(GxEPD_WHITE);
        u8.setFont(needsWide(n.title) ? u8g2_font_wqy12_t_gb2312 : u8g2_font_helvB10_tf);
        String t = fit(n.title, 192);
        u8.setCursor(4, 167); u8.print(t);
        const char *sub = n.msg[0] ? n.msg : n.artist;
        u8.setFont(needsWide(sub) ? u8g2_font_wqy12_t_gb2312 : u8g2_font_helvR08_tf);
        String a = fit(sub, 192);
        u8.setCursor(4, 180); u8.print(a);
        drawStrip(n);
    } while (display.nextPage());
}

static void drawPartialStrip(const NowPlaying &n) {
    display.setRotation(1);
    display.setPartialWindow(0, STRIP_Y, 200, STRIP_H);
    display.firstPage();
    do { drawStrip(n); } while (display.nextPage());
}

static void displayTask(void *) {
    uint32_t lastFull = millis(), lastPartial = millis();
    for (;;) {
        NowPlaying n;
        portENTER_CRITICAL(&mux); n = np; portEXIT_CRITICAL(&mux);
        bool full = needFull, part = needPartial;
        if (!full && n.have && n.playing && millis() - lastPartial > 15000) part = true;     // förloppsstapel
        if (!full && n.have && n.playing && millis() - lastFull > 600000) full = true;       // rensa spöken
        if (full)      { needFull = false; needPartial = false; drawFullScreen(n);  lastFull = lastPartial = millis(); }
        else if (part) { needPartial = false;                   drawPartialStrip(n); lastPartial = millis(); }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ================= Knappar =================
static bool pressed(int pin, bool &was) {
    bool now = digitalRead(pin) == LOW;
    if (now && !was) { delay(30); if (digitalRead(pin) == LOW) { was = true; return true; } }
    if (!now) was = false;
    return false;
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_VBAT, OUTPUT);    digitalWrite(PIN_VBAT, HIGH);
    pinMode(PIN_EPD_PWR, OUTPUT); digitalWrite(PIN_EPD_PWR, LOW);
    pinMode(PIN_AUD_PWR, OUTPUT); digitalWrite(PIN_AUD_PWR, HIGH);   // ljud av
    pinMode(PIN_PA, OUTPUT);      digitalWrite(PIN_PA, LOW);
    pinMode(PIN_BOOT, INPUT_PULLUP);
    pinMode(PIN_PWR, INPUT_PULLUP);
    delay(100);

    gray = (uint8_t *)ps_malloc(COVER * COVER);
    coverBW[0] = (uint8_t *)ps_malloc(COVER * COVER);
    coverBW[1] = (uint8_t *)ps_malloc(COVER * COVER);
    if (!gray || !coverBW[0] || !coverBW[1]) { Serial.println("Minne (PSRAM) saknas!"); }

    SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
    display.init(115200, true, 2, false);
    u8.begin(display);

    prefs.begin("spotify", false);
    refreshToken = prefs.getString("rt", "");
    // Ny token i secrets.h (efter ny inloggning) ska vinna över den som ligger sparad i kortets minne.
    if (!refreshToken.length() || prefs.getString("src", "") != SPOTIFY_REFRESH_TOKEN) {
        refreshToken = SPOTIFY_REFRESH_TOKEN;
        prefs.putString("rt", refreshToken);
        prefs.putString("src", SPOTIFY_REFRESH_TOKEN);
    }

    if (!strcmp(WIFI_SSID, "DITT_WIFI_NAMN") || !strcmp(SPOTIFY_CLIENT_ID, "DITT_CLIENT_ID") ||
        !strncmp(SPOTIFY_REFRESH_TOKEN, "SKRIVS_AV", 9)) {
        setMsg("Fyll i secrets.h");
        drawFullScreen(np);
        for (;;) delay(1000);
    }

    setMsg("Ansluter till WiFi...");
    drawFullScreen(np);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int tries = 0; WiFi.status() != WL_CONNECTED; ++tries) {
        delay(500);
        if (tries % 40 == 39) { WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    }
    Serial.printf("WiFi ok, IP %s\n", WiFi.localIP().toString().c_str());

    setMsg("Ansluter till Spotify...");
    needFull = true;
    xTaskCreatePinnedToCore(displayTask, "display", 10240, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(netTask, "net", 24576, nullptr, 2, nullptr, 1);
}

void loop() {
    static bool bootWas = false, pwrWas = false;
    if (pressed(PIN_BOOT, bootWas)) pendingCmd = 1;     // start / stopp
    if (pressed(PIN_PWR, pwrWas))   pendingCmd = 2;     // nästa låt
    delay(10);
}
