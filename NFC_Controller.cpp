#include <Wire.h>
#include <Adafruit_PN532.h>
#include <U8g2lib.h>
#include <Ndef.h>
#include <NdefMessage.h>
#include <NdefRecord.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include <I2S.h>

// ==================== 正确引脚定义（K1=键1 K2=键2 K3=键3 K4=键4）====================
#define PN532_SDA      17
#define PN532_SCL      18
#define BUZZER_PIN     8

#define BTN1_PIN       4   // K1 → 按键1
#define BTN2_PIN       5   // K2 → 按键2
#define BTN3_PIN       6   // K3 → 按键3
#define BTN4_PIN       7   // K4 → 按键4

#define I2S_SCK_PIN    10
#define I2S_WS_PIN     9
#define I2S_SD_PIN     11
// ==================== 以下代码完全不用动 ====================


// ==================== 默认配置 ====================
const String defaultSSID     = "OPPO Reno 12";
const String defaultPWD      = "1234ABCD";
const int    holdToShowIP    = 800;

String ssid;
String password;
String adminUID = "";
bool nfcLoginOK = false;

Preferences prefs;
WebServer server(80);
Adafruit_PN532 nfc(PN532_SDA, PN532_SCL);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

bool isPowerOn = true;

// 按钮初始配置
struct Btn {
  String name;
  String data;
} btn[4] = {
  {"酷狗概念", "intent:#Intent;launchFlags=0x10000000;component=com.kugou.android.lite/.activity.SplashActivity;end"},
  {"王者荣耀", "intent:#Intent;launchFlags=0x10000000;component=com.tencent.tmgp.sgame/.SGameActivity;end"},
  {"微信", "intent:#Intent;launchFlags=0x10000000;component=com.tencent.mm/.ui.LauncherUI;end"},
  {"支付宝", "intent:#Intent;launchFlags=0x10000000;component=com.eg.android.AlipayGphone/.AlipayLogin;end"}
};

enum State { STANDBY, WRITING, WRITTEN, VOICE_RECORD };
State sysState = STANDBY;

int currentBtn;
unsigned long writeTime;
bool needErase = false;

// 语音相关
bool isRecording = false;
String voiceText = "";

// 日志缓冲区
String logBuffer = "";
const int maxLogLines = 50;

// ==================== 日志函数 ====================
void addLog(String msg) {
  Serial.println(msg);
  logBuffer += msg + "\n";
  while (logBuffer.length() > 5000) {
    int pos = logBuffer.indexOf("\n");
    if (pos == -1) break;
    logBuffer = logBuffer.substring(pos + 1);
  }
}

// ==================== 工具函数 ====================
void beep(int n) {
  for (int i=0; i<n; i++) {
    tone(BUZZER_PIN, 1000, 80);
    delay(100);
  }
  noTone(BUZZER_PIN);
}

void powerOff() {
  isPowerOn = false;
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  u8g2.setPowerSave(1);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  beep(2);
  delay(200);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0); // 适配 BTN1_PIN=4
  esp_deep_sleep_start();
}

// 全屏麦克风界面
void showMicScreen() {
  u8g2.clearBuffer();
  u8g2.drawBox(50, 10, 28, 35);
  u8g2.drawBox(44, 45, 40, 8);
  u8g2.drawBox(54, 53, 20, 4);
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 63, voiceText.c_str());
  u8g2.sendBuffer();
}

// 开始语音
void startVoiceRecord() {
  if (sysState != STANDBY) return;
  isRecording = true;
  sysState = VOICE_RECORD;
  voiceText = "listening...";
  beep(1);
  static bool i2sInited = false;
  if (!i2sInited) {
    i2sInited = true;
    I2S.begin(I2S_PHILIPS_MODE, 16000, 16);
    I2S.setPins(I2S_SCK_PIN, I2S_WS_PIN, I2S_SD_PIN, -1);
  }
}

// 停止语音
void stopVoiceRecord() {
  isRecording = false;
  // 模拟语音识别结果，实际项目中替换为你的语音识别库调用
  voiceText = "设置按键1为微信"; // 示例：你说的话会被解析成这个字符串
  parseVoiceCommand(voiceText);
  sysState = STANDBY;
}

