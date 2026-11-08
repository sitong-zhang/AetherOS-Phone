# phone-v18「一直重连」问题定位记录（v18.1 修订）

记录时间：2026-10-20 至 2026-10-21
涉及代码：`src/com/aetheros/terminal/WsBridge.java`、`assets/app.js`、
`src/com/aetheros/terminal/AetherServer.java`、`AetherCore.java`

## 现象

phone-v18 首发版 APK 配 phone-v15 镜像，真机导出诊断日志显示桥接循环
重连：

```
[aether] bridge: 已连接 VNC 127.0.0.1:5900
[aether] bridge: 收到 QEMU 首字节（等了 3ms，共 12 字节）
[aether] bridge c->v 结束（已上传 32 字节）: 连接关闭
[aether] bridge v->c 结束（已转发 131118 字节）: Broken pipe
[aether] bridge: 已连接 VNC 127.0.0.1:5900
[aether] bridge: 收到 QEMU 首字节（等了 2ms，共 12 字节）
[aether] bridge c->v 结束（已上传 32 字节）: Connection reset
[aether] bridge v->c 结束（已转发 4608062 字节）: Socket closed
```

三个特征：v2c 方向累计转发字节数在 131118 到 4608062 之间随机；前端
没有任何 `[screen]` 日志；桥接日志中没有「浏览器正常关闭」记录。

## 一、WsBridge.encode() 扩展长度分支

WsBridge 将 VNC 的 TCP 流转成 RFC6455 二进制帧，载荷长度分段编码：
小于 126 用 1 字节，小于 65536 用 2 字节，以上用 8 字节。原实现在 8 字节
分支直接对 int 长度做移位：

```java
int len = payload.length;
for (int i = 0; i < 8; i++) head[2 + i] = (byte) (len >> (56 - 8 * i));
```

Java 的 int 移位位数按 32 取模，`len >> 56` 实际执行的是 `len >> 24`。
桥接的 v2c 线程按 65536 字节块从 QEMU 读取，块长落在 8 字节分支时，
写出的长度字段即错误值。

以 65536 字节载荷实测（payload 逐字节为 `i & 0xFF`）：

| 实现 | 帧头（10 字节） | 声明载荷长度 | 实际载荷 |
|---|---|---|---|
| 原实现 | `82 7f 00 01 00 00 00 01 00 00` | 281474976776192 | 65536 |
| 修订后 | `82 7f 00 00 00 00 00 01 00 00` | 65536 | 65536 |

用 Node ws 库作客户端验证（该库与 Chromium 的协议错误处理一致）：收到
原实现的帧后连接以 Max payload size exceeded 失败，不产生 close 帧；
收到修订后的帧则完整收到 65536 字节。

对应真机现象：QEMU 推送 4.6MB 首帧期间，只要某次读取恰好返回 64KB，
该帧必然损坏，浏览器在协议层断开，前端 onclose 触发重连。断开位置由
TCP 分块决定，所以随机；断点在 WebSocket 协议层以下，所以前端无日志、
桥接看不到正常关闭。

修复：

```java
long v = len & 0xFFFFFFFFL;
for (int i = 0; i < 8; i++) head[2 + i] = (byte) (v >>> (56 - 8 * i));
```

## 二、app.js 多矩形 FramebufferUpdate 解析

harness 回归测试（mock DOM 加载真 app.js，连接真 QEMU，720×1600）发现：
首帧绘制完成、进入增量更新后，出现 `[RFB] 不支持的 VNC 消息类型 243`，
前端主动断开重连。

一条 FramebufferUpdate 消息可含多个矩形，矩形头（12 字节：x、y、w、h、
encoding）紧邻排列，共用 4 字节消息头。原解析器在 ready 分支画完一个
矩形后 continue 回到消息类型分发，把下一个矩形头的首字节当消息类型读。
SystemUI 有时钟和动画，增量帧必然多矩形，该路径每次增量更新都会走到。

修复：矩形头读取抽为 `readRectHeader()`，返回 ok / wait / fail 三种状态。
消息内矩形按序消费；循环顶以 `pendTotal > 0 && pendDone < pendTotal`
判断是否仍在同一条消息内，TCP 分片截断在矩形头中间时也走该分支等待，
不再误入消息类型分发。

## 三、SSE 日志流重复推送

`sseLogs()` 每 400 毫秒调用一次 `core.tailLog(30)` 并全部重发，日志
活跃时约 75 行每秒重复写入页面。该问题不导致断连，但持续消耗前端性能。

修复：AetherCore 增加 `allLog()` 返回全量快照；sseLogs 维护已发送行数
游标，每轮只发送新增部分；logLines 环形缓冲写满翻转（size 变小）时
重发尾部 100 行并重置游标。

## 验证

| 项 | 方法 | 结果 |
|---|---|---|
| 帧长度错误 | Java 双版本编码对比，payload 65536 | 原实现声明 281474976776192，修订后正确 |
| 帧长度修复 | Node ws 客户端收帧 | 原帧 Max payload size exceeded；修订帧完整送达 |
| app.js 解析 | harness 连真 QEMU（5901，720×1600），60 秒 | ServerInit、首帧、增量更新均通过，无协议错误，RSS 66MB |
| SSE 增量 | dexdump 反汇编 sseLogs | 调用 allLog，循环内无 tailLog |
| encode 编译产物 | dexdump 反汇编 encode | 含 and-long/2addr 与 ushr-long |
| APK | apksigner（min 24 / max 34）、zipalign -P 16、发布资产头尾 2MB 区间 md5 | 通过 |

## 已排除的可能

- QEMU 版本差异：APK 内嵌 11.1.2，harness 用 8.2.2。两个缺陷都在协议
  实现层，与 QEMU 版本无关。
- WebView 渲染进程被杀：MainActivity 已处理 onRenderProcessGone 并记
  `[WebView 崩溃]`，真机日志中无该记录。
- 端口探测误判：v18 已改三态探测，本版未再改动。

## 复现

```sh
# 帧长度 bug（Java 证明 + ws 客户端验证）
cd encode-proof && java EncodeProof . \
  && node ws-client-proof.js buggy 18921 \
  && node ws-client-proof.js fixed 18922

# app.js 回归（需 5901 有 QEMU：-vnc 127.0.0.1:1）
cd vnc-repro && node appjs-harness.js
```
