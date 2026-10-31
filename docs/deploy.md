# xbridge 部署手册（小雅HOME → Home Assistant 本地 MQTT）

> 目标：HA 里控制小雅HOME 音箱（播放/暂停/上一曲/下一曲/音量）+ 本地 MP3/电台/TTS 播放
> + 实体按键上报 + NAS 歌单自动连播。
>
> 全程只写 `/userdata`（可写分区），**不刷机**；卸载时删除两个文件即完全还原（见第 10 节）。
>
> 前置条件：
> - 一台能 adb 连接（root）的小雅HOME 音箱
> - 一个 MQTT broker（HA 的 Mosquitto broker 加载项，或 NAS/其他主机上的 mosquitto 容器）
> - 局域网内的 Home Assistant
> - （可选）NAS 或任意 HTTP 服务器托管 MP3 歌单，用于上下曲/自动连播

## 0. 主题约定（前缀默认 `xiaoya`）

| 方向 | 主题 | 载荷 |
|---|---|---|
| HA → 音箱 | `xiaoya/cmd/play` | 音频 URL（http，MP3/WAV/电台流） |
| HA → 音箱 | `xiaoya/cmd/tts` | 同上（播报，打断当前） |
| HA → 音箱 | `xiaoya/cmd/stop` | 任意 |
| HA → 音箱 | `xiaoya/cmd/volume` | 0-100 或 0.0-1.0 |
| HA → 音箱 | `xiaoya/cmd/media` | play / pause / stop / **toggle / prev / next / reload** |
| HA → 音箱 | `xiaoya/cmd/exec` | shell 命令（仅 `-e` 开启时） |
| 音箱 → HA | `xiaoya/state/playback` | playing / paused / idle（retained） |
| 音箱 → HA | `xiaoya/state/volume` | 0-100（retained） |
| 音箱 → HA | `xiaoya/state/track` | 当前曲目名（URL 解码后，retained） |
| 音箱 → HA | `xiaoya/state/available` | online / offline（LWT，retained） |
| 音箱 → HA | `xiaoya/event/button` | `{"code":116,"value":1}` |

**`cmd/media` 命令语义**：
- `toggle`：播放↔暂停；空闲时从歌单当前位置开始播（合并 play/pause/stop 三键）
- `prev` / `next`：上一曲/下一曲（歌单首尾环绕）
- `reload`：重新拉取歌单（音乐目录新增文件后用）
- 播完一首自动连播下一首（歌单末尾停止）；仅配置了 `-l` 歌单时可用

## 1. 前置验证（第一件事！）

音箱插 USB 连电脑：

```bat
adb devices              :: 有设备号
adb shell id             :: 应显示 uid=0(root)
adb shell ls /userdata/cfg/rockchip_test/   :: 钩子目录在不在
adb shell which aplay amixer dbus-send      :: 工具齐全
adb shell aplay -L | head -20               :: 看有哪些 PCM 设备
adb shell amixer -c 0 contents | head -40   :: 找音量控件名
```

- 如果 `adb devices` 空列表：换线、装 Rockchip 驱动（DriverAssistant），或走串口（UART2 / 115200，接上就是 root shell）。
- **音量控件名**：代码里写的是 `Master Playback Volume`（来自 asound.conf 的 softvol）。
  如果 `amixer -c 0 contents` 里不是这个名字，编译前改 `xbridge.c` 里 `set_volume()` 的控件名。
  注意这代固件的 amixer 要用 `cset name=`（`sset` 找不到 softvol 控件），音量量程实际是 0-99，
  代码已把 0-100% 映射过去。

## 2. 编译

