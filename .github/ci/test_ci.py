"""Behavioral checks for exact-revision push gating and fail-closed runtime results."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name('ci.py')


class CiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SCRIPT.is_file():
            raise AssertionError('push CI entry point is not implemented')
        spec = importlib.util.spec_from_file_location('dy_ci', SCRIPT)
        cls.ci = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.ci)

    def test_default_runtime_does_not_inject_product_options(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'src').mkdir()
            (root / 'src/real.cpp').write_text('int original = 1;')
            with (mock.patch.object(self.ci, 'setup_environment', return_value={}),
                  mock.patch.object(self.ci, 'configure') as configure,
                  mock.patch.object(self.ci, 'build_targets'),
                  mock.patch.object(self.ci, 'read_inventory', return_value=(dict(targets=[], compiler='fixture'), [])),
                  mock.patch.object(self.ci, 'runtime_checks')):
                status = self.ci.main(['run', '--phase', 'runtime', '--root', str(root), '--revision', 'fixture'])
            self.assertEqual(status, 0)
            self.assertEqual(configure.call_args.kwargs,
                             dict(clang=False, sanitize=False, dependencies=None))
            self.assertEqual((root / 'src/real.cpp').read_text(), 'int original = 1;')

    def test_product_source_change_during_ci_is_blocked(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'examples').mkdir()
            source = root / 'examples/main.cpp'
            source.write_text('int original = 1;')
            with (mock.patch.object(self.ci, 'setup_environment', return_value={}),
                  mock.patch.object(self.ci, 'configure', side_effect=lambda *a, **k: source.write_text('int changed = 2;')),
                  mock.patch.object(self.ci, 'build_targets'),
                  mock.patch.object(self.ci, 'read_inventory', return_value=(dict(targets=[], compiler='fixture'), [])),
                  mock.patch.object(self.ci, 'runtime_checks')):
                status = self.ci.main(['run', '--phase', 'runtime', '--root', str(root), '--revision', 'fixture'])
            self.assertEqual(status, 2)

    def test_cpu_keeps_sanitizer_with_native_windows_compiler(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'src').mkdir()
            (root / 'src/main.cpp').write_text('int main() {}')
            for platform in ('win32', 'linux'):
                with (mock.patch.object(self.ci.sys, 'platform', platform),
                      mock.patch.object(self.ci, 'setup_environment', return_value={}),
                      mock.patch.object(self.ci, 'configure') as configure,
                      mock.patch.object(self.ci, 'build_targets'),
                      mock.patch.object(self.ci, 'cpu_checks'),
                      mock.patch.object(self.ci, 'read_inventory', return_value=(dict(targets=[]), []))):
                    status = self.ci.main(['run', '--phase', 'cpu', '--root', str(root), '--revision', 'fixture'])
                self.assertEqual(status, 0)
                self.assertTrue(configure.call_args.kwargs['sanitize'])
                self.assertEqual(configure.call_args.kwargs['clang'], platform != 'win32')

    def test_external_runtime_cache_is_not_old_instrumented_cache(self):
        self.assertEqual(self.ci.build_name('runtime', 'vulkan', 'Debug'), 'runtime-external-vulkan-Debug')

    def test_runtime_records_later_evidence_after_a_failed_example(self):
        import json
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / '.github/ci').mkdir(parents=True)
            profile = {'kind': 'capability', 'expected_exit': 77, 'markers': ['Feature unavailable']}
            profiles = {'version': 1, 'examples': {'examples/first': profile, 'examples/second': profile}}
            (root / '.github/ci/examples.json').write_text(json.dumps(profiles), encoding='utf-8')
            targets = [dict(name=name, directory='examples/' + name, kind='cpu', runtime=True,
                            binary=str(root / name)) for name in ('first', 'second')]
            report = self.ci.Report(root / 'logs', 'fixture', 'null')
            results = [types.SimpleNamespace(returncode=77, stdout=output)
                       for output in ('missing diagnostic', 'Feature unavailable')]
            with mock.patch.object(self.ci.subprocess, 'run', side_effect=results):
                with self.assertRaises(self.ci.CiError) as failure:
                    self.ci.runtime_checks(root, root, 'Debug',
                                          dict(api='null', targets=targets, unsupported=[]), [], report, {})
            self.assertEqual(failure.exception.status, 'FAIL')
            self.assertEqual([result['status'] for result in report.results], ['FAIL', 'UNSUPPORTED'])
            self.assertEqual(len(list((root / 'logs').rglob('checks.json'))), 2)

    def test_headless_runtime_runs_both_modes_and_compares_saved_pixels_without_a_window(self):
        import json
        import observe
        for mismatch in (False, True):
            with self.subTest(mismatch=mismatch), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                (root / '.github/ci').mkdir(parents=True)
                profile = dict(kind='headless-compare', markers=['gpu ok'], dimensions=[3, 1],
                               pixels=[[0, 0, 255, 0, 0]],
                               cases=[dict(name=name, arguments=['--mode', name, '--frames', '3'])
                                      for name in ('serial', 'parallel')])
                (root / '.github/ci/examples.json').write_text(json.dumps(
                    dict(version=1, examples={'examples/headless': profile})), encoding='utf-8')
                target = dict(name='Headless', directory='examples/headless', kind='render', runtime=True,
                              binary=str(root / 'example'))
                report = self.ci.Report(root / 'logs', 'fixture', 'vulkan')
                def run(command, **kwargs):
                    self.assertEqual(command[1:2], ['--mode'])
                    capture = Path(command[command.index('--capture') + 1])
                    last = 128 if mismatch and command[2] == 'parallel' else 255
                    capture.write_bytes(b'P6\n3 1\n255\n' + bytes((255, 0, 0, 0, 255, 0, 0, 0, last)))
                    return types.SimpleNamespace(returncode=0, stdout='gpu ok')
                with (mock.patch.object(self.ci.subprocess, 'run', side_effect=run) as process,
                      mock.patch.object(observe, 'observe') as window):
                    arguments = (root, root, 'Debug', dict(api='vulkan', gpu_checks=False, targets=[target], unsupported=[]),
                                 [], report, {})
                    if mismatch:
                        with self.assertRaises(self.ci.CiError) as error:
                            self.ci.runtime_checks(*arguments)
                        self.assertEqual(error.exception.status, 'FAIL')
                    else:
                        self.ci.runtime_checks(*arguments)
                    self.assertEqual(process.call_count, 2)
                    window.assert_not_called()
                self.assertEqual(report.results[-1]['status'], 'FAIL' if mismatch else 'PASS')
                self.assertEqual(len(list((root / 'logs').rglob('readback.ppm'))), 2)
                self.assertEqual(len(list((root / 'logs').rglob('process.log'))), 2)

    def test_headless_timeout_keeps_diagnostics_and_runs_remaining_case(self):
        import checks
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            profile = dict(kind='headless-compare', cases=[dict(name=name, arguments=[]) for name in ('serial', 'parallel')])
            timeout = subprocess.TimeoutExpired(['example'], 120, output=b'partial diagnostic')
            with mock.patch.object(self.ci.subprocess, 'run', side_effect=[timeout, types.SimpleNamespace(returncode=0, stdout='done')]) as process:
                result = self.ci.observe_headless_comparison(root / 'example', profile, {}, root)
            self.assertEqual(process.call_count, 2)
            self.assertTrue(result['forced_termination'])
            self.assertEqual((root / 'serial/process.log').read_text(), 'partial diagnostic')
            self.assertEqual(checks.check_observation(profile, result)['status'], 'FAIL')

    def test_headless_profile_cannot_override_generated_capture_path(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            profile = dict(cases=[dict(name='serial', arguments=['--capture', 'stale.ppm']),
                                 dict(name='parallel', arguments=[])])
            with mock.patch.object(self.ci.subprocess, 'run') as process:
                with self.assertRaises(self.ci.CiError):
                    self.ci.observe_headless_comparison(root / 'example', profile, {}, root)
                process.assert_not_called()

    def test_push_deletion_dedup_and_invalid_input(self):
        zero, sha = '0' * 40, 'a' * 40
        lines = f'refs/heads/a {sha} refs/heads/a {zero}\nrefs/heads/b {sha} refs/heads/b {zero}\n(delete) {zero} refs/heads/deleted {sha}\n'
        self.assertEqual(self.ci.push_updates(lines), [(sha, zero)])
        mixed = f'refs/heads/a {sha} refs/heads/a {"b" * 40}\nrefs/heads/new {sha} refs/heads/new {zero}\n'
        self.assertEqual(self.ci.push_updates(mixed), [(sha, zero)])
        with self.assertRaises(self.ci.CiError):
            self.ci.push_updates('not a push update\n')







    def test_snapshot_excludes_dirty_files_and_removes_previous_revision_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'repo'
            root.mkdir()
            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), '-c', 'user.name=CI Fixture',
                                                '-c', 'user.email=ci@example.invalid',
                                                '-c', 'core.hooksPath=disabled-hooks', *args], text=True).strip()
            git('init', '-q')
            (root / 'code.cpp').write_text('committed\n', encoding='utf-8')
            (root / 'obsolete.cpp').write_text('old\n', encoding='utf-8')
            git('add', '.')
            git('commit', '-qm', 'fixture one')
            first = git('rev-parse', 'HEAD')
            cache = root / 'build-ci/push'
            snapshot = self.ci.materialize_revision(root, first, cache)
            self.assertEqual((snapshot / 'code.cpp').read_text(), 'committed\n')
            (root / 'code.cpp').write_text('dirty\n', encoding='utf-8')
            (root / 'secret-untracked').write_text('must not be copied', encoding='utf-8')
            snapshot = self.ci.materialize_revision(root, first, cache)
            self.assertEqual((snapshot / 'code.cpp').read_text(), 'committed\n')
            self.assertFalse((snapshot / 'secret-untracked').exists())
            (root / 'obsolete.cpp').unlink()
            git('add', 'code.cpp', 'obsolete.cpp')
            git('commit', '-qm', 'fixture two')
            snapshot = self.ci.materialize_revision(root, git('rev-parse', 'HEAD'), cache)
            self.assertFalse((snapshot / 'obsolete.cpp').exists())
            self.assertEqual((snapshot / 'code.cpp').read_text(), 'dirty\n')
            self.assertTrue((root / 'secret-untracked').exists())

    def test_selection_evidence_must_match_revision_and_docs_need_no_toolchain(self):
        import json
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'src').mkdir()
            (root / 'src/a.cpp').write_text('int a;')
            evidence = root / 'selection.json'
            evidence.write_text(json.dumps(dict(version=1, revision='fixture', paths=['README.md'])))
            with self.assertRaises(self.ci.CiError) as error:
                self.ci.read_impact(evidence, 'another-commit')
            self.assertEqual(error.exception.status, 'BLOCKED')
            with mock.patch.object(self.ci, 'setup_environment') as setup:
                code = self.ci.main(['run', '--phase', 'runtime', '--api', 'vulkan',
                                     '--root', str(root), '--revision', 'fixture', '--changes-file', str(evidence)])
            self.assertEqual(code, 0)
            setup.assert_not_called()

    def test_real_push_docs_skip_and_changed_example_cannot_be_hidden_by_dirty_rules(self):
        import json
        with tempfile.TemporaryDirectory() as folder:
            root, remote = Path(folder) / 'repo', Path(folder) / 'remote.git'
            root.mkdir()
            env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1')
            def git(*args, check=True):
                return subprocess.run(['git', '-C', str(root), '-c', 'user.name=CI Fixture',
                                       '-c', 'user.email=ci@example.invalid', *args], env=env,
                                      capture_output=True, text=True, check=check)
            git('init', '-q')
            subprocess.run(['git', 'init', '--bare', '-q', str(remote)], env=env, check=True)
            scripts = root / '.github/ci'
            scripts.mkdir(parents=True)
            for name in ('ci.py', 'selection.py'):
                (scripts / name).write_bytes(SCRIPT.with_name(name).read_bytes())
            (root / 'examples/Cube').mkdir(parents=True)
            (root / 'examples/Cube/main.cpp').write_text('int main() {}')
            (root / 'README.md').write_text('first')
            git('add', '.')
            git('commit', '-qm', 'initial')
            first = git('rev-parse', 'HEAD').stdout.strip()
            git('push', str(remote), 'HEAD:refs/heads/main')
            self.assertIsNone(self.ci.changed_paths(root, first, '0' * 40))
            self.assertEqual(self.ci.changed_paths(root, first, first), [])
            installed = subprocess.run([sys.executable, '-B', str(scripts / 'ci.py'), 'install-hook',
                                        '--root', str(root)], capture_output=True, text=True, env=env)
            self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
            (root / 'README.md').write_text('docs only')
            git('add', 'README.md')
            git('commit', '-qm', 'docs')
            pushed = git('push', str(remote), 'HEAD:refs/heads/main')
            self.assertIn('[SKIPPED]', pushed.stdout)
            self.assertFalse((root / 'build-ci/push/builds').exists())
            base = git('rev-parse', 'HEAD').stdout.strip()
            git('mv', 'examples/Cube/main.cpp', 'examples/Cube/renamed.cpp')
            git('commit', '-qm', 'rename source')
            tip = git('rev-parse', 'HEAD').stdout.strip()
            self.assertEqual(set(self.ci.changed_paths(root, tip, base)),
                             {'examples/Cube/main.cpp', 'examples/Cube/renamed.cpp'})
            (scripts / 'selection.py').write_text('def plan(paths): return {"mode": "none"}\n')
            pushed = git('push', str(remote), 'HEAD:refs/heads/main', check=False)
            self.assertNotEqual(pushed.returncode, 0)  # No product CMake exists in this fixture.
            evidence = json.loads((root / 'build-ci/push/selection.json').read_text())
            self.assertEqual(evidence['mode'], 'related')
            self.assertEqual(evidence['revision'], tip)
            actual = subprocess.check_output(['git', '--git-dir', str(remote), 'rev-parse',
                                              'refs/heads/main'], text=True).strip()
            self.assertEqual(actual, base)

    def test_commit_does_not_run_checks_and_failed_push_preserves_old_hook(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'repo'
            root.mkdir()
            remote = Path(folder) / 'remote.git'
            env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1')
            def git(*args, check=True):
                return subprocess.run(['git', '-C', str(root), '-c', 'user.name=CI Fixture',
                                       '-c', 'user.email=ci@example.invalid', *args], env=env,
                                      capture_output=True, text=True, check=check)
            git('init', '-q')
            subprocess.run(['git', 'init', '--bare', '-q', str(remote)], env=env, check=True)
            ci_dir = root / '.github/ci'
            ci_dir.mkdir(parents=True)
            (ci_dir / 'ci.py').write_bytes(SCRIPT.read_bytes())
            (ci_dir / 'selection.py').write_bytes(SCRIPT.with_name('selection.py').read_bytes())
            (root / 'README.md').write_text('fixture', encoding='utf-8')
            git('add', '.')
            git('commit', '-qm', 'fixture initial')
            old = root / '.git/hooks/pre-push'
            old.write_text('#!/bin/sh\ncat >prior-input\n', encoding='utf-8')
            old.chmod(0o755)
            install = subprocess.run([sys.executable, '-B', str(ci_dir / 'ci.py'), 'install-hook', '--root', str(root)],
                                     capture_output=True, text=True, env=env)
            self.assertEqual(install.returncode, 0, install.stdout + install.stderr)
            self.assertFalse((root / '.git/hooks/pre-commit').exists())
            self.assertTrue((root / '.git/hooks/pre-push.before-dy-ci').is_file())
            (root / 'README.md').write_text('second commit', encoding='utf-8')
            git('add', 'README.md')
            git('commit', '-qm', 'fixture after hook install')
            self.assertFalse((root / 'prior-input').exists())
            self.assertFalse((root / 'build-ci/push').exists())
            pushed = git('push', str(remote), 'HEAD:refs/heads/main', check=False)
            self.assertNotEqual(pushed.returncode, 0)  # Fixture deliberately lacks the framework build.
            self.assertTrue((root / 'prior-input').read_text().startswith('HEAD '))
            refs = subprocess.run(['git', '--git-dir', str(remote), 'show-ref'], capture_output=True, text=True)
            self.assertEqual(refs.stdout, '')


if __name__ == '__main__':
    unittest.main()
