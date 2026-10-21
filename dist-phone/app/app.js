/* AetherOS Phone App — 前端逻辑
 * 1) 终端：EventSource 接收 SSE 日志；"运行"触发 POST /api/install
 * 2) 屏幕：最小 RFB 3.8 客户端，经 ws://host/ws/vnc 桥接 QEMU VNC
 *    支持触摸/鼠标点按（解锁、启动应用）与键盘事件，自动适配 720x1600。
 */
(function () {
  "use strict";

  // ----------------------------- 终端日志 -----------------------------
  var termBody = document.getElementById("term-body");
  var statusEl = document.getElementById("status");
  var runBtn = document.getElementById("run");
  var cmdInput = document.getElementById("cmd");

  function appendLine(text, cls) {
    var span = document.createElement("div");
    if (cls) span.className = cls;
    span.textContent = text;
    termBody.appendChild(span);
    var term = document.getElementById("term");
    term.scrollTop = term.scrollHeight;
  }

  var es = null;
  function startLogs() {
    if (es) return;
    es = new EventSource("/api/logs");
    es.onmessage = function (e) {
      var t = e.data || "";
      var cls = "";
      if (/不可用|失败|异常|error|fail/i.test(t)) cls = "err";
      else if (/\[aether\]/i.test(t)) cls = "sys";
      appendLine(t, cls);
    };
    es.onerror = function () { /* 自动重连由浏览器处理 */ };
  }

  function setStatus(text, kind) {
    statusEl.textContent = text;
    statusEl.className = "pill" + (kind ? " " + kind : "");
  }

  runBtn.addEventListener("click", function () {
    runBtn.disabled = true;
    var cmd = (cmdInput.value || "").trim();
    appendLine("aether@phone:~$ " + cmd, "sys");
    setStatus("安装中…", "busy");
    fetch("/api/install", { method: "POST" })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (!j.started) appendLine("[app] 已在安装/运行中，无需重复。", "sys");
      })
      .catch(function (e) { appendLine("[app] 请求失败: " + e, "err"); });
  });
  cmdInput.addEventListener("keydown", function (e) {
    if (e.key === "Enter") runBtn.click();
  });

  // ----------------------------- 视图切换 -----------------------------
  var viewTerm = document.getElementById("view-term");
  var viewScreen = document.getElementById("view-screen");
  var btnToggle = document.getElementById("btn-toggle");
  var btnBack = document.getElementById("btn-back");
  var btnReconnect = document.getElementById("btn-reconnect");

  function showScreen() {
    viewTerm.hidden = true; viewScreen.hidden = false;
    btnToggle.textContent = "终端";
    connectVNC();
  }
  function showTerm() {
    viewScreen.hidden = true; viewTerm.hidden = false;
    btnToggle.textContent = "屏幕";
  }
  btnToggle.addEventListener("click", function () {
    if (viewScreen.hidden) showScreen(); else showTerm();
  });
  btnBack.addEventListener("click", showTerm);
  btnReconnect.addEventListener("click", connectVNC);

  // ----------------------------- 状态轮询 -----------------------------
  var lastBooted = false;
  function pollStatus() {
    fetch("/api/status").then(function (r) { return r.json(); }).then(function (s) {
      if (s.installing) setStatus("安装中…", "busy");
      else if (s.booted) setStatus("系统就绪", "ok");
      else if (s.installed) setStatus("已安装·未启动", "");
      else setStatus("未安装", "");
      if (s.booted && !lastBooted && viewTerm.hidden === false) {
        showScreen(); // 就绪自动切到屏幕
      }
      if (s.booted) lastBooted = true;
    }).catch(function () {});
  }
  setInterval(pollStatus, 1500);

  // ----------------------------- RFB 客户端 -----------------------------
  var canvas = document.getElementById("rfb");
  var ctx = canvas.getContext("2d");
  var screenMsg = document.getElementById("screen-msg");
  var ws = null;
  var fbW = 720, fbH = 1600;
  var imgData = null;
  var buf = new Uint8Array(0);
  var rfstate = "version";
  var wsOpen = false;

  function setMsg(t) { screenMsg.textContent = t; screenMsg.style.display = t ? "block" : "none"; }

  function connectVNC() {
    if (ws && (ws.readyState === 0 || ws.readyState === 1)) { try { ws.close(); } catch (e) {} }
    var proto = location.protocol === "https:" ? "wss" : "ws";
    ws = new WebSocket(proto + "//" + location.host + "/ws/vnc");
    ws.binaryType = "arraybuffer";
    setMsg("正在连接系统屏幕…");
    ws.onopen = function () {
      wsOpen = true;
      rfstate = "version";
      buf = new Uint8Array(0);
    };
    ws.onclose = function () {
      wsOpen = false;
      setMsg("屏幕连接断开，点“重新连接”。");
    };
    ws.onerror = function () { setMsg("屏幕连接出错，点“重新连接”。"); };
    ws.onmessage = function (ev) {
      var data = new Uint8Array(ev.data);
      var merged = new Uint8Array(buf.length + data.length);
      merged.set(buf); merged.set(data, buf.length);
      buf = merged;
      parseRFB();
    };
  }

  function send(bytes) {
    if (ws && wsOpen) ws.send(bytes);
  }
  function u32(v) { var b = new Uint8Array(4); b[0] = v >> 24 & 255; b[1] = v >> 16 & 255; b[2] = v >> 8 & 255; b[3] = v & 255; return b; }
  function u16(v) { return new Uint8Array([v >> 8 & 255, v & 255]); }

  function sendSetPixelFormat() {
    // 32bpp, LE, truecolor, R<<16 G<<8 B<<0
    var pf = new Uint8Array([
      32, 24, 0, 1,
      255, 0, 255, 0, 255, 0,
      16, 8, 0, 0, 0, 0
    ]);
    var m = new Uint8Array(20);
    m[0] = 0; // padding high
    m[1] = 0; m[2] = 0;
    m.set(pf, 4);
    send(m);
  }
  function sendSetEncodings() {
    // 只请求 raw(0)：QEMU 对定尺寸竖屏只会回 raw 矩形，
    // 不请求 -223/-224 可避免扩展桌面头(附带 screens 数据)导致解析错位。
    var list = new Uint8Array([0, 0, 0, 0]); // encoding 0 = raw
    var m = new Uint8Array(4 + list.length);
    m[0] = 2; m[1] = 0;
    m[2] = 0; m[3] = 1; // 1 encoding
    m.set(list, 4);
    send(m);
  }
  function sendFBReq(incremental) {
    var m = new Uint8Array(10);
    m[0] = 3; m[1] = incremental ? 1 : 0;
    m.set(u16(0), 2); m.set(u16(0), 4);
    m.set(u16(fbW), 6); m.set(u16(fbH), 8);
    send(m);
  }

  function have(n) { return buf.length >= n; }
  function take(n) { var r = buf.subarray(0, n); buf = buf.subarray(n); return r; }
  // 从 buf 读一个 16/32 位大端整数并消费（一次性取够，避免跨字段错位）
  function take16() { var t = take(2); return (t[0] << 8) | t[1]; }
  function take32() { var t = take(4); return ((t[0] << 24) | (t[1] << 16) | (t[2] << 8) | t[3]); }

  function parseRFB() {
    try {
      if (rfstate === "version") {
        var idx = buf.indexOf(0x0a);
        if (idx < 0) return;
        var ver = take(idx + 1);
        var vs = String.fromCharCode.apply(null, ver);
        // 回显相同版本
        send(new Uint8Array(Array.prototype.map.call(vs, function (c) { return c.charCodeAt(0); })));
        rfstate = "sec";
        return parseRFB();
      }
      if (rfstate === "sec") {
        if (!have(1)) return;
        var n = take(1)[0];
        if (n === 0) { // 错误
          if (!have(4)) return;
          var rl = take32();
          // 简化：读原因
          if (!have(rl)) return;
          var reason = String.fromCharCode.apply(null, take(rl));
          setMsg("VNC 错误: " + reason);
          return;
        }
        if (!have(n)) return; // 需要 n 个安全类型字节（计数已消费）
        var types = take(n);
        var pick = 1; // None
        send(new Uint8Array([pick]));
        rfstate = "secresult";
        return parseRFB();
      }
      if (rfstate === "secresult") {
        if (!have(4)) return;
        var code = take32();
        if (code !== 0) { setMsg("VNC 认证失败"); return; }
        rfstate = "clientinit";
        send(new Uint8Array([1])); // shared
        rfstate = "serverinit";
        return parseRFB();
      }
      if (rfstate === "serverinit") {
        if (!have(24)) return;
        // ServerInit: width(2) height(2) PIXEL_FORMAT(16) name-length(4) name
        var w = take16();
        var h = take16();
        take(16); // pixel format
        var nl = take32();
        if (!have(nl)) return;
        take(nl); // name
        fbW = w; fbH = h;
        canvas.width = w; canvas.height = h;
        imgData = ctx.createImageData(w, h);
        setMsg("");
        rfstate = "ready";
        sendSetPixelFormat();
        sendSetEncodings();
        sendFBReq(0); // 首帧全量
        return parseRFB();
      }
      if (rfstate === "ready") {
        while (have(1)) {
          var type = buf[0];
          if (type === 0) { // FramebufferUpdate
            if (!have(4)) return;
            var nrects = (buf[2] << 8) | buf[3];
            // 预扫描：算出整条更新需要的字节数 p（不消费），数据不全则等
            // 矩形头布局：x[0:2] y[2:4] w[4:6] h[6:8] encoding[8:12]
            var p = 4, ok = true;
            for (var i = 0; i < nrects; i++) {
              if (buf.length - p < 12) { ok = false; break; }
              var rw = (buf[p + 4] << 8) | buf[p + 5];
              var rh = (buf[p + 6] << 8) | buf[p + 7];
              var enc = (buf[p + 8] << 24) | (buf[p + 9] << 16) | (buf[p + 10] << 8) | buf[p + 11];
              p += 12;
              if (enc === 0) { p += rw * rh * 4; }
              else { ok = false; break; } // 只处理 raw
            }
            if (!ok || buf.length < p) return; // 等更多数据
            // 数据齐了再消费：先吃 4 字节头(type+pad+nrects)，再逐矩形读
            take(4);
            for (var r = 0; r < nrects; r++) {
              var x = (buf[0] << 8) | buf[1];
              var y = (buf[2] << 8) | buf[3];
              var ww = (buf[4] << 8) | buf[5];
              var hh = (buf[6] << 8) | buf[7];
              take(12); // x,y,w,h,encoding（只请求了 raw，encoding 必为 0）
              if (ww === 0 || hh === 0) continue;
              var pix = take(ww * hh * 4);
              drawRaw(x, y, ww, hh, pix);
            }
            ctx.putImageData(imgData, 0, 0); // 全量刷新（本地，足够快）
            sendFBReq(1); // 增量请求下一帧
          } else if (type === 2) { // Bell
            take(1);
          } else if (type === 3) { // ServerCutText
            if (!have(8)) return;
            take(4); var tl = take32();
            if (!have(tl)) return; take(tl);
          } else {
            setMsg("不支持的 VNC 消息类型 " + type);
            return;
          }
        }
        return;
      }
    } catch (e) {
      setMsg("RFB 解析异常: " + e.message);
    }
  }

  function drawRaw(x, y, w, h, pix) {
    if (!imgData) return;
    var data = imgData.data;
    var fw = fbW;
    for (var row = 0; row < h; row++) {
      var di = ((y + row) * fw + x) * 4;
      var si = row * w * 4;
      for (var c = 0; c < w; c++) {
        var b0 = pix[si], b1 = pix[si + 1], b2 = pix[si + 2];
        // 32位 LE: v = b0|b1<<8|b2<<16 ; R=(v>>16)&255 即 b2, G=b1, B=b0
        data[di] = b2; data[di + 1] = b1; data[di + 2] = b0; data[di + 3] = 255;
        di += 4; si += 4;
      }
    }
  }

  // ----------------------------- 输入：触摸/鼠标 -----------------------------
  function sendPointer(mask, clientX, clientY) {
    var rect = canvas.getBoundingClientRect();
    if (rect.width === 0 || rect.height === 0) return;
    var x = Math.round((clientX - rect.left) / rect.width * fbW);
    var y = Math.round((clientY - rect.top) / rect.height * fbH);
    x = Math.max(0, Math.min(fbW - 1, x));
    y = Math.max(0, Math.min(fbH - 1, y));
    var m = new Uint8Array(6);
    m[0] = 5; m[1] = mask;
    m.set(u16(x), 2); m.set(u16(y), 4);
    send(m);
  }
  function sendKey(down, key) {
    var m = new Uint8Array(8);
    m[0] = 4; m[1] = down ? 1 : 0;
    m.set(u32(key), 4);
    send(m);
  }

  var pointerDown = false;
  canvas.addEventListener("pointerdown", function (e) {
    pointerDown = true;
    sendPointer(1, e.clientX, e.clientY);
  });
  canvas.addEventListener("pointermove", function (e) {
    if (pointerDown) sendPointer(1, e.clientX, e.clientY);
  });
  canvas.addEventListener("pointerup", function (e) {
    pointerDown = false;
    sendPointer(0, e.clientX, e.clientY);
  });
  canvas.addEventListener("pointercancel", function () { pointerDown = false; sendPointer(0, 0, 0); });
  canvas.addEventListener("contextmenu", function (e) { e.preventDefault(); });

  // 物理键盘（若有）
  window.addEventListener("keydown", function (e) {
    // 简易映射：字母/数字/空格/回车
    var k = e.keyCode;
    var key = 0;
    if (k >= 48 && k <= 90) key = k; // ASCII 直接
    else if (k === 32) key = 32;
    else if (k === 13) key = 13;
    else if (k === 8) key = 8;
    if (key) { sendKey(true, key); e.preventDefault(); }
  });
  window.addEventListener("keyup", function (e) {
    var k = e.keyCode; var key = 0;
    if (k >= 48 && k <= 90) key = k; else if (k === 32) key = 32; else if (k === 13) key = 13; else if (k === 8) key = 8;
    if (key) sendKey(false, key);
  });

  // ----------------------------- 启动 -----------------------------
  startLogs();
  pollStatus();
  // 若已就绪直接进屏幕
  setInterval(function () {
    if (lastBooted && viewTerm.hidden === false) showScreen();
  }, 2000);
})();
