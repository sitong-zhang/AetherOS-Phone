package com.aetheros.terminal;

import android.util.Base64;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;

/**
 * 手写 RFC6455 WebSocket ⇄ VNC TCP 桥（零第三方依赖，移植自已验证的 Python 版）。
 *
 * 握手后：
 *  - 线程 A：WS 帧解码 → VNC 原始字节
 *  - 线程 B：VNC 原始字节 → WS 二进制帧
 */
public class WsBridge {
    private static final String WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    private final Socket client;       // 浏览器 WebSocket
    private final String wsKey;        // 已由 HTTP 层解析出的 Sec-WebSocket-Key
    private final String vncHost;
    private final int vncPort;
    private final AetherCore core;

    public WsBridge(Socket client, String wsKey, String vncHost, int vncPort, AetherCore core) {
        this.client = client;
        this.wsKey = wsKey;
        this.vncHost = vncHost;
        this.vncPort = vncPort;
        this.core = core;
    }

    private static byte[] readExact(InputStream in, int n) throws IOException {
        byte[] buf = new byte[n];
        int off = 0;
        while (off < n) {
            int r = in.read(buf, off, n - off);
            if (r < 0) throw new IOException("连接关闭");
            off += r;
        }
        return buf;
    }

    public void run() {
        try {
            // 长连接：清除 HTTP 层的读超时
            client.setSoTimeout(0);
            client.setTcpNoDelay(true);
            InputStream in = client.getInputStream();
            OutputStream out = client.getOutputStream();

            if (wsKey == null) {
                core.log("WS 握手缺少 Sec-WebSocket-Key");
                return;
            }
            String accept = Base64.encodeToString(
                    sha1((wsKey + WS_GUID).getBytes(StandardCharsets.ISO_8859_1)),
                    Base64.NO_WRAP);
            String resp = "HTTP/1.1 101 Switching Protocols\r\n"
                    + "Upgrade: websocket\r\n"
                    + "Connection: Upgrade\r\n"
                    + "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
            out.write(resp.getBytes(StandardCharsets.ISO_8859_1));
            out.flush();

            Socket vsock = new Socket();
            vsock.setTcpNoDelay(true);
            vsock.connect(new java.net.InetSocketAddress(vncHost, vncPort), 5000);
            final long t0 = System.currentTimeMillis();
            core.log("bridge: 已连接 VNC " + vncHost + ":" + vncPort);

            final OutputStream vout = vsock.getOutputStream();
            final InputStream vin = vsock.getInputStream();

            // 任一方向死亡都要关 vsock：旧实现只有 c->v 关，QEMU 先断时
            // vsock 永久悬着（fd 泄漏 + QEMU 端僵尸连接堆积）。
            final Socket fsock = vsock;

            // 看门狗：QEMU 的 VNC 服务挂死时，连接能建立但永远不吐数据
            // （真机复现过：端口在、isBooted 为真、但 0 字节被 reset）。
            // 12 秒无首字节就自动重启 QEMU 自愈。
            final java.util.concurrent.atomic.AtomicLong received =
                    new java.util.concurrent.atomic.AtomicLong(0);
            final Socket wsock = vsock;
            Thread watchdog = new Thread(new Runnable() {
                @Override
                public void run() {
                    try { Thread.sleep(12000); } catch (InterruptedException e) { return; }
                    if (received.get() == 0 && !wsock.isClosed()) {
                        core.log("bridge: 12 秒未收到 QEMU 任何数据 —— VNC 服务挂死");
                        core.restartQemu("连接后无数据");
                        closeQuietly(fsock);
                    }
                }
            }, "ws-watchdog");
            watchdog.setDaemon(true);
            watchdog.start();

            Thread t1 = new Thread(new Runnable() {
                @Override
                public void run() {
                    long n = 0;
                    try {
                        while (true) {
                            byte[] h = readExact(in, 2);
                            int opcode = h[0] & 0x0F;
                            boolean masked = (h[1] & 0x80) != 0;
                            long len = h[1] & 0x7F;
                            if (len == 126) {
                                byte[] e = readExact(in, 2);
                                len = ((e[0] & 0xFF) << 8) | (e[1] & 0xFF);
                            } else if (len == 127) {
                                byte[] e = readExact(in, 8);
                                len = 0;
                                for (int i = 0; i < 8; i++) len = (len << 8) | (e[i] & 0xFF);
                            }
                            byte[] mask = null;
                            if (masked) mask = readExact(in, 4);
                            byte[] data = len > 0 ? readExact(in, (int) len) : new byte[0];
                            if (masked && data.length > 0) {
                                for (int i = 0; i < data.length; i++) data[i] ^= mask[i % 4];
                            }
                            if (opcode == 0x8) {
                                core.log("bridge: 浏览器正常关闭");
                                break;
                            }
                            if (opcode == 0x9) {                    // ping → pong
                                synchronized (out) {
                                    out.write(encode(0xA, data));
                                    out.flush();
                                }
                                continue;
                            }
                            if (opcode == 0xA) continue;            // pong
                            if (opcode == 0x1 || opcode == 0x2) {
                                n += data.length;
                                vout.write(data);
                                vout.flush();
                            }
                        }
                        core.log("bridge c->v 正常结束，共上传 " + n + " 字节");
                    } catch (IOException e) {
                        core.log("bridge c->v 结束（已上传 " + n + " 字节）: " + e.getMessage());
                    } finally {
                        closeQuietly(fsock);
                    }
                }
            }, "ws-c2v");

            Thread t2 = new Thread(new Runnable() {
                @Override
                public void run() {
                    long total = 0;
                    try {
                        byte[] buf = new byte[65536];
                        while (true) {
                            int n = vin.read(buf);
                            if (n < 0) break;
                            received.addAndGet(n);
                            if (total == 0) {
                                // 首字节诊断：QEMU 的 VNC 服务挂死时，连接能建立
                                // 但永远等不到版本串——这个数字能直接定位。
                                core.log("bridge: 收到 QEMU 首字节（等了 "
                                        + (System.currentTimeMillis() - t0) + "ms，共 "
                                        + n + " 字节）");
                            }
                            total += n;
                            synchronized (out) {
                                out.write(encode(0x2, Arrays.copyOf(buf, n)));
                                out.flush();
                            }
                        }
                        core.log("bridge v->c 结束，共转发 " + total + " 字节");
                    } catch (IOException e) {
                        core.log("bridge v->c 结束（已转发 " + total + " 字节）: " + e.getMessage());
                    } finally {
                        closeQuietly(fsock);
                    }
                }
            }, "ws-v2c");

            t1.start();
            t2.start();
            t1.join();
            t2.join();
        } catch (InterruptedException e) {
            // 服务关停时的正常中断
        } catch (Exception e) {
            core.log("WS 桥接异常: " + e);
        } finally {
            closeQuietly(client);
        }
    }

