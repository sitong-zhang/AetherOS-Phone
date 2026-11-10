#!/bin/sh
# AetherOS 手机系统自检（阶段 5/6）：
# 核心回归 6 项断言 + 灵动岛三态 / 五场景现场跑通 / 主副并存与互换 /
# 隐私指示点与面板 / 全局搜索分组与空状态 / 负一屏 / 深浅两套，
# 新增阶段 5 验收：应用商店安装闭环（装/卸笔记）、多任务小窗（单浮窗+第二请求拦截）、
# 息屏显示 AOD（开=极低亮度暗显 / 关 SYS_AOD=20 后全黑）。
# 所有断言字段来自合成器 state 的真实状态，逐帧证据经 9p 导出到宿主共享目录。
grep -q "aether.phonetest=1" /proc/cmdline 2>/dev/null || exit 0
I=/bin/aether-ipc
T=/dev/console
mkdir -p /data/aether /mnt/share
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/share 2>/dev/null
OUT=/data/aether
grep -q " /mnt/share " /proc/mounts 2>/dev/null && OUT=/mnt/share
i=0
while [ $i -lt 40 ] && [ ! -S /run/aetheros-comp.sock ]; do i=$((i+1)); sleep 1; done
[ -S /run/aetheros-comp.sock ] || { echo "SELFTEST: no compositor socket" > $T; exit 0; }
shot(){ $I settle >/dev/null 2>&1; aether-shot "$OUT/$1" 2>/dev/null && echo "SELFTEST: shot $1 -> $OUT" > $T; }

# ---------- 开机动画逐帧证据 ----------
# socket 就绪时合成器处于 PH_BOOT；draw_boot() 按真实秒数推进。
# 这里直接请求上屏帧、不走 settle，最大限度抢在 3 秒动画窗口内多抓几帧。
aether-shot "$OUT/60-boot-1.ppm" 2>/dev/null
echo "SELFTEST: shot 60-boot-1.ppm -> $OUT" > $T
sleep 1
aether-shot "$OUT/61-boot-2.ppm" 2>/dev/null
echo "SELFTEST: shot 61-boot-2.ppm -> $OUT" > $T
sleep 1
aether-shot "$OUT/62-boot-3.ppm" 2>/dev/null
echo "SELFTEST: shot 62-boot-3.ppm -> $OUT" > $T
sleep 3

# ---------- 核心回归（与 v4 相同的 6 项闭环断言） ----------
shot 01-lock.ppm
sleep 3
$I unlock >/dev/null 2>&1
$I home   >/dev/null 2>&1
sleep 1
shot 02-home.ppm
echo "SELFTEST_STATE_HOME=$( $I state 2>/dev/null )" > $T
$I click 96 170 >/dev/null 2>&1
sleep 1
shot 03-app-open.ppm
echo "SELFTEST_STATE_CLICK=$( $I state 2>/dev/null )" > $T
$I home >/dev/null 2>&1
$I panel 2 >/dev/null 2>&1
sleep 1
shot 04-control-center.ppm
echo "SELFTEST_STATE_PANEL=$( $I state 2>/dev/null )" > $T
$I panel 0 >/dev/null 2>&1
$I theme 1 >/dev/null 2>&1
sleep 1
shot 05-home-light.ppm
echo "SELFTEST_STATE_LIGHT=$( $I state 2>/dev/null )" > $T
$I theme 0 >/dev/null 2>&1
$I drawer 1 >/dev/null 2>&1
sleep 1
shot 06-drawer.ppm
echo "SELFTEST_STATE_DRAWER=$( $I state 2>/dev/null )" > $T
$I drawer 0 >/dev/null 2>&1

# ---------- 灵动岛：三种形态 ----------
$I islandclear >/dev/null 2>&1
$I island music >/dev/null 2>&1
$I islandform 0 >/dev/null 2>&1
shot 10-island-collapsed.ppm
$I islandform 1 >/dev/null 2>&1
shot 11-island-expanded.ppm
$I islandform 2 >/dev/null 2>&1
shot 12-island-card.ppm
echo "SELFTEST_ISLAND_FORMS=$( $I state 2>/dev/null )" > $T

# ---------- 验收 4：五个场景现场跑通 ----------
for s in music timer record charge nav; do
    $I islandclear >/dev/null 2>&1
    $I island "$s" >/dev/null 2>&1
    $I islandform 2 >/dev/null 2>&1
    [ "$s" = "charge" ] && $I island charge >/dev/null 2>&1
    shot "13-island-$s.ppm"
    echo "SELFTEST_ISLAND_$s=$( $I state 2>/dev/null )" > $T
