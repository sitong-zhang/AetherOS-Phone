#include <stdio.h>
#include <math.h>
#include "aether-glass.h"
void aether_render_framebuffer(unsigned char **b,int *w,int *h,int *p){
    if(b)*b=0; if(w)*w=0; if(h)*h=0; if(p)*p=0;
}
static int pass=0, fail=0;
#define CK(c,m) do{ if(c){printf("  [OK] %s\n",m);pass++;}else{printf("  [FAIL] %s\n",m);fail++;} }while(0)
static float luma(float r,float g,float b){ return GLASS_LUMA_R*r+GLASS_LUMA_G*g+GLASS_LUMA_B*b; }
int main(void){
    printf("=== SDF / 折射（Kyant0 Shaders.kt）===\n");
    CK(glass_sd_rrect(0,0,50,50,16)<0,"中心 sd<0");
    CK(glass_sd_rrect(60,0,50,50,16)>0,"外部 sd>0");
    CK(fabsf(glass_circle_map(0.0f))<1e-6f,"circleMap(0)=0");
    CK(fabsf(glass_circle_map(1.0f)-1.0f)<1e-5f,"circleMap(1)=1");
    CK(glass_refraction_d(-100,12,24)==0.0f,"深处不折射");
    CK(glass_refraction_d(-1,12,24)>0.0f,"边缘有折射");
    printf("=== 高光角分离（修正 45° vs 135°）===\n");
    float a45=45.0f*3.14159265f/180.0f,a135=135.0f*3.14159265f/180.0f;
    CK(GLASS_HIGHLIGHT_ANGLE_DEG==45.0f&&GLASS_LIGHT_ANGLE_DEG==135.0f,"两角已分离");
    CK(glass_highlight(0.7071f,0.7071f,a45,1.0f)>glass_highlight(0.7071f,0.7071f,a135,1.0f),
       "45° 高光对 45° 法线更强");
    CK(GLASS_HIGHLIGHT_ALPHA==0.5f,"高光 alpha=.5");
    printf("=== 参数出处修正 ===\n");
    CK(GLASS_REFRACTION_H==12.0f&&GLASS_REFRACTION_A==24.0f,"H/A=12/24 来自 LiquidButton.kt");
    CK(GLASS_BLUR_DEFAULT==3.0f,"blur=3 来自 glass_defaults.dart");
    CK(GLASS_CHROMATIC_ABERRATION==0.5f&&GLASS_REFRACTIVE_INDEX==1.15f,"CA/index 已补齐");
    printf("=== 亮度保持饱和度（applySaturation）===\n");
    float r=100,g=150,b=200;float L0=luma(r,g,b);
    glass_saturate(&r,&g,&b,1.5f);
    CK(fabsf(luma(r,g,b)-L0)<0.01f,"饱和度保持亮度");
    printf("=== 亮度保持着色（applyGlassColor，本次补实现）===\n");
    /* 无色灰玻璃 chroma=0 → 纯 directMix */
    float gr=140,gg=140,gb=140;
    glass_apply_glass_color(&gr,&gg,&gb,128,128,128,0.5f);
    CK(fabsf(gr-134.0f)<0.5f&&fabsf(gg-134.0f)<0.5f&&fabsf(gb-134.0f)<0.5f,
       "无色玻璃 chroma=0 走 directMix=mix(140,128,.5)=134");
    /* 彩色红玻璃 chroma=255 → luminosityMix，绿/蓝通道与 directMix 不同 */
    float cr=100,cg=150,cb=200;
    glass_apply_glass_color(&cr,&cg,&cb,255,0,0,0.3f);
    float direct_g=150+(0-150)*0.3f;   /* 105 */
    CK(fabsf(cg-direct_g)>3.0f,"彩色玻璃走 luminosityMix（绿通道偏离 directMix）");
    CK(luma(cr,cg,cb)>100.0f&&luma(cr,cg,cb)<200.0f,"亮度保持未塌成纯红");
    printf("=== 7 段色散权重 ===\n");
    const struct glass_disp_tap *d=glass_disp_taps();
    float sr=0,sg=0,sb=0;for(int i=0;i<GLASS_DISP_TAPS;i++){sr+=d[i].wr;sg+=d[i].wg;sb+=d[i].wb;}
    CK(GLASS_DISP_TAPS==7,"7 段");
    CK(sr>0.9f&&sr<1.1f&&sg>0.9f&&sg<1.1f&&sb>0.9f&&sb<1.1f,"三通道权重和≈1");
    printf("=== 组件规范常量（spec.h 照搬）===\n");
    CK(GLASS_RADIUS==16.0f&&GLASS_RADIUS_SMALL==8.0f&&GLASS_RADIUS_LARGE==20.0f,"圆角 16/8/20");
    CK(GLASS_H_BUTTON==48.0f&&GLASS_H_CONTROL==32.0f,"高度 48/32");
    CK(GLASS_SHADOW_ELEV_BLUR==8&&GLASS_SHADOW_CONTACT_BLUR==2,"双层阴影 blur 8/2");
    CK(GLASS_BARRIER_ARGB==0x8A000000u,"遮罩 54% 黑");
    CK(glass_role_allows_glass(GLASS_ROLE_NAV)==1&&glass_role_allows_glass(GLASS_ROLE_CONTENT)==0,"层级规则");
    CK(glass_role_can_refract(GLASS_ROLE_NAV,1)==0,"Rule2 禁止嵌套折射");
    printf("\n=== RESULT: %d PASS / %d FAIL ===\n",pass,fail);
    return fail?1:0;
}
