// ---------------------------------------------------------------------------------------
// Math-o-Mat (M&Ms dispenser) — ESP32 + WebSockets, server-side validation, dialog UI
// Based on esp-websocket tutorial by mo thunderz (YT), refactored for secure validation.
// ---------------------------------------------------------------------------------------

#include <WiFi.h>
#include <DNSServer.h>
#include <WiFiManager.h>  // WiFiManager by tzapu (works on ESP32 incl. LOLIN D32)
#include <ESPmDNS.h>       // mDNS for hostname like http://mathomat.local
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "A4988.h"

// ---------------needed for max connection frequency-----

#define MAX_CLIENTS WEBSOCKETS_SERVER_CLIENT_MAX
#define MINTIME2NEXTTRY 30000UL   // 30 seconds guard before a client can submit again

static unsigned long lastSubmit[MAX_CLIENTS] = {0};

// ---------------- Stepper config ----------------
#define MOTOR_STEPS 400
#define MOTOR_ACCEL 2000
#define MOTOR_DECEL 2000
#define MICROSTEPS 16

#define DIR 2
#define STEP 4
#define ENABLE 17
#define MS1 14
#define MS2 16
#define MS3 27
#define PWRHOLD 25                   // Pin to operate optocopler for main PWR hold/off
#define PWROFF 120*1000               // Switch Power off after this time of inactivity (in Milliseconds)


#define STEPS_EINELINSE -681

A4988 stepper(MOTOR_STEPS, DIR, STEP, ENABLE, MS1, MS2, MS3);
int RPM = 150;
long int lastActivity;

// ---------------- WiFi / WS ---------------------
// WiFiManager handles WiFi credentials & connection (no global needed)
WebServer server(80);
WebSocketsServer webSocket(81);

// ---------------- Game state --------------------
struct Problem {
  long sum1, sum2;
  long sub1, sub2;
  long prod1, prod2;
  long div1, div2;
  long addAns, subAns, prodAns, divAns; // correct solutions (server only)
  uint32_t id;
} current;

volatile bool bolNewGame = false;
uint32_t nextProblemId = 1;

// JSON buffers (size up a bit for the structured messages)
StaticJsonDocument<512> doc_tx;
StaticJsonDocument<512> doc_rx;