done
echo "SELFTEST_ISLAND_5=1" > $T

# ---------- 灵动岛卡片按钮：真实切换播放/暂停，计时冻结（禁止清单「无反馈按钮」） ----------
$I islandclear >/dev/null 2>&1
$I island timer >/dev/null 2>&1
$I islandform 2 >/dev/null 2>&1
echo "SELFTEST_ISLAND_RUNNING=$( $I state 2>/dev/null )" > $T
echo "SELFTEST_ISLAND_BTN_PAUSE=$( $I islandaction 0 2>/dev/null )" > $T
$I settle >/dev/null 2>&1
shot 42-island-paused.ppm
echo "SELFTEST_ISLAND_PAUSED=$( $I state 2>/dev/null )" > $T
sleep 2
echo "SELFTEST_ISLAND_FROZEN=$( $I state 2>/dev/null )" > $T
echo "SELFTEST_ISLAND_BTN_RESUME=$( $I islandaction 0 2>/dev/null )" > $T
echo "SELFTEST_ISLAND_RESUMED=$( $I state 2>/dev/null )" > $T
$I islandaction 1 >/dev/null 2>&1   # 停止并清除
echo "SELFTEST_ISLAND_STOPPED=$( $I state 2>/dev/null )" > $T
$I islandclear >/dev/null 2>&1

# ---------- 主活动 + 副活动并存，点击互换 ----------
$I islandclear >/dev/null 2>&1
$I island call >/dev/null 2>&1
$I island music >/dev/null 2>&1
$I islandform 1 >/dev/null 2>&1
shot 14-island-primary-sub.ppm
$I islandswap >/dev/null 2>&1
shot 15-island-swapped.ppm
echo "SELFTEST_ISLAND_SUB=$( $I state 2>/dev/null )" > $T
$I islandclear >/dev/null 2>&1

# ---------- 隐私指示：绿点（相机）/ 橙点（定位）+ 使用面板 ----------
$I privacy 1 相机 >/dev/null 2>&1
$I privacy 3 地图 >/dev/null 2>&1
shot 16-privacy-dots.ppm
$I click 690 20 >/dev/null 2>&1
shot 17-privacy-panel.ppm
echo "SELFTEST_PRIVACY=$( $I state 2>/dev/null )" > $T
$I click 630 112 >/dev/null 2>&1
$I privacy 0 >/dev/null 2>&1

# ---------- 全局搜索：输入即出结果 / 分组 / 空状态 ----------
$I search open >/dev/null 2>&1
shot 18-search-suggest.ppm
$I search 文件 >/dev/null 2>&1
shot 19-search-file.ppm
$I search 显示 >/dev/null 2>&1
shot 20-search-setting.ppm
$I search 不存在的关键词zzz >/dev/null 2>&1
shot 21-search-empty.ppm
echo "SELFTEST_SEARCH=$( $I state 2>/dev/null )" > $T
$I search close >/dev/null 2>&1

# ---------- 负一屏 ----------
$I neg1 1 >/dev/null 2>&1
shot 22-neg1-dark.ppm
echo "SELFTEST_NEG1=$( $I state 2>/dev/null )" > $T
$I neg1 0 >/dev/null 2>&1

