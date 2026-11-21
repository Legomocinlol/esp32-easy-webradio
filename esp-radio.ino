#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "driver/i2s_std.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define PIN_BCLK 16
#define PIN_LRC  17
#define PIN_DOUT 4
#define PIN_SD   15
#define PIN_TCH1 18
#define PIN_TCH2 27
#define PIN_POT  33
#define BUF_SZ   32768
#define MAX_ST   15

Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// === НАСТРОЙКИ WI-FI (впишите свои) ===
const char* ssid = "YOUR_WIFI_NAME";
const char* pass = "YOUR_WIFI_PASSWORD";

// Параметры автозагрузки станций (страна KZ, только MP3, только рабочие, топ-15 по популярности)
const char* API_URL = "http://radio-browser.info";

struct St { String n, u; };
St pl[MAX_ST];
int N = 0;

volatile int st = 0, vol = 60;
volatile bool chg = false, upd = false;
volatile uint8_t bars = {0};
String msg = "LOAD LIST...";

WiFiClient cli;
WiFiClientSecure scli;
HTTPClient http;
WiFiClient* stm = nullptr;
i2s_chan_handle_t tx = nullptr;
int sr = 44100, bin = 0;
mp3dec_t dec;
mp3dec_frame_info_t inf;
uint8_t buf[BUF_SZ];
int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

// Загрузка и парсинг списка станций Казахстана при старте
void fetchStations() {
  HTTPClient api;
  api.setConnectTimeout(8000);
  api.setTimeout(8000);
  api.setUserAgent("ESP32Radio/1.0");

  if (api.begin(cli, API_URL) && api.GET() == 200) {
    String json = api.getString();
    int pos = 0;
    while (N < MAX_ST) {
      int nKey = json.indexOf("\"name\":\"", pos);
      int uKey = json.indexOf("\"url_resolved\":\"", pos);
      if (nKey < 0 || uKey < 0) break;

      int nStart = nKey + 8;
      int nEnd = json.indexOf("\"", nStart);
      int uStart = uKey + 16;
      int uEnd = json.indexOf("\"", uStart);

      String sName = json.substring(nStart, nEnd);
      String sUrl  = json.substring(uStart, uEnd);
      sName.trim();

      if (sUrl.startsWith("http") && sName.length() > 0) {
        pl[N].n = sName.substring(0, 20); // Обрезаем длинное имя под ширину OLED
        pl[N].u = sUrl;
        N++;
      }
      pos = max(nEnd, uEnd) + 1;
    }
  }
  api.end();

  // Резервный список, если API временно недоступен при очень слабом Wi-Fi
  if (N == 0) {
    pl[0] = {"Europa Plus KZ", "http://hostingradio.ru"};
    pl[1] = {"Lux FM KZ",      "http://luxfm.kz"};
    pl[2] = {"Radio City",     "http://cityradio.kz"};
    pl[3] = {"Vesti FM",       "http://cdnvideo.ru"};
    N = 4;
  }
}

void setI2S(int rate) {
  if (tx) { i2s_channel_disable(tx); i2s_del_channel(tx); }
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.auto_clear = true;
  i2s_new_channel(&cc, &tx, NULL);
  i2s_std_config_t sc = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { I2S_GPIO_UNUSED, (gpio_num_t)PIN_BCLK, (gpio_num_t)PIN_LRC, (gpio_num_t)PIN_DOUT, I2S_GPIO_UNUSED }
  };
  i2s_channel_init_std_mode(tx, &sc);
  i2s_channel_enable(tx);
  sr = rate;
}

void draw() {
  oled.clearDisplay();
  oled.setTextColor(1);
  oled.setTextSize(1);

  oled.setCursor(0, 0);  oled.print(msg);
  oled.setCursor(92, 0); oled.printf("%d/%d", st + 1, N);
  oled.drawLine(0, 9, 127, 9, 1);

  oled.setCursor(0, 13);
  oled.println(N > 0 ? pl[st].n : "Loading KZ...");

  // 12 столбиков эквалайзера
  for (int b = 0; b < 12; b++) {
    int h = bars[b];
    if (h > 0) oled.fillRect(b * 10 + 4, 49 - h, 7, h, 1);
    else oled.drawFastHLine(b * 10 + 4, 48, 7, 1);
  }

  oled.setCursor(0, 55); oled.printf("VOL:%d%%", vol);
  oled.drawRect(54, 55, 74, 8, 1);
  if (vol > 0) oled.fillRect(56, 57, map(vol, 0, 100, 0, 70), 4, 1);

  oled.display();
}

