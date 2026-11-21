#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "driver/i2s_std.h"
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* COUNTRY_CODE = ""; // "KZ", "RU", "UA", "BY" or "" for world top
const int MAX_STATIONS = 15;
#define OLED_SDA  21
#define OLED_SCL  22
#define I2S_BCLK  16
#define I2S_LRC   17
#define I2S_DOUT  4
#define AMP_SD    15
#define TOUCH_EXT 18
#define TOUCH_INT 27
#define POT_PIN   33
#define BUF_SZ    32768
#define NUM_BARS  12
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
struct Station { String name, url; };
Station playlist[25];
int stationCount = 0;
volatile int currentStation = 0;
volatile int volumePercent = 60;
volatile bool needChange = false;
volatile bool needRedraw = false;
volatile uint8_t eqBars[NUM_BARS];
String statusMsg = "ЗАГРУЗКА...";
WiFiClient plainClient;
WiFiClientSecure secureClient;
HTTPClient http;
WiFiClient* streamPtr = nullptr;
i2s_chan_handle_t txChan = nullptr;
int sampleRate = 44100;
int bytesInBuf = 0;
mp3dec_t mp3d;
mp3dec_frame_info_t frameInfo;
uint8_t mp3Buf[BUF_SZ];
int16_t pcmBuf[MINIMP3_MAX_SAMPLES_PER_FRAME];
String decodeJsonUtf8(String s) {
String out = "";
for (int i = 0; i < s.length(); i++) {
if (s[i] == '\' && i + 5 < s.length() && s[i + 1] == 'u') {
uint16_t code = strtol(s.substring(i + 2, i + 6).c_str(), NULL, 16);
if (code < 0x80) out += (char)code;
else if (code < 0x800) {
out += (char)(0xC0 | (code >> 6));
out += (char)(0x80 | (code & 0x3F));
} else {
out += (char)(0xE0 | (code >> 12));
out += (char)(0x80 | ((code >> 6) & 0x3F));
out += (char)(0x80 | (code & 0x3F));
}
i += 5;
} else out += s[i];
}
return out;
}
void addFallback(const char* name, const char* hostAndPath) {
if (stationCount < 25) {
playlist[stationCount].name = name;
playlist[stationCount].url = String("http") + "://" + hostAndPath;
stationCount++;
}
}
void fetchStations() {
HTTPClient api;
api.setConnectTimeout(8000);
api.setTimeout(8000);
api.setUserAgent("ESP32WebRadio/1.0");
String apiPath = "/json/stations/search?codec=MP3&hidebroken=true&order=clicktrend&reverse=true&bitrateMax=128&limit=" + String(MAX_STATIONS);
if (strlen(COUNTRY_CODE) > 0) apiPath += "&countrycode=" + String(COUNTRY_CODE);
if (api.begin(plainClient, "de1.api.radio-browser.info", 80, apiPath) && api.GET() == 200) {
String json = api.getString();
int pos = 0;
while (stationCount < MAX_STATIONS && stationCount < 25) {
int nKey = json.indexOf(""name":"", pos);
int uKey = json.indexOf(""url_resolved":"", pos);
if (nKey < 0 || uKey < 0) break;
int nStart = nKey + 8, nEnd = json.indexOf(""", nStart);
int uStart = uKey + 16, uEnd = json.indexOf(""", uStart);
String sName = decodeJsonUtf8(json.substring(nStart, nEnd));
String sUrl = json.substring(uStart, uEnd);
sName.trim();
if (sUrl.startsWith("http") && sName.length() > 0) {
playlist[stationCount].name = sName;
playlist[stationCount].url = sUrl;
stationCount++;
}
pos = max(nEnd, uEnd) + 1;
}
}
api.end();
if (stationCount == 0) {
addFallback("Jazz FM 89.1", "nashe1.hostingradio.ru/jazz-128.mp3");
addFallback("Europa Plus", "emgregion.hostingradio.ru:8064/kazakhstan.europaplus.mp3");
addFallback("Vesti FM", "icecast.vgtrk.cdnvideo.ru/vestifm_mp3_128kbps");
addFallback("Nostalgia FM", "nostalgiafm.hostingradio.ru:8000/nostalgiafm.mp3");
}
}
void setupI2S(int rate) {
if (txChan) {
i2s_channel_disable(txChan);
i2s_del_channel(txChan);
}
i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
chanCfg.auto_clear = true;
i2s_new_channel(&chanCfg, &txChan, NULL);
i2s_std_config_t stdCfg = {
.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate),
.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
.gpio_cfg = { I2S_GPIO_UNUSED, (gpio_num_t)I2S_BCLK, (gpio_num_t)I2S_LRC, (gpio_num_t)I2S_DOUT, I2S_GPIO_UNUSED }
};
i2s_channel_init_std_mode(txChan, &stdCfg);
i2s_channel_enable(txChan);
sampleRate = rate;
}
void drawDisplay() {
u8g2.clearBuffer();
u8g2.setFont(u8g2_font_6x12_t_cyrillic);
u8g2.drawUTF8(0, 9, statusMsg.c_str());
char countBuf[16];
snprintf(countBuf, sizeof(countBuf), "%d/%d", currentStation + 1, stationCount);
u8g2.drawUTF8(94, 9, countBuf);
u8g2.drawHLine(0, 11, 128);
u8g2.drawUTF8(0, 23, stationCount > 0 ? playlist[currentStation].name.c_str() : "Подключение...");
for (int b = 0; b < NUM_BARS; b++) {
int h = eqBars[b];
if (h > 0) u8g2.drawBox(b * 10 + 4, 50 - h, 7, h);
else u8g2.drawHLine(b * 10 + 4, 49, 7);
}
char volBuf[16];
snprintf(volBuf, sizeof(volBuf), "VOL:%d%%", volumePercent);
u8g2.drawUTF8(0, 63, volBuf);
u8g2.drawFrame(54, 55, 74, 8);
if (volumePercent > 0) u8g2.drawBox(56, 57, map(volumePercent, 0, 100, 0, 70), 4);
u8g2.sendBuffer();
}
void findWorkingStation() {
for (int i = 0; i < stationCount; i++) {
needChange = false;
statusMsg = "ПОИСК...";
for (int b = 0; b < NUM_BARS; b++) eqBars[b] = 0;
needRedraw = true;
http.end();
streamPtr = nullptr;
bytesInBuf = 0;
mp3dec_init(&mp3d);
String url = playlist[currentStation].url;
http.setConnectTimeout(8000);
http.setTimeout(8000);
http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
http.setUserAgent("WinampMPEG/5.0");
if (url.startsWith("https")) {
secureClient.setInsecure();
http.begin(secureClient, url);
} else http.begin(plainClient, url);
if (http.GET() == 200) {
streamPtr = http.getStreamPtr();
statusMsg = "БУФЕР...";
needRedraw = true;
return;
}
currentStation = (currentStation + 1) % stationCount;
vTaskDelay(150 / portTICK_PERIOD_MS);
}
statusMsg = "НЕТ СИГНАЛА";
needRedraw = true;
}
void audioTask(void* parameter) {
unsigned long lastDataTime = millis();
bool buffering = true;
findWorkingStation();
lastDataTime = millis();
while (true) {
if (needChange) {
needChange = false;
buffering = true;
findWorkingStation();
lastDataTime = millis();
}
if (WiFi.status() != WL_CONNECTED) {
statusMsg = "НЕТ WI-FI";
needRedraw = true;
vTaskDelay(500 / portTICK_PERIOD_MS);
continue;
}
if (streamPtr && http.connected()) {
int avail = streamPtr->available();
if (avail > 0 && bytesInBuf < BUF_SZ) {
int r = streamPtr->readBytes(mp3Buf + bytesInBuf, min(avail, BUF_SZ - bytesInBuf));
if (r > 0) {
bytesInBuf += r;
lastDataTime = millis();
}
}
}
if (millis() - lastDataTime > 12000) {
currentStation = (currentStation + 1) % stationCount;
buffering = true;
findWorkingStation();
lastDataTime = millis();
continue;
}
if (buffering) {
if (bytesInBuf >= 2500) {
buffering = false;
statusMsg = "ЭФИР";
needRedraw = true;
} else {
vTaskDelay(10 / portTICK_PERIOD_MS);
continue;
}
}
if (bytesInBuf > 2048) {
int samples = mp3dec_decode_frame(&mp3d, mp3Buf, bytesInBuf, pcmBuf, &frameInfo);
if (frameInfo.frame_bytes > 0) {
bytesInBuf -= frameInfo.frame_bytes;
if (bytesInBuf > 0) memmove(mp3Buf, mp3Buf + frameInfo.frame_bytes, bytesInBuf);
} else if (!samples) {
bytesInBuf--;
memmove(mp3Buf, mp3Buf + 1, bytesInBuf);
}
if (samples > 0) {
if (frameInfo.hz != sampleRate && frameInfo.hz > 0) setupI2S(frameInfo.hz);
int totalSamples = samples * frameInfo.channels;
float gain = volumePercent / 100.0f;
int step = max(1, totalSamples / NUM_BARS);
for (int b = 0; b < NUM_BARS; b++) {
int peak = abs(pcmBuf[b * step]) * gain;
uint8_t target = constrain(map(peak, 0, 14000, 0, 22), 0, 22);
eqBars[b] = (target > eqBars[b]) ? target : (eqBars[b] > 2 ? eqBars[b] - 2 : 0);
}
for (int i = 0; i < totalSamples; i++) pcmBuf[i] *= gain;
if (frameInfo.channels == 1) {
for (int i = samples - 1; i >= 0; i--) pcmBuf[i * 2] = pcmBuf[i * 2 + 1] = pcmBuf[i];
totalSamples = samples * 2;
}
size_t written;
i2s_channel_write(txChan, pcmBuf, totalSamples * 2, &written, portMAX_DELAY);
}
} else {
buffering = true;
statusMsg = "БУФЕР...";
for (int b = 0; b < NUM_BARS; b++) eqBars[b] = 0;
needRedraw = true;
vTaskDelay(10 / portTICK_PERIOD_MS);
}
}
}
void setup() {
Serial.begin(115200);
pinMode(AMP_SD, OUTPUT);
digitalWrite(AMP_SD, HIGH);
pinMode(TOUCH_EXT, INPUT);
analogReadResolution(12);
Wire.begin(OLED_SDA, OLED_SCL);
Wire.setClock(400000);
u8g2.begin();
u8g2.enableUTF8Print();
drawDisplay();
WiFi.mode(WIFI_STA);
WiFi.setSleep(false);
WiFi.begin(WIFI_SSID, WIFI_PASS);
while (WiFi.status() != WL_CONNECTED) delay(300);
fetchStations();
setupI2S(44100);
xTaskCreatePinnedToCore(audioTask, "AudioTask", 32768, NULL, 2, NULL, 1);
}
void loop() {
static int prevVol = -1;
static bool prevTouch = false;
static unsigned long tVol = 0, tTouch = 0, tDraw = 0;
if (needRedraw || (statusMsg == "ЭФИР" && millis() - tDraw >= 65)) {
needRedraw = false;
tDraw = millis();
drawDisplay();
}
if (millis() - tVol >= 80) {
tVol = millis();
int v = map(analogRead(POT_PIN), 0, 4095, 0, 100);
if (abs(v - prevVol) >= 2 || (!v && prevVol) || (v == 100 && prevVol != 100)) {
volumePercent = prevVol = v;
drawDisplay();
}
}
bool touched = digitalRead(TOUCH_EXT) || (touchRead(TOUCH_INT) < 35);
if (touched && !prevTouch && millis() - tTouch > 350) {
tTouch = millis();
currentStation = (currentStation + 1) % stationCount;
statusMsg = "ДАЛЕЕ...";
drawDisplay();
needChange = true;
}
prevTouch = touched;
delay(10);
}
