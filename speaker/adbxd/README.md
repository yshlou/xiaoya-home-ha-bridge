# adbxd — 小雅 HOME 网络 ADB 守护进程

给这台 RK3308 / Buildroot 音箱补上**网络调试**：装好后无需 USB 线，
`adb connect <音箱IP>:5555` 即是 **root shell**。

**v1.2（2026-11-01）**：v1.1 部署后暴露的新缺陷全部修复，`adb push / pull`
（含目录双向）在真实 adb 1.0.41 客户端下字节级验证通过——

1. **快速输出随机截断**：子进程输出循环"先 read 再判 await_ok"，已读出但未确认的
   数据被直接丢弃（`echo a; echo b; cat f` 只出第一块）。改为先判再读，数据留管道。
2. **push 挂死 / stdin 丢数据**：客户端→设备 WRTE 里设备侧流号在 `a1`（`a0` 恒为
   发送方自己的 id），旧代码按 `a0` 匹配——双方流号同步时碰巧可用，adb server 流号
   漂移后（实测 47、403）所有客户端 WRTE 被丢。改按 `a1` 匹配。
3. **SIGPIPE 杀进程**：向已退出子进程写 stdin 会杀死整个连接处理进程。改为 SIG_IGN。
4. **CNXN max_data=4096**：客户端直接拒发长命令（`shell command too long`），
   64KB 命令缓冲形同虚设。改报 64KB，接收缓冲扩到 128KB。
5. **流式 sync 解析器**：DATA 帧负载直写磁盘，不再整帧缓冲——该客户端会流水线
   连发多个 64KB WRTE（不等逐包确认），固定缓冲必溢出（200KB push 实测翻车）。
6. **STAT 不存在路径回 FAIL** → 改回 STAT+全零（真实 adbd 行为；FAIL 会让客户端
   报 `stat response has wrong message id` 中止）。
7. **SEND/DONE 应答只发 4 字节** → 8 字节（客户端读完整 id+size 头，缺 4 字节
   会让 push 完成后客户端永不退出）。
8. **LIST 结束符 8 字节** → 20 字节（客户端按完整 DENT 头 20B 读帧，8 字节 DONE
   让递归 pull 永远等剩余 12 字节）；LIST/RECV 结束后不再关流（目录 pull 在同一
   sync 流上连续 RECV 多个文件）。
9. **secure_mkdirs**：SEND 自动创建缺失父目录（`adb push <目录> <目标>` 必需）。
10. **每流待发队列**：客户端流水线化导致我方上一包未 ack 时新回复到达——静默丢弃
    会让多文件目录 push 卡死。未确认时排队，OKAY 到达后按序冲刷。

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
5. 这台设备的原生 adbd 连 USB 通道的 sync 服务也被裁剪（push/pull 同样报
   protocol fault），所以 adbxd 是这台设备**唯一**可用的 push/pull 通道。

结论：**必须自己说 ADB 协议**，`adbxd.c` 就是这么做的。

## 目录

| 文件 | 说明 |
|---|---|
| `adbxd.c` | 网络 adb 守护进程源码（aarch64 musl 静态，69KB） |
| `adbxd` / `adbxd.aarch64-static` | 编译好的二进制（同一份，v1.2） |
| `adbnetd.c` | 早期尝试的纯转发方案（转 5555→127.0.0.1:5037），已证明不可行，留作记录 |
| `probe.py` | 原始 ADB 协议探针，用于握手排障 |

## 编译

```bat
zig cc -target aarch64-linux-musl -O2 -static -s -o adbxd adbxd.c
```

本机若无 zig：`python -m pip install ziglang==0.13.0`（PyPI 被限时用阿里云镜像
`-i https://mirrors.aliyun.com/pypi/simple`，速度约 4MB/s），
之后 `python -m ziglang cc ...` 亦可。v1.2 起 `adb push / pull` 已可用；
若手头还是旧版二进制，传文件用「PC 起 `python -m http.server` + 音箱 `wget -O`」
或纯 hex 分块 `printf '\x..'` 追加（无引号转义问题，约 68KB/6 秒）。

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

## 已支持 / 已知限制

- ✅ `adb shell <cmd>`、`adb shell`（交互式，无 PTY，所以 `vi`/`top` 这类要 tty 的程序不像）
- ✅ `adb push` / `adb pull` 单文件（含 200KB+ 大文件，字节级校验通过）
- ✅ `adb push <目录>` / `adb pull <目录>` 递归双向（自动建父目录）
- ✅ `adb shell cmd < file` stdin 数据转发（内容完整落盘）
- ✅ `adb exec-out`、`adb shell` 长命令（≤ 客户端 4096 限制，见下）
- ✅ root 权限（本身就是 root 子进程）、多并发连接、adb server 流号漂移免疫
- ⚠️ **超 4096 字符的 shell 命令**被 adb 客户端自身拒绝（`shell command too long`，
  客户端硬限制，与设备端 64KB 缓冲无关；实测 4000 过、4200 拒）
- ⚠️ **stdin-EOF 无信号**：legacy shell 协议（无 shell_v2）下 `adb shell cmd < file`
  数据全部到达，但客户端要等超时/手动杀才退出（老协议固有，真机老 adbd 同样如此）
- ❌ v2 sync / shell_v2 / abb（故意不报这些特性，客户端自动走 v1）
- ❌ PTY、端口转发（需要再说）

## 安全警告

**无认证 root shell。** 任何能访问该局域网 5555 端口的人都能拿到音箱 root。
放在可信网络里用；不需要时 `adbxd` 关掉即可（删掉 auto_reboot.sh 里的那一行）。

> AI生成
