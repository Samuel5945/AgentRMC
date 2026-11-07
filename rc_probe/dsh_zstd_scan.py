"""DSH 会话库 zstd 扫描探针（第 3 批定向派发·DSH 线，2026-11-07）。

用途：验证「定向派发的标记只落在目标会话」——把 ~/.dsh/sessions 下全部
session-*/ 的 zstd 压缩事件日志解出来，找标记串，报告命中文件清单。

坑（实测踩过）：DSH 的 session.v4.jsonl.zstd 是**多帧拼接**，zstandard 的
decompress() 只解第一帧，追加的轮次全在后面的帧里——必须
stream_reader(read_across_frames=True) 跨帧流式读，否则扫描零命中假阴性。

用法：python rc_probe/dsh_zstd_scan.py <标记串>
"""

import glob
import os
import sys

import zstandard


def main() -> None:
    if len(sys.argv) < 2:
        print('usage: dsh_zstd_scan.py <marker>')
        raise SystemExit(2)
    mark = sys.argv[1]
    root = os.path.expanduser('~/.dsh/sessions')
    dctx = zstandard.ZstdDecompressor()
    hits, scanned, errors = [], 0, []
    for f in glob.glob(os.path.join(root, '*', 'session-*', '*.zstd')):
        scanned += 1
        try:
            with open(f, 'rb') as fh, dctx.stream_reader(fh, read_across_frames=True) as r:
                text = r.read().decode('utf-8', 'replace')
        except Exception as error:  # 单文件坏了不挡全库扫描
            errors.append((f, str(error)))
            continue
        if mark in text:
            hits.append(f)
            for line in text.splitlines():
                if mark in line:
                    print('HIT-LINE:', os.path.basename(os.path.dirname(f)), '|', line[:260])
    print('---')
    print(f'scanned: {scanned} | files with marker: {len(hits)} | errors: {len(errors)}')
    for f in hits:
        print('HIT:', f)
    for f, e in errors:
        print('ERR:', f, e)


if __name__ == '__main__':
    main()