// 语音指令解析（完全保留你的原逻辑）
void parseVoiceCommand(String txt) {
  txt.toLowerCase();
  int target = -1;

  if (txt.indexOf("1") >= 0 || txt.indexOf("一") >= 0 || txt.indexOf("key1") >= 0) target = 0;
  else if (txt.indexOf("2") >= 0 || txt.indexOf("二") >= 0 || txt.indexOf("key2") >= 0) target = 1;
  else if (txt.indexOf("3") >= 0 || txt.indexOf("三") >= 0 || txt.indexOf("key3") >= 0) target = 2;
  else if (txt.indexOf("4") >= 0 || txt.indexOf("四") >= 0 || txt.indexOf("key4") >= 0) target = 3;

  if (target == -1) {
    voiceText = "key?";
    beep(1);
    delay(800);
    return;
  }

  // ========== 社交 ==========
  if (txt.indexOf("微信") >= 0) {
    btn[target].name = "微信";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.tencent.mm/.ui.LauncherUI;end";
  }
  else if (txt.indexOf("qq") >= 0 || txt.indexOf("扣扣") >= 0) {
    btn[target].name = "QQ";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.tencent.mobileqq/.activity.SplashActivity;end";
  }
  else if (txt.indexOf("抖音") >= 0) {
    btn[target].name = "抖音";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.ss.android.ugc.aweme/.main.MainActivity;end";
  }
  else if (txt.indexOf("小红书") >= 0) {
    btn[target].name = "小红书";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.xingin.xhs/.index.SplashActivity;end";
  }
  else if (txt.indexOf("微博") >= 0) {
    btn[target].name = "微博";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.sina.weibo/.SplashActivity;end";
  }

  // ========== 音乐视频 ==========
  else if (txt.indexOf("酷狗概念") >= 0 || txt.indexOf("听歌") >= 0 || txt.indexOf("音乐") >= 0) {
    btn[target].name = "酷狗概念";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.kugou.android.lite/.activity.SplashActivity;end";
  }
  else if (txt.indexOf("网易云") >= 0) {
    btn[target].name = "网易云音乐";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.netease.cloudmusic/.activity.MainActivity;end";
  }
  else if (txt.indexOf("qq音乐") >= 0) {
    btn[target].name = "QQ音乐";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.tencent.qqmusic/.activity.AppStarterActivity;end";
  }
  else if (txt.indexOf("酷狗") >= 0) {
    btn[target].name = "酷狗音乐";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.kugou.android/.app.splash.SplashActivity;end";
  }
  else if (txt.indexOf("b站") >= 0 || txt.indexOf("哔哩") >= 0 || txt.indexOf("bilibili") >= 0) {
    btn[target].name = "哔哩哔哩";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=tv.danmaku.bili/.ui.splash.SplashActivity;end";
  }
  else if (txt.indexOf("爱奇艺") >= 0 || txt.indexOf("视频") >= 0) {
    btn[target].name = "爱奇艺";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.qiyi.video/.WelcomeActivity;end";
  }
  else if (txt.indexOf("腾讯视频") >= 0) {
    btn[target].name = "腾讯视频";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.tencent.qqlive/.ona.activity.SplashActivity;end";
  }

  // ========== 游戏 ==========
  else if (txt.indexOf("王者") >= 0 || txt.indexOf("荣耀") >= 0) {
    btn[target].name = "王者荣耀";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.tencent.tmgp.sgame/.SGameActivity;end";
  }
  else if (txt.indexOf("和平精英") >= 0 || txt.indexOf("吃鸡") >= 0) {
    btn[target].name = "和平精英";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.tencent.tmgp.pubgmhd/.PEGameActivity;end";
  }
  else if (txt.indexOf("原神") >= 0) {
    btn[target].name = "原神";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.miHoYo.Yuanshen/.SplashActivity;end";
  }
  else if (txt.indexOf("星穹铁道") >= 0 || txt.indexOf("铁道") >= 0) {
    btn[target].name = "星穹铁道";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.miHoYo.hkrpg/.SplashActivity;end";
  }

  // ========== 工具 ==========
  else if (txt.indexOf("支付宝") >= 0) {
    btn[target].name = "支付宝";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.eg.android.AlipayGphone/.AlipayLogin;end";
  }
  else if (txt.indexOf("高德") >= 0 || txt.indexOf("地图") >= 0 || txt.indexOf("导航") >= 0) {
    btn[target].name = "高德地图";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.autonavi.minimap/.activity.WelcomeActivity;end";
  }
  else if (txt.indexOf("百度地图") >= 0) {
    btn[target].name = "百度地图";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.baidu.BaiduMap/.app.WelcomeActivity;end";
  }
  else if (txt.indexOf("天气") >= 0) {
    btn[target].name = "天气";
    btn[target].data = "intent:#Intent;component=com.android.weather/.MainActivity;end";
  }
  else if (txt.indexOf("闹钟") >= 0 || txt.indexOf("时钟") >= 0) {
    btn[target].name = "时钟";
    btn[target].data = "intent:#Intent;component=com.android.deskclock/.DeskClock;end";
  }
  else if (txt.indexOf("计算器") >= 0) {
    btn[target].name = "计算器";
    btn[target].data = "intent:#Intent;component=com.android.calculator2/.Calculator;end";
  }

  // ========== 购物生活 ==========
  else if (txt.indexOf("淘宝") >= 0) {
    btn[target].name = "淘宝";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.taobao.tao/.HomeActivity;end";
  }
  else if (txt.indexOf("京东") >= 0) {
    btn[target].name = "京东";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.jingdong.app.mall/.MainActivity;end";
  }
  else if (txt.indexOf("拼多多") >= 0 || txt.indexOf("拼多") >= 0) {
    btn[target].name = "拼多多";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.xunmeng.pinduoduo/.activity.LaunchActivity;end";
  }
  else if (txt.indexOf("美团") >= 0 || txt.indexOf("外卖") >= 0) {
    btn[target].name = "美团";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.sankuai.meituan/.activity.MTWelcomeActivity;end";
  }
  else if (txt.indexOf("饿了么") >= 0) {
    btn[target].name = "饿了么";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=me.ele/.activity.SplashActivity;end";
  }

  // ========== 学习办公 ==========
  else if (txt.indexOf("wps") >= 0 || txt.indexOf("文档") >= 0 || txt.indexOf("表格") >= 0 || txt.indexOf("ppt") >= 0) {
    btn[target].name = "WPS";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=cn.wps.moffice_eng/.activity.SplashActivity;end";
  }
  else if (txt.indexOf("飞书") >= 0) {
    btn[target].name = "飞书";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.bytedance.feishu/.applink.activity.LauncherActivity;end";
  }
  else if (txt.indexOf("钉钉") >= 0) {
    btn[target].name = "钉钉";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.alibaba.android.rimet/.biz.SplashActivity;end";
  }
  else if (txt.indexOf("微信读书") >= 0 || txt.indexOf("看书") >= 0 || txt.indexOf("读书") >= 0) {
    btn[target].name = "微信读书";
    btn[target].data = "intent:#Intent;launchFlags=0x10000000;component=com.tencent.weread/.launch.WereadSplashActivity;end";
  }

  // ========== 网站 ==========
  else if (txt.indexOf("皮肤") >= 0 || txt.indexOf("皮肤站") >= 0) {
    btn[target].name = "皮肤站";
    btn[target].data = "https://littleskin.cn";
  }
  else if (txt.indexOf("百度") >= 0) {
    btn[target].name = "百度";
    btn[target].data = "https://www.baidu.com";
  }
  else {
    voiceText = "what?";
    beep(1);
    delay(800);
    return;
  }

  prefs.begin("cfg", false);
  prefs.putString("n"+String(target), btn[target].name);
  prefs.putString("d"+String(target), btn[target].data);
  prefs.end();

  addLog("Voice set btn " + String(target+1) + ": " + btn[target].name);

  voiceText = "OK!";
  showMicScreen();
  beep(3);
  delay(800);
}

