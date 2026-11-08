/* Aether VM — JNI 桥（自研）
 * 1) 把 core/vm_config.c 组装的 QEMU argv 交给 Java 层；
 * 2) 用 core/vm_runtime.c 的 dlopen 后端在引擎线程里启动 QEMU。
 * 与宿主端验证器共用同一份 core 源码，不另写一套参数/驱动。 */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "vm_config.h"
#include "vm_runtime.h"

static aether_vm_t *g_vm = NULL;

static void fill(aether_vm_config_t *c, const char *path)
{
    aether_vm_config_defaults(c);
    aether_vm_config_load(c, path);   /* 读不到就保留默认值 */
}

JNIEXPORT jobjectArray JNICALL
Java_dev_aether_vm_VmConfig_buildArgs(JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    aether_vm_config_t cfg;
    fill(&cfg, path);

    char *argv[AETHER_VM_ARGV_MAX];
    char scratch[AETHER_VM_SCRATCH];
    int argc = aether_vm_build_argv(&cfg, argv, AETHER_VM_ARGV_MAX,
                                    scratch, sizeof(scratch));
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (argc <= 0) return NULL;

    /* SDL2 的 nativeRunMain() 会自行补 argv[0]（程序名），再把本数组整体
     * 依次塞进 argv[1..]。所以这里必须只返回「选项部分」argv[1..argc-1]。
     * 若把程序名 "qemu-system-aarch64" 也返回，QEMU 会把它当成一个多余的
     * 位置参数，导致启动失败/黑屏。宿主 EXEC 后端仍用完整 argv（execvp 需要 argv[0]）。 */
    int optc = argc - 1;
    if (optc <= 0) return NULL;
    jclass strCls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, optc, strCls, NULL);
    for (int i = 1; i < argc; i++)
        (*env)->SetObjectArrayElement(env, arr, i - 1,
                                      (*env)->NewStringUTF(env, argv[i]));
    return arr;
}

JNIEXPORT jstring JNICALL
Java_dev_aether_vm_VmConfig_summary(JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    aether_vm_config_t cfg;
    fill(&cfg, path);
    char buf[512];
    aether_vm_config_dump(&cfg, buf, sizeof(buf));
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return (*env)->NewStringUTF(env, buf);
}

/* ---------------- VmEngine.start / stop ---------------- */
JNIEXPORT jint JNICALL
Java_dev_aether_vm_VmEngine_start(JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);

    aether_vm_config_t cfg;
    aether_vm_config_defaults(&cfg);
    aether_vm_config_load(&cfg, path);
    (*env)->ReleaseStringUTFChars(env, jpath, path);

    /* Android 端必须走 dlopen 后端（引擎已由 SDL glue 加载进进程） */
    cfg.backend = AETHER_BACKEND_DLOPEN;
    cfg.enable_qmp = 0;                 /* 无 QMP socket，避免干扰 */
    snprintf(cfg.display, sizeof(cfg.display), "sdl");
    if (!cfg.engine[0])
        snprintf(cfg.engine, sizeof(cfg.engine), "libqemu-system-aarch64.so");

    if (g_vm) { aether_vm_stop(g_vm); aether_vm_destroy(g_vm); g_vm = NULL; }
    g_vm = aether_vm_create(&cfg);
    if (!g_vm) return -1;
    int rc = aether_vm_start(g_vm);
    if (rc != 0) {
        /* 引擎 .so 已由 loadLibraries 载入：dlopen 同名会返回已加载句柄 */
        return rc;
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_dev_aether_vm_VmEngine_stop(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_vm) {
        aether_vm_stop(g_vm);
        aether_vm_destroy(g_vm);
        g_vm = NULL;
    }
}

/* ---------------- VmEngine.captureStdio ----------------
 * QEMU 在 Android 上默认把 stderr 丢到 /dev/null，报错看不见。
 * 这里把 stdout/stderr 重定向到应用私有目录，诊断浮层读它显示引擎错误。 */
JNIEXPORT jint JNICALL
Java_dev_aether_vm_VmEngine_captureStdio(JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (fd < 0) return -2;
    fflush(NULL);
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        close(fd);
        return -3;
    }
    if (fd > STDERR_FILENO) close(fd);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return 0;
}
