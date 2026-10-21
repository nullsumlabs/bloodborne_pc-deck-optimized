#!/usr/bin/env python3
"""Frame-time statistics for BB_FRAME_LOG CSVs, and what the long frames have in common.

    tools/frames.py out/ab_A.frames.csv [out/ab_B.frames.csv]

One run: the frame-time distribution (median, p95, p99, max), how many frames ran long, and the
columns (compiles, preupload_mb, protect_ms, ...) that are high on those long frames rather than on
all of them. Two runs: the same lines, then each figure as A -> B, so a build/toggle A/B reads as
one table. Frame times are measured per presented frame (frame_stats.cpp), so these are the numbers
that answer "does it stutter", not just the average frame rate.
"""
import math
import sys


def percentile(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, math.ceil(p / 100.0 * len(ordered)))
    return ordered[min(rank, len(ordered)) - 1]


def load(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as stream:
            lines = [line.strip() for line in stream if line.strip()]
    except OSError as error:
        raise SystemExit(f'{path}: {error.strerror}')
    if not lines:
        raise SystemExit(f'{path}: empty')
    header = lines[0].split(',')
    if 'frame_ms' not in header:
        raise SystemExit(f'{path}: no frame_ms column (is this a BB_FRAME_LOG file?)')
    rows = []
    for line in lines[1:]:
        fields = line.split(',')
        if len(fields) != len(header):
            continue
        try:
            rows.append({name: float(value) for name, value in zip(header, fields)})
        except ValueError:
            continue
    return header, rows


def summarize(path, warmup):
    header, rows = load(path)
    if rows and warmup > 0:
        start = rows[0].get('t_s', 0.0) + warmup
        rows = [row for row in rows if row.get('t_s', 0.0) >= start]
    if not rows:
        raise SystemExit(f'{path}: no frames left after {warmup:g} s of warm-up')
    frames = [row['frame_ms'] for row in rows]
    median = percentile(frames, 50)
    long = [row for row in rows if row['frame_ms'] > 2 * median] if median > 0 else []
    skip = {'t_s', 'frame_ms'}
    columns = []
    for name in header:
        if name in skip or name not in rows[0]:
            continue
        all_mean = sum(row[name] for row in rows) / len(rows)
        long_mean = (sum(row[name] for row in long) / len(long)) if long else 0.0
        excess = long_mean - all_mean
        # Rank by how much more of it a long frame carries; above 1.0 it is a hitch suspect.
        columns.append((excess, long_mean / all_mean if all_mean > 0 else 0.0, name, all_mean, long_mean))
    columns.sort(reverse=True)
    seconds = rows[-1].get('t_s', 0.0) - rows[0].get('t_s', 0.0)
    return dict(
        path=path, frames=len(rows), seconds=seconds, median=median,
        mean=sum(frames) / len(frames), p95=percentile(frames, 95), p99=percentile(frames, 99),
        worst=max(frames), long=len(long),
        over_33=sum(1 for value in frames if value > 33.4),
        over_50=sum(1 for value in frames if value > 50.0),
        columns=[column for column in columns if column[0] > 0][:5],
    )


def report(stats):
    print(f'{stats["path"]}')
    print(f'  {stats["frames"]} frames in {stats["seconds"]:.1f} s, mean {stats["mean"]:.2f} ms '
          f'({1000 / stats["mean"]:.1f} FPS)')
    print(f'  median {stats["median"]:.2f} ms ({1000 / stats["median"]:.1f} FPS), '
          f'p95 {stats["p95"]:.2f}, p99 {stats["p99"]:.2f}, worst {stats["worst"]:.1f}')
    share = 100.0 * stats['long'] / stats['frames']
    print(f'  hitches: {stats["long"]} frames over 2x the median ({share:.2f}%), '
          f'{stats["over_33"]} over 33 ms, {stats["over_50"]} over 50 ms')
    if stats['columns']:
        print('  on those long frames (mean on them vs on all frames):')
        for _, ratio, name, all_mean, long_mean in stats['columns']:
            more = f'{ratio:.1f}x more' if ratio > 1.05 else 'the same'
            print(f'    {name:16s} {long_mean:9.2f} vs {all_mean:9.2f}   {more}')


def compare(first, second):
    print(f'\n{second["path"]} against {first["path"]}:\n')
    print(f'  {"":12s} {"A":>10s} {"B":>10s}  {"B - A":>10s}')
    rows = (('frames', 'frames'), ('median ms', 'median'), ('mean ms', 'mean'),
            ('p95 ms', 'p95'), ('p99 ms', 'p99'), ('worst ms', 'worst'),
            ('hitches', 'long'), ('over 33 ms', 'over_33'), ('over 50 ms', 'over_50'))
    for label, key in rows:
        a, b = first[key], second[key]
        count = key in ('frames', 'long', 'over_33', 'over_50')
        if count:
            print(f'  {label:12s} {a:10.0f} {b:10.0f}  {b - a:+10.0f}')
        else:
            print(f'  {label:12s} {a:10.2f} {b:10.2f}  {b - a:+10.2f}')
    print('\n  p99 and the hitch counts are what "less smooth" means at the same frame rate;'
          ' a higher mean alone is just a lower frame rate.')


def main():
    args = [arg for arg in sys.argv[1:] if not arg.startswith('-')]
    warmup = 5.0
    for arg in sys.argv[1:]:
        if arg.startswith('--warmup='):
            warmup = float(arg.split('=', 1)[1])
    if not args or len(args) > 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    runs = [summarize(path, warmup) for path in args]
    for index, stats in enumerate(runs):
        if index:
            print()
        report(stats)
    if len(runs) == 2:
        compare(*runs)
    return 0


if __name__ == '__main__':
    sys.exit(main())
