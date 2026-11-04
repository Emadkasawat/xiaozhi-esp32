// Arabic setup page served by the device itself (no app to install).
//
// The device remembers up to 5 Wi-Fi networks and joins the strongest one it
// can see. When none is around (or on request) it opens the "Gemini-Setup"
// hotspot with this page. Changing Wi-Fi never touches the other settings.
//
// Included once from main.cpp, after the settings, display and helpers.
#pragma once

#include <DNSServer.h>
#include <WebServer.h>
#include <esp_wifi.h>

struct SavedNet {
  char ssid[33];
  char pass[65];
};
static const int MAX_NETS = 5;
SavedNet nets[MAX_NETS];

void saveNets() {
  prefs.begin("gemini", false);
  prefs.putBytes("nets", nets, sizeof(nets));
  prefs.end();
}

int netCount() {
  int n = 0;
  for (const SavedNet &s : nets)
    if (s.ssid[0]) n++;
  return n;
}

// Call after WiFi.mode(): also carries over the network the old setup page saved.
void loadNets() {
  memset(nets, 0, sizeof(nets));
  prefs.begin("gemini", true);
  if (prefs.getBytesLength("nets") == sizeof(nets)) prefs.getBytes("nets", nets, sizeof(nets));
  prefs.end();
  if (netCount() == 0) {
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0]) {
      strlcpy(nets[0].ssid, (const char *)cfg.sta.ssid, sizeof(nets[0].ssid));
      strlcpy(nets[0].pass, (const char *)cfg.sta.password, sizeof(nets[0].pass));
      saveNets();
    }
  }
}

// Adds or updates a network and puts it first.
void addNet(const String &ssid, const String &pass) {
  SavedNet fresh = {};
  strlcpy(fresh.ssid, ssid.c_str(), sizeof(fresh.ssid));
  strlcpy(fresh.pass, pass.c_str(), sizeof(fresh.pass));
  SavedNet out[MAX_NETS] = {};
  out[0] = fresh;
  int k = 1;
  for (const SavedNet &s : nets) {
    if (!s.ssid[0] || ssid == s.ssid || k >= MAX_NETS) continue;
    out[k++] = s;
  }
  memcpy(nets, out, sizeof(nets));
  saveNets();
}

void removeNet(const String &ssid) {
  SavedNet out[MAX_NETS] = {};
  int k = 0;
  for (const SavedNet &s : nets) {
    if (s.ssid[0] && ssid != s.ssid) out[k++] = s;
  }
  memcpy(nets, out, sizeof(nets));
  saveNets();
}

