/* AetherOS 终端 App — 前端逻辑（自包含架构，无 Termux 依赖）· phone-v18
 * 1) 终端：EventSource 接收 SSE 日志；命令行支持
 *      install / run / boot / stop / reboot / status / reset / update / help / clear / export
 * 2) 屏幕：最小 RFB 3.8 客户端，经 ws://host/ws/vnc 桥接 QEMU VNC
 *    支持触摸/鼠标点按（解锁、启动应用）与键盘事件，自动适配 720x1600。
 *
 * phone-v18 关键修复（治「一直重连」）：
 *  - 流式解析：QEMU 把首帧打包成一条 4.6MB 整屏 raw 消息，旧实现要整个
 *    缓冲齐才画，渲染进程内存被顶爆、且推帧期间 App 的端口探测超时把
 *    booted 误判成掉线并主动 closeVNC，形成 重连→推帧→误判→重连 死循环。
 *    现在逐行到达逐行画，缓冲恒定 <100KB。
 *  - 观测：WS 关闭原因、booted 状态翻转、RFB 协议错误、帧中途停滞全部
 *    落进终端日志（[screen]/[RFB] 前缀）并进导出文件。
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
      if (/不可用|失败|异常|错误|error|fail/i.test(t)) cls = "err";
      else if (/\[aether\]/i.test(t)) cls = "sys";
      appendLine(t, cls);
    };
    es.onerror = function () { /* 自动重连由浏览器处理 */ };
  }

  function setStatus(text, kind) {
    statusEl.textContent = text;
    statusEl.className = "pill" + (kind ? " " + kind : "");
  }

  function post(url) {
    return fetch(url, { method: "POST" }).then(function (r) { return r.json(); });
  }

  // ----------------------------- 命令解析 -----------------------------
  var COMMANDS = [
    ["aether install / install", "下载并安装 AetherOS 系统镜像（首次约 1–3 分钟）"],
    ["aether run / run", "启动系统（已安装时；启动就绪自动显示系统画面）"],
    ["aether stop / stop", "停止运行中的系统"],
    ["aether reboot / reboot", "重启系统"],
    ["aether status / status", "查看安装/运行状态"],
    ["aether update / update", "检查并更新到最新版系统（客机内更新）"],
    ["aether reset / reset", "清除本地镜像（下次 install 重新下载）"],
    ["aether export / export", "导出诊断日志（崩溃监测记录，可分享给开发者）"],
    ["help", "显示本帮助"],
    ["clear", "清空终端"],
  ];

  function printHelp() {
    appendLine("可用命令：", "sys");
    for (var i = 0; i < COMMANDS.length; i++) {
      appendLine("  " + COMMANDS[i][0].padEnd(26) + " " + COMMANDS[i][1]);
    }
    appendLine("提示：也可直接点右上角「屏幕」查看系统画面。", "sys");
  }

  function doInstall() {
    runBtn.disabled = true;
    setStatus("安装中…", "busy");
    appendLine("[app] 开始安装（下载 + 解包 + 启动，请稍候）…", "sys");
    post("/api/install").then(function (j) {
      if (!j.started) appendLine("[app] 已在安装/运行中，无需重复。", "sys");
    }).catch(function (e) { appendLine("[app] 请求失败: " + e, "err"); });
  }

  function doBoot() {
    setStatus("启动中…", "busy");
    appendLine("[app] 正在启动系统…", "sys");
    post("/api/boot").then(function (j) {
      if (j.ok) appendLine("[app] 启动指令已下发，画面就绪后自动显示。", "sys");
      else appendLine("[app] 启动失败（可能未安装，先执行 install）。", "err");
    }).catch(function (e) { appendLine("[app] 请求失败: " + e, "err"); });
  }

  function doStop() {
    post("/api/stop").then(function (j) {
      appendLine("[app] 已停止系统。" + (j.ok ? "" : "（失败）"), "sys");
    }).catch(function (e) { appendLine("[app] 请求失败: " + e, "err"); });
  }

  function doReset() {
    appendLine("[app] 正在清除本地系统镜像…", "sys");
    post("/api/reset").then(function (j) {
      if (j.ok) {
        appendLine("[app] 已清除。下次执行 install 将下载最新版。", "sys");
        setStatus("未安装", "");
      } else appendLine("[app] 清除失败。", "err");
    }).catch(function (e) { appendLine("[app] 请求失败: " + e, "err"); });
  }

  function doUpdate() {
    // 客机内系统更新（与「系统更新」应用同一端点）：停 → 清 → 下载最新版 → 自动启动
    appendLine("[app] 系统更新：停止当前系统并拉取最新版…", "sys");
    setStatus("更新中…", "busy");
    post("/api/update")
      .then(function (j) {
        if (j.started) {
          appendLine("[app] 更新已开始：正在后台下载最新版，装好后自动启动。", "sys");
        } else {
          appendLine("[app] 更新未启动（查看日志或重试）。", "err");
          setStatus("就绪", "");
        }
      })
      .catch(function (e) {
        appendLine("[app] 更新失败: " + e, "err");
        setStatus("就绪", "");
      });
  }

  function doStatus() {
    fetch("/api/status").then(function (r) { return r.json(); }).then(function (s) {
      appendLine("[app] 安装状态: " + (s.installed ? "已安装" : "未安装")
        + " | 安装中: " + (s.installing ? "是" : "否")
        + " | 运行中: " + (s.booted ? "是" : "否")
        + " | 版本: " + (s.version || "-")
        + " | VNC: " + s.vnc_host + ":" + s.vnc_port, "sys");
      if (s.error) appendLine("[app] 最近错误: " + s.error, "err");
    }).catch(function (e) { appendLine("[app] 请求失败: " + e, "err"); });
  }

  function execCommand(raw) {
    var cmd = (raw || "").trim();
    if (!cmd) return;
    appendLine("aether@phone:~$ " + cmd, "sys");
    // 去掉可选前缀 aether
    var c = cmd.replace(/^aether\s+/, "").replace(/^aether$/, "").trim().toLowerCase();
    if (c === "install" || c === "i") { doInstall(); return; }
    if (c === "run" || c === "boot" || c === "start") { doBoot(); return; }
    if (c === "stop" || c === "kill") { doStop(); return; }
    if (c === "reboot") { doStop(); setTimeout(doBoot, 1200); return; }
    if (c === "status") { doStatus(); return; }
    if (c === "update") { doUpdate(); return; }
    if (c === "reset") { doReset(); return; }
    if (c === "export" || c === "log" || c === "logs") {
      appendLine("[app] 正在生成诊断日志（App 日志 + QEMU 报错 + 客机串口）…", "sys");
      // 经自定义 scheme 交给原生层：写文件 + 弹系统分享面板
      location.href = "aether://export-log";
      return;
    }
    if (c === "help" || c === "?" || c === "") { printHelp(); return; }
    if (c === "clear" || c === "cls") { termBody.innerHTML = ""; return; }
    appendLine("未知命令: " + cmd + "（输入 help 查看可用命令）", "err");
  }

  runBtn.addEventListener("click", function () {
    var cmd = cmdInput.value || "";
    cmdInput.value = "";
    execCommand(cmd);
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

  var screenDiag = document.getElementById("screen-diag");
  var booted = false, starting = false, installedFlag = false;
  var userInScreen = false;   // 用户此刻是否停在屏幕页
  var autoOpened = false;     // 本次会话只自动跳一次屏幕，之后完全听用户的
  var retryTimer = null, retryDelay = 1000, retryCount = 0;
  var dropStreak = 0;         // booted 连续掉线计数（瞬时抖动不判死刑）

  function showScreen() {
    viewTerm.hidden = true; viewScreen.hidden = false;
    btnToggle.textContent = "终端";
    userInScreen = true;
    if (!booted) {
      // 没就绪就不去连 5900：否则只会是一串 ECONNREFUSED，用户还以为在连
      setMsg(starting ? "系统正在启动，画面就绪后自动出现…"
                      : "系统未启动。回到终端执行 aether install，或点「运行」。");
      return;
    }
    connectVNC();
  }
  function showTerm() {
    viewScreen.hidden = true; viewTerm.hidden = false;
    btnToggle.textContent = "屏幕";
    userInScreen = false;
    clearRetry();
    closeVNC("用户切换到终端");   // 离开屏幕就断开，不在后台偷偷重连
  }
  btnToggle.addEventListener("click", function () {
    if (viewScreen.hidden) showScreen(); else showTerm();
  });
  btnBack.addEventListener("click", showTerm);
  btnReconnect.addEventListener("click", function () {
    if (!booted) {
      setMsg(starting ? "系统正在启动，请稍候…" : "系统未启动，先回终端执行 aether install。");
      return;
    }
    retryCount = 0;               // 手动重连时清零退避计数
    connectVNC();
  });
  // 顶栏「日志」按钮：一键导出诊断日志（App 日志 + QEMU 报错 + 客机串口）
  document.getElementById("btn-export").addEventListener("click", function () {
    appendLine("[app] 正在生成诊断日志，稍后弹出分享面板…", "sys");
    location.href = "aether://export-log";
  });

  function clearRetry() {
    if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
  }
  function scheduleRetry() {
    if (retryTimer || !userInScreen || !booted) return;
    if (retryCount >= 5) {
      setMsg("连续重连失败 " + retryCount + " 次，已暂停自动重连。"
           + "可点「重新连接」重试，或点右上角「日志」导出诊断信息。");
      return;
    }
    retryCount++;
    retryTimer = setTimeout(function () {
      retryTimer = null;
      if (userInScreen && booted) connectVNC();
    }, retryDelay);
    retryDelay = Math.min(retryDelay * 2, 8000);   // 退避，别把日志刷爆
  }

  // ----------------------------- 状态轮询 -----------------------------
  function pollStatus() {
    fetch("/api/status").then(function (r) { return r.json(); }).then(function (s) {
      var wasBooted = booted;
      booted = !!s.booted; starting = !!s.starting; installedFlag = !!s.installed;

      if (s.installing) { setStatus("安装中…", "busy"); runBtn.disabled = true; }
      else if (booted) { setStatus("系统就绪", "ok"); runBtn.disabled = false; }
      else if (starting) { setStatus("启动中…", "busy"); runBtn.disabled = false; }
      else if (installedFlag) { setStatus("已安装·未启动", ""); runBtn.disabled = false; }
      else { setStatus("未安装", ""); runBtn.disabled = false; }

      if (booted && !wasBooted) {
        retryDelay = 1000;
        dropStreak = 0;
        if (userInScreen) connectVNC();
        else if (!autoOpened) { autoOpened = true; showScreen(); }  // 只自动一次
      }
      // 系统掉了：连续 3 次 poll（约 4.5 秒）都掉线才认定真掉线。
      // phone-v17 及以前一帧掉线就 closeVNC，而 QEMU 推 4.6MB 首帧期间
      // 端口探测必然超时，直接把用户电视成「系统已停止」并断开重连——
      // 那就是「一直重连」的扳机点。
      if (!booted && wasBooted) {
        dropStreak++;
        if (dropStreak >= 3) {
          clearRetry(); closeVNC("系统掉线");
          if (userInScreen) setMsg("系统已停止。" + (s.qemu_exit ? " " + s.qemu_exit
                                              : s.error ? " " + s.error : ""));
        } else {
          appendLine("[app] 系统状态抖动 " + dropStreak + "/3（qemu_running="
            + (s.qemu_running ? "是" : "否") + "），继续观察不重连", "sys");
        }
      } else if (booted) {
        dropStreak = 0;
      }
      if (userInScreen && !booted && !starting && s.qemu_exit) {
        setMsg("系统未能启动：" + s.qemu_exit);
      }
      if (screenDiag) {
        screenDiag.textContent = "状态：" + (booted ? "就绪" : starting ? "启动中"
            : installedFlag ? "未启动" : "未安装")
          + " · VNC " + (s.vnc_host || "-") + ":" + (s.vnc_port || "-")
          + (s.version ? " · " + s.version : "")
          + (s.error ? " · " + s.error : "");
      }
    }).catch(function () {});
  }
  setInterval(pollStatus, 1500);

  // ----------------------------- RFB 客户端（流式） -----------------------------
  var canvas = document.getElementById("rfb");
  var ctx = canvas.getContext("2d");
  var screenMsg = document.getElementById("screen-msg");
  var ws = null;
  var fbW = 720, fbH = 1600;
  var imgData = null, img32 = null;
  // 可增长接收缓冲：roff=已消费偏移，rlen=有效长度。追加是均摊 O(1)，
  // 不再每次整块拷贝（旧实现每 64KB 帧全量 memcpy 一遍 4.6MB，O(n²)）。
  var rbuf = new Uint8Array(1 << 16);
  var rlen = 0, roff = 0;
  var rfstate = "version";
  var wsOpen = false;
  // 流式帧状态：一条 FramebufferUpdate 可能含多个 rect，像素按行到达即画
  var pend = null;          // {x,y,w,h,row}
  var pendTotal = 0, pendDone = 0;
  var lastByteAt = 0;       // 最近一次收到 VNC 字节的时刻（停滞检测用）
  var firstFrameAt = 0;     // 本次连接首帧画完的时刻（观测用）

  function setMsg(t) { screenMsg.textContent = t; screenMsg.style.display = t ? "block" : "none"; }

  function closeVNC(tag) {
    if (ws) {
      appendLine("[screen] 主动断开 WebSocket（" + (tag || "未注明") + "）", "sys");
      try { ws.close(); } catch (e) {}
      ws = null;
    }
    wsOpen = false;
    pend = null; pendTotal = 0; pendDone = 0;
  }

  function connectVNC() {
    clearRetry();
    if (ws) { try { ws.close(); } catch (e) {} ws = null; }
    var proto = location.protocol === "https:" ? "wss" : "ws";
    // 必须拼成 ws://host/ws/vnc。少写一个冒号会得到 "ws//127.0.0.1:port"，
    // WebSocket 构造函数直接抛 SyntaxError，屏幕永远连不上。
    ws = new WebSocket(proto + "://" + location.host + "/ws/vnc");
    ws.binaryType = "arraybuffer";
    setMsg("正在连接系统屏幕…");
    ws.onopen = function () {
      wsOpen = true;
      retryDelay = 1000;
      rfstate = "version";
      rlen = 0; roff = 0;
      pend = null; pendTotal = 0; pendDone = 0;
      firstFrameAt = 0;
      lastByteAt = Date.now();
      appendLine("[screen] WS 已打开，开始 RFB 握手", "sys");
    };
    ws.onclose = function (ev) {
      wsOpen = false;
      // 关闭原因全部记账：1005=无状态码（本地主动关），1006=异常断开（RST/进程死）
      var why = "code=" + (ev && ev.code) + " wasClean=" + (ev && ev.wasClean)
              + (ev && ev.reason ? " reason=" + ev.reason : "");
      appendLine("[screen] WS 已关闭: " + why, "sys");
      if (!userInScreen) return;
      // 服务端在系统没就绪时会直接回 503。这里问一次状态，把原因说清楚，
      // 而不是无脑“重新连接”——那是 phone-v10 死循环刷屏的根源。
      fetch("/api/status").then(function (r) { return r.json(); }).then(function (s) {
        booted = !!s.booted; starting = !!s.starting;
        if (s.booted) {
          setMsg("屏幕连接断开，正在重试（第 " + (retryCount + 1) + " 次）…");
          scheduleRetry();
        }
        else if (s.starting) setMsg("系统正在启动，画面就绪后自动出现…");
        else setMsg("系统未启动。" + (s.qemu_exit ? " " + s.qemu_exit
                                     : s.error ? " " + s.error : ""));
      }).catch(function () { scheduleRetry(); });
    };
    ws.onerror = function () {};   // 紧接着必有 onclose，提示只写一处免得闪两次
    ws.onmessage = function (ev) {
      lastByteAt = Date.now();
      rbAppend(new Uint8Array(ev.data));
      parseRFB();
    };
  }

  // ---- 缓冲原语 ----
  function rbHave(n) { return rlen - roff >= n; }
  function rbCompact() {
    if (roff === 0) return;
    if (roff === rlen) { roff = 0; rlen = 0; return; }
    rbuf.copyWithin(0, roff, rlen);
    rlen -= roff; roff = 0;
  }
  function rbAppend(data) {
    if (data.length === 0) return;
    if (roff > (1 << 16)) rbCompact();          // 消费过半才整理，均摊 O(1)
    var need = rlen + data.length;
    if (rbuf.length < need) {
      var cap = Math.max(need, rbuf.length * 2);
      var nb = new Uint8Array(cap);
      nb.set(rbuf.subarray(0, rlen));
      rbuf = nb;
    }
    rbuf.set(data, rlen);
    rlen += data.length;
  }
  function rbU8(o) { return rbuf[o]; }
  function rbU16(o) { return (rbuf[o] << 8) | rbuf[o + 1]; }
  function rbU32(o) { return ((rbuf[o] << 24) | (rbuf[o + 1] << 16) | (rbuf[o + 2] << 8) | rbuf[o + 3]) >>> 0; }

  function send(bytes) {
    if (ws && wsOpen) ws.send(bytes);
  }
  function u32(v) { var b = new Uint8Array(4); b[0] = v >> 24 & 255; b[1] = v >> 16 & 255; b[2] = v >> 8 & 255; b[3] = v & 255; return b; }
  function u16(v) { return new Uint8Array([v >> 8 & 255, v & 255]); }

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

  // 读一个 rect 头（12B）。返回 "ok"=pend 就绪；"wait"=字节不够等后续数据；
  // "fail"=非法矩形（已 failRFB）。一条 FramebufferUpdate 消息可含多个 rect，
  // 它们共享消息头、rect 头紧邻排列——处理完一个 rect 后必须继续顺序读下一个
  // rect 头，否则会把 rect 头的首字节误当消息类型（真机回归实测：243）。
  function readRectHeader() {
    if (!rbHave(12)) return "wait";
    var ho = roff;
    var rx = rbU16(ho), ry = rbU16(ho + 2);
    var rw = rbU16(ho + 4), rh = rbU16(ho + 6), enc = rbU32(ho + 8);
    if (enc !== 0 || rw < 1 || rh < 1 || rw > 8192 || rh > 8192
        || rx + rw > fbW + 64 || ry + rh > fbH + 64) {
      failRFB("非法矩形 enc=" + enc + " " + rx + "," + ry + " " + rw + "x" + rh);
      return "fail";
    }
    pend = { x: rx, y: ry, w: rw, h: rh, row: 0 };
    roff += 12;
    return "ok";
  }

  // 画一整行（流式：像素按行到达即画，不等整帧）
  function drawRawRow(x, y, w, src) {
    if (!img32) return;
    var di = y * fbW + x;
    for (var c = 0; c < w; c++) {
      var si = src + c * 4;
      var b0 = rbuf[si], b1 = rbuf[si + 1], b2 = rbuf[si + 2];
      // ServerInit 默认 32bpp LE，R@16 G@8 B@0 → 内存字节序 [B,G,R,pad]；
      // ImageData 要 [R,G,B,A]，小端 uint32 = A<<24|B<<16|G<<8|R。
      img32[di + c] = (255 << 24) | (b0 << 16) | (b1 << 8) | b2;
    }
  }

  function failRFB(msg) {
    reportCrash("RFB", msg);
    setMsg("屏幕数据异常：" + msg + "（正在重连）");
    closeVNC("RFB 协议错误: " + msg);
  }

  function parseRFB() {
    try {
      rbCompact();
      if (rfstate === "version") {
        // 找 \n（从 roff 起）
        var idx = -1;
        for (var i = roff; i < rlen; i++) { if (rbuf[i] === 0x0a) { idx = i; break; } }
        if (idx < 0) return;
        var vstart = roff, vend = idx + 1;
        roff = vend;
        var vs = "";
        for (var j = vstart; j < vend; j++) vs += String.fromCharCode(rbuf[j]);
        // 回显相同版本
        var echo = new Uint8Array(vend - vstart);
        for (var k = vstart; k < vend; k++) echo[k - vstart] = rbuf[k];
        send(echo);
        rfstate = "sec";
        return parseRFB();
      }
      if (rfstate === "sec") {
        if (!rbHave(1)) return;
        var n = rbU8(roff); roff += 1;
        if (n === 0) { // 错误
          if (!rbHave(4)) return;
          var rl = rbU32(roff); roff += 4;
          if (!rbHave(rl)) return;
          var reason = "";
          for (var r0 = roff; r0 < roff + rl; r0++) reason += String.fromCharCode(rbuf[r0]);
          roff += rl;
          setMsg("VNC 错误: " + reason);
          return;
        }
        if (!rbHave(n)) return;
        roff += n;
        send(new Uint8Array([1])); // None
        rfstate = "secresult";
        return parseRFB();
      }
      if (rfstate === "secresult") {
        if (!rbHave(4)) return;
        var code = rbU32(roff); roff += 4;
        if (code !== 0) { setMsg("VNC 认证失败"); return; }
        send(new Uint8Array([1])); // shared
        rfstate = "serverinit";
        return parseRFB();
      }
      if (rfstate === "serverinit") {
        if (!rbHave(24)) return;
        var w = rbU16(roff), h = rbU16(roff + 2);
        var nl = rbU32(roff + 20);
        if (!rbHave(24 + nl)) return;
        roff += 24 + nl;
        fbW = w; fbH = h;
        canvas.width = w; canvas.height = h;
        imgData = ctx.createImageData(w, h);
        img32 = new Uint32Array(imgData.data.buffer);
        setMsg("");
        rfstate = "ready";
        pend = null; pendTotal = 0; pendDone = 0;
        appendLine("[screen] ServerInit " + w + "x" + h + "，请求首帧…", "sys");
        // 关键：不要发 SetPixelFormat！这条 QEMU 的 VNC 对主动像素格式协商
        // 支持有 bug（depth24 会直接断连、depth32 收不到帧，本地可稳定复现），
        // 而 ServerInit 默认格式就是 32bpp LE R16 G8 B0——与 drawRawRow 的
        // 解析完全一致，直接用默认格式即可。
        sendSetEncodings();
        sendFBReq(0); // 首帧全量
        return parseRFB();
      }
      if (rfstate === "ready") {
        for (;;) {
          if (pend) {
            // 当前矩形：像素按行到达，齐一行画一行（缓冲里最多压一行）
            var rowBytes = pend.w * 4;
            var drawn = 0;
            while (pend.row < pend.h && rbHave(rowBytes)) {
              drawRawRow(pend.x, pend.y + pend.row, pend.w, roff);
              roff += rowBytes;
              pend.row++;
              drawn++;
              // 每 80 行刷一次屏，让用户看着画面逐段出现（全帧 4.6MB 要传 ~2 秒）
              if (drawn % 80 === 0) ctx.putImageData(imgData, 0, 0);
            }
            if (pend.row < pend.h) return;   // 等这一帧剩下的行
            pend = null; pendDone++;
            ctx.putImageData(imgData, 0, 0);
            if (pendDone >= pendTotal) {
              // 整条消息画完：这才是“真的连上了”，清零退避计数
              retryCount = 0;
              if (!firstFrameAt) {
                firstFrameAt = Date.now();
                appendLine("[screen] 首帧绘制完成，进入增量更新", "sys");
              }
              sendFBReq(1); // 增量请求下一帧
              pendTotal = 0; pendDone = 0;
            }
            continue;     // pendDone<pendTotal 时由循环顶的 rect 头分支接着读
          } else if (pendTotal > 0 && pendDone < pendTotal) {
            // 正在一条 FramebufferUpdate 消息内（消息头已到、矩形未画完）：
            // 顺序读下一个 rect 头。不能落到消息类型分发——roff 指的是 rect
            // 头而不是消息类型（TCP 分片时同样会走到这里）。
            var stMore = readRectHeader();
            if (stMore === "wait" || stMore === "fail") return;
            continue;
          }
          if (!rbHave(1)) return;
          var type = rbuf[roff];
          if (type === 0) { // FramebufferUpdate
            if (!rbHave(4)) return;
            pendTotal = rbU16(roff + 2);
            pendDone = 0;
            roff += 4;
            if (pendTotal === 0) { sendFBReq(1); continue; }
            var st = readRectHeader();
            if (st === "wait" || st === "fail") return;
            continue;
          } else if (type === 2) { // Bell
            roff += 1;
          } else if (type === 3) { // ServerCutText
            if (!rbHave(8)) return;
            var tl = rbU32(roff + 4);
            if (!rbHave(8 + tl)) return;
            roff += 8 + tl;
          } else if (type === 1) { // SetColourMapEntries（truecolor 下不常见，但要会消费）
            if (!rbHave(8)) return;
            var nc = rbU16(roff + 6);
            if (!rbHave(8 + nc * 6)) return;
            roff += 8 + nc * 6;
          } else {
            failRFB("不支持的 VNC 消息类型 " + type);
            return;
          }
        }
        return;
      }
    } catch (e) {
      failRFB("解析异常: " + (e && e.message));
    }
  }

  // 停滞看门狗：帧传一半不来了（QEMU 侧挂死）→ 15 秒后主动重连自愈。
  // 画面静止时 pend 为 null，不会误判。
  setInterval(function () {
    if (!wsOpen) return;
    var idle = Date.now() - lastByteAt;
    if (pend && idle > 15000) {
      failRFB("帧数据中途停滞 " + Math.round(idle / 1000) + " 秒");
    } else if (rfstate !== "ready" && idle > 15000) {
      failRFB("握手阶段数据停滞 " + Math.round(idle / 1000) + " 秒");
    } else if (pend && idle > 5000 && !pend._warned) {
      pend._warned = true;
      appendLine("[screen] 帧数据传输暂停 " + Math.round(idle / 1000) + " 秒，继续等待…", "sys");
    }
  }, 1000);

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
    if (k >= 48 && k <= 90) key = k; // ASCII 直接
    else if (k === 32) key = 32;
    else if (k === 13) key = 13;
    else if (k === 8) key = 8;
    if (key) sendKey(false, key);
  });

  // ----------------------------- 启动 -----------------------------
  startLogs();
  pollStatus();
  appendLine("AetherOS 终端 v18 · 自包含架构（内置 QEMU，无需 Termux）", "sys");
  appendLine("首次使用请输入: aether install", "sys");
  appendLine("输入 help 查看全部命令。", "sys");
  // 这里以前有个每 2 秒强制 showScreen() 的定时器，会把用户主动切回终端
  // 的操作立刻撤销，形成“点什么都被拉回重连页”的鬼打墙。已删除。

  // ----------------------------- 崩溃监测器 -----------------------------
  // 任何 JS 错误都同时落到三处：终端日志（[崩溃] 前缀）、屏幕页提示、
  // 宿主日志（POST /api/crash）。用户截图任意一处都能拿到完整信息。
  function reportCrash(kind, msg) {
    var line = "[" + kind + "] " + msg;
    try { appendLine("[崩溃] " + line, "err"); } catch (e) {}
    try { setMsg("发生错误：" + line); } catch (e) {}
    try {
      fetch("/api/crash", { method: "POST", body: line });
    } catch (e) {}
  }
  window.onerror = function (msg, src, row, col) {
    var where = String(src || "?").split("/").pop();
    reportCrash("JS错误", msg + "（" + where + ":" + row + ":" + col + "）");
  };
  window.addEventListener("unhandledrejection", function (e) {
    var r = e.reason;
    reportCrash("Promise", (r && (r.stack || r.message)) || String(r));
  });
})();
