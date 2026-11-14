# 取证证据目录

本目录**只存放取证机制说明与各轮证据清单索引**，不随仓库分发大取证产物
（串口日志、图形首帧、JSON 快照、界面图）。依据项目约定：非系统本体（研究性素材、
演练证据、研判报告）存本地归档，GitHub 仓库只放产品本体与对外文档。

## 为什么证据不入库

- 用户明确约定：仓库只上架「真正的系统 / 整个产品」，截图、日志、报告等取证素材存本地。
- 取证产物由**仓库内的脚本现场生成**，任何人 clone 后按下方路径即可复现并自证，
  不依赖作者云电脑上的本地归档。

## 可复现路径（clone 后即成立）

```bash
git clone https://github.com/sitong-zhang/AetherOS-Phone.git
cd AetherOS-Phone

# 1) 下载并校验外部资产（Linux 内核源码等；按 boot/ASSETS.sha256 校验）
bash boot/fetch-assets.sh

# 2) 5 步构建：Linux 6.6.70 ARM64 内核 + aarch64 用户态 + 安全引擎 + 自建 ext4 根盘
bash boot/build.sh

# 3) QEMU ARM64 TCG 冷启动，抓取真实图形首帧与串口证据到 evidence/
bash boot/run-qemu.sh

# 4) 开机门禁：逐环节断言串口证据与首帧像素（默认读 evidence/round-009）
python3 tools/gate_boot.py

# 接口层（十五域 HAL）五门静态机检
python3 tools/gate_l2.py

# 安全整机全套自测
bash scripts/secure.sh test
```

无交叉工具链 / 无 root / apt 不可用的环境需先具备 `aarch64-linux-gnu-gcc` 与
`qemu-system-aarch64`（见 boot/README.md 的「环境依赖」）。构建约 30–60 分钟。

## 门禁默认证据位置

| 脚本 | 默认证据根 | 缺证据时行为 |
| --- | --- | --- |
| `tools/gate_boot.py` | `evidence/round-009` | 打印可复现路径，返回非零，不崩溃、不虚报 |
| `tools/gate_l2.py` | `evidence/round-l2` | 输出机检 JSON；纯静态分析，不读外部证据 |
| `tools/render_secure_ui.py` | `evidence/round-002` | 界面图为设计稿渲染产物，打印复现方式，返回非零 |

## 各轮证据清单（本地归档）

本地归档路径（开发者云电脑，不公开下载）：`../aetheros-archive/evidence/round-XXX/`

| 轮次 | 主题 | 关键产物 |
| --- | --- | --- |
| round-006 | 仓库产品化收尾 + 攻击意图判定层 | `test_secure.stdout.txt`(356/0)、`gate_l2.json` |
| round-007 | 复检遗留闭环 + M1 A 轨真开机取证 | `boot-serial.log`、`gate_boot.json`(6/6) |
| round-008 | 商用界面文案清理 + 自毁动效 | `0*.png`、`wipe-animation/frame-*.png` |
| round-009 | A 轨真实图形首帧 + 安全引擎随开机自检 | `first-frame.png`、`boot-serial.log`、`gate_boot.json`(10/10) |
| round-010 | 复检 01:41（证据可复现化 + N2 钉死 + 文档边界） | `gate_boot.json`、`verify-rc.txt` |

状态提升一律由上述脚本现场取证，不凭文字陈述。
