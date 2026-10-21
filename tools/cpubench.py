#!/usr/bin/env python3
"""Compare bb-cpu-bench logs: one run's table, or two builds' case by case.

    tools/cpubench.py bb-cpu-bench-standard-*.log [bb-cpu-bench-znver2-*.log]

Both binaries run the same cases with the same buffers, so ns per iteration is directly
comparable: the second column of a two-run table is the same work done by the other build.
A negative B - A means B is faster on that case. The summary at the end is the geometric mean
of the per-case ratios: below 100% B is faster overall, and the spread says whether the FLAGS
changed one hot loop or moved everything.
"""
import math
import sys


def load(path):
    host = build = None
    rows = []
    try:
        with open(path, encoding='utf-8', errors='replace') as stream:
            lines = stream.read().splitlines()
    except OSError as error:
        raise SystemExit(f'{path}: {error.strerror}')
    header = None
    for line in lines:
        if line.startswith('# host:'):
            host = line.split(':', 1)[1].strip()
        elif line.startswith('# build:'):
            build = line.split(':', 1)[1].strip()
        elif not line.startswith('#') and line.strip():
            fields = line.split(',')
            if header is None:
                header = fields
                if not {'case', 'ns_per_iter'} <= set(header):
                    raise SystemExit(f'{path}: not a bb-cpu-bench log (no case/ns_per_iter)')
                continue
            try:
                row = dict(zip(header, fields))
                rows.append((row['case'], float(row['ns_per_iter']), float(row['mb_per_s'])))
            except (ValueError, KeyError):
                continue
    if not rows:
        raise SystemExit(f'{path}: no measurement rows')
    return {'path': path, 'host': host or 'unknown host', 'build': build or 'unknown build',
            'rows': rows}


def report(stats):
    print(f'{stats["path"]}')
    print(f'  {stats["host"]}')
    print(f'  {stats["build"]}')
    print(f'  {"case":14s} {"ns/iter":>12s} {"MB/s":>10s}')
    for name, ns, mbps in stats['rows']:
        print(f'  {name:14s} {ns:12.1f} {mbps:10.0f}')


def compare(first, second):
    left = {name: ns for name, ns, _ in first['rows']}
    right = {name: ns for name, ns, _ in second['rows']}
    shared = [name for name in left if name in right]
    if not shared:
        raise SystemExit('the two logs share no case names')
    print(f'\n{second["path"]} against {first["path"]}')
    print(f'  A: {first["build"]}   ({first["host"]})')
    print(f'  B: {second["build"]}   ({second["host"]})')
    if first['host'].split(',')[0] != second['host'].split(',')[0]:
        print('  WARNING: the two runs name different CPUs; only compare them on the same machine')
    print(f'\n  {"case":14s} {"A ns":>10s} {"B ns":>10s} {"B - A":>9s}  verdict')
    ratios = []
    for name in shared:
        a, b = left[name], right[name]
        change = (b - a) / a * 100.0 if a else 0.0
        ratios.append(b / a if a else 1.0)
        verdict = 'B faster' if change < -1.0 else 'B slower' if change > 1.0 else 'the same'
        print(f'  {name:14s} {a:10.1f} {b:10.1f} {change:+8.1f}%  {verdict}')
    missing_left = [name for name in right if name not in left]
    missing_right = [name for name in left if name not in right]
    for name in missing_left:
        print(f'  {name:14s} {"-":>10s} {right[name]:10.1f} {"":>9s}  only in B')
    for name in missing_right:
        print(f'  {name:14s} {left[name]:10.1f} {"-":>10s} {"":>9s}  only in A')
    geomean = math.exp(sum(math.log(ratio) for ratio in ratios) / len(ratios))
    faster = sum(1 for ratio in ratios if ratio < 0.99)
    slower = sum(1 for ratio in ratios if ratio > 1.01)
    print(f'\n  geomean B/A {geomean * 100:.1f}% over {len(ratios)} cases '
          f'({faster} faster, {slower} slower, {len(ratios) - faster - slower} unchanged)')
    print('  ns/iter is one call of the case; lower is better on every line.')


def main():
    args = [arg for arg in sys.argv[1:] if not arg.startswith('-')]
    if not args or len(args) > 2:
        print('usage: tools/cpubench.py A.log [B.log]', file=sys.stderr)
        return 2
    runs = [load(path) for path in args]
    for index, stats in enumerate(runs):
        if index:
            print()
        report(stats)
    if len(runs) == 2:
        compare(*runs)
    return 0


if __name__ == '__main__':
    sys.exit(main())
