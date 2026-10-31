#!/usr/bin/env python3
"""为 busybox httpd 音乐目录生成 index.html。

busybox httpd 没有目录自动索引，而 xbridge 的 -l 歌单参数依赖目录列表页里的
.mp3 链接，所以新增歌曲后需要重新生成。

用法：
    python3 gen_index.py [音乐目录]
不传参数时使用下方 MUSIC_DIR。

建议挂 cron 每日刷新（新增歌曲后无需手动重建索引）：
    17 4 * * * python3 /path/to/gen_index.py /path/to/music
"""
import html
import os
import sys
import urllib.parse

MUSIC_DIR = '/path/to/music'  # ← 改成你的音乐目录


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else MUSIC_DIR
    files = sorted(
        f for f in os.listdir(d)
        if f.lower().endswith('.mp3') and os.path.isfile(os.path.join(d, f))
    )
    rows = []
    for f in files:
        enc = urllib.parse.quote(f)
        rows.append(f'<li><a href="{enc}">{html.escape(f)}</a></li>')
    page = ('<!DOCTYPE html><html><head><meta charset="utf-8"><title>music</title></head>'
            '<body><ul>' + ''.join(rows) + '</ul></body></html>\n')
    with open(os.path.join(d, 'index.html'), 'w', encoding='utf-8') as fp:
        fp.write(page)
    print('index.html written:', len(files), 'mp3 files')


if __name__ == '__main__':
    main()
