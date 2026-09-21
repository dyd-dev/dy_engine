"""Automatic push impact selection from Git paths and CMake's input inventory."""
from pathlib import Path, PurePosixPath


APIS = ['vulkan', 'd3d12', 'metal', 'null']


def plan(paths):
    def result(mode, reason, apis=APIS, cpu=True):
        return dict(version=1, mode=mode, reason=reason, paths=paths, apis=list(apis), cpu=cpu)
    if paths is None:
        return result('all', 'No trustworthy previous remote commit; select all checks')
    if not isinstance(paths, list) or not all(isinstance(p, str) for p in paths):
        raise ValueError('Changed paths must be a list of strings or null')
    # Only known prose locations are unrelated. Unknown config/code always selects all.
    paths = [p for p in paths if not ((p.startswith('docs/') or '/' not in p
                                      or p in ('.github/ci/README.md', '.github/ci/VALIDATION.md'))
                                     and PurePosixPath(p).suffix.lower() in ('.md', '.rst'))]
    if not paths:
        return result('none', 'No example-affecting changes', [], False)
    backends = {'src/Backends/' + api + '/': api.lower() for api in ('Vulkan', 'D3D12', 'Metal', 'Null')}
    affected = {api for prefix, api in backends.items() if any(p.startswith(prefix) for p in paths)}
    if affected and all(any(p.startswith(prefix) for prefix in backends) for p in paths):
        return result('all', 'Backend-only change', [a for a in APIS if a in affected], 'null' in affected)
    if all(p.startswith('examples/') and len(PurePosixPath(p).parts) >= 3 for p in paths):
        # Headers can be included across targets without appearing in target SOURCES.
        if any(PurePosixPath(p).suffix.lower() in ('.h', '.hpp', '.hxx', '.inc', '.cmake')
               or PurePosixPath(p).name == 'CMakeLists.txt' for p in paths):
            return result('all', 'Example header/build dependency cannot be safely narrowed')
        shader_apis = {'.glsl': 'vulkan', '.hlsl': 'd3d12', '.metal': 'metal'}
        if all(PurePosixPath(p).suffix in shader_apis for p in paths):
            selected = {shader_apis[PurePosixPath(p).suffix] for p in paths}
            return result('related', 'Backend-specific example shaders', [a for a in APIS if a in selected], False)
        return result('related', 'Resolve changed example inputs through CMake')
    return result('all', 'Shared or unknown dependency changed; select all checks')


def select(root, manifest, programs, impact, phase):
    """Select build AND execution targets; missing dependency evidence expands coverage."""
    targets = manifest['targets']
    excluded = manifest.get('unsupported', [])
    all_targets, all_programs = targets, programs
    mode, reason = impact['mode'], impact['reason']
    api = manifest.get('api')
    if mode == 'none' or api not in impact['apis']:
        targets, programs, excluded = [], [], []
        mode = 'none'
    elif mode == 'related':
        paths = [(Path(root) / p).resolve() for p in impact['paths']]
        def contains(input_path, changed):
            return changed == input_path or input_path in changed.parents
        def matches(item):
            inputs = [Path(p).resolve() for p in item.get('inputs', [])]
            if 'directory' in item:
                inputs.append((Path(root) / item['directory']).resolve())
            return any(contains(p, changed) for p in inputs for changed in paths)
        evidence = all(t.get('inputs_complete') is True for t in targets + programs)
        mapped = all(any(contains(Path(p).resolve(), changed)
                         for item in targets + programs for p in item.get('inputs', []))
                     or any(contains((Path(root) / item['directory']).resolve(), changed)
                            for item in targets + excluded)
                     for changed in paths)
        if not evidence or not mapped:
            mode, reason = 'all', 'Incomplete CMake inputs or unmapped change; select all checks'
        else:
            targets = [t for t in targets if matches(t)]
            programs = [p for p in programs if matches(p)]
            excluded = [t for t in excluded if matches(t)]
    if mode == 'all':
        targets, programs = all_targets, all_programs
    if phase == 'cpu':
        targets = [t for t in targets if t.get('kind') == 'cpu']
        programs = [p for p in programs if p.get('kind') == 'cpu-check']
    elif phase == 'runtime':
        programs = [p for p in programs if p.get('kind') in ('rhi-smoke', 'gpu-probe')]
    elif not impact['cpu']:
        programs = [p for p in programs if p.get('kind') != 'cpu-check']
    return dict(manifest, targets=targets, unsupported=excluded,
                gpu_checks=mode == 'all' and api != 'null', selection_reason=reason, selection_mode=mode), programs


def github_outputs(impact):
    platforms = {'vulkan': [('Windows', 'windows-2022'), ('Ubuntu', 'ubuntu-24.04')],
                 'd3d12': [('Windows', 'windows-2022')], 'metal': [('macOS', 'macos-15')],
                 'null': [('Ubuntu', 'ubuntu-24.04')]}
    build = [dict(platform=platform, os=os, api=api, config=config)
             for api in impact['apis'] for platform, os in platforms[api]
             for config in (['Release'] if api == 'null' else ['Debug', 'Release'])]
    runtime = [dict(api=api, runner='dy-ci-macos-metal' if api == 'metal' else 'dy-ci-windows-gpu')
               for api in impact['apis'] if api != 'null']
    return dict(heavy=bool(build), cpu=impact['cpu'], runtime=bool(runtime),
                consumer=impact['mode'] == 'all' and impact['cpu'],
                build_matrix=dict(include=build), runtime_matrix=dict(include=runtime))
