# adbxd — 小雅 HOME 网络 ADB 守护进程

给这台 RK3308 / Buildroot 音箱补上**网络调试**：装好后无需 USB 线，
`adb connect <音箱IP>:5555` 即是 **root shell**。

**v1.1（2026-10-31）**：修复三个生产环境实测缺陷——

1. **长命令截断**：OPEN 目标串缓冲只有 512 字节，超过 511 字符的 shell 命令被静默砍断
   （甚至可能把引号腰斩，`sh -c` 报 unterminated quoted string）。现扩到 64KB。
2. **adb push/pull 不可用**：`sync:` 服务未实现，OPEN 被回 OKAY+CLSE，客户端报
   `protocol fault: failed to read stat response`；且特性串谎报 stat_v2/sendrecv_v2
   等特性诱导客户端用 v2 帧。现实现完整 **sync v1**（STAT/SEND/RECV/LIST/QUIT），
   特性串改为不报任何 v2，客户端自动回退 v1。
3. **stdin 转发挂死**：WRTE 的确认帧 OKAY(a0,a1) 参数写反（把客户端的 a0/a1 原样回），
   adb 客户端无法解析确认，`adb shell cmd < file` 永远卡死；同时自身发 WRTE 从不等
   OKAY（无流控）。现已修正交换并加每流 await_ok 流控。

## 为什么不能用原生 adbd

`/usr/sbin/adbd`（android-tools 4.2.2 改版）经实测有这些事实：

1. **只绑 USB FunctionFS**。`adb tcpip 5555` 会打印 `restarting in TCP mode port: 5555`，
   然后进程直接退出，**从未绑定 5555**；随后 `/oem/watch_adb_status.sh`（每 8s 轮询 UDC）
   发现 UDC 为空会把 USB 拉回来 —— 这正是 5555 永远起不来的原因。
2. 它确实读 `persist.adb.tcp.port` 字符串存在，但本机**没有 property 后端**
   （无 `getprop`，`libcutils.so` 里也没有 prop 文件实现），所以该属性恒为空。
3. 以下方式全部试过，无效（进程立即退出或直连被断）：
   裸跑 `adbd`、`adbd 5555`、`service.adb.tcp.port=5555 adbd`、
   `ADB_LOCAL_TRANSPORT_PORT=5555 adbd`、先断 UDC 再启 adbd。
4. adbd 确实在 **127.0.0.1:5037** 开了监听（inode 与 adbd 的 fd 一致），
   但它**不是可用的客户端传输口**：转发过去后一发 CNXN 就被断开。

结论：**必须自己说 ADB 协议**，`adbxd.c` 就是这么做的。

## 目录

| 文件 | 说明 |
|---|---|
| `adbxd.c` | 网络 adb 守护进程源码（aarch64 musl 静态，47KB） |
| `adbxd` / `adbxd.aarch64-static` | 编译好的二进制（同一份） |
| `adbnetd.c` | 早期尝试的纯转发方案（转 5555→127.0.0.1:5037），已证明不可行，留作记录 |
| `probe.py` | 原始 ADB 协议探针，用于握手排障 |

## 编译

```bat
zig cc -target aarch64-linux-musl -O2 -static -s -o adbxd adbxd.c
```

本机若无 zig：`python -m pip install ziglang==0.13.0`（PyPI 被限时用阿里云镜像
`-i https://mirrors.aliyun.com/pypi/simple`，速度约 4MB/s），
之后 `python -m ziglang cc ...` 亦可。v1.1 起 `adb push / pull` 已可用；
若手头还是旧版二进制，传文件用「PC 起 `python -m http.server` + 音箱 `wget -O`」
或另一台能 USB 直连的电脑。

## 部署

```bat
adb shell "mkdir -p /userdata/local-mqtt"
:: 用 wget 拉二进制，或 adb push 可用时直接 push
adb shell "cd /userdata/local-mqtt && chmod 755 adbxd"
adb shell "setsid /userdata/local-mqtt/adbxd -q >> /userdata/local-mqtt/adbxd.log 2>&1 </dev/null &"
adb connect 192.168.0.185:5555
adb -s 192.168.0.185:5555 shell id      :: uid=0(root)
```

开机自启写在 `/userdata/cfg/rockchip_test/auto_reboot.sh`（由 `S98_lunch_init` source），
内容为幂等地拉起 adbxd。**注意该脚本是被 source 的，里面不能有 exit。**

## 参数

```
-l <port>  监听端口，默认 5555
-q         静默（默认打印协议日志，排障时很有用）
```

## 已支持 / 未支持

- ✅ `adb shell <cmd>`、`adb shell`（交互式，无 PTY，所以 `vi`/`top` 这类要 tty 的程序不像）
- ✅ `adb push` / `adb pull` / 目录 LIST（sync v1；STAT/SEND/RECV/LIST/QUIT 全套，
  带每流 OKAY 流控，DONE 后恢复文件 mtime）
- ✅ `adb shell cmd < file`（stdin 转发 + 正确的 WRTE 确认交换）
- ✅ root 权限（本身就是 root 子进程）
- ✅ 多个并发连接；已验证 adb 1.0.41 与老 adb(v36) 客户端均可用
- ❌ v2 sync / shell_v2 / abb（故意不报这些特性，客户端自动走 v1）
- ❌ PTY、端口转发（需要再说）

## 安全警告

**无认证 root shell。** 任何能访问该局域网 5555 端口的人都能拿到音箱 root。
放在可信网络里用；不需要时 `adbxd` 关掉即可（删掉 auto_reboot.sh 里的那一行）。

> AI生成
