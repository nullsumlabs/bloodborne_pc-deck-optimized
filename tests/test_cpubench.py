"""tools/cpubench.py against synthetic bb-cpu-bench logs, and the built binary's log contract."""
from paths import ROOT
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CPUBENCH = ROOT / 'tools/cpubench.py'
BENCH = ROOT / 'dist/bb-cpu-bench-standard'

LOG = '''# bb-cpu-bench {tag}
# host: {host}
# build: {build}
# case copy_64b: small buffer/texture copies
case,bytes_per_iter,ns_per_iter,mb_per_s
copy_64b,65536,{copy:.1f},30000
detile_1080p,16588800,{detile:.1f},11000
cp_decode,262144,{cp:.1f},2300
'''


def load_module():
    spec = importlib.util.spec_from_file_location('cpubench', CPUBENCH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cpubench = load_module()


class CpuBenchTests(unittest.TestCase):
    def log(self, directory, name, **values):
        defaults = dict(tag='standard', host='Test CPU, 8 threads', build='-O2', copy=2000.0,
                        detile=1400000.0, cp=110000.0)
        defaults.update(values)
        path = Path(directory) / name
        path.write_text(LOG.format(**defaults), encoding='utf-8')
        return path

    def test_load_keeps_host_build_and_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            stats = cpubench.load(self.log(tmp, 'a.log'))
        self.assertEqual(stats['host'], 'Test CPU, 8 threads')
        self.assertEqual(stats['build'], '-O2')
        self.assertEqual([row[0] for row in stats['rows']],
                         ['copy_64b', 'detile_1080p', 'cp_decode'])

    def test_compare_reports_the_change_and_the_geomean(self):
        from io import StringIO
        import contextlib
        with tempfile.TemporaryDirectory() as tmp:
            first = self.log(tmp, 'a.log')
            # B is 10% faster on copy, 10% slower on detile, unchanged on cp_decode.
            second = self.log(tmp, 'b.log', tag='znver2', build='-O2 -march=znver2',
                              copy=1800.0, detile=1540000.0)
            out = StringIO()
            with contextlib.redirect_stdout(out):
                cpubench.compare(cpubench.load(first), cpubench.load(second))
        text = out.getvalue()
        self.assertIn('-10.0%', text)  # copy_64b, B faster
        self.assertIn('+10.0%', text)  # detile_1080p, B slower
        # cbrt(0.9 * 1.1 * 1.0) = 0.9967: the two 10% moves nearly cancel over the three cases.
        self.assertIn('geomean B/A 99.7%', text)
        self.assertIn('1 faster, 1 slower, 1 unchanged', text)
        self.assertNotIn('WARNING', text)

    def test_compare_warns_about_two_different_cpus(self):
        from io import StringIO
        import contextlib
        with tempfile.TemporaryDirectory() as tmp:
            first = self.log(tmp, 'a.log')
            second = self.log(tmp, 'b.log', host='Other CPU, 4 threads')
            out = StringIO()
            with contextlib.redirect_stdout(out):
                cpubench.compare(cpubench.load(first), cpubench.load(second))
        self.assertIn('different CPUs', out.getvalue())

    def test_compare_names_a_case_only_one_log_has(self):
        from io import StringIO
        import contextlib
        with tempfile.TemporaryDirectory() as tmp:
            first = self.log(tmp, 'a.log')
            second = self.log(tmp, 'b.log')
            second.write_text(second.read_text().replace('cp_decode,262144,110000.0,2300\n',
                                                         'hash_16k,262144,17000.0,15000\n'),
                              encoding='utf-8')
            out = StringIO()
            with contextlib.redirect_stdout(out):
                cpubench.compare(cpubench.load(first), cpubench.load(second))
        text = out.getvalue()
        self.assertIn('only in A', text)
        self.assertIn('only in B', text)

    def test_cli_exit_codes(self):
        def run(*arguments):
            return subprocess.run([sys.executable, str(CPUBENCH), *arguments],
                                  capture_output=True, text=True)
        self.assertEqual(run().returncode, 2)
        self.assertEqual(run('a', 'b', 'c').returncode, 2)
        missing = run('/nonexistent/bb-cpu-bench.log')
        self.assertEqual(missing.returncode, 1)
        self.assertIn('/nonexistent/bb-cpu-bench.log', missing.stderr)
        with tempfile.TemporaryDirectory() as tmp:
            empty = Path(tmp) / 'empty.log'
            empty.write_text('# bb-cpu-bench standard\n', encoding='utf-8')
            self.assertEqual(run(str(empty)).returncode, 1)
            first = self.log(tmp, 'a.log')
            second = self.log(tmp, 'b.log')
            one = run(str(first))
            self.assertEqual(one.returncode, 0, one.stderr)
            self.assertIn('case', one.stdout)
            two = run(str(first), str(second))
            self.assertEqual(two.returncode, 0, two.stderr)
            self.assertIn('against', two.stdout)


@unittest.skipUnless(BENCH.exists(), 'build tools/build_cpu_bench.sh first')
class CpuBenchBinaryTests(unittest.TestCase):
    def test_binary_writes_its_log_and_lists_cases(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'run.log'
            result = subprocess.run([str(BENCH), '--out', str(out), '--budget', '1'],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('bb-cpu-bench standard', result.stdout)
            text = out.read_text(encoding='utf-8')
        self.assertIn('# host:', text)
        self.assertIn('case,bytes_per_iter,ns_per_iter,mb_per_s', text)
        self.assertIn('copy_64b,', text)
        self.assertIn('cp_decode,', text)

    def test_binary_rejects_a_bad_argument(self):
        result = subprocess.run([str(BENCH), '--nonsense'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('usage', result.stderr)


if __name__ == '__main__':
    unittest.main()