// Joins the strongest saved network in range. Returns true when connected.
bool connectKnown() {
  if (netCount() == 0) return false;
  WiFi.mode(WIFI_STA);
  setState(State::Wifi, "Looking for saved Wi-Fi");
  drawScreen();
  int found = WiFi.scanNetworks();
  struct Cand {
    int idx;
    int rssi;
  } cand[MAX_NETS];
  int cn = 0;
  for (int i = 0; i < MAX_NETS; i++) {
    if (!nets[i].ssid[0]) continue;
    int best = -1000;
    for (int j = 0; j < found; j++)
      if (WiFi.SSID(j) == nets[i].ssid) best = max(best, (int)WiFi.RSSI(j));
    // A failed scan (found <= 0) still tries every saved network, e.g. hidden ones.
    if (best > -1000 || found <= 0) cand[cn++] = {i, best};
  }
  WiFi.scanDelete();
  for (int a = 0; a < cn; a++)
    for (int b = a + 1; b < cn; b++)
      if (cand[b].rssi > cand[a].rssi) std::swap(cand[a], cand[b]);
  for (int c = 0; c < cn; c++) {
    const SavedNet &s = nets[cand[c].idx];
    setState(State::Wifi, String("Connecting to ") + s.ssid);
    drawScreen();
    WiFi.begin(s.ssid, s.pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect();
    delay(200);
  }
  return false;
}

// ---------------------------------------------------------------- the page
String esc(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

static const char PAGE_CSS[] PROGMEM = R"CSS(
*{box-sizing:border-box}
body{margin:0;background:#f4f6f5;color:#17201e;font-family:system-ui,-apple-system,"Segoe UI",Tahoma,sans-serif}
main{max-width:460px;margin:0 auto;padding:24px 18px 40px;display:flex;flex-direction:column;gap:16px}
h1{margin:0;font-size:26px}h2{margin:8px 0 0;font-size:17px}
p{margin:0}.sub{color:#4a5753;font-size:15px;line-height:1.6}
.card{background:#fff;border:1px solid #dfe5e3;border-radius:16px}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 16px;border-bottom:1px solid #edf1ef}
.row:last-child{border-bottom:0}
.name{font-size:16px;font-weight:600;word-break:break-all}.small{font-size:13px;color:#4a5753}.ok{color:#0b6e6e}
.btn{display:flex;align-items:center;justify-content:center;min-height:56px;border-radius:14px;border:0;background:#0b6e6e;color:#fff;font-size:18px;font-weight:700;text-decoration:none;width:100%;font-family:inherit;cursor:pointer}
.btn2{display:flex;align-items:center;justify-content:center;min-height:48px;border-radius:12px;border:1px solid #c9d3d0;background:#fff;color:#17201e;font-size:15px;text-decoration:none;width:100%;font-family:inherit;cursor:pointer}
.icon{width:44px;height:44px;border:0;background:transparent;display:flex;align-items:center;justify-content:center;cursor:pointer}
label{display:flex;flex-direction:column;gap:6px;font-size:14px;color:#4a5753}
label.inline{flex-direction:row;align-items:center;gap:8px;min-height:44px}
input,select{height:52px;border-radius:12px;border:1.5px solid #c9d3d0;padding:0 14px;font-size:17px;font-family:inherit;background:#fff;color:#17201e;width:100%}
input[type=checkbox]{width:22px;height:22px}
input:focus,select:focus{outline:2px solid #0b6e6e;border-color:#0b6e6e}
details.card{padding:0}details>summary{padding:16px;font-size:16px;cursor:pointer;list-style-position:inside}
details[open]>summary{border-bottom:1px solid #edf1ef}
.form{display:flex;flex-direction:column;gap:12px;padding:14px 16px}
.notice{background:#e6f2f1;color:#084f4f;border-radius:12px;padding:12px 14px;font-size:15px}
.bad{background:#fdecea;color:#8a1c12}
.back{display:flex;align-items:center;min-height:44px;color:#0b6e6e;text-decoration:none;font-size:15px}
.done{width:96px;height:96px;border-radius:48px;background:#0b6e6e;display:flex;align-items:center;justify-content:center;margin:40px auto 0}
.center{text-align:center}
)CSS";

String page(const String &title, const String &body, const char *extraHead = "") {
  String html;
  html.reserve(body.length() + 3000);
  html += "<!doctype html><html lang=\"ar\" dir=\"rtl\"><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>";
  html += title;
  html += "</title>";
  html += extraHead;
  html += "<style>";
  html += FPSTR(PAGE_CSS);
  html += "</style></head><body><main>";
  html += body;
  html += "</main></body></html>";
  return html;
}

static const char TRASH_ICON[] PROGMEM =
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#6b7672\" stroke-width=\"2\" "
    "stroke-linecap=\"round\" aria-hidden=\"true\"><path d=\"M4 7h16\"/><path d=\"M9 7V4h6v3\"/>"
    "<path d=\"M6 7l1 13h10l1-13\"/></svg>";

String signalWord(int rssi) {
  if (rssi > -60) return "إشارة قوية";
  if (rssi > -72) return "متوسطة";
  return "ضعيفة";
}

String savedList(const String &connectedSsid) {
  String b = "<div class=\"card\">";
  if (netCount() == 0) {
    b += "<div class=\"row\"><span class=\"small\">لسه مفيش شبكات محفوظة</span></div>";
  }
  for (const SavedNet &s : nets) {
    if (!s.ssid[0]) continue;
    bool on = connectedSsid == s.ssid;
    b += "<div class=\"row\"><div><div class=\"name\">" + esc(s.ssid) + "</div><div class=\"small" +
         String(on ? " ok\">متصل دلوقتي" : "\">هيتصل بيها لوحده لما تكون قريبة") + "</div></div>";
    b += "<form method=\"post\" action=\"/del\"><input type=\"hidden\" name=\"ssid\" value=\"" + esc(s.ssid) +
         "\"><button class=\"icon\" aria-label=\"امسح شبكة " + esc(s.ssid) + "\">" + FPSTR(TRASH_ICON) +
         "</button></form></div>";
  }
  b += "</div>";
  return b;
}

struct ScanItem {
  String ssid;
  int rssi;
};

// ---------------------------------------------------------------- the portal
// Returns when the user finishes, a new network connects, or after 10 idle
// minutes (when not forced) so the device can look for its networks again.
void runPortal(bool forced, const char *screenHint) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Gemini-Setup");
  delay(200);
  IPAddress ip = WiFi.softAPIP();
  DNSServer dns;
  dns.start(53, "*", ip);
  WebServer server(80);

  std::vector<ScanItem> scan;
  auto doScan = [&]() {
    scan.clear();
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.isEmpty()) continue;
      bool dup = false;
      for (ScanItem &it : scan)
        if (it.ssid == s) {
          it.rssi = max(it.rssi, (int)WiFi.RSSI(i));
          dup = true;
        }
      if (!dup) scan.push_back({s, (int)WiFi.RSSI(i)});
    }
    WiFi.scanDelete();
    std::sort(scan.begin(), scan.end(), [](const ScanItem &a, const ScanItem &b) { return a.rssi > b.rssi; });
    if (scan.size() > 15) scan.resize(15);
  };
  doScan();

  bool done = false;
  int joinState = 0;  // 0 idle, 1 trying, 2 connected, 3 failed
  String joinSsid;
  uint32_t joinStart = 0, joinedAt = 0, lastActivity = millis();

  setState(State::Setup, screenHint);
  drawScreen();

  server.on("/", HTTP_GET, [&]() {
    lastActivity = millis();
    String msg = server.arg("msg");
    String b = "<h1>إعداد الجهاز</h1><p class=\"sub\">غيّر الواي فاي بس، وباقي الإعدادات بتفضل زي ما هي.</p>";
    if (msg == "saved") b += "<div class=\"notice\">اتحفظت الإعدادات.</div>";
    if (msg == "deleted") b += "<div class=\"notice\">اتمسحت الشبكة.</div>";
    if (settings.apiKey.isEmpty())
      b += "<div class=\"notice bad\">لسه مفيش مفتاح Gemini. حطه تحت في الإعدادات المتقدمة.</div>";
    String cur = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
    b += "<div class=\"card row\"><div><div class=\"small\">" +
         String(cur.length() ? "متصل دلوقتي بـ" : "الحالة") + "</div><div class=\"name\">" +
         (cur.length() ? esc(cur) : String("مش متصل بأي شبكة")) + "</div></div></div>";
    b += "<h2>الشبكات المحفوظة</h2>" + savedList(cur);
    b += "<a class=\"btn\" href=\"/add\">+ إضافة شبكة جديدة</a>";

    b += "<details class=\"card\"><summary>إعدادات متقدمة (محفوظة)</summary>"
         "<form class=\"form\" method=\"post\" action=\"/adv\">";
    b += "<label>مفتاح Gemini<input type=\"password\" name=\"key\" autocomplete=\"off\" placeholder=\"" +
         String(settings.apiKey.length() ? "محفوظ، سيبه فاضي عشان يفضل زي ما هو" : "الصق المفتاح هنا") +
         "\"></label>";
    b += "<label>رابط شيت المحل<input type=\"url\" name=\"sheeturl\" dir=\"ltr\" placeholder=\"" +
         String(settings.sheetUrl.length() ? "محفوظ، سيبه فاضي عشان يفضل" : "https://script.google.com/...") +
         "\"></label>";
    b += "<label>باسورد الشيت<input type=\"password\" name=\"sheetpw\" autocomplete=\"off\" placeholder=\"" +
         String(settings.sheetPassword.length() ? "محفوظ، سيبه فاضي عشان يفضل" : "") + "\"></label>";
    b += "<label>علو الصوت (1 لـ 100)<input type=\"number\" name=\"volume\" min=\"1\" max=\"100\" value=\"" +
         String(settings.volume) + "\"></label>";
    b += "<label>طريقة الكلام<select name=\"handsfree\"><option value=\"0\"" +
         String(settings.handsFree ? "" : " selected") + ">بالزرار (اضغط واتكلم)</option><option value=\"1\"" +
         String(settings.handsFree ? " selected" : "") + ">من غير زرار (بيسمع على طول)</option></select></label>";
    b += "<label>حساسية المايك من غير زرار (أقل = أحسّ)<input type=\"number\" name=\"micthr\" min=\"50\" "
         "max=\"5000\" value=\"" + String(settings.micThreshold) + "\"></label>";
    b += "<label class=\"inline\"><input type=\"checkbox\" name=\"flip\" value=\"1\"" +
         String(settings.flip ? " checked" : "") + ">اقلب الشاشة</label>";
    b += "<label>الموديل<input type=\"text\" name=\"model\" dir=\"ltr\" value=\"" + esc(settings.model) + "\"></label>";
    b += "<label class=\"inline\"><input type=\"checkbox\" name=\"clearsheet\" value=\"1\">افصل الشيت</label>";
    b += "<button class=\"btn\">حفظ الإعدادات</button></form></details>";

    if (netCount() && settings.apiKey.length())
      b += "<a class=\"btn2\" href=\"/exit\">خلصت، شغّل الجهاز</a>";
    server.send(200, "text/html; charset=utf-8", page("إعداد الجهاز", b));
  });

  server.on("/add", HTTP_GET, [&]() {
    lastActivity = millis();
    if (server.hasArg("rescan")) doScan();
    String b = "<a class=\"back\" href=\"/\">&rarr; رجوع</a><h1>اختار الشبكة</h1><div class=\"card\">";
    if (scan.empty()) b += "<div class=\"row\"><span class=\"small\">مفيش شبكات ظاهرة</span></div>";
    int i = 0;
    for (const ScanItem &it : scan) {
      String id = "p" + String(i++);
      b += "<details><summary class=\"row\"><span class=\"name\">" + esc(it.ssid) + "</span><span class=\"small\">" +
           signalWord(it.rssi) + "</span></summary><form class=\"form\" method=\"post\" action=\"/save\">"
           "<input type=\"hidden\" name=\"ssid\" value=\"" + esc(it.ssid) + "\">"
           "<label for=\"" + id + "\">باسورد الشبكة</label><input id=\"" + id + "\" type=\"password\" name=\"pass\">"
           "<label class=\"inline\"><input type=\"checkbox\" onclick=\"var p=document.getElementById('" + id +
           "');p.type=this.checked?'text':'password'\">إظهار الباسورد</label>"
           "<button class=\"btn\">حفظ واتصال</button></form></details>";
    }
    b += "</div>";
    b += "<details class=\"card\"><summary>شبكتك مش ظاهرة؟ اكتب اسمها</summary><form class=\"form\" method=\"post\" "
         "action=\"/save\"><label>اسم الشبكة<input type=\"text\" name=\"ssid\" required></label>"
         "<label>باسورد الشبكة<input type=\"password\" name=\"pass\"></label>"
         "<button class=\"btn\">حفظ واتصال</button></form></details>";
    b += "<a class=\"btn2\" href=\"/add?rescan=1\">دوّر تاني على الشبكات</a>"
         "<p class=\"small\">الجهاز بيشتغل على شبكات 2.4 جيجا بس. لو شبكتك مش ظاهرة، قرّب الجهاز من الراوتر.</p>";
    server.send(200, "text/html; charset=utf-8", page("إضافة شبكة", b));
  });

  server.on("/save", HTTP_POST, [&]() {
    lastActivity = millis();
    String ssid = server.arg("ssid");
    ssid.trim();
    if (ssid.isEmpty()) {
      server.sendHeader("Location", "/add");
      server.send(302);
      return;
    }
    addNet(ssid, server.arg("pass"));
    joinSsid = ssid;
    joinState = 1;
    joinStart = millis();
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), server.arg("pass").c_str());
    setState(State::Setup, "Connecting to " + ssid);
    server.sendHeader("Location", "/done");
    server.send(302);
  });

  server.on("/done", HTTP_GET, [&]() {
    lastActivity = millis();
    String b;
    if (joinState == 1) {
      b = "<div class=\"done\" style=\"background:#c9d3d0\"></div><h1 class=\"center\">جاري الاتصال بـ " +
          esc(joinSsid) + "...</h1><p class=\"sub center\">ثواني. لو الصفحة قفلت لوحدها، بص على شاشة الجهاز.</p>";
      server.send(200, "text/html; charset=utf-8",
                  page("جاري الاتصال", b, "<meta http-equiv=\"refresh\" content=\"2\">"));
      return;
    }
    if (joinState == 2) {
      b = "<div class=\"done\"><svg width=\"48\" height=\"48\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#fff\" "
          "stroke-width=\"2.5\" stroke-linecap=\"round\" role=\"img\" aria-label=\"تم\"><path d=\"M5 12.5l4.5 4.5L19 "
          "7.5\"/></svg></div><h1 class=\"center\">اتصل بشبكة " + esc(joinSsid) +
          "</h1><p class=\"sub center\">الشبكة اتحفظت. المفتاح والشيت والمنبهات زي ما هما، ومش محتاج تدخلهم تاني.</p>"
          "<h2>الجهاز فاكر الشبكات دي:</h2>" + savedList(joinSsid) +
          "<p class=\"sub center\">الجهاز هيكمّل شغله خلال ثواني. ارجع لشبكة موبايلك العادية.</p>";
      server.send(200, "text/html; charset=utf-8", page("تم الاتصال", b));
      return;
    }
    if (joinState == 3) {
      b = "<h1>مقدرش يتصل بـ " + esc(joinSsid) + "</h1><div class=\"notice bad\">غالبًا الباسورد غلط، أو الشبكة "
          "بعيدة، أو شبكة 5 جيجا. الشبكة اتحفظت، وتقدر تمسحها من الصفحة الرئيسية.</div>"
          "<a class=\"btn\" href=\"/add\">جرّب تاني</a><a class=\"btn2\" href=\"/\">الصفحة الرئيسية</a>";
      server.send(200, "text/html; charset=utf-8", page("مقدرش يتصل", b));
      return;
    }
    server.sendHeader("Location", "/");
    server.send(302);
  });

  server.on("/del", HTTP_POST, [&]() {
    lastActivity = millis();
    removeNet(server.arg("ssid"));
    server.sendHeader("Location", "/?msg=deleted");
    server.send(302);
  });

  server.on("/adv", HTTP_POST, [&]() {
    lastActivity = millis();
    String key = server.arg("key");
    key.trim();
    if (key.length()) settings.apiKey = key;
    String model = server.arg("model");
    model.trim();
    if (model.length()) settings.model = model.startsWith("models/") ? model : "models/" + model;
    int vol = server.arg("volume").toInt();
    if (vol > 0) settings.volume = constrain(vol, 1, 100);
    settings.handsFree = server.arg("handsfree") == "1";
    int thr = server.arg("micthr").toInt();
    if (thr > 0) settings.micThreshold = thr;
    settings.flip = server.arg("flip") == "1";
    String url = server.arg("sheeturl");
    url.trim();
    if (url.startsWith("https://")) settings.sheetUrl = url;
    String pw = server.arg("sheetpw");
    pw.trim();
    if (pw.length()) settings.sheetPassword = pw;
    if (server.arg("clearsheet") == "1") {
      settings.sheetUrl = "";
      settings.sheetPassword = "";
    }
    saveSettings();
    oled.setDisplayRotation(settings.flip ? U8G2_R2 : U8G2_R0);
    screenDirty = true;
    server.sendHeader("Location", "/?msg=saved");
    server.send(302);
  });

  server.on("/exit", HTTP_GET, [&]() {
    String b = "<h1 class=\"center\" style=\"margin-top:60px\">تمام، الجهاز بيشتغل</h1>"
               "<p class=\"sub center\">ارجع لشبكة موبايلك العادية.</p>";
    server.send(200, "text/html; charset=utf-8", page("تمام", b));
    done = true;
  });

  // Phones probe random URLs to detect a sign-in page: send them all to the setup page.
  server.onNotFound([&]() {
    server.sendHeader("Location", String("http://") + ip.toString() + "/");
    server.send(302);
  });

  server.begin();
  uint32_t doneAt = 0;
  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    if (joinState == 1) {
      if (WiFi.status() == WL_CONNECTED) {
        joinState = 2;
        joinedAt = millis();
        setState(State::Setup, "Connected to " + joinSsid);
      } else if (millis() - joinStart > 20000) {
        joinState = 3;
        WiFi.disconnect();
        setState(State::Setup, "Could not join " + joinSsid + ". Check the password on the setup page.");
      }
    }
    // Leave a little time for the phone to show the success page.
    if (joinState == 2 && millis() - joinedAt > 10000 && settings.apiKey.length()) done = true;
    if (done) {
      if (!doneAt) doneAt = millis();
      if (millis() - doneAt > 1500) break;
    }
    if (!forced && millis() - lastActivity > 10 * 60 * 1000UL) break;
    drawScreen();
    delay(2);
  }
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}