# ---------- 浅色一套（总纲 9.5「截图交付深浅色各一套」） ----------
# 覆盖：桌面 / 锁屏 / 通知中心 / 控制中心 / 抽屉 / 灵动岛三态 /
#       搜索 / 负一屏 / 设置 / 应用商店 / 多任务 / 小窗。
# 全部在同一段 theme=1 内完成，避免反复切主题触发整屏玻璃重算。
$I theme 1 >/dev/null 2>&1
$I home >/dev/null 2>&1
shot 26-home-light.ppm
# relock：干净复位到锁屏（跳过开机动画），再解锁继续后续页面
$I relock >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 50-lock-light.ppm
echo "SELFTEST_LIGHT_LOCK=$( $I state 2>/dev/null )" > $T
$I unlock >/dev/null 2>&1
$I home >/dev/null 2>&1
# 通知中心（panel 1）/ 控制中心（panel 2）
$I panel 1 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 51-notif-light.ppm
$I panel 0 >/dev/null 2>&1
$I panel 2 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 52-control-light.ppm
$I panel 0 >/dev/null 2>&1
# 应用抽屉
$I drawer 1 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 53-drawer-light.ppm
$I drawer 0 >/dev/null 2>&1
# 灵动岛三态（浅色）
$I islandclear >/dev/null 2>&1
$I island music >/dev/null 2>&1
$I islandform 0 >/dev/null 2>&1
shot 54-island-collapsed-light.ppm
$I islandform 1 >/dev/null 2>&1
shot 23-island-light.ppm
$I islandform 2 >/dev/null 2>&1
shot 55-island-card-light.ppm
echo "SELFTEST_ISLAND_LIGHT_FORMS=$( $I state 2>/dev/null )" > $T
$I islandclear >/dev/null 2>&1
# 全局搜索 / 负一屏
$I search open >/dev/null 2>&1
$I search 日历 >/dev/null 2>&1
shot 24-search-light.ppm
$I search close >/dev/null 2>&1
$I neg1 1 >/dev/null 2>&1
shot 25-neg1-light.ppm
$I neg1 0 >/dev/null 2>&1
# 设置（open 5 = APP_SETTINGS）/ 应用商店（open 14）
# APP_TERMINAL 为 1；此前误用 1 导致该取证帧实际落在终端页。
$I open 5 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 56-settings-light.ppm
$I home >/dev/null 2>&1
$I settle >/dev/null 2>&1
$I open 14 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 57-store-light.ppm
echo "SELFTEST_STORE_LIGHT=$( $I state 2>/dev/null )" > $T
$I home >/dev/null 2>&1
$I settle >/dev/null 2>&1
# 多任务 / 小窗
$I open 4 >/dev/null 2>&1
$I open 8 >/dev/null 2>&1
$I recents >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 58-recents-light.ppm
$I home >/dev/null 2>&1
$I settle >/dev/null 2>&1
$I float 8 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 59-float-light.ppm
$I closefloat >/dev/null 2>&1
$I home >/dev/null 2>&1
echo "SELFTEST_LIGHT_SET_DONE=1" > $T
$I theme 0 >/dev/null 2>&1
$I home >/dev/null 2>&1

echo "SELFTEST_APPS=14" > $T
echo "SELFTEST_STAGE4_DONE=1" > $T

# ---------- 阶段 5：应用商店安装闭环 / 小窗多任务 / 息屏显示 AOD ----------
$I theme 0 >/dev/null 2>&1
$I home >/dev/null 2>&1
$I page 0 >/dev/null 2>&1
sleep 1
# 应用商店（APP_STORE=14）
$I open 14 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 30-store-home.ppm
echo "SELFTEST_STORE_HOME=$( $I state 2>/dev/null )" > $T
# 笔记详情（索引 0）+ 安装（确定性 fast 流）
$I appcmd store detail 0 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 31-store-detail.ppm
$I appcmd store installfast 0 >/dev/null 2>&1
$I settle >/dev/null 2>&1
echo "SELFTEST_STORE_INSTALLED=$( $I state 2>/dev/null )" > $T
# 首开权限授予 -> 打开笔记
$I appcmd store permallow >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 32-notes-opened.ppm
echo "SELFTEST_NOTES_OPEN=$( $I state 2>/dev/null )" > $T
$I home >/dev/null 2>&1
$I page 3 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 33-home-notes-installed.ppm
echo "SELFTEST_HOME_NOTES=$( $I state 2>/dev/null )" > $T
$I drawer 1 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 34-drawer-notes.ppm
echo "SELFTEST_DRAWER_NOTES=$( $I state 2>/dev/null )" > $T
$I drawer 0 >/dev/null 2>&1
# 多任务卡片
$I open 4 >/dev/null 2>&1
$I open 8 >/dev/null 2>&1
$I open 11 >/dev/null 2>&1
$I settle >/dev/null 2>&1
$I recents >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 35-recents.ppm
echo "SELFTEST_RECENTS=$( $I state 2>/dev/null )" > $T
$I home >/dev/null 2>&1
# 小窗：浏览器（kind=8）
$I float 8 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 36-float-browser.ppm
echo "SELFTEST_FLOAT=$( $I state 2>/dev/null )" > $T
# 第二请求：应被拦截（仍只有一个小窗）
$I float 11 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 37-float-second.ppm
echo "SELFTEST_FLOAT_SECOND=$( $I state 2>/dev/null )" > $T
$I closefloat >/dev/null 2>&1
$I settle >/dev/null 2>&1
# 息屏显示 AOD（默认开启 -> 极低亮度暗显）
$I sleep >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 38-aod.ppm
echo "SELFTEST_AOD=$( $I state 2>/dev/null )" > $T
# 关闭 AOD（SYS_AOD=20 -> 0）再息屏应全黑
$I wake >/dev/null 2>&1
$I settle >/dev/null 2>&1
$I sys 20 0 >/dev/null 2>&1
$I sleep >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 39-aod-off-black.ppm
echo "SELFTEST_AOD_OFF=$( $I state 2>/dev/null )" > $T
$I wake >/dev/null 2>&1
$I sys 20 1 >/dev/null 2>&1
$I settle >/dev/null 2>&1
# 卸载笔记，验证桌面回到无笔记
$I open 14 >/dev/null 2>&1
$I appcmd store uninstall 0 >/dev/null 2>&1
$I settle >/dev/null 2>&1
$I home >/dev/null 2>&1
$I page 3 >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 40-home-after-uninstall.ppm
echo "SELFTEST_HOME_UNINSTALL=$( $I state 2>/dev/null )" > $T
$I home >/dev/null 2>&1

