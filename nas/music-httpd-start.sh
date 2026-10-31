#!/bin/sh
# 在 NAS / 任意 Linux 主机上用 busybox httpd 发布音乐目录（xbridge -l 的歌单源）。
# 用法：改好 MUSIC_DIR 后以 root 运行；busybox httpd 默认守护进程化，无需 -f。
#
# 前置：目录里要先生成 index.html（见 gen_index.py），否则 xbridge 拉不到歌单。
# 验证：curl -s http://<本机IP>:8899/index.html | head
# 停止：pkill -x httpd

MUSIC_DIR=/path/to/music
BIND=0.0.0.0:8899

pkill -f 'busybox httpd' 2>/dev/null
sleep 1
busybox httpd -p "$BIND" -h "$MUSIC_DIR"
echo "httpd serving $MUSIC_DIR on http://$BIND/"
