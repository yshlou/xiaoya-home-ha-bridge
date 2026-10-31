#!/bin/sh
# 小雅HOME 开机自启钩子模板（由 /etc/init.d/S98_lunch_init source 执行，
# 部署到 /userdata/cfg/rockchip_test/auto_reboot.sh，详见 docs/deploy.md 第 5 节）。
#
# 使用前替换占位符：
#   <BROKER_IP>   MQTT broker 地址（必填）
#   <BROKER_USER> broker 用户名（无认证可去掉 -u/-P）
#   <BROKER_PASS> broker 密码
#   <MUSIC_URL>   歌单目录 URL（没有歌单可去掉 -l 参数）
#
# 第一段 adbxd 是可选的网络 adb 守护（仅方便远程调试，没有该二进制就删掉这一段，
# 不影响 xbridge）。
export TZ=CST-8
D=/userdata/local-mqtt
if ! ps | grep "[a]dbxd" >/dev/null; then
    setsid $D/adbxd -q >> $D/adbxd.log 2>&1 </dev/null &
fi
if ! ps | grep "[x]bridge" >/dev/null; then
    setsid $D/xbridge -b <BROKER_IP> -u <BROKER_USER> -P <BROKER_PASS> \
        -d default -l <MUSIC_URL> -v >> $D/xbridge.log 2>&1 </dev/null &
fi