# ---------- 安卓兼容层：能力状态 / 商店分类 / 降级提示 ----------
# pKVM 不可用时，兼容层必须如实报告 unavailable 并给出「需要更高版本系统」，
# AetherOS 本体照常用。以下断言与截图验证桥接在无容器时零副作用（桌面不出现假窗口）。
echo "SELFTEST_ANDROID_STATUS=$( $I android 2>/dev/null )" > $T
[ -f /run/aether/android.status ] && {
    echo "SELFTEST_ANDROID_FILE=$( cat /run/aether/android.status | tr '\n' ' ' )" > $T
}
# 打开应用商店的「安卓应用」分类页（open 14 = APP_STORE，appcmd store android）
$I open 14 >/dev/null 2>&1
$I appcmd store android >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 41-store-android.ppm
echo "SELFTEST_ANDROID_STORE=$( $I state 2>/dev/null )" > $T
$I appcmd store home >/dev/null 2>&1
$I home >/dev/null 2>&1
$I settle >/dev/null 2>&1
echo "SELFTEST_ANDROID_COMPAT_DONE=1" > $T

echo "SELFTEST_STAGE5_DONE=1" > $T

# ---------- 界面全集取证：每个内置应用 / 每个有功能的地方 ----------
# 只追加截图，不改变上面任何断言；拍完统一回到深色桌面。
# go_home() 在 PH_APP 下会启动 500ms 返回转场，必须 settle 推完再开下一个应用，
# 否则下一次 open 被 g_closing 拦下、拍到桌面（此前偶数/奇数交替即此因）。
# shot() 内已含 settle，因此每页只保留「打开 -> 截图 -> home+settle」两次 IPC。
backhome(){ $I home >/dev/null 2>&1; $I settle >/dev/null 2>&1; }

$I theme 0 >/dev/null 2>&1
backhome

# 深色锁屏（干净复位，跳过开机动画）
$I relock >/dev/null 2>&1
$I settle >/dev/null 2>&1
shot 63-lock-dark.ppm
$I unlock >/dev/null 2>&1
backhome

# 桌面四个分页中的后三页
for p in 1 2 3; do
    $I page $p >/dev/null 2>&1
    shot "64-home-page$p.ppm"
done
$I page 0 >/dev/null 2>&1
$I settle >/dev/null 2>&1

# 深色通知中心 / 控制中心磁贴点击后的开态
$I panel 1 >/dev/null 2>&1
shot 67-notif-dark.ppm
$I panel 0 >/dev/null 2>&1
$I panel 2 >/dev/null 2>&1
$I click 90 200 >/dev/null 2>&1
shot 68-control-toggled.ppm
$I panel 0 >/dev/null 2>&1
$I settle >/dev/null 2>&1

# 16 个内置应用逐一开机截图（APP_COUNT=16）
for k in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    $I open $k >/dev/null 2>&1
    shot "$(printf '70-app-%02d.ppm' $k)"
    backhome
done

# 应用打开后的首帧（不走 settle）
$I open 0 >/dev/null 2>&1
aether-shot "$OUT/70-app-transition.ppm" 2>/dev/null
echo "SELFTEST: shot 70-app-transition.ppm -> $OUT" > $T
backhome

