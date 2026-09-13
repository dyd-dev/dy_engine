"""Exercise example discovery against real, disposable CMake projects."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


MODULE = Path(__file__).resolve().parents[2] / "cmake" / "ExampleSupport.cmake"


class DiscoveryTests(unittest.TestCase):
    def test_non_render_and_null_examples_need_no_shaders(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'main.cpp').write_text('int main() {}\n', encoding='utf-8')
            for kind, vulkan in (('window', 'ON'), ('cpu', 'ON'), ('', 'OFF')):
                declaration = f'dy_example_support(KIND {kind})\n' if kind else ''
                (root / 'CMakeLists.txt').write_text(
                    'cmake_minimum_required(VERSION 3.20)\nproject(Probe CXX)\n'
                    f'include("{MODULE.as_posix()}")\n' + declaration +
                    'add_executable(Probe main.cpp)\ndy_example_shaders(Probe)\n', encoding='utf-8')
                result = subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'),
                                         '-DDY_CI=ON', '-DUSE_VULKAN=' + vulkan], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unknown_shader_stage_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'Shaders').mkdir()
            (root / 'Shaders/unknown.glsl').write_text('void main() {}\n', encoding='utf-8')
            (root / 'main.cpp').write_text('int main() {}\n', encoding='utf-8')
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(Probe CXX)\n'
                f'include("{MODULE.as_posix()}")\n'
                'add_executable(Probe main.cpp)\ndy_example_shaders(Probe)\n', encoding='utf-8')
            result = subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'), '-DUSE_VULKAN=ON'],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('unknown shader stage', result.stdout + result.stderr)

    def test_real_compile_and_link_failures_are_not_hidden(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(Probe CXX)\n'
                'add_executable(Probe main.cpp)\n', encoding='utf-8')
            source = root / 'main.cpp'
            source.write_text('int main() { return 0; }\n', encoding='utf-8')
            configured = subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build')],
                                        capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            def build():
                return subprocess.run(['cmake', '--build', str(root / 'build'), '--config', 'Debug',
                                       '--target', 'Probe'], capture_output=True, text=True)
            self.assertEqual(build().returncode, 0)
            for broken in ('int main() { syntax error; }\n',
                           'extern int missing(); int main() { return missing(); }\n'):
                source.write_text(broken, encoding='utf-8')
                result = build()
                self.assertNotEqual(result.returncode, 0, 'Broken target unexpectedly built')
                self.assertIn('Probe', result.stdout + result.stderr)

    def test_asset_only_updates_and_deletions(self):
        script = MODULE.with_name('SyncAssets.cmake')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, build = root / 'input', root / 'build'
            source.mkdir()
            build.mkdir()
            target = build / 'example/Models'
            def sync(destination=target):
                return subprocess.run(['cmake', '-DSOURCE=' + str(source), '-DDESTINATION=' + str(destination),
                                       '-DBUILD_ROOT=' + str(build), '-P', str(script)], capture_output=True, text=True)
            (source / 'model.obj').write_text('first', encoding='utf-8')
            self.assertEqual(sync().returncode, 0)
            self.assertEqual((target / 'model.obj').read_text(), 'first')
            (source / 'model.obj').write_text('changed without relink', encoding='utf-8')
            self.assertEqual(sync().returncode, 0)
            self.assertEqual((target / 'model.obj').read_text(), 'changed without relink')
            (source / 'model.obj').unlink()
            self.assertEqual(sync().returncode, 0)
            self.assertFalse((target / 'model.obj').exists())
            self.assertNotEqual(sync(root / 'outside-build').returncode, 0)

    def test_add_rename_remove_and_api_exclusions(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "examples").mkdir()
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(Probe CXX)\n'
                'add_library(Probe INTERFACE)\n'
                f'include("{MODULE.as_posix()}")\n'
                'add_subdirectory(examples)\n', encoding="utf-8")
            (root / "examples/CMakeLists.txt").write_text('dy_discover_examples()\n', encoding="utf-8")

            def example(name, declaration):
                path = root / "examples" / name
                path.mkdir()
                (path / "main.cpp").write_text('int main() { return 0; }\n', encoding="utf-8")
                (path / "CMakeLists.txt").write_text(
                    declaration + '\nadd_executable(' + name + ' main.cpp)\n'
                    'set_target_properties(' + name + ' PROPERTIES LINKER_LANGUAGE CXX)\n', encoding="utf-8")

            def configure():
                result = subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'),
                                         '-DDY_CI=ON', '-DUSE_VULKAN=ON'],
                                        capture_output=True, text=True, encoding="utf-8", errors="replace")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                files = sorted((root / 'build').glob('ci-manifest-*.json'))
                return json.loads(files[0].read_text(encoding="utf-8"))

            example('First', 'dy_example_support(KIND cpu)')
            example('MetalOnly', 'dy_example_support(APIS metal REASON "Metal only")')
            manifest = configure()
            self.assertEqual([x['name'] for x in manifest['targets']], ['First'])
            self.assertEqual(Path(manifest['targets'][0]['sources'][0]).name, 'main.cpp')
            self.assertEqual(len(manifest['unsupported']), 1)
            example('Added', 'dy_example_support(KIND cpu)')
            self.assertEqual({x['name'] for x in configure()['targets']}, {'First', 'Added'})
            (root / 'examples/Added').rename(root / 'examples/Renamed')
            renamed = root / 'examples/Renamed/CMakeLists.txt'
            renamed.write_text(renamed.read_text().replace('Added', 'Renamed'), encoding="utf-8")
            self.assertEqual({x['name'] for x in configure()['targets']}, {'First', 'Renamed'})
            shutil.rmtree(root / 'examples/First')
            self.assertEqual([x['name'] for x in configure()['targets']], ['Renamed'])


if __name__ == '__main__':
    unittest.main()
