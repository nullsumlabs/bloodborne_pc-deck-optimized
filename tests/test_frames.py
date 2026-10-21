"""tools/frames.py: the frame-time distribution and the long frames' columns, on synthetic CSVs."""
from paths import ROOT
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

FRAMES = ROOT / 'tools/frames.py'


def load_module():
    spec = importlib.util.spec_from_file_location('frames', FRAMES)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


frames = load_module()

HEADER = 't_s,frame_ms,compiles,preupload_mb,protect_ms\n'


def rows(values, compiles=None):
    lines = []
    for index, value in enumerate(values):
        compiles_on_frame = (compiles or {}).get(index, 0)
        lines.append(f'{index * 0.016:.3f},{value:.2f},{compiles_on_frame},'
                     f'{0.5 if compiles_on_frame else 0.0:.2f},0.10\n')
    return ''.join(lines)


class FramesTests(unittest.TestCase):
    def csv(self, text, directory):
        path = Path(directory) / 'run.frames.csv'
        path.write_text(text, encoding='utf-8')
        return path

    def test_percentile_is_the_nearest_rank_and_empty_is_zero(self):
        self.assertEqual(frames.percentile([], 99), 0.0)
        self.assertEqual(frames.percentile([4.0], 50), 4.0)
        self.assertEqual(frames.percentile(list(range(1, 101)), 50), 50)
        self.assertEqual(frames.percentile(list(range(1, 101)), 99), 99)
        self.assertEqual(frames.percentile([3.0, 1.0, 2.0], 100), 3.0)

    def test_load_rejects_a_log_without_frame_times(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.csv('t_s,compiles\n0.0,1\n', tmp)
            with self.assertRaises(SystemExit) as raised:
                frames.load(path)
            self.assertIn('no frame_ms column', str(raised.exception))

    def test_load_rejects_an_empty_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit) as raised:
                frames.load(self.csv('', tmp))
            self.assertIn('empty', str(raised.exception))

    def test_summary_reports_the_tail_and_the_hitch_columns(self):
        values = [16.0] * 10 + [120.0, 140.0]
        text = HEADER + rows(values, compiles={10: 3, 11: 4})
        with tempfile.TemporaryDirectory() as tmp:
            stats = frames.summarize(self.csv(text, tmp), warmup=0.0)
        self.assertEqual(stats['frames'], 12)
        self.assertEqual(stats['median'], 16.0)
        self.assertEqual(stats['worst'], 140.0)
        self.assertEqual(stats['long'], 2)
        self.assertEqual(stats['over_33'], 2)
        self.assertEqual(stats['over_50'], 2)
        # The shader compiles ride on the long frames, so compiles ranks first.
        self.assertEqual(stats['columns'][0][2], 'compiles')
        self.assertGreater(stats['columns'][0][1], 1.0)

    def test_warm_up_drops_the_frames_it_names(self):
        values = [500.0] * 10 + [16.0] * 20
        text = HEADER + rows(values)
        with tempfile.TemporaryDirectory() as tmp:
            path = self.csv(text, tmp)
            all_frames = frames.summarize(path, warmup=0.0)
            warmed = frames.summarize(path, warmup=0.16)
        self.assertEqual(all_frames['frames'], 30)
        # Frames from t_s >= 0.16 s: 20 of them (the ten slow ones are all before it).
        self.assertEqual(warmed['frames'], 20)
        self.assertEqual(warmed['median'], 16.0)

    def test_warm_up_that_leaves_nothing_fails_loudly(self):
        text = HEADER + rows([16.0] * 5)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit) as raised:
                frames.summarize(self.csv(text, tmp), warmup=10.0)
        self.assertIn('warm-up', str(raised.exception))

    def test_compare_prints_both_sides_and_the_difference(self):
        first = dict(path='A', frames=100, seconds=2.0, median=16.0, mean=17.0, p95=18.0,
                     p99=20.0, worst=40.0, long=1, over_33=0, over_50=0, columns=[])
        second = dict(path='B', frames=100, seconds=2.0, median=15.0, mean=16.0, p95=17.0,
                      p99=19.0, worst=30.0, long=0, over_33=0, over_50=0, columns=[])
        from io import StringIO
        import contextlib
        out = StringIO()
        with contextlib.redirect_stdout(out):
            frames.compare(first, second)
        text = out.getvalue()
        self.assertIn('B against A', text)
        self.assertIn('  median ms', text)
        self.assertIn('-1.00', text)  # B - A for the median

    def test_cli_exit_codes(self):
        def run(*arguments):
            return subprocess.run([sys.executable, str(FRAMES), *arguments],
                                  capture_output=True, text=True)
        self.assertEqual(run().returncode, 2)
        self.assertEqual(run('a', 'b', 'c').returncode, 2)
        missing = run('/nonexistent/run.frames.csv')
        self.assertEqual(missing.returncode, 1)
        self.assertIn('/nonexistent/run.frames.csv', missing.stderr)
        with tempfile.TemporaryDirectory() as tmp:
            path = self.csv(HEADER + rows([16.0] * 20 + [90.0]), tmp)
            one = run(str(path), '--warmup=0')
            self.assertEqual(one.returncode, 0, one.stderr)
            self.assertIn('21 frames in', one.stdout)
            self.assertIn('median 16.00 ms', one.stdout)
            self.assertIn('hitches: 1', one.stdout)
            second = self.csv(HEADER + rows([15.0] * 21), tmp)
            two = run(str(path), str(second), '--warmup=0')
            self.assertEqual(two.returncode, 0, two.stderr)
            self.assertIn('against', two.stdout)


if __name__ == '__main__':
    unittest.main()
