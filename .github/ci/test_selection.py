"""Push impact selection: narrow known inputs, never guess unknown dependencies."""
from pathlib import Path
import unittest

import selection


class SelectionTests(unittest.TestCase):
    def test_baseline_docs_backend_and_unknown_changes(self):
        self.assertEqual(selection.plan(None)['mode'], 'all')
        self.assertEqual(selection.plan([])['mode'], 'none')
        self.assertEqual(selection.plan(['README.md', 'docs/a.md'])['mode'], 'none')
        backend = selection.plan(['src/Backends/Vulkan/VulkanDevice.cpp'])
        self.assertEqual(backend['apis'], ['vulkan'])
        self.assertFalse(backend['cpu'])
        for path in ('src/RHI/IDevice.cpp', 'cmake/ExampleSupport.cmake', 'new-config.toml',
                     'examples/Cube/shared.h'):
            self.assertEqual(selection.plan([path])['mode'], 'all')

    def inventory(self):
        root = Path.cwd()
        def target(name, inputs):
            return dict(name=name, directory='examples/' + name, kind='render', runtime=True,
                        inputs=[str(root / 'examples' / name), *[str(root / p) for p in inputs]],
                        inputs_complete=True)
        return root, dict(api='vulkan', unsupported=[], targets=[
            target('Cube', []), target('Model', ['examples/Model/Models']),
            target('Instances', ['examples/Model/Models'])]), []

    def test_example_and_shared_asset_are_selected_without_manual_target_list(self):
        root, manifest, programs = self.inventory()
        for path, names in [('examples/Cube/main.cpp', ['Cube']),
                            ('examples/Model/Models/removed.glb', ['Model', 'Instances'])]:
            chosen, checks = selection.select(root, manifest, programs, selection.plan([path]), 'runtime')
            self.assertEqual([t['name'] for t in chosen['targets']], names)
            self.assertFalse(chosen['gpu_checks'])
            self.assertEqual(checks, [])

    def test_unmapped_or_opaque_input_falls_back_to_all(self):
        root, manifest, programs = self.inventory()
        for path in ('examples/deleted/main.cpp', 'examples/Cube/main.cpp'):
            if 'Cube' in path:
                manifest['targets'][1]['inputs_complete'] = False
            chosen, _ = selection.select(root, manifest, programs, selection.plan([path]), 'runtime')
            self.assertEqual(len(chosen['targets']), 3)
            self.assertTrue(chosen['gpu_checks'])

    def test_unsupported_example_does_not_select_unrelated_examples(self):
        root, manifest, programs = self.inventory()
        manifest['unsupported'] = [dict(directory='examples/MetalOnly', reason='not this API')]
        chosen, _ = selection.select(root, manifest, programs,
                                     selection.plan(['examples/MetalOnly/main.cpp']), 'runtime')
        self.assertEqual(chosen['targets'], [])
        self.assertEqual(len(chosen['unsupported']), 1)

    def test_shader_backend_and_github_jobs_are_filtered(self):
        plan = selection.plan(['examples/Cube/Shaders/mesh_vs.glsl'])
        self.assertEqual(plan['apis'], ['vulkan'])
        self.assertFalse(plan['cpu'])
        output = selection.github_outputs(plan)
        self.assertEqual({row['api'] for row in output['build_matrix']['include']}, {'vulkan'})
        self.assertEqual({row['api'] for row in output['runtime_matrix']['include']}, {'vulkan'})
        self.assertFalse(selection.github_outputs(selection.plan([]))['heavy'])

    def test_cpu_program_uses_the_same_shared_asset_dependencies(self):
        root, manifest, _ = self.inventory()
        programs = [dict(name='Cpu', kind='cpu-check', inputs_complete=True,
                         inputs=[str(root / 'examples/Model/Models')])]
        chosen, checks = selection.select(root, manifest, programs,
                                         selection.plan(['examples/Model/Models/new.glb']), 'cpu')
        self.assertEqual(chosen['targets'], [])
        self.assertEqual([p['name'] for p in checks], ['Cpu'])

    def test_cmake_collects_shared_library_and_asset_inputs_and_builds_only_selected(self):
        import json
        import subprocess
        import tempfile
        module = Path(__file__).resolve().parents[2] / 'cmake/ExampleSupport.cmake'
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'examples/A').mkdir(parents=True)
            (root / 'examples/B').mkdir()
            (root / 'shared/Models').mkdir(parents=True)
            (root / 'cmake').mkdir()
            (root / 'cmake/SyncAssets.cmake').write_bytes(module.with_name('SyncAssets.cmake').read_bytes())
            (root / 'shared/lib.cpp').write_text('int helper() { return 0; }')
            (root / 'examples/A/main.cpp').write_text('extern int helper(); int main() { return helper(); }')
            (root / 'examples/B/main.cpp').write_text('unrelated deliberate syntax error')
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(Selection CXX)\n'
                f'include("{module.as_posix()}")\n'
                'add_library(Engine STATIC $<TARGET_OBJECTS:Shared>)\nadd_subdirectory(examples)\n')
            (root / 'examples/CMakeLists.txt').write_text('dy_discover_examples()\n')
            (root / 'examples/A/CMakeLists.txt').write_text(
                'dy_example_support(KIND cpu)\nadd_executable(A main.cpp)\n'
                'target_link_libraries(A PRIVATE Engine)\n'
                'dy_example_assets(A "${CMAKE_SOURCE_DIR}/shared/Models" Models)\n')
            (root / 'examples/B/CMakeLists.txt').write_text(
                'dy_example_support(KIND cpu)\n'
                'add_library(Shared OBJECT "${CMAKE_SOURCE_DIR}/shared/lib.cpp")\n'
                'add_executable(B main.cpp)\n')
            configured = subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'),
                                         '-DCMAKE_BUILD_TYPE=Debug'], capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            manifest = json.loads((root / 'build/ci-manifest-Debug.json').read_text())
            a = next(t for t in manifest['targets'] if t['name'] == 'A')
            self.assertIn((root / 'shared/lib.cpp').as_posix(), a['inputs'])
            self.assertIn((root / 'shared/Models').as_posix(), a['inputs'])
            selected, _ = selection.select(root, manifest, [],
                                            selection.plan(['examples/A/main.cpp']), 'build')
            names = [t['name'] for t in selected['targets']]
            self.assertEqual(names, ['A'])
            built = subprocess.run(['cmake', '--build', str(root / 'build'), '--config', 'Debug',
                                    '--target', *names], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)


if __name__ == '__main__':
    unittest.main()
