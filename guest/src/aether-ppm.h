/*
 * aether-ppm.h - AetherOS 真实像素图解码（PPM P6）
 *
 * 供内置应用（图片查看器 / 视频播放器）与手机外壳（桌面壁纸）共用，
 * 保证「设为壁纸」等设置改动在桌面真实生效，而不是只改一个配置项。
 * 实现位于 aether-apps2.c。
 */
#ifndef AETHER_PPM_H
#define AETHER_PPM_H

/* 解码 PPM P6 为 RGB888 连续缓冲；调用方 free(*rgb)。成功返回 1。 */
int  aether_ppm_read(const char *path, unsigned char **rgb, int *w, int *h);
/* 把 RGB888 缓冲按 contain 等比缩放绘制到目标矩形；step 为抽样块边长。 */
void aether_ppm_draw(unsigned char *rgb, int sw, int sh,
                     int dx, int dy, int dw, int dh, int step);

#endif /* AETHER_PPM_H */