**方式 A（本地）**：装 [zig](https://ziglang.org/download/)（0.13+ 均可），Windows 下直接跑
`bridge/build.cmd`，或手动：

```sh
zig cc -target aarch64-linux-musl -O2 -DHAVE_MINIMP3 -static -s -o xbridge xbridge.c
```

**方式 B（GitHub Actions，本地啥都不用装）**：
1. 把本仓库推到你的 GitHub（保持 `bridge/` 目录结构）
2. Actions 页签跑 `build-xbridge`，从 Artifacts 下载 `xbridge`（aarch64 静态二进制）

## 3. broker 准备

任选其一：

**HA Mosquitto broker 加载项**：安装后在加载项配置里建账号（如 `xiaoya`），允许局域网访问，端口 1883。

**mosquitto 容器（NAS/其他主机）**：

```sh
# 初次建账号（会提示输入密码）
mosquitto_passwd -c /path/to/mosquitto/passwd xiaoya

# mosquitto.conf 关键内容
#   listener 1883 0.0.0.0
#   allow_anonymous false
#   password_file /path/to/mosquitto/passwd
```

记下 broker 的局域网 IP 与端口 1883。HA 的 **MQTT 集成**会自动发现并连上 broker。

## 4. 部署到音箱

```bat
adb shell mkdir -p /userdata/local-mqtt
adb push xbridge /userdata/local-mqtt/xbridge
adb shell chmod +x /userdata/local-mqtt/xbridge
```

**先手动前台跑一次看日志**（别急着写钩子）：

```bat
adb shell /userdata/local-mqtt/xbridge -b <broker的IP> -u <用户名> -P <密码> -v
```

正常应输出：`MQTT 已连接 x.x.x.x:1883`，此时 HA 里应自动出现「小雅HOME」的实体组。

**常用参数**：
- `-d default`：走 ALSA loopback 由 XYOS 混音。**建议必带**——默认的 `real_playback`
  dshare 通道被原厂进程占用，直接用会 device busy
- `-l <url>`：歌单目录 URL（音乐目录的 HTTP 列表页，只解析 `.mp3` 链接）。
  上一曲/下一曲/自动连播/当前曲目依赖此参数，见第 7 节
- `-v`：详细日志
- `-probe`：按键探测模式（不连 broker），见第 6 节

**验证播放**（HA 开发者工具 → MQTT → 发布，或 PC 上用 mosquitto_pub）：

```text
topic:   xiaoya/cmd/play
payload: http://<局域网主机>/test.mp3     ← 随便一个局域网可下载的 MP3
```

音箱出声即通。测音量：`xiaoya/cmd/volume` = `50`。

> 没声音先看音箱日志：
> - `aplay 启动失败` → 设备忙，加 `-d default`
> - HTTP 失败 → URL 不可达（HA 的 TTS 代理 URL 用 `http://<HA内网IP>:8123/...`，别用 localhost）

## 5. 写开机钩子（永久生效）

用 `speaker/auto_reboot.sh` 模板（替换其中的 broker IP、账号密码、歌单 URL 占位符），写入音箱：

```bat
adb push auto_reboot.sh /tmp/auto_reboot.sh
adb shell "cp /tmp/auto_reboot.sh /userdata/cfg/rockchip_test/auto_reboot.sh && chmod +x /userdata/cfg/rockchip_test/auto_reboot.sh"
adb shell cat /userdata/cfg/rockchip_test/auto_reboot.sh   :: 核对内容
adb reboot
```

重启后：`adb shell ps | grep xbridge` 应有进程；HA 实体应在线。

> ⚠️ 若你的固件版本 S98_lunch_init 没有引用 auto_reboot.sh
> （用 `adb shell cat /etc/init.d/S98_lunch_init` 确认），换一个被引用的钩子文件。

## 6. 按键接入

```bat
adb shell /userdata/local-mqtt/xbridge -probe
```

逐个按音箱按键，记下 code（常见值：播放/暂停=164、音量+=115、音量-=114、下一首=407……以实测为准）。Ctrl+C 退出。

之后正常模式下，每次按键都会发到 `xiaoya/event/button`，HA 自动化示例见
[ha/automations-example.yaml](../ha/automations-example.yaml)（播放/暂停 toggle、音量加减三条）。

> 按键事件是"广播"读取，XYOS 同时也在读，互不影响——不想让原厂响应某个键时再研究屏蔽。

## 7. NAS 音乐歌单（上下曲/自动连播，可选）

xbridge 的 `-l` 需要一个**音乐目录的 HTTP 列表页**。busybox httpd 没有目录自动索引，
先用 `nas/gen_index.py` 生成 `index.html`，再启动 httpd：

```sh
python3 gen_index.py /path/to/music        # 生成 index.html（见脚本内 MUSIC_DIR 说明）
sh music-httpd-start.sh                    # busybox httpd -p 0.0.0.0:8899 -h /path/to/music
```

建议挂 cron 每日刷新索引（新增歌曲不用手动重建）：

```cron
17 4 * * * python3 /path/to/gen_index.py /path/to/music
```

然后 xbridge 启动参数加：`-l http://<NAS_IP>:8899/`。NAS 新加歌后在 HA 里发
`xiaoya/cmd/media` = `reload` 即可刷新歌单。

## 8. TTS 播报

发音频 URL 即可（`cmd/tts` 会打断当前播放）：

```yaml
- service: mqtt.publish
  data:
    topic: xiaoya/cmd/tts
    payload: "http://<HA_IP>:8123/local/tts/doorbell.mp3"
```

配合 `shell_command` 在 HA 主机上用 ffmpeg/edge-tts 定期生成 MP3 到 `config/www/tts/`
（`http://HA_IP:8123/local/...` 对应 `config/www`）。

## 9. 常见问题

| 现象 | 处理 |
|---|---|
| 实体没出现 | HA → 设备与服务 → MQTT → 配置里看日志；或 `mosquitto_sub -t 'homeassistant/#' -v` 看 discovery 是否发出 |
| 播放时与原厂抢设备 | `aplay: device busy` → 启动参数加 `-d default`（走 loopback，由 XYOS 混音） |
| MP3 没声只有杂音 | 编译时漏了 `-DHAVE_MINIMP3`（重编译），或流实际不是 MP3（看日志嗅探结果） |
| 电台流放几秒停 | 源站限速/断连，桥会自动结束；HA 侧配重试 |
| 音量改了没变化 | 控件名不对，按第 1 步确认后改代码重编译 |
| 上下曲/连播不工作 | 没配 `-l` 歌单 URL，或 index.html 里没有 `.mp3` 链接 |
| 时区不对 | 钩子里加 `export TZ=CST-8` |

## 10. 卸载（完全还原）

```bat
adb shell "rm /userdata/cfg/rockchip_test/auto_reboot.sh"
adb shell "rm -rf /userdata/local-mqtt"
adb reboot
```

## 附：HA 版本兼容性

实体用 MQTT **button / number / sensor** discovery（HA 2021.x 起支持），HA 2026.9 移除
MQTT `media_player` 平台不影响本桥。桥启动时会清掉旧版本残留的 media_player discovery。

> AI生成
