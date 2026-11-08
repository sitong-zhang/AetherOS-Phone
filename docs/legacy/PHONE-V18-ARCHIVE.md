# phone-v18.1 封存说明

封存日期：2026-10-21
状态：封存。后续问题请开新版本号，不要在本版本上继续迭代。

## 版本

phone-v18.1（versionCode 18，versionName phone-v18）。tag `phone-v18` 指向的
release 资产已替换为本版；tag 不新建，沿用 v18 通道。

## 交付物清单

| 位置 | 文件 | 说明 |
|---|---|---|
| `dist-phone/terminal-apk/aetheros-terminal.apk` | 49045342 B | 自包含终端 App，arm64-v8a，minSdk 24 / targetSdk 34 |
| `dist-phone/terminal-apk/src/` | Java 源码 4 个文件 | AetherCore / AetherServer / WsBridge / MainActivity |
| `dist-phone/terminal-apk/assets/` | app.js / index.html / favicon.ico / qemu-data/ | WebView 前端与 QEMU keymap |
| `dist-phone/terminal-apk/lib/arm64-v8a/` | 28 个 .so | QEMU 11.1.2 及依赖闭包（含 compat stub） |
| `dist-phone/terminal-apk/build-apk.sh` | 构建入口 | javac + d8 + aapt2 + zipalign(-P 16) + apksigner |
| `dist-phone/terminal-apk/fix-sonames.sh`、`fix-verneed.py`、`make-compat-stub.py` | 库治理脚本 | .so 提取名、verneed 清理、符号 stub 生成 |
| `dist-phone/aetheros-phone.tar.gz` | 27160427 B | 客机镜像（kernel Image + ext4 rootfs） |
| `dist-phone/terminal-apk-src-v18.1.tar.gz` | 71069 B | terminal-apk 源码快照（src/assets/脚本/文档，不含构建产物） |
| `AetherOS安装器/` | APK + README-手机版.md + v18-修订说明.md + 源码快照 | 用户侧交付副本 |
| GitHub `sitong-zhang/AetherOS-Phone` release `phone-v18` | 三个资产 | aetheros-terminal.apk / aetheros-phone.tar.gz / v18-notes.md |

## 本版相对 v18 首发的修改

1. `WsBridge.encode()`：8 字节扩展长度分支改 long 移位，修掉 65536 字节
   载荷写出垃圾长度字段的问题（根因与复现见 `PHONE-V18-ROOT-CAUSE.md`）。
2. `assets/app.js`：FramebufferUpdate 多矩形按序消费，抽出
   `readRectHeader()`；TCP 分片截断在矩形头中间不再误判为消息类型。
3. `AetherCore.allLog()` + `AetherServer.sseLogs()`：SSE 日志流改增量推送，
   去掉每轮重发尾部 30 行的行为。

## 重建

```sh
cd dist-phone/terminal-apk
bash build-apk.sh
```

依赖：JDK（javac/d8，source/target 8）、Android SDK build-tools 35.0.0
（zipalign 需支持 -P 16）、platforms/android-34/android.jar、debug keystore。
脚本内含硬自检：assets 齐全、.so 可提取、targetSdkVersion 34、16KB 对齐。
产物签名校验：

```sh
apksigner verify --min-sdk-version 24 --max-sdk-version 34 aetheros-terminal.apk
zipalign -c -P 16 4 aetheros-terminal.apk
```

## 验证记录（2026-10-20 至 21）

| 项 | 方法 | 结果 |
|---|---|---|
| WsBridge 帧长 bug | Java 双版本编码对比，payload=65536 | 旧版声称 281474976776192 B，新版正确 |
| WsBridge 修复 | Node ws 客户端收帧 | 旧帧 Max payload size exceeded；新帧完整送达 |
| app.js 流式解析 | mock DOM + 真 QEMU（720x1600） | ServerInit / 首帧 / 增量更新通过，无协议错误 |
| SSE 增量 | dexdump 反汇编 sseLogs | 调用 allLog，无 tailLog 重发 |
| APK 完整性 | 签名 + 16KB 对齐 + 发布资产头尾 2MB 区间 md5 | 全部一致 |

## 已知边界

- 客机镜像为 phone-v15，未随本版更换；镜像与 App 版本独立演进，
  App 内 `aether update` 可热更换。
- QEMU 为 TCG 软件模拟，720x1600 首帧约 4.6MB，传输耗时随宿主性能波动。
- 全量 md5 下载校验未做（沙箱出口带宽约 20KB/s），以 API 元数据
  （size 49045342、state uploaded）加头尾区间 md5 替代。
