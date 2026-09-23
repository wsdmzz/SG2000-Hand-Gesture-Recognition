#!/usr/bin/env python3
"""分析板上运行日志: 每个手势阶段的 bsc / 检出率 / 识别结果。

用法: python3 analyze_run_log.py [日志路径]

识别规则(与 tdl_gesture_kpt_stream.c 一致):
  - bsc = [DBG-DET] 行里的 top1 类别置信度; 每 10 帧一行
  - bsc < 0.30 → 判无手, 不输出 FA 行
  - FA 行只在检到手时输出 → 用 FA 行密度衡量"检出率"
"""
import re
import sys
from collections import Counter

PATH = sys.argv[1] if len(sys.argv) > 1 else '/tmp/gest.log'

det_re = re.compile(r'\[DBG-DET\] best=(\d+) cls=(\d+) bsc=([\d.]+)')
fa_re = re.compile(r'^FA ([\d ]+)\| B ([\-\d ]+)')
pose_re = re.compile(r'^POSE cls=(\d+)\((\w+)\) sc=([\d.]+)')
gest_re = re.compile(r'^GEST (\d+)')

det = []      # (cls, bsc)
fa_lines = []  # (FA 五值, B 五值)
pose = []     # (cls, name, sc)
gest = []

lines = open(PATH, encoding='utf-8', errors='replace').read().splitlines()
for ln in lines:
    m = det_re.search(ln)
    if m:
        det.append((int(m.group(2)), float(m.group(3))))
        continue
    m = fa_re.match(ln.strip())
    if m:
        fa = [int(x) for x in m.group(1).split()]
        b = [int(x) for x in m.group(2).split()]
        fa_lines.append((fa, b))
        continue
    m = pose_re.match(ln.strip())
    if m:
        pose.append((int(m.group(1)), m.group(2), float(m.group(3))))
        continue
    m = gest_re.match(ln.strip())
    if m:
        gest.append(int(m.group(1)))

print(f'=== 日志: {PATH} ===')
print(f'总行数={len(lines)}  DBG-DET={len(det)}  FA行={len(fa_lines)}  POSE={len(pose)}  GEST={len(gest)}')

if not det and not fa_lines:
    print('\n未解析到相关内容。请确认日志含 [DBG-DET] / FA 行。')
    raise SystemExit(0)

print('\n============ 1. bsc 分布 ([DBG-DET]) ============')
if det:
    bscs = [b for _, b in det]
    over = sum(1 for b in bscs if b >= 0.30)
    print(f'  样本 {len(bscs)}   min={min(bscs):.4f} max={max(bscs):.4f} '
          f'mean={sum(bscs)/len(bscs):.4f}')
    print(f'  bsc>=0.30 的比例: {over}/{len(bscs)} = {100*over/len(bscs):.0f}%  (>=0.30 才会输出 FA)')
    print('  分箱: ' + ' '.join(
        f'[{lo:.2f}-{hi:.2f})={sum(1 for b in bscs if lo<=b<hi)}'
        for lo, hi in [(0,0.01),(0.01,0.1),(0.1,0.3),(0.3,0.6),(0.6,1.01)]))
    print('  cls 分布(所有帧): ' + str(dict(sorted(Counter(c for c,_ in det).items()))))
    print('  cls 分布(仅 bsc>=0.3): ' + str(dict(sorted(
        Counter(c for c,b in det if b >= 0.30).items()))))

print('\n============ 2. 检出手的帧数 (FA 行) ============')
print(f'  FA 行总数 = {len(fa_lines)}  (每帧最多 1 行, 只在 bsc>=0.30 时输出)')
if det:
    print(f'  推算检出率 ≈ {len(fa_lines)} / (det 采样帧数×10) 左右')

if fa_lines:
    print('\n  B 模式分布 (5指 弯=0/直=1):')
    for b, n in Counter(tuple(b) for _, b in fa_lines).most_common(10):
        print(f'    {b}  ×{n}')
    print('\n  FA 五值统计 (各指角度 min/mean/max):')
    import statistics
    for i, nm in enumerate(['拇指','食指','中指','无名指','小指']):
        v = [f[i] for f, _ in fa_lines]
        print(f'    {nm}: {min(v):>4} / {statistics.mean(v):>6.1f} / {max(v):>4}')

if pose:
    print('\n============ 3. 模型类别输出 (POSE 行) ============')
    print('  cls 分布: ' + str(dict(sorted(Counter(c for c,_,_ in pose).items()))))
    print('  sc 统计: min=%.2f mean=%.2f max=%.2f' % (
        min(s for _,_,s in pose), sum(s for _,_,s in pose)/len(pose), max(s for _,_,s in pose)))
    print('  低置信(sc<0.5)帧数: %d/%d' % (sum(1 for _,_,s in pose if s < 0.5), len(pose)))

if gest:
    print('\n============ 4. 稳定手势输出 (GEST) ============')
    print('  ' + str(dict(sorted(Counter(gest).items()))))
    print(f'  共 {len(gest)} 次')
