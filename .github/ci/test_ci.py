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

    def test_external_runtime_cache_is_not_old_instrumented_cache(self):
        self.assertEqual(self.ci.build_name('runtime', 'vulkan', 'Debug'), 'runtime-external-vulkan-Debug')

    def test_runtime_records_later_evidence_after_a_failed_example(self):
        import json
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / '.github/ci').mkdir(parents=True)
            profiles = {'version': 1, 'examples': {'examples/first': {'kind': 'render-graph'},
                                                  'examples/second': {'kind': 'render-graph'}}}
            (root / '.github/ci/examples.json').write_text(json.dumps(profiles), encoding='utf-8')
            targets = [dict(name=name, directory='examples/' + name, kind='cpu', runtime=True,
                            binary=str(root / name)) for name in ('first', 'second')]
            report = self.ci.Report(root / 'logs', 'fixture', 'null')
            good = ('[VERIFICATION PASSED]\n-> Executing [ShadowPass]\n'
                    '-> Executing [MainForwardPass]\n-> Executing [PostProcessingPass]\n')
            with mock.patch.object(report, 'command', side_effect=['no callbacks', good]):
                with self.assertRaises(self.ci.CiError) as failure:
                    self.ci.runtime_checks(root, root, 'Debug',
                                          dict(api='null', targets=targets, unsupported=[]), [], report, {})
            self.assertEqual(failure.exception.status, 'FAIL')
            self.assertEqual([result['status'] for result in report.results], ['FAIL', 'PASS'])
            self.assertEqual(len(list((root / 'logs').rglob('checks.json'))), 2)

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

    def test_document_changes_are_narrowly_classified(self):
        self.assertTrue(self.ci.documentation_only(['README.md', 'docs/design.md']))
        for files in ([], ['CMakeLists.txt'], ['src/README.md', 'src/RHI/IDevice.h'], ['.github/ci/README.md']):
            self.assertFalse(self.ci.documentation_only(files))

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