void scan() {
  for (int i = 0; i < N; i++) {
    chg = false; msg = "SEARCH...";
    for (int b = 0; b < 12; b++) bars[b] = 0;
    upd = true;

    http.end(); stm = nullptr; bin = 0; mp3dec_init(&dec);
    String u = pl[st].u;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("WinampMPEG/5.0");

    if (u.startsWith("https")) { scli.setInsecure(); http.begin(scli, u); }
    else http.begin(cli, u);

    if (http.GET() == 200) {
      stm = http.getStreamPtr();
      msg = "BUFFER...";
      upd = true;
      return;
    }
    st = (st + 1) % N;
    vTaskDelay(150 / portTICK_PERIOD_MS);
  }
  msg = "NO SIGNAL"; upd = true;
}

void task(void* p) {
  unsigned long tData = millis();
  bool waitBuf = true;
  scan(); tData = millis();

  while (1) {
    if (chg) { chg = false; waitBuf = true; scan(); tData = millis(); }
    if (WiFi.status() != WL_CONNECTED) { msg = "NO WIFI"; upd = true; vTaskDelay(500); continue; }

    if (stm && http.connected()) {
      int a = stm->available();
      if (a > 0 && bin < BUF_SZ) {
        int r = stm->readBytes(buf + bin, min(a, BUF_SZ - bin));
        if (r > 0) { bin += r; tData = millis(); }
      }
    }

    if (millis() - tData > 12000) { st = (st + 1) % N; waitBuf = true; scan(); tData = millis(); continue; }
    if (waitBuf) {
      if (bin >= 2500) { waitBuf = false; msg = "PLAYING"; upd = true; }
      else { vTaskDelay(10); continue; }
    }

    if (bin > 2048) {
      int smp = mp3dec_decode_frame(&dec, buf, bin, pcm, &inf);
      if (inf.frame_bytes > 0) { bin -= inf.frame_bytes; if (bin > 0) memmove(buf, buf + inf.frame_bytes, bin); }
      else if (!smp) { bin--; memmove(buf, buf + 1, bin); }

      if (smp > 0) {
        if (inf.hz != sr && inf.hz > 0) setI2S(inf.hz);
        int tot = smp * inf.channels;
        float g = vol / 100.0f;

        int step = max(1, tot / 12);
        for (int b = 0; b < 12; b++) {
          int peak = abs(pcm[b * step]) * g;
          uint8_t target = constrain(map(peak, 0, 14000, 0, 22), 0, 22);
          bars[b] = (target > bars[b]) ? target : (bars[b] > 2 ? bars[b] - 2 : 0);
        }

        for (int i = 0; i < tot; i++) pcm[i] *= g;
        if (inf.channels == 1) {
          for (int i = smp - 1; i >= 0; i--) pcm[i * 2] = pcm[i * 2 + 1] = pcm[i];
          tot = smp * 2;
        }
        size_t w;
        i2s_channel_write(tx, pcm, tot * 2, &w, portMAX_DELAY);
      }
    } else {
      waitBuf = true; msg = "BUFFER...";
      for (int b = 0; b < 12; b++) bars[b] = 0;
      upd = true; vTaskDelay(10);
    }
  }
}

void setup() {
  pinMode(PIN_SD, OUTPUT); digitalWrite(PIN_SD, 1);
  pinMode(PIN_TCH1, INPUT);
  analogReadResolution(12);
  Wire.begin(21, 22); Wire.setClock(400000);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  draw();

  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  fetchStations(); // Скачиваем свежий список MP3-станций Казахстана
  setI2S(44100);
  xTaskCreatePinnedToCore(task, "A", 32768, NULL, 2, NULL, 1);
}

void loop() {
  static int pVol = -1;
  static bool pTch = 0;
  static unsigned long tVol = 0, tTch = 0, tDraw = 0;

  if (upd || (msg == "PLAYING" && millis() - tDraw >= 65)) { upd = false; tDraw = millis(); draw(); }

  if (millis() - tVol >= 80) {
    tVol = millis();
    int v = map(analogRead(PIN_POT), 0, 4095, 0, 100);
    if (abs(v - pVol) >= 2 || (!v && pVol) || (v == 100 && pVol != 100)) { vol = pVol = v; draw(); }
  }

  bool tch = digitalRead(PIN_TCH1) || (touchRead(PIN_TCH2) < 35);
  if (tch && !pTch && millis() - tTch > 350) {
    tTch = millis(); st = (st + 1) % N; msg = "NEXT..."; draw(); chg = true;
  }
  pTch = tch;
  delay(10);
}