// 显示IP
void showIP() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0,12,"IP:");
  u8g2.drawStr(0,28, WiFi.localIP().toString().c_str());
  u8g2.drawStr(0,44,"Long B1=off");
  u8g2.drawStr(0,60,"Hold B3=voice");
  u8g2.sendBuffer();
  beep(1);
  delay(2000);
}

String uid2str(uint8_t *uid, uint8_t len) {
  String s;
  for (int i=0; i<len; i++) {
    if (uid[i] < 16) s += "0";
    s += String(uid[i], HEX);
  }
  return s;
}

// 主界面
void drawStandby() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0,14,"1:" + btn[0].name);
  u8g2.drawStr(0,29,"2:" + btn[1].name);
  u8g2.drawStr(0,44,"3:" + btn[2].name);
  u8g2.drawStr(0,59,"4:" + btn[3].name);
  u8g2.sendBuffer();
}

// 按键处理
void checkButtons() {
  static unsigned long t1 = 0;
  if (digitalRead(BTN1_PIN) == LOW) {
    t1++;
    if (t1 > 60) powerOff();
  } else t1 = 0;

  static unsigned long t2 = 0;
  if (digitalRead(BTN2_PIN) == LOW) {
    t2++;
    if (t2 > holdToShowIP) { showIP(); t2=0; }
  } else t2 = 0;

  if (digitalRead(BTN3_PIN) == LOW) {
    if (!isRecording) startVoiceRecord();
    voiceText = "speaking...";
  } else {
    if (isRecording) stopVoiceRecord();
  }

  if (sysState == STANDBY) {
    if (digitalRead(BTN1_PIN) == LOW) { currentBtn=0; sysState=WRITING; delay(300); }
    if (digitalRead(BTN2_PIN) == LOW) { currentBtn=1; sysState=WRITING; delay(300); }
    if (digitalRead(BTN4_PIN) == LOW) { currentBtn=3; sysState=WRITING; delay(300); }
  }
}

