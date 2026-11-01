# WAN block — 无 iptables 固件的内核级断外网方案

> 适用场景：嵌入式 buildroot 设备（本仓库的音箱固件即如此）想彻底切断
> 对外网访问，但固件里 **iptables 不可用**。这是一个纯用户态、可逆、
> 开机自动生效的替代方案，实测有效。

## 为什么需要它

停用厂商云服务栈（`docs/deploy.md` 的 XYOS offline 一节）之后，设备理论上
已经没有外联组件。但"现在没有"不等于"永远没有"：固件升级、厂商远程开关、
未知的唤醒逻辑都可能让流量再冒出来。**在网络层物理封死**是独立于进程存活
状态的第二道保险——无论哪个进程想出网，包都出不了网卡。

## 为什么不能用 iptables

在这台固件上实测：

```
# iptables -L
iptables v1.6.1: can't initialize iptables table `filter': Table does not exist
modprobe: can't change directory to '/lib/modules': No such file or directory
```

内核没有编入 filter 表，`/lib/modules` 整个不存在，模块化加载无从谈起。
`iptables`/`ip6tables` 二进制虽在，但只是空壳。

## 方案：路由层 egress block

### 原理

内核 FIB 按「最长前缀匹配优先，同前缀比 metric（值小者优先）」选路。
DHCP 客户端（本机是 dhcpcd）装好的默认网关路由带 metric 302，因此：

```
ip route add prohibit default metric 1
```

一条 metric 1 的 `prohibit` 默认路由会**优先于网关路由**被命中——所有目的
地址不在任何具体路由（局域网链路路由、组播路由）里的出站包，内核直接向
应用返回 `EACCES`（Permission denied），**数据包根本不会被发到网卡**。
比 DROP 更干脆：连包都不构造出去。

同时显式补一条组播链路路由，避免 mDNS/SSDP 这类局域网发现协议被误伤
（组播地址 224.0.0.0/4 本不在网关路由内，但显式声明最稳）：

```
ip route add 224.0.0.0/4 dev wlan0
```

DHCP 续约走 255.255.255.255 链路层广播，不经过默认路由，不受影响。

### IPv6 直接禁用

路由级封堵在 IPv4 上可靠，在 IPv6 上**守不住**：路由器通告（RA）会周期性
重装运营商默认路由，且其 metric 可能与你的 prohibit 路由相同（实测本机同为
metric 1 时 FIB 选择了网关路由，公网 v6 仍通）。反复对抗不如整体关闭：

```
echo 1 > /proc/sys/net/ipv6/conf/wlan0/disable_ipv6
```

前提是设备没有 v6 依赖（本机 MQTT broker / HTTP 歌单 / adb 全部走 IPv4，
DNS 首选也是局域网网关）。有 v6 依赖的设备请改用路由器侧封堵。

### 落地到开机自启

把下面这段追加进开机钩子（模板见 `speaker/auto_reboot.sh` 第 3 段；
本机钩子路径 `/userdata/cfg/rockchip_test/auto_reboot.sh`，由
`/etc/init.d/S98_lunch_init` source 执行）：

```sh
# ---- WAN block section (delete this block to restore) ----
IFACE=wlan0
ip route add prohibit default metric 1 2>/dev/null      # 已存在时报错忽略
ip route replace prohibit default metric 1 2>/dev/null  # 幂等兜底
ip route add 224.0.0.0/4 dev $IFACE 2>/dev/null
echo 1 > /proc/sys/net/ipv6/conf/$IFACE/disable_ipv6 2>/dev/null
```

幂等设计：`add` 失败说明路由已在，`replace` 兜底对齐；重复执行无副作用，
重启后 DHCP 重装网关路由也不影响（prohibit 的 metric 仍更小）。

## 验证清单（重启后逐项过）

| 检查 | 预期 |
|---|---|
| `ip route show` | `prohibit default  metric 1` 出现在网关路由之上 |
| `ip -6 addr show wlan0` | 无 inet6 地址 |
| `ping -c1 -W3 <公网IP>` | `sendto: Permission denied` |
| `wget -T5 -O /dev/null http://<公网IP>/` | `can't connect: Permission denied` |
| `ping -c1 -W3 <网关IP>` | 正常通 |
| 访问局域网服务（broker / 歌单目录 / adb） | 正常通 |
| 业务日志（MQTT 心跳/周期重发） | 持续刷新 |

## 回滚

- 临时恢复（重启后失效）：`ip route del prohibit default metric 1`，
  v6 重开 `echo 0 > /proc/sys/net/ipv6/conf/wlan0/disable_ipv6`
- 永久恢复：删除钩子里的 WAN block 段并重启

## 局限与边界（务必读）

1. **metric 必须实测核对**。本机 dhcpcd 给网关路由 metric 302，prohibit
   用 metric 1 即可压制；**别的固件可能给 metric 0 甚至负值**——部署前先
   `ip route show` 看网关路由的 metric，保证 prohibit 的比它小，否则封堵
   无效（流量照走网关）。
2. **设备自治方案，root 可解除**。拿到设备 root 就能删路由。它防的是
   "意外/未知的出网行为"，不防"针对这台设备的入侵"。这也意味着：网络 adb
   （`speaker/adbxd`，5555 端口无认证 root）必须只在可信局域网使用。
3. **路由器侧封堵更彻底**。有 OpenWrt 之类可控网关的，在路由器上按源
   MAC/IP 拒绝 FORWARD 是更硬的边界（不依赖设备自身）。本方案的价值在于
   设备侧兜底、以及**可移植到任何没有 iptables 的 buildroot 设备**。
4. **公网 DNS 残留无害**。resolv.conf 里厂商遗留的公网 nameserver 不用
   清理：查询本身发给局域网网关，即使解析成功，连接也出不去。
5. **需要局域网 NTP**。断外网后设备无法用公网 NTP 对时（若在跑 ntpd，
   换局域网时间源或接受时钟漂移；本机 ntpd 已随 XYOS offline 停用，
   音箱无对时刚需）。

> AI生成
