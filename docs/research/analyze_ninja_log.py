"""分析 ninja 的 .ninja_log：每次构建的墙钟时间、并行度、各类文件平均编译耗时。
用法: python3 analyze_ninja_log.py <build>/.ninja_log"""
import sys, collections
rows = []
for l in open(sys.argv[1], errors='ignore'):
    if l.startswith('#'): continue
    f = l.rstrip('\n').split('\t')
    if len(f) >= 4: rows.append((int(f[0]), int(f[1]), f[3]))
runs = [[]]
for r in rows:  # 同一次构建内结束时间单调递增
    if runs[-1] and r[1] < runs[-1][-1][1] - 1000: runs.append([])
    runs[-1].append(r)
run = max(runs, key=len)
s, e = min(r[0] for r in run), max(r[1] for r in run)
tot = sum(b - a for a, b, _ in run)
print(f'edges={len(run)} wall={(e-s)/1000:.0f}s sum-of-job-time={tot/1000:.0f}s avg-parallel-jobs={tot/(e-s):.1f}')
S, C = collections.Counter(), collections.Counter()
for a, b, o in run:
    k = ('main .cc' if '__idf_main' in o else 'lvgl' if 'lvgl' in o else
         '.cc/.cpp' if o.endswith(('.cc.obj', '.cpp.obj')) else '.c' if o.endswith('.c.obj') else
         '.S' if o.endswith('.S.obj') else 'other')
    S[k] += b - a; C[k] += 1
for k in S: print(f'{k:10s} n={C[k]:5d} avg={S[k]/C[k]/1000:6.2f}s')
