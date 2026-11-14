#!/bin/sh
# secure-selftest.sh - A 轨开机安全自检
#
# 在真 Linux aarch64 内核上运行一次安全引擎设备自证，校验系统摘要锚与签名，
# 输出可机检的完成标记。运行根置于 tmpfs /run，自检不留持久数据；
# 完整自证报告保留在 /run 供工程取证。
R=/run/aether/secure-probe
OUT=/run/aether/secure-attest.json
mkdir -p "$R"

/sbin/aether-os --root "$R" attest > "$OUT" 2>&1

sig=$(sed -n 's/.*"signature": "\([0-9a-f]*\)".*/\1/p' "$OUT" | cut -c1-16)
sys=$(sed -n 's/.*"sys_digest": "\([0-9a-f]*\)".*/\1/p' "$OUT" | cut -c1-16)

if grep -q '"type": "AETHER_DEVICE_ATTESTATION"' "$OUT" && \
   grep -qE '"signature": "[0-9a-f]{64}"' "$OUT" && \
   [ -n "$sig" ] && [ -n "$sys" ]; then
    echo "AETHEROS_SECURE_SELFTEST: arch=$(uname -m) kernel=$(uname -r) attest=OK sig=${sig} sys=${sys}"
    echo "AETHEROS_SECURE_READY=1"
else
    echo "AETHEROS_SECURE_SELFTEST: FAIL 设备自证未通过"
fi
