# xbridge — 小雅HOME 音箱的本地 MQTT 桥接

把小雅HOME 音箱（Rockchip RK3308，XYOS 固件）接进 Home Assistant 的纯本地桥：
**不刷机、不上云、不依赖厂商账号**，只写音箱可写的 `/userdata` 分区，删除两个文件即可完全还原。

> ⚠️ 本项目仅用于个人已购设备上的本地自动化研究，与音箱厂商无关亦未获授权。改动系统有风险，请自行评估。

## 功能

- **MQTT 接入**：自带极简 MQTT 3.1.1 客户端（单文件 C，零外部依赖），LWT 在线/离线 + 自动重连
- **HA 自动发现**：发布 button / number / sensor discovery，自动生成「播放/暂停、上一曲、下一曲、
  音量滑条、播放状态、当前曲目」实体（HA 2026.9 起 MQTT `media_player` 平台已移除，组合实体方案不受影响）
- **本地播放**：minimp3 解码 MP3、自解析 WAV 头，管道喂 `aplay`，走 ALSA loopback 与原厂 XYOS 混音共存
- **NAS 歌单**：`-l` 指定音乐目录的 HTTP 列表页，即得上/下一曲、播完自动连播、当前曲目上报
- **实体按键**：读取 `/dev/input/event*`，把按键广播到 `xiaoya/event/button`，与原厂 XYOS 互不干扰
- **状态自愈**：每 60s 周期重发 available / 音量 / 播放状态 / 曲目（均 retain），任何一端断线重连
  错过消息时自动恢复，HA 实体不再卡「不可用」
- **TTS/电台**：`cmd/tts` / `cmd/play` 播放任意局域网 MP3/WAV/电台流 URL
- **体积**：aarch64 musl 静态单文件约 160 KB，音箱侧零依赖

## 架构

```text
┌──────────────────────┐  MQTT :1883   ┌────────────────────────┐
│ 小雅HOME 音箱         │◄─────────────►│ MQTT broker             │
│ RK3308 · XYOS        │    xbridge    │ （HA 加载项 / NAS 容器） │
│ /userdata/local-mqtt │               └───────────┬────────────┘
└──────────▲───────────┘                           │ discovery + 自动化
           │ HTTP :8899 歌单(MP3)        ┌─────────▼─────────┐
┌──────────┴───────────┐                │  Home Assistant   │
│ NAS 音乐目录          │                │ 实体 / 自动化     │
│ busybox httpd        │                └───────────────────┘
└──────────────────────┘
```

## 仓库结构

| 路径 | 说明 |
|---|---|
| `bridge/xbridge.c` | 桥接主程序（单文件 C，内嵌 minimp3 编译） |
| `bridge/minimp3.h` | MP3 解码库（公有领域 CC0，[lieff/minimp3](https://github.com/lieff/minimp3)） |
| `bridge/build.cmd` | Windows 本地交叉编译脚本（zig cc） |
| `.github/workflows/build-xbridge.yml` | GitHub Actions 云端编译，Artifacts 直接下载二进制 |
| `docs/deploy.md` | 完整部署手册（前置验证 / 部署 / 开机自启 / 按键 / TTS / 歌单 / 排错） |
| `ha/automations-example.yaml` | 实体按键 → 播放控制/音量的 3 条自动化示例 |
| `nas/gen_index.py` | 为 busybox httpd 音乐目录生成 index.html（busybox 无目录列表，必需） |
| `nas/music-httpd-start.sh` | NAS 上把音乐目录发布到 HTTP 的启动脚本 |
| `speaker/auto_reboot.sh` | 音箱开机自启钩子模板（写入 `/userdata/cfg/rockchip_test/`） |

## 快速开始

1. **编译**：仓库 Actions 页运行 `build-xbridge`，从 Artifacts 下载 `xbridge`（aarch64 静态）；
   或本地执行 `zig cc -target aarch64-linux-musl -O2 -DHAVE_MINIMP3 -static -s -o xbridge xbridge.c`
2. **部署**：按 [docs/deploy.md](docs/deploy.md) 操作——adb push 到 `/userdata/local-mqtt/`，
   前台跑通验证后再写开机钩子
3. **接入 HA**：broker 与账号就绪后实体自动出现；实体按键联动从 `ha/automations-example.yaml`
   复制三条自动化，按实测的按键 code 与实体 ID 修改

## MQTT 主题（前缀默认 `xiaoya`，可用 `-t` 改）

| 方向 | 主题 | 载荷 |
|---|---|---|
| HA → 音箱 | `xiaoya/cmd/play` / `xiaoya/cmd/tts` | 音频 URL（http，MP3/WAV/电台流） |
| HA → 音箱 | `xiaoya/cmd/stop` | 任意 |
| HA → 音箱 | `xiaoya/cmd/volume` | 0-100 或 0.0-1.0 |
| HA → 音箱 | `xiaoya/cmd/media` | play / pause / stop / **toggle / prev / next / reload** |
| HA → 音箱 | `xiaoya/cmd/exec` | shell 命令（仅 `-e` 开启时，默认关闭） |
| 音箱 → HA | `xiaoya/state/playback` | playing / paused / idle（retained） |
| 音箱 → HA | `xiaoya/state/volume` | 0-100（retained） |
| 音箱 → HA | `xiaoya/state/track` | 当前曲目名（retained） |
| 音箱 → HA | `xiaoya/state/available` | online / offline（LWT，retained） |
| 音箱 → HA | `xiaoya/event/button` | `{"code":164,"value":1}` |

## 许可

- `bridge/minimp3.h`：公有领域（CC0 1.0），见文件头声明
- 其余代码：MIT，见 [LICENSE](LICENSE)

> AI生成