// ---------------- Web page ----------------------
// Dialog style: chat (left), coder/math pad (right). WebSocket client sends/receives JSON.
const char webpage[] PROGMEM = R"====(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Train your Brain!</title>
<style>
  :root { font-family: system-ui, Segoe UI, Roboto, Arial, sans-serif; }
  body { margin: 0; }
  .page { max-width: 760px; margin: 24px auto; padding: 0 16px; }
  .header h2 { margin:0; }
  .status { font-size:.9em; color:#555; padding:4px 8px; border:1px solid #e6e6e6; background:#fafafa; border-radius:8px; margin-top:6px; display:inline-block; }
  .status.ok  { color:#1a7f37; border-color:#cfe8cf; background:#f2fbf3; }
  .status.err { color:#b42318; border-color:#f5c5c0; background:#fff4f3; }
  table { width: 100%; border-collapse: collapse; margin-top: 12px; }
  th, td { padding: 8px; }
  td.op, td.eq { text-align:center; width: 2rem; }
  input[type=number] { width: 9ch; padding: .35rem .5rem; font-size: 1rem; }
  .row { border-bottom: 1px solid #eee; }
  .green{ color:#1a7f37; font-weight:600; }
  .red  { color:#b42318; font-weight:600; }
  button { padding: .6rem .9rem; margin-right:.5rem; }
  .muted{ color:#666; font-size:.9em; margin-top:8px; }
  [hidden]{ display:none !important; }
</style>
</head><body>
  <div class="page">
    <div class="header">
      <h2>Train your Brain!</h2>
    </div>
    <span id="status" class="status" aria-live="polite">Connecting…</span>

    <table>
      <thead><tr><th>No1</th><th class="op">…</th><th>No2</th><th class="eq">=</th><th>Result</th></tr></thead>
      <tbody>
        <tr class="row">
          <td><strong><span id="sum1">-</span></strong></td><td class="op"><strong>+</strong></td>
          <td><strong><span id="sum2">-</span></strong></td><td class="eq"><strong>=</strong></td>
          <td><input type="number" id="lsgAdd" step="1" min="-9999" max="9999"></td>
        </tr>
        <tr class="row">
          <td><strong><span id="sub1">-</span></strong></td><td class="op"><strong>−</strong></td>
          <td><strong><span id="sub2">-</span></strong></td><td class="eq"><strong>=</strong></td>
          <td><input type="number" id="lsgSub" step="1" min="-9999" max="9999"></td>
        </tr>
        <tr class="row">
          <td><strong><span id="prod1">-</span></strong></td><td class="op"><strong>×</strong></td>
          <td><strong><span id="prod2">-</span></strong></td><td class="eq"><strong>=</strong></td>
          <td><input type="number" id="lsgProd" step="1" min="-9999" max="9999"></td>
        </tr>
        <tr class="row">
          <td><strong><span id="div1">-</span></strong></td><td class="op"><strong>÷</strong></td>
          <td><strong><span id="div2">-</span></strong></td><td class="eq"><strong>=</strong></td>
          <td><input type="number" id="lsgDiv" step="1" min="-9999" max="9999"></td>
        </tr>
      </tbody>
    </table>

    <div style="margin-top:10px;">
      <button id="BTN_CHECK" type="button">Check result</button>
      <button id="BTN_SEND_BACK" type="button" hidden>Continue</button>
    </div>
  </div>

<script>
  // ---- Status helper (small box) ----
  const statusEl = document.getElementById('status');
  function setStatus(text, cls){ statusEl.textContent = text || ''; statusEl.className = 'status' + (cls? (' ' + cls) : ''); }

  // ---- UI refs ----
  const $ = (id)=>document.getElementById(id);
  const spans = { sum1:$('sum1'), sum2:$('sum2'), sub1:$('sub1'), sub2:$('sub2'),
                  prod1:$('prod1'), prod2:$('prod2'), div1:$('div1'), div2:$('div2') };
  const inputs = ['lsgAdd','lsgSub','lsgProd','lsgDiv'].map($);
  function setInputsEnabled(on){ inputs.forEach(i=> on? i.removeAttribute('disabled'): i.setAttribute('disabled','')); }
  function resetInputs(){ inputs.forEach(i=>{ i.value=''; i.classList.remove('green','red'); i.style.color=''; }); }

  // ---- WebSocket protocol ----
  let socket, problemId=null;

  function connectWS(){
    socket = new WebSocket('ws://' + window.location.hostname + ':81/');
    socket.onopen  = ()=> setStatus('Connected. Waiting for a problem…','ok');
    socket.onclose = ()=> setStatus('Disconnected.','');
    socket.onerror = ()=> setStatus('Connection error.','err');

    socket.onmessage = (e)=>{
      try{
        const msg = JSON.parse(e.data);
        if(msg.type==='problem'){
          problemId = msg.problemId;
          spans.sum1.textContent  = msg.sum1;  spans.sum2.textContent  = msg.sum2;
          spans.sub1.textContent  = msg.sub1;  spans.sub2.textContent  = msg.sub2;
          spans.prod1.textContent = msg.prod1; spans.prod2.textContent = msg.prod2;
          spans.div1.textContent  = msg.div1;  spans.div2.textContent  = msg.div2;
          resetInputs(); setInputsEnabled(true);
          $('BTN_CHECK').hidden = false; $('BTN_SEND_BACK').hidden = true;
          setStatus('Solve the tasks and press “Check result”.','ok');
        } else if (msg.type==='verdict'){
          const map=[['lsgAdd',msg.correct?.add],['lsgSub',msg.correct?.sub],['lsgProd',msg.correct?.prod],['lsgDiv',msg.correct?.div]];
          map.forEach(([id,ok])=>{
            const el=$(id); el.classList.remove('green','red');
            if(ok===true){ el.classList.add('green'); el.style.color='green'; }
            if(ok===false){ el.classList.add('red');   el.style.color='red'; }
          });
          setStatus(`You got ${msg.count} correct${msg.count===4?' 🎉':''}`, msg.count===4?'ok':'');
          $('BTN_SEND_BACK').hidden=false;
        } else if (msg.type==='info'){
          setStatus(msg.message || '');
        }
      }catch(_){ /* ignore malformed */ }
    };
  }

  function onCheck(){
    if(!socket || socket.readyState!==WebSocket.OPEN){ setStatus('Not connected.','err'); return; }
    if(!problemId){ setStatus('No active problem.','err'); return; }
    setInputsEnabled(false);
    const payload = {
      type:'answers', problemId,
      add: $('lsgAdd').value, sub: $('lsgSub').value, prod: $('lsgProd').value, div: $('lsgDiv').value
    };
    socket.send(JSON.stringify(payload));
    $('BTN_CHECK').hidden = true;
    setStatus('Checking…');
  }

  function onNext(){
    if(!socket || socket.readyState!==WebSocket.OPEN){ setStatus('Not connected.','err'); return; }
    socket.send(JSON.stringify({type:'next'}));
    $('BTN_SEND_BACK').hidden = true;
    setStatus('Requesting a new problem…');
  }

  window.addEventListener('load', ()=>{
    connectWS();
    $('BTN_CHECK').addEventListener('click', onCheck);
    $('BTN_SEND_BACK').addEventListener('click', onNext);
    setInputsEnabled(false);
  });
</script>
</body></html>
)====";
// ---------------------------------------------------------------------------------------
// ESP32 logic
// ---------------------------------------------------------------------------------------

void setup() {
  lastActivity = millis();
  pinMode(PWRHOLD, OUTPUT);    // sets the digital PWRHOLD as output  
  digitalWrite(PWRHOLD, HIGH); // sets the digital pin 13 on
  Serial.begin(115200);

  stepper.begin(RPM, MICROSTEPS);
  stepper.setEnableActiveState(LOW);
  stepper.setSpeedProfile(stepper.LINEAR_SPEED, MOTOR_ACCEL, MOTOR_DECEL);
  stepper.disable();

  randomSeed(analogRead(0));

  // ---- WiFi (WiFiManager) ----
  WiFi.mode(WIFI_STA);
#ifdef ESP32
  WiFi.setHostname("mathomat");  // DHCP hostname (keep lowercase, no spaces)
#endif
  {
    WiFiManager wm;                // captive portal if no known WiFi
    wm.setConfigPortalTimeout(180); // 3 min portal timeout
    // For a password-protected portal, use: wm.autoConnect("Math-o-Mat-Setup","12345678");
    bool ok = wm.autoConnect("Math-o-Mat-Setup");
    if(!ok){ Serial.println("WiFi connect failed, restarting..."); delay(1000); ESP.restart(); }
  }
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // ---- mDNS (http://mathomat.local) ----
  if (!MDNS.begin("mathomat")) {  // mDNS hostname, same subnet only
    Serial.println("mDNS start failed");
  } else {
    Serial.println("mDNS responder started: http://mathomat.local");
    MDNS.addService("http", "tcp", 80);   // _http._tcp
    MDNS.addService("ws",   "tcp", 81);   // _ws._tcp (optional)
  }

  // ---- HTTP ----
  server.on("/", [](){
    server.send(200, "text/html", webpage);
  });
  server.begin();

  // ---- WebSocket ----
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

void loop() {
  if ((millis()-lastActivity) > PWROFF){
      digitalWrite(PWRHOLD, LOW);                     // Switch Main PWR off
  }
  server.handleClient();
  if (bolNewGame) {
    bolNewGame = false;
    lastActivity = millis();
    sendNewProblemToAll();
  }
  webSocket.loop();
}

// ---------------- Problem generation & sending ----------------
void makeNewProblem() {
  current.id = nextProblemId++;

  long div2 = random(4,9);
  long div1 = random(3,13) * div2;

  current.sum1  = random(-100,100);
  current.sum2  = random(-current.sum1,200);
  current.sub1  = random(11,100);
  current.sub2  = random(-100,current.sub1);
  current.prod1 = random(3,20);
  current.prod2 = random(3,50);
  current.div1  = div1;
  current.div2  = div2;

  current.addAns  = current.sum1  + current.sum2;
  current.subAns  = current.sub1  - current.sub2;
  current.prodAns = current.prod1 * current.prod2;
  current.divAns  = current.div1  / current.div2;
}

void sendProblemTo(uint8_t clientNum) {
  doc_tx.clear();
  doc_tx["type"] = "problem";
  doc_tx["problemId"] = current.id;
  doc_tx["sum1"]  = current.sum1;  doc_tx["sum2"]  = current.sum2;
  doc_tx["sub1"]  = current.sub1;  doc_tx["sub2"]  = current.sub2;
  doc_tx["prod1"] = current.prod1; doc_tx["prod2"] = current.prod2;
  doc_tx["div1"]  = current.div1;  doc_tx["div2"]  = current.div2;
  String out; serializeJson(doc_tx, out);
  webSocket.sendTXT(clientNum, out);
}

void sendNewProblemToAll() {
  makeNewProblem();
  doc_tx.clear();
  doc_tx["type"] = "problem";
  doc_tx["problemId"] = current.id;
  doc_tx["sum1"]  = current.sum1;  doc_tx["sum2"]  = current.sum2;
  doc_tx["sub1"]  = current.sub1;  doc_tx["sub2"]  = current.sub2;
  doc_tx["prod1"] = current.prod1; doc_tx["prod2"] = current.prod2;
  doc_tx["div1"]  = current.div1;  doc_tx["div2"]  = current.div2;
  String out; serializeJson(doc_tx, out);
  webSocket.broadcastTXT(out);
}

// ---------------- Verdict & motor ----------------
void sendVerdictTo(uint8_t clientNum, uint32_t pid, bool okAdd, bool okSub, bool okProd, bool okDiv) {
  int count = (okAdd?1:0) + (okSub?1:0) + (okProd?1:0) + (okDiv?1:0);

  // Drive motor based on server-side count
  TurnWheel(count);

  // Send verdict back (per client)
  doc_tx.clear();
  doc_tx["type"] = "verdict";
  doc_tx["problemId"] = pid;
  JsonObject corr = doc_tx.createNestedObject("correct");
  corr["add"]  = okAdd;
  corr["sub"]  = okSub;
  corr["prod"] = okProd;
  corr["div"]  = okDiv;
  doc_tx["count"] = count;
  String out; serializeJson(doc_tx, out);
  webSocket.sendTXT(clientNum, out);
}

void TurnWheel(int count){
  Serial.println("Correct Answers = " + String(count));
  stepper.enable();
  if (count == 4) {
    stepper.rotate(2 * STEPS_EINELINSE);  // 2 M&Ms
  } else if (count == 3) {
    stepper.rotate(1 * STEPS_EINELINSE);  // 1 M&M
  } else {
    stepper.rotate(-0.5 * STEPS_EINELINSE);
    stepper.rotate( 0.5 * STEPS_EINELINSE);
  }
  stepper.disable();
}

// ---------------- WebSocket events ----------------
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length){
  switch(type){
    case WStype_DISCONNECTED:
      Serial.printf("Client %u disconnected\\n", num);
      break;

    case WStype_CONNECTED:

      Serial.printf("Client %u connected\\n", num);
      makeNewProblem();
      sendProblemTo(num);
      break;

    case WStype_TEXT: {
      DeserializationError err = deserializeJson(doc_rx, payload, length);
      if (err) { Serial.println("JSON parse error"); return; }

      const char* mtype = doc_rx["type"] | "";
      if (strcmp(mtype, "answers") == 0) {
        // Rate limit answer submissions
        if (millis() - lastSubmit[num] < MINTIME2NEXTTRY) {
          Serial.printf("Client %u tried to submit too quickly\\n", num);
          StaticJsonDocument<128> msg;
          msg["type"] = "info";
          msg["message"] = "Please wait before trying again.";
          String out; serializeJson(msg, out);
          webSocket.sendTXT(num, out);
          return;
        }
        lastSubmit[num] = millis();

        uint32_t pid = doc_rx["problemId"] | 0;
        if (pid != current.id) {
          // send info + re-send current problem
          doc_tx.clear(); doc_tx["type"]="info"; doc_tx["message"]="Problem expired. Sending a new one.";
          String out; serializeJson(doc_tx,out); webSocket.sendTXT(num,out);
          sendProblemTo(num);
          return;
        }
        long a = String((const char*)doc_rx["add"]).toInt();
        long s = String((const char*)doc_rx["sub"]).toInt();
        long p = String((const char*)doc_rx["prod"]).toInt();
        long d = String((const char*)doc_rx["div"]).toInt();

        bool okAdd  = (a == current.addAns);
        bool okSub  = (s == current.subAns);
        bool okProd = (p == current.prodAns);
        bool okDiv  = (d == current.divAns);

        sendVerdictTo(num, pid, okAdd, okSub, okProd, okDiv);
      }
      else if (strcmp(mtype, "next") == 0) {
        makeNewProblem();
        lastActivity = millis();
        sendProblemTo(num);
      }
      break;
    }

    default: break;
  }
}
