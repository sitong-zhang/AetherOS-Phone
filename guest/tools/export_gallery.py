#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 QEMU TCG 真机取证帧按界面顺序导出到工作区，命名为「系统图片（N）.png」。

来源：/workspace/aether-vm/out/run-v6/frames/*.png（720x1280 上屏帧）
仅做无损拷贝与编号，不二次压缩、不改动像素。
"""
import os
import shutil
import sys

SRC = '/workspace/aether-vm/out/run-v6/frames'
DST = '/workspace'
INDEX = '/workspace/系统图片-目录.txt'

# (分组, 源帧名（不含扩展名）, 界面说明)
ITEMS = [
    # ---------- 开机与锁屏 ----------
    ('开机与锁屏', '60-boot-1',          '开机动画·暗场光晕 / 环形光点 / 品牌显影'),
    ('开机与锁屏', '61-boot-2',          '开机动画·光点游走 / 进度线推进'),
    ('开机与锁屏', '62-boot-3',          '开机动画收尾 → 进入锁屏'),
    ('开机与锁屏', '63-lock-dark',       '深色锁屏'),
    ('开机与锁屏', '50-lock-light',      '浅色锁屏'),

    # ---------- 桌面与分页 ----------
    ('桌面与分页', '02-home',            '深色桌面（第一页）'),
    ('桌面与分页', '64-home-page1',      '桌面第二页'),
    ('桌面与分页', '64-home-page2',      '桌面第三页'),
    ('桌面与分页', '64-home-page3',      '桌面第四页'),
    ('桌面与分页', '26-home-light',      '浅色桌面'),

    # ---------- 应用抽屉与安装前后 ----------
    ('抽屉与安装', '06-drawer',          '深色应用抽屉'),
    ('抽屉与安装', '53-drawer-light',    '浅色应用抽屉'),
    ('抽屉与安装', '34-drawer-notes',    '安装笔记后的抽屉'),
    ('抽屉与安装', '33-home-notes-installed', '安装笔记后的桌面'),
    ('抽屉与安装', '40-home-after-uninstall',  '卸载笔记后的桌面'),

    # ---------- 通知 / 控制 / 负一屏 / 隐私 ----------
    ('通知·控制·负一屏·隐私', '67-notif-dark',       '深色通知中心'),
    ('通知·控制·负一屏·隐私', '51-notif-light',      '浅色通知中心'),
    ('通知·控制·负一屏·隐私', '04-control-center',   '深色控制中心'),
    ('通知·控制·负一屏·隐私', '52-control-light',    '浅色控制中心'),
    ('通知·控制·负一屏·隐私', '68-control-toggled',  '控制中心磁贴点击后的开态'),
    ('通知·控制·负一屏·隐私', '22-neg1-dark',        '深色负一屏'),
    ('通知·控制·负一屏·隐私', '25-neg1-light',       '浅色负一屏'),
    ('通知·控制·负一屏·隐私', '16-privacy-dots',     '隐私指示点（相机 / 定位）'),
    ('通知·控制·负一屏·隐私', '17-privacy-panel',    '隐私使用面板'),

    # ---------- 全局搜索 ----------
    ('全局搜索', '18-search-suggest',    '搜索建议 / 默认态'),
    ('全局搜索', '19-search-file',       '搜索「文件」结果'),
    ('全局搜索', '20-search-setting',    '搜索「显示」结果'),
    ('全局搜索', '21-search-empty',      '无结果空状态'),
    ('全局搜索', '24-search-light',      '浅色搜索'),

    # ---------- 灵动岛 ----------
    ('灵动岛', '10-island-collapsed',    '收起态'),
    ('灵动岛', '11-island-expanded',     '展开态'),
    ('灵动岛', '12-island-card',         '卡片态'),
    ('灵动岛', '13-island-music',        '场景·音乐播放'),
    ('灵动岛', '13-island-timer',        '场景·计时器'),
    ('灵动岛', '13-island-record',       '场景·录音'),
    ('灵动岛', '13-island-charge',       '场景·充电'),
    ('灵动岛', '13-island-nav',          '场景·导航'),
    ('灵动岛', '14-island-primary-sub',  '主活动 + 副活动并存'),
    ('灵动岛', '15-island-swapped',      '点击互换主副活动'),
    ('灵动岛', '42-island-paused',       '卡片按钮暂停 / 计时冻结'),
    ('灵动岛', '54-island-collapsed-light', '浅色·收起态'),
    ('灵动岛', '23-island-light',        '浅色·展开态'),
    ('灵动岛', '55-island-card-light',   '浅色·卡片态'),

    # ---------- 16 个内置应用 ----------
    ('内置应用（深色）', '70-app-00', '文件管理器'),
    ('内置应用（深色）', '70-app-01', '终端'),
    ('内置应用（深色）', '70-app-02', '文本编辑器'),
    ('内置应用（深色）', '70-app-03', '计算器'),
    ('内置应用（深色）', '70-app-04', '系统监视器'),
    ('内置应用（深色）', '70-app-05', '设置（一级页）'),
    ('内置应用（深色）', '70-app-06', '关于本机'),
    ('内置应用（深色）', '70-app-07', '系统模块清单'),
    ('内置应用（深色）', '70-app-08', '浏览器'),
    ('内置应用（深色）', '70-app-09', '图片查看器'),
    ('内置应用（深色）', '70-app-10', '视频播放器'),
    ('内置应用（深色）', '70-app-11', '音乐播放器'),
    ('内置应用（深色）', '70-app-12', '时钟（闹钟·计时器·秒表）'),
    ('内置应用（深色）', '70-app-13', '日历'),
    ('内置应用（深色）', '70-app-14', '应用商店'),
    ('内置应用（深色）', '70-app-15', '笔记'),

    # ---------- 设置二级页 ----------
    ('设置二级页', '71-settings-page02', '网络与连接'),
    ('设置二级页', '71-settings-page03', '显示'),
    ('设置二级页', '71-settings-page05', '电池与性能'),
    ('设置二级页', '71-settings-page07', '应用管理'),
    ('设置二级页', '71-settings-page16', '关于手机'),

    # ---------- 应用商店 ----------
    ('应用商店', '30-store-home',      '商店首页'),
    ('应用商店', '31-store-detail',    '应用详情'),
    ('应用商店', '72-store-detail2',   '第二款应用详情'),
    ('应用商店', '72-store-search',    '商店搜索'),
    ('应用商店', '41-store-android',   '安卓应用分类（pKVM 不可用时的降级提示）'),
    ('应用商店', '57-store-light',     '浅色商店'),

    # ---------- 多任务 / 小窗 / 安装闭环 ----------
    ('多任务与小窗', '35-recents',        '深色多任务'),
    ('多任务与小窗', '58-recents-light',  '浅色多任务'),
    ('多任务与小窗', '36-float-browser',  '小窗浏览器'),
    ('多任务与小窗', '37-float-second',   '第二次小窗请求被拦截（仍只有一个）'),
    ('多任务与小窗', '59-float-light',    '浅色小窗'),
    ('多任务与小窗', '32-notes-opened',   '商店安装后首开笔记'),

    # ---------- 息屏显示 / 交互 ----------
    ('息屏与交互', '38-aod',            '息屏显示 AOD（开启，极低亮度暗显）'),
    ('息屏与交互', '39-aod-off-black',  'AOD 关闭后息屏全黑（功能验证帧）'),
    ('息屏与交互', '03-app-open',       '桌面点击图标打开应用'),
    ('息屏与交互', '56-settings-light', '浅色设置（96px 大标题一级页）'),

    # ---------- 浅色应用补充 ----------
    ('浅色应用补充', '73-light-app-00', '浅色·文件管理器'),
    ('浅色应用补充', '73-light-app-01', '浅色·终端'),
    ('浅色应用补充', '73-light-app-13', '浅色·日历'),
]


def main():
    missing = [s for _, s, _ in ITEMS
               if not os.path.isfile(os.path.join(SRC, s + '.png'))]
    if missing:
        print('缺少取证帧:', ', '.join(missing), file=sys.stderr)
        return 1

    lines = [
        'AetherOS 手机版 · 系统图片对照表',
        '来源：QEMU TCG 真机开机取证帧，720x1280 原始上屏像素，无损拷贝',
        '说明：文件按界面顺序编号，括号内数字即查看顺序',
        '',
    ]
    last = None
    files = []
    for i, (group, src, desc) in enumerate(ITEMS, 1):
        out = os.path.join(DST, '系统图片（%d）.png' % i)
        shutil.copyfile(os.path.join(SRC, src + '.png'), out)
        files.append(out)
        if group != last:
            lines.append('【%s】' % group)
            last = group
        lines.append('  系统图片（%d）.png   %s   <- %s.png' % (i, desc, src))
    with open(INDEX, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('导出 %d 张到 %s' % (len(files), DST))
    print('对照表: %s' % INDEX)
    return 0


if __name__ == '__main__':
    sys.exit(main())