    private static byte[] encode(int opcode, byte[] payload) {
        int len = payload.length;
        byte[] head;
        if (len < 126) {
            head = new byte[]{(byte) (0x80 | opcode), (byte) len};
        } else if (len < 65536) {
            head = new byte[4];
            head[0] = (byte) (0x80 | opcode);
            head[1] = 126;
            head[2] = (byte) (len >> 8);
            head[3] = (byte) len;
        } else {
            // 8 字节大端长度。必须用 long 右移：len 是 int，Java 的 int 移位位数
            // 按 32 取模——`len >> 56` 实际等于 `len >> 24`，写出的长度是垃圾
            // （65536 会变成声称 4.29GB 载荷），Chromium 直接失败整条连接，
            // 且不吐 close 帧。真机 phone-v18「一直重连」的扳机点之一：
            // QEMU 推 4.6MB 首帧时 read 时不时恰好返回 64KB，帧必 corrupt。
            head = new byte[10];
            head[0] = (byte) (0x80 | opcode);
            head[1] = 127;
            long v = len & 0xFFFFFFFFL;
            for (int i = 0; i < 8; i++) head[2 + i] = (byte) (v >>> (56 - 8 * i));
        }
        byte[] r = new byte[head.length + len];
        System.arraycopy(head, 0, r, 0, head.length);
        System.arraycopy(payload, 0, r, head.length, len);
        return r;
    }

    private static byte[] sha1(byte[] input) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-1");
            return md.digest(input);
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    private static void closeQuietly(Socket s) {
        try { s.close(); } catch (IOException ignored) {}
    }
}