// NFC写入
bool writeNfc(int idx) {
  u8g2.clearBuffer();
  u8g2.drawStr(0,20,"Put card...");
  u8g2.sendBuffer();
  uint8_t uid[8], len;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 4000)) return false;

  String uidStr = uid2str(uid, len);
  addLog("NFC Write UID: " + uidStr + " -> Btn " + String(idx+1));

  NdefMessage msg;
  String d = btn[idx].data;
  if (d.startsWith("http")) {
    NdefRecord r = NdefRecord(NDEF_TNF_WELL_KNOWN, NDEF_RTD_URI);
    r.setUri(d.c_str());
    msg.addRecord(r);
  } else {
    NdefRecord r = NdefRecord(NDEF_TNF_WELL_KNOWN, NDEF_RTD_TEXT);
    r.setPayload((uint8_t*)d.c_str(), d.length());
    msg.addRecord(r);
  }
  int sz = msg.getEncodedSize();
  uint8_t *buf = (uint8_t*)malloc(sz);
  msg.encode(buf);
  bool ok = nfc.ntag2xx_WriteNdefMessage(buf, sz);
  free(buf);
  if (ok) { beep(3); writeTime = millis(); }
  return ok;
}

void eraseNfc() {
  uint8_t uid[8], len;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 1000)) {
    nfc.ntag2xx_FormatNDEF();
    addLog("Erase card: " + uid2str(uid, len));
  }
  needErase = false;
  sysState = STANDBY;
  beep(2);
}

void checkNfcRead() {
  uint8_t uid[8], len;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 300)) {
    needErase = true;
  }
}

// ==================== 网页后台（登录 + 配置 + 日志）====================
bool checkLogin() {
  if (!server.hasArg("user") || !server.hasArg("pwd")) return false;
  String u = server.arg("user");
  String p = server.arg("pwd");
  return (u == "Slate" && p == defaultPWD);
}

void handleRoot() {
  if (!checkLogin()) {
    server.send(200, "text/html", R"HTML(
      <h3>Admin Login</h3>
      <form method=post>
      User:<input name=user><br>
      Pwd:<input name=pwd type=password><br>
      <button>Login</button>
      </form>
    )HTML");
    return;
  }

  String html = "<h3>NFC Button Config</h3>";
  html += "<a href='/log'>View Log</a><br><br>";
  html += "<form method=post action=/save>";
  for (int i=0; i<4; i++) {
    html += "Button " + String(i+1) + " Name:<br>";
    html += "<input name='n"+String(i)+"' value='"+btn[i].name+"'><br>";
    html += "Data:<br><textarea name='d"+String(i)+"' rows=3>"+btn[i].data+"</textarea><br><br>";
  }
  html += "<button>Save</button></form>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (!checkLogin()) { server.redirect("/"); return; }
  prefs.begin("cfg", false);
  for (int i=0; i<4; i++) {
    if (server.hasArg("n"+String(i))) btn[i].name = server.arg("n"+String(i));
    if (server.hasArg("d"+String(i))) btn[i].data = server.arg("d"+String(i));
    prefs.putString("n"+String(i), btn[i].name);
    prefs.putString("d"+String(i), btn[i].data);
  }
  prefs.end();
  addLog("Web config saved");
  server.redirect("/");
}

void handleLog() {
  if (!checkLogin()) { server.redirect("/"); return; }
  String html = "<h3>System Log</h3><a href='/'>Back</a><br><br><pre>";
  html += logBuffer;
  html += "</pre>";
  server.send(200, "text/html", html);
}

// ==================== 初始化 ====================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(BTN3_PIN, INPUT_PULLUP);
  pinMode(BTN4_PIN, INPUT_PULLUP);

  u8g2.begin();
  u8g2.setPowerSave(0);
  beep(1);

  nfc.begin();
  nfc.SAMConfig();

  WiFi.begin(defaultSSID.c_str(), defaultPWD.c_str());
  addLog("WiFi connecting...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/log", handleLog);
  server.begin();

  prefs.begin("cfg", true);
  for (int i=0; i<4; i++) {
    btn[i].name = prefs.getString("n"+String(i), btn[i].name);
    btn[i].data = prefs.getString("d"+String(i), btn[i].data);
  }
  prefs.end();

  addLog("Server started");
}

// ==================== 主循环 ====================
void loop() {
  server.handleClient();
  if (!isPowerOn) return;

  checkButtons();

  if (sysState == VOICE_RECORD) {
    showMicScreen();
    delay(80);
    return;
  }

  if (sysState == WRITING) {
    if(writeNfc(currentBtn)) sysState=WRITTEN;
    else sysState=STANDBY;
  } else if (sysState == WRITTEN) {
    checkNfcRead();
    if(millis()-writeTime>30000 || needErase) eraseNfc();
  }

  drawStandby();
  delay(80);
}