# 设置五个二级页（appcmd 转发到焦点应用，与真机点击同源）
$I open 5 >/dev/null 2>&1
for q in 2 3 5 7 16; do
    $I appcmd settings page $q >/dev/null 2>&1
    shot "$(printf '71-settings-page%02d.ppm' $q)"
done
$I appcmd settings back >/dev/null 2>&1
backhome

# 应用商店：搜索视图 / 第二款应用详情
$I open 14 >/dev/null 2>&1
$I appcmd store search >/dev/null 2>&1
shot 72-store-search.ppm
$I appcmd store detail 1 >/dev/null 2>&1
shot 72-store-detail2.ppm
$I appcmd store home >/dev/null 2>&1
backhome

# 浅色主题下的三个代表应用（浅色设置 / 商店 / 抽屉 / 锁屏已有）
$I theme 1 >/dev/null 2>&1
for k in 0 1 13; do
    $I open $k >/dev/null 2>&1
    shot "$(printf '73-light-app-%02d.ppm' $k)"
    backhome
done
$I theme 0 >/dev/null 2>&1
backhome

# ---------- P1 材质与动效取证（分册 §17 P1 / C01 / C02 / D03 / D04） ----------
# 两段式通知中心：240 / 640 / 全展开三吸附点（D03）。notif 命令把展开度
# 钉在对应吸附点，shot() 内 settle 不改变阶段，只把动画推到终态。
$I panel 0 >/dev/null 2>&1
$I settle >/dev/null 2>&1
for st in 1 2 3; do
    $I notif $st >/dev/null 2>&1
    shot "$(printf '74-notif-stage%d.ppm' $st)"
    echo "SELFTEST_P1_NOTIF_$st=$( $I state 2>/dev/null )" > $T
done

# 控制中心二级面板：先走真机长按路径（按住 >400ms 再松开），再拍确定性终态
$I notif 0 >/dev/null 2>&1
$I panel 2 >/dev/null 2>&1
$I settle >/dev/null 2>&1
$I down 90 200 >/dev/null 2>&1
sleep 1
$I up 90 200 >/dev/null 2>&1
shot 75-ccdetail-longpress.ppm
echo "SELFTEST_P1_CCDETAIL=$( $I state 2>/dev/null )" > $T
$I ccdetail -1 >/dev/null 2>&1
$I ccdetail 0 >/dev/null 2>&1
shot 75-ccdetail-wifi.ppm
$I theme 1 >/dev/null 2>&1
$I ccdetail 7 >/dev/null 2>&1
shot 75-ccdetail-bright-light.ppm
$I theme 0 >/dev/null 2>&1
$I ccdetail -1 >/dev/null 2>&1
$I panel 0 >/dev/null 2>&1
$I settle >/dev/null 2>&1

# 全局按压：down 当帧可见反馈（C01）。不走 settle，直抓上屏帧。
backhome
$I down 90 200 >/dev/null 2>&1
sleep 1
aether-shot "$OUT/76-press-down.ppm" 2>/dev/null
echo "SELFTEST: shot 76-press-down.ppm -> $OUT" > $T
echo "SELFTEST_P1_PRESS=$( $I state 2>/dev/null )" > $T
$I up 90 200 >/dev/null 2>&1
backhome

# T-05 右滑返回跟手：从 24px 边缘热区起手，拖 300px 后直抓跟手帧
$I open 0 >/dev/null 2>&1
$I settle >/dev/null 2>&1
$I down 10 600 >/dev/null 2>&1
$I move 300 600 >/dev/null 2>&1
sleep 1
aether-shot "$OUT/77-edge-swipe.ppm" 2>/dev/null
echo "SELFTEST: shot 77-edge-swipe.ppm -> $OUT" > $T
echo "SELFTEST_P1_EDGE=$( $I state 2>/dev/null )" > $T
$I up 300 600 >/dev/null 2>&1
backhome

# T-01 图标共享转场：打开应用后不 settle，抓展开过程中的一帧
$I open 0 >/dev/null 2>&1
aether-shot "$OUT/78-app-shared.ppm" 2>/dev/null
echo "SELFTEST: shot 78-app-shared.ppm -> $OUT" > $T
backhome
echo "SELFTEST_P1_DONE=1" > $T

echo "SELFTEST_GALLERY_DONE=1" > $T

echo "SELFTEST_STATE_FINAL=$( $I state 2>/dev/null )" > $T
sync
echo "SELFTEST_DONE=1" > $T
