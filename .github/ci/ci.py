"""Push-only CI. All checks run against an explicit commit or an explicit working-copy run.

full --revision COMMIT materializes Git's tree without touching the user's checkout.
run is also used by Actions and while developing the checks themselves.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import time
import uuid
import xml.etree.ElementTree as ET

import selection


ROOT = Path(__file__).resolve().parents[2]
APIS = ('vulkan', 'd3d12', 'metal', 'null')
SHA = re.compile(r'^[0-9a-f]{40}(?:[0-9a-f]{24})?$')


class CiError(RuntimeError):
    def __init__(self, message, status='FAIL'):
        super().__init__(message)
        self.status = status


def git(root, *arguments):
    result = subprocess.run(['git', '-C', str(root), *arguments], capture_output=True,
                            text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        raise CiError(result.stderr.strip() or 'Git command failed', 'BLOCKED')
    return result.stdout.strip()


def resolve_commit(root, revision):
    value = git(root, 'rev-parse', '--verify', '--end-of-options', revision + '^{commit}')
    if not SHA.fullmatch(value):
        raise CiError('Expected a full commit object ID', 'BLOCKED')
    return value


def push_updates(text):
    updates = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) != 4 or not SHA.fullmatch(fields[1]) or not SHA.fullmatch(fields[3]):
            raise CiError('Malformed pre-push update', 'BLOCKED')
        _, local, _, remote = fields
        if set(local) == {'0'}:
            continue
        if local in updates and updates[local] != remote:
            updates[local] = '0' * len(local)  # Different remote baselines require a full check.
        else:
            updates[local] = remote
    return list(updates.items())


def changed_paths(root, revision, base):
    if not base or set(base) == {'0'}:
        return None  # Unknown baseline must differ from a known empty change set.
    try:
        base = resolve_commit(root, base)
    except CiError:
        return None
    output = subprocess.check_output(['git', '-C', str(root), 'diff', '--no-renames', '--name-only', '-z', base, revision])
    return [x.decode('utf-8', errors='strict') for x in output.split(b'\0') if x]


def materialize_revision(root, revision, cache):
    """Synchronize only an owned build snapshot; preserve unchanged mtimes for incremental builds."""
    root, cache = Path(root).resolve(), Path(cache).resolve()
    expected = root / 'build-ci' / 'push'
    if cache != expected or cache.is_symlink():
        raise CiError('Snapshot cache must be the repository-owned build-ci/push directory', 'BLOCKED')
    cache.mkdir(parents=True, exist_ok=True)
    source = cache / 'source'
    marker = source / '.dy-ci-snapshot'
    if source.is_symlink() or (source.exists() and not marker.is_file()):
        raise CiError('Refusing to overwrite an unowned snapshot directory', 'BLOCKED')
    source.mkdir(exist_ok=True)
    if marker.exists() and marker.read_text(encoding='utf-8') != str(root):
        raise CiError('Snapshot ownership mismatch', 'BLOCKED')
    marker.write_text(str(root), encoding='utf-8')
    process = subprocess.run(['git', '-C', str(root), 'archive', '--format=tar', revision], capture_output=True)
    if process.returncode:
        raise CiError('Cannot export the pushed commit', 'BLOCKED')
    revision_marker = source / '.dy-ci-revision'
    members, wanted = [], {marker, revision_marker}
    with tarfile.open(fileobj=io.BytesIO(process.stdout), mode='r:') as archive:
        for item in archive.getmembers():
            relative = PurePosixPath(item.name)
            if (relative.is_absolute() or '..' in relative.parts or '\\' in item.name or ':' in item.name
                    or item.name in ('.dy-ci-snapshot', '.dy-ci-revision') or not (item.isfile() or item.isdir())):
                raise CiError(f'Unsupported or unsafe Git tree entry: {item.name}', 'BLOCKED')
            path = source.joinpath(*relative.parts)
            if not path.resolve().is_relative_to(source.resolve()):
                raise CiError('Snapshot path escapes its owned directory', 'BLOCKED')
            members.append((item, path))
            if item.isfile():
                wanted.add(path)
        # Existing symlinks are never traversed or rewritten.
        for path in source.rglob('*'):
            if path.is_symlink():
                raise CiError('Snapshot contains a symlink; remove this cache explicitly', 'BLOCKED')
        for path in sorted(source.rglob('*'), key=lambda p: len(p.parts), reverse=True):
            if path.is_file() and path not in wanted:
                path.unlink()
            elif path.is_dir() and not any(path.iterdir()):
                path.rmdir()
        for item, path in members:
            if item.isdir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                data = archive.extractfile(item).read()
                if not path.is_file() or path.read_bytes() != data:
                    path.write_bytes(data)
                if os.name != 'nt':
                    path.chmod(item.mode & 0o777)
    revision_marker.write_text(revision, encoding='utf-8')
    return source


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def source_digest(root):
    digest = hashlib.sha256()
    files = [root / 'CMakeLists.txt']
    for directory in ('src', 'examples', 'cmake', '.github/ci'):
        files.extend(p for p in (root / directory).rglob('*')
                     if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc')
    for path in sorted(files):
        digest.update(path.relative_to(root).as_posix().encode('utf-8'))
        digest.update(file_digest(path).encode('ascii'))
    return digest.hexdigest()


def product_fingerprint(root):
    """Protect source and shaders: CI may generate artifacts, never rewrite the program."""
    suffixes = {'.h', '.hpp', '.cpp', '.cc', '.c', '.mm', '.glsl', '.hlsl', '.metal', '.inc'}
    return {path.relative_to(root).as_posix(): file_digest(path)
            for directory in ('src', 'examples') for path in (root / directory).rglob('*')
            if path.is_file() and path.suffix in suffixes}


def terminate(process):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True)
    else:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
    process.wait()


class Report:
    def __init__(self, directory, revision, api, source_mode='working-copy'):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.revision, self.api, self.results = revision, api, []
        self.compiler = None
        self.source_mode = source_mode

    def add(self, name, status, message='', seconds=0, log=None):
        self.results.append(dict(name=name, status=status, message=message, seconds=seconds,
                                 log=str(log) if log else None))
        print(f'[{status}] {name}: {message}', flush=True)

    def command(self, name, arguments, cwd, timeout=1800, env=None):
        log = self.directory / (re.sub(r'[^A-Za-z0-9_.-]', '_', name) + '.log')
        started = time.monotonic()
        with log.open('w', encoding='utf-8') as output:
            output.write(json.dumps([str(x) for x in arguments], ensure_ascii=False) + '\n')
            output.flush()
            try:
                process = subprocess.Popen([str(x) for x in arguments], cwd=cwd, env=env,
                                           stdout=output, stderr=subprocess.STDOUT,
                                           start_new_session=os.name != 'nt')
            except OSError as error:
                raise CiError(f'{name}: {error}', 'BLOCKED') from error
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                terminate(process)
                self.add(name, 'FAIL', f'Timed out after {timeout}s', time.monotonic() - started, log)
                raise CiError(f'{name} timed out; see {log}')
        text = log.read_text(encoding='utf-8', errors='replace')
        if code:
            known_checker = Path(str(arguments[0])).stem.startswith('Ci')
            status = 'BLOCKED' if code == 2 and known_checker else 'FAIL'
            self.add(name, status, f'Exit {code}; {log}', time.monotonic() - started, log)
            print('\n'.join(text.splitlines()[-18:]), flush=True)
            raise CiError(f'{name} exited {code}', status)
        self.add(name, 'PASS', str(log), time.monotonic() - started, log)
        return text

    def save(self):
        document = dict(revision=self.revision, source_mode=self.source_mode, api=self.api,
                        compiler=self.compiler, results=self.results)
        (self.directory / 'result.json').write_text(json.dumps(document, indent=2), encoding='utf-8')
        suite = ET.Element('testsuite', name='dy_engine CI', tests=str(len(self.results)))
        for result in self.results:
            case = ET.SubElement(suite, 'testcase', name=result['name'], time=str(result['seconds']))
            if result['status'] in ('FAIL', 'BLOCKED'):
                ET.SubElement(case, 'failure', type=result['status']).text = result['message']
            elif result['status'] in ('UNSUPPORTED', 'SKIPPED'):
                ET.SubElement(case, 'skipped').text = result['message']
        ET.ElementTree(suite).write(self.directory / 'junit.xml', encoding='utf-8', xml_declaration=True)
        summary = [f'Commit: `{self.revision}` / API: `{self.api}` / Source: `{self.source_mode}`',
                   '', '| Check | Result |', '|---|---|']
        if self.compiler:
            summary.insert(1, f'Compiler: `{self.compiler}`')
        summary.extend(f"| {r['name']} | {r['status']} |" for r in self.results)
        content = '\n'.join(summary) + '\n'
        (self.directory / 'summary.md').write_text(content, encoding='utf-8')
        if os.environ.get('GITHUB_STEP_SUMMARY'):
            with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as output:
                output.write(content)


def native_apis():
    return ['vulkan', 'd3d12'] if sys.platform == 'win32' else ['metal'] if sys.platform == 'darwin' else ['vulkan']


def setup_environment(root):
    env = os.environ.copy()
    paths = []
    if os.name == 'nt':
        vswhere = Path(env.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        if vswhere.is_file():
            install = subprocess.check_output([str(vswhere), '-latest', '-property', 'installationPath'], text=True).strip()
            if install:
                vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
                if vcvars.is_file():
                    result = subprocess.run(f'call "{vcvars}" >nul && set', shell=True,
                                            capture_output=True, text=True, errors='replace')
                    if result.returncode:
                        raise CiError('Visual Studio environment initialization failed', 'BLOCKED')
                    for line in result.stdout.splitlines():
                        key, separator, value = line.partition('=')
                        if separator and key and not key.startswith('='):
                            env[key] = value
                    if env.get('VCToolsInstallDir'):
                        paths.append(str(Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64'))
                paths.append(str(Path(install) / 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'))
        sdk_root = Path(env.get('WindowsSdkDir', 'C:/Program Files (x86)/Windows Kits/10'))
        compilers = sorted(sdk_root.glob('bin/*/x64/fxc.exe'), reverse=True)
        if compilers:
            paths.append(str(compilers[0].parent))
        if not env.get('VULKAN_SDK'):
            sdks = sorted(Path('C:/VulkanSDK').glob('*/Bin/glslc.exe'), reverse=True)
            if sdks:
                env['VULKAN_SDK'] = str(sdks[0].parent.parent)
    if env.get('VULKAN_SDK'):
        sdk = Path(env['VULKAN_SDK'])
        paths.extend((str(sdk / 'Bin'), str(sdk / 'bin')))
    if env.get('DY_CI_LLVM'):
        llvm_bins = [Path(env['DY_CI_LLVM']) / 'bin']
    else:
        llvm_bins = sorted((root / 'build-ci-tools').glob('clang*/bin'), reverse=True)
        if os.name == 'nt':
            llvm_bins.append(Path('C:/Program Files/LLVM/bin'))
    for binary_dir in llvm_bins:
        if (binary_dir / ('clang-cl.exe' if os.name == 'nt' else 'clang++')).is_file():
            # Snapshot children must keep the same toolchain and sanitizer DLLs.
            env['DY_CI_LLVM'] = str(binary_dir.parent)
            paths.append(str(binary_dir))
            paths.extend(str(p) for p in (binary_dir.parent / 'lib/clang').glob('*/lib/windows'))
            break
    # Windows environment variables are case-insensitive, but Python dict keys are not.
    inherited_path = ''
    for key in list(env):
        if key.upper() == 'PATH':
            inherited_path = env.pop(key)
    env['PATH'] = os.pathsep.join(paths + [inherited_path])
    return env


def tool(name, env):
    path = shutil.which(name, path=env.get('PATH'))
    if not path:
        raise CiError(f'Required tool is missing: {name}', 'BLOCKED')
    return path


def configure(root, build, api, config, report, env, *, clang=False, sanitize=False, dependencies=None):
    tool('cmake', env)
    if api == 'vulkan':
        tool('glslc', env)
        sdk = env.get('VULKAN_SDK')
        if sdk and not any((Path(sdk) / folder / 'vulkan/vulkan.h').is_file() for folder in ('Include', 'include')):
            raise CiError('VULKAN_SDK does not contain Vulkan headers', 'BLOCKED')
    if api == 'd3d12':
        tool('dxc', env)
    if api == 'metal':
        tool('xcrun', env)
    if sys.platform.startswith('linux'):
        pkg_config = tool('pkg-config', env)
        if subprocess.run([pkg_config, '--exists', 'x11', 'xrandr', 'xinerama', 'xcursor', 'xi'], env=env).returncode:
            raise CiError('X11 development dependencies are missing', 'BLOCKED')
    options = ['cmake', '-S', str(root), '-B', str(build), '-DDY_CI=ON', '-DDY_ENABLE_TRACY=OFF', '-DBUILD_SHARED_LIBS=OFF',
               '-DDY_EXTEND_MODEL=ON',
               '-DUSE_VULKAN=OFF', '-DUSE_D3D12=OFF', '-DUSE_METAL=OFF',
               '-DDY_CI_SANITIZERS=' + ('ON' if sanitize else 'OFF'),
               # Binary dependency directories must not be shared across compilers/configs/sanitizers.
               '-DFETCHCONTENT_BASE_DIR=' + str(build / '_deps')]
    if api != 'null':
        options.append('-DUSE_' + api.upper() + '=ON')
    if os.name == 'nt' and sanitize:
        # clang-cl ASan requires the release CRT even with unoptimized Debug code.
        options.append('-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL')
    if os.name == 'nt' and not clang:
        options.extend(['-G', 'Visual Studio 17 2022', '-A', 'x64'])
    else:
        options.extend(['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=' + config, '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
        tool('ninja', env)
        if clang:
            compiler = tool('clang-cl' if os.name == 'nt' else 'clang++', env)
            options.append('-DCMAKE_CXX_COMPILER=' + compiler)
            if os.name == 'nt':
                options.extend(['-DCMAKE_C_COMPILER=' + compiler, '-DCMAKE_CXX_FLAGS_DEBUG=/Zi /Od'])
            else:
                options.append('-DCMAKE_C_COMPILER=' + tool('clang', env))
                if sys.platform == 'darwin':
                    options.append('-DCMAKE_OBJCXX_COMPILER=' + compiler)
    if sys.platform.startswith('linux'):
        options.append('-DDY_LINUX_WINDOW_SYSTEM=X11')
    if dependencies:
        for name in ('glfw', 'stb', 'fastgltf', 'ufbx'):
            source = Path(dependencies).resolve() / (name + '-src')
            if source.is_dir():
                options.append('-DFETCHCONTENT_SOURCE_DIR_' + name.upper() + '=' + str(source))
    else:
        for name in ('glfw', 'stb', 'fastgltf', 'ufbx'):
            options.append('-DFETCHCONTENT_SOURCE_DIR_' + name.upper() + '=')
    report.command('configure-' + api + '-' + config, options, root, env=env)
    return build


def build_targets(root, build, config, report, env, targets=None):
    command = ['cmake', '--build', str(build), '--config', config, '--parallel', '4']
    if targets is not None:
        if not targets:
            raise CiError('Refusing an empty target list that would build everything', 'BLOCKED')
        command.extend(['--target', *targets])
    report.command('build-' + config, command, root, env=env)


def read_inventory(build, config, verify=True):
    try:
        manifest = json.loads((build / f'ci-manifest-{config}.json').read_text(encoding='utf-8-sig'))
        programs = json.loads((build / f'ci-programs-{config}.json').read_text(encoding='utf-8-sig'))
    except (OSError, ValueError) as error:
        raise CiError(f'Missing or invalid CMake inventory: {error}', 'BLOCKED') from error
    if manifest.get('version') != 1 or not isinstance(manifest.get('targets'), list) or not isinstance(programs, list):
        raise CiError('Unsupported CMake inventory format', 'BLOCKED')
    if verify:
        verify_artifacts(build, manifest, programs)
    return manifest, programs


def verify_artifacts(build, manifest, programs):
    names = set()
    for target in manifest['targets'] + programs:
        name = target['name']
        if not re.fullmatch(r'[A-Za-z0-9_.+-]+', name) or name in names:
            raise CiError('Invalid or duplicate target in inventory', 'BLOCKED')
        names.add(name)
        for raw in [target['binary'], *target.get('shaders', [])]:
            path = Path(raw).resolve()
            if not path.is_relative_to(build.resolve()) or not path.is_file() or not path.stat().st_size:
                raise CiError(f'Missing or out-of-build artifact: {raw}')




def cpu_checks(root, build, config, programs, report, env, seconds, seed):
    runners = [p for p in programs if p['kind'] == 'cpu-check']
    if not runners:
        raise CiError('No CPU check runners were discovered', 'BLOCKED')
    cpu_env = dict(env, ASAN_OPTIONS='halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    for runner in runners:
        for item in runner.get('unsupported', []):
            report.add('cpu-' + runner['name'] + '-' + item['name'], 'UNSUPPORTED', item['reason'])
        binary = Path(runner['binary'])
        listing = subprocess.run([str(binary), '--list-scenarios'], cwd=binary.parent, env=cpu_env,
                                 capture_output=True, text=True, timeout=30)
        scenarios = listing.stdout.splitlines()
        if listing.returncode or not scenarios or any(not re.fullmatch(r'[A-Za-z0-9_]+', s) for s in scenarios):
            raise CiError(f"CPU runner scenario listing failed (exit {listing.returncode}): "
                          + (listing.stderr or repr(listing.stdout))[-1500:], 'BLOCKED')
        for scenario in scenarios:
            cases = report.directory / 'cases' / runner['name'] / scenario
            cases.mkdir(parents=True, exist_ok=True)
            command = [str(binary), '--scenario', scenario, '--seed', str(seed), '--seconds', str(seconds), '--case-dir', str(cases)]
            try:
                report.command('cpu-' + runner['name'] + '-' + scenario, command, binary.parent,
                               timeout=seconds + 120, env=cpu_env)
            except CiError:
                for case in cases.rglob('*.case'):
                    wrapper = dict(version=1, revision=report.revision, api=report.api, config=config,
                                   runner=runner['name'], case=case.name, build=str(build),
                                   binary_sha256=file_digest(binary), source_root=str(root), source_sha256=source_digest(root))
                    saved = case.with_suffix('.json')
                    saved.write_text(json.dumps(wrapper, indent=2), encoding='utf-8')
                    print(f'Replay: {sys.executable} {root / ".github/ci/ci.py"} replay --case "{saved}"')
                raise


def observe_headless_comparison(binary, profile, env, evidence):
    """Run the authored modes to completion and preserve their own GPU readback files."""
    cases = profile.get('cases')
    if not isinstance(cases, list) or len(cases) < 2:
        raise CiError('Headless comparison requires at least two authored cases', 'BLOCKED')
    names = set()
    for case in cases:
        if not isinstance(case, dict):
            raise CiError('Invalid headless comparison case', 'BLOCKED')
        name, arguments = case.get('name'), case.get('arguments')
        if (not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_.-]+', name)
                or name in ('.', '..') or name in names or not isinstance(arguments, list)
                or not all(isinstance(arg, str) for arg in arguments)
                or any(arg == '--capture' or arg.startswith('--capture=') for arg in arguments)):
            raise CiError('Invalid headless comparison case or capture override', 'BLOCKED')
        names.add(name)
    runs = []
    for case in cases:
        folder = evidence / case['name']
        folder.mkdir()
        capture = (folder / 'readback.ppm').resolve()
        command = [str(binary), *case['arguments'], '--capture', str(capture)]
        (folder / 'command.json').write_text(json.dumps(command, indent=2), encoding='utf-8')
        try:
            completed = subprocess.run(command, cwd=binary.parent, env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120,
                                       encoding='utf-8', errors='replace')
            run = dict(name=case['name'], exit_code=completed.returncode, output=completed.stdout,
                       capture=str(capture), forced_termination=False)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or ''
            if isinstance(output, bytes):
                output = output.decode('utf-8', errors='replace')
            run = dict(name=case['name'], exit_code=None, output=output,
                       capture=str(capture), forced_termination=True)
        (folder / 'process.log').write_text(run['output'], encoding='utf-8')
        runs.append(run)
    return dict(status='PASS', exit_code=next((run['exit_code'] for run in runs if run['exit_code'] != 0), 0),
                forced_termination=any(run['forced_termination'] for run in runs), runs=runs,
                output='\n'.join(run['output'] for run in runs))


def runtime_checks(root, build, config, manifest, programs, report, env):
    from checks import check_observation
    from observe import observe

    profiles_path = root / '.github/ci/examples.json'
    profiles = json.loads(profiles_path.read_text(encoding='utf-8'))
    if profiles.get('version') != 1 or not isinstance(profiles.get('examples'), dict):
        raise CiError('Invalid maintainer example profiles', 'BLOCKED')
    for item in manifest['unsupported']:
        report.add(item['directory'], 'UNSUPPORTED', item['reason'])
    failures = []
    jobs = []
    for target in manifest['targets']:
        if not target['runtime']:
            report.add(target['name'], 'UNSUPPORTED', target['reason'])
            continue
        profile = profiles['examples'].get(target['directory'])
        if profile is None:
            report.add(target['name'], 'BLOCKED', 'Official example has no external check profile; add .github/ci/examples.json entry')
            failures.append('BLOCKED')
            continue
        if profile.get('kind') == 'headless-compare':
            jobs.append((target, 'comparison', [], profile))
        else:
            for case in profile.get('cases', [dict(name='default', arguments=[])]):
                jobs.append((target, case['name'], case.get('arguments', []), profile))
    probes = {program['kind']: program for program in programs if program['kind'] in ('rhi-smoke', 'gpu-probe')}
    if manifest['api'] != 'null' and manifest.get('gpu_checks', True):
        for kind in ('rhi-smoke', 'gpu-probe'):
            if kind not in probes:
                raise CiError('Missing independent GPU test executable: ' + kind, 'BLOCKED')
        common = dict(kind='pixels', seconds=2, warmup=1, tolerance=10,
                      markers=['backend=' + manifest['api'], 'resource_lifecycle=ok', 'shutdown=ok'],
                      coverage='independent public API expected pixels; not an official example result')
        report.add('resource-allocation-counters', 'UNSUPPORTED',
                   'The current public RHI has no allocation counters; lifecycle checks do not prove leak balance')
        jobs.append((probes['rhi-smoke'], 'clear', [], dict(common, pixels=[[.1,.1,255,0,0],[.5,.5,255,0,0],[.9,.9,255,0,0]])))
        for case, pixels in (
                ('triangle', [[.5,.5,255,0,0],[.1,.1,0,0,255]]),
                ('depth', [[.5,.5,0,255,0],[.2,.8,255,0,0],[.1,.1,0,0,255]]),
                ('texture', [[.3,.3,255,0,0],[.7,.3,0,255,0],[.3,.7,0,0,255],[.7,.7,255,255,255],[.05,.05,0,0,255]])):
            jobs.append((probes['gpu-probe'], case, ['--case', case], dict(common, pixels=pixels)))
    for target, case, arguments, profile in jobs:
        name = target['name'] + '-' + case
        try:
            if not re.fullmatch(r'[A-Za-z0-9_.-]+', name) or not isinstance(arguments, list) or not all(isinstance(arg, str) for arg in arguments):
                raise CiError('Invalid versioned external test profile', 'BLOCKED')
            binary = Path(target['binary'])
            evidence = report.directory / 'observations' / (name + '-' + uuid.uuid4().hex)
            evidence.mkdir(parents=True)
            if profile.get('kind') == 'headless-compare':
                observed = observe_headless_comparison(binary, profile, env, evidence)
            elif profile.get('kind') == 'capability':
                completed = subprocess.run([str(binary), *arguments], cwd=binary.parent, env=env,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120,
                                           encoding='utf-8', errors='replace')
                (evidence / 'process.log').write_text(completed.stdout, encoding='utf-8')
                observed = dict(status='PASS', exit_code=completed.returncode, output=completed.stdout)
            elif target.get('kind') == 'cpu':
                output = report.command('run-' + name, [binary, *arguments], binary.parent, timeout=120, env=env)
                observed = dict(status='PASS', exit_code=0, output=output)
            else:
                observed = observe(binary, arguments, binary.parent, env, evidence,
                                   seconds=float(profile.get('seconds', 3)), timeout=120,
                                   warmup=float(profile.get('warmup', 0)))
            observation_path = evidence / 'observation.json'
            observation_path.write_text(json.dumps(observed, indent=2, default=str), encoding='utf-8')
            result = check_observation(profile, observed)
            result['source'] = 'official-example' if 'directory' in target else 'independent-api-test'
            result['diagnostics_scope'] = 'captured process output only; no claim of hidden driver error coverage'
            check_path = evidence / 'checks.json'
            check_path.write_text(json.dumps(result, indent=2), encoding='utf-8')
            report.add('behavior-' + name, result['status'], result['message'], observed.get('observed_seconds', 0), check_path)
            if result['status'] not in ('PASS', 'UNSUPPORTED'):
                failures.append(result['status'])
        except subprocess.TimeoutExpired:
            report.add('behavior-' + name, 'FAIL', 'Console example exceeded 120 seconds')
            failures.append('FAIL')
        except CiError as error:
            report.add('behavior-' + name, error.status, str(error))
            failures.append(error.status)
        except (OSError, ValueError, KeyError) as error:
            report.add('behavior-' + name, 'BLOCKED', str(error))
            failures.append('BLOCKED')
    if failures:
        raise CiError('External example/API checks did not all pass', 'FAIL' if 'FAIL' in failures else 'BLOCKED')


def build_name(phase, api, config):
    return f'{phase}-external-{api}-{config}'


def read_impact(path, revision):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if data.get('version') != 1 or data.get('revision') != revision or 'paths' not in data:
        raise CiError('Selection evidence does not match this revision', 'BLOCKED')
    return selection.plan(data['paths'])


def run_phase(args):
    root = args.root.resolve()
    revision = args.revision or git(root, 'rev-parse', 'HEAD')
    build = args.build_dir.resolve() if args.build_dir else root / 'build-ci' / build_name(args.phase, args.api, args.config)
    marker = root / '.dy-ci-revision'
    mode = 'revision-snapshot' if marker.is_file() and marker.read_text(encoding='utf-8') == revision else 'working-copy'
    report = Report(build / 'ci-logs', revision, args.api, mode)
    before = product_fingerprint(root)
    code = 0
    try:
        if not before:
            raise CiError('No framework source/examples found', 'BLOCKED')
        impact = read_impact(args.changes_file, revision) if args.changes_file else None
        if impact and (args.api not in impact['apis'] or (args.phase == 'cpu' and not impact['cpu'])):
            raise CiError(impact['reason'], 'SKIPPED')
        env = setup_environment(root)
        if args.api not in native_apis() + ['null']:
            raise CiError(f'{args.api} is unavailable on {sys.platform}', 'BLOCKED')
        configure(root, build, args.api, args.config, report, env,
                  clang=args.phase == 'cpu' and sys.platform != 'win32',
                  sanitize=args.phase == 'cpu', dependencies=args.dependencies)
        if impact:
            manifest, programs = read_inventory(build, args.config, verify=False)
            manifest, programs = selection.select(root, manifest, programs, impact, args.phase)
            names = [t['name'] for t in manifest['targets'] + programs]
            for name in names:
                if not re.fullmatch(r'[A-Za-z0-9_.+-]+', name):
                    raise CiError('Invalid selected target name', 'BLOCKED')
            selection_path = report.directory / 'selection.json'
            selection_path.write_text(json.dumps(dict(impact, targets=names,
                                                      mode=manifest['selection_mode'],
                                                      reason=manifest['selection_reason']), indent=2), encoding='utf-8')
            report.add('selection', 'PASS' if names else 'SKIPPED',
                       manifest['selection_reason'] + '; targets=' + ', '.join(names), log=selection_path)
            if not names:
                if manifest['selection_mode'] == 'all':
                    raise CiError('No check targets discovered for a required full check', 'BLOCKED')
                for item in manifest.get('unsupported', []):
                    report.add(item['directory'], 'UNSUPPORTED', item['reason'])
                raise CiError('No affected targets for this API/phase', 'SKIPPED')
            build_targets(root, build, args.config, report, env, targets=names)
            verify_artifacts(build, manifest, programs)
        else:
            build_targets(root, build, args.config, report, env)
            manifest, programs = read_inventory(build, args.config)
        report.compiler = manifest.get('compiler')
        report.add('artifact-inventory', 'PASS', f'{len(manifest["targets"])} examples, {len(programs)} check programs')
        if args.phase == 'cpu':
            if programs or not impact:
                cpu_checks(root, build, args.config, programs, report, env, args.fuzz_seconds, args.seed)
            for target in manifest['targets']:
                if target['kind'] == 'cpu':
                    report.command('cpu-example-' + target['name'], [target['binary']], Path(target['binary']).parent, timeout=120, env=env)
        elif args.phase == 'runtime':
            runtime_checks(root, build, args.config, manifest, programs, report, env)
    except CiError as error:
        report.add(args.phase, error.status, str(error))
        code = 0 if error.status == 'SKIPPED' else 2 if error.status == 'BLOCKED' else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        report.add(args.phase, 'BLOCKED', str(error))
        code = 2
    finally:
        changed = before != product_fingerprint(root)
        report.add('product-source-integrity', 'BLOCKED' if changed else 'PASS',
                   'Product source changed while checks ran' if changed else f'{len(before)} source/shader files unchanged')
        if changed and code == 0:
            code = 2
        report.save()
    return code


def full(args):
    root = args.root.resolve()
    revision = resolve_commit(root, args.revision)
    paths = changed_paths(root, revision, args.base)
    cache = root / 'build-ci/push'
    cache.mkdir(parents=True, exist_ok=True)
    lock = cache / 'active.lock'
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise CiError('Another push check owns build-ci/push/active.lock; wait or inspect a stale lock', 'BLOCKED') from error
    try:
        source = materialize_revision(root, revision, cache)
        script = source / '.github/ci/ci.py'
        if not script.is_file():
            raise CiError('The pushed commit does not contain the CI runner; no working-copy fallback is allowed', 'BLOCKED')
        raw_changes, selected = cache / 'changes.json', cache / 'selection.json'
        raw_changes.write_text(json.dumps(dict(version=1, revision=revision, paths=paths)), encoding='utf-8')
        # The pushed runner owns the selection rules, never an uncommitted working-copy rule.
        planned = subprocess.run([sys.executable, '-B', str(script), 'changes', '--revision', revision,
                                  '--changes-file', str(raw_changes), '--output', str(selected)])
        if planned.returncode:
            raise CiError('The pushed runner could not select checks', 'BLOCKED')
        impact = json.loads(selected.read_text(encoding='utf-8'))
        if impact['mode'] == 'none':
            print('[SKIPPED] ' + impact['reason'])
            return 0
        env = setup_environment(root)
        apis = [api for api in (native_apis() if args.api == 'auto' else [args.api]) if api in impact['apis']]
        phases = [('cpu', 'null', 'Debug')] if impact['cpu'] else []
        if not phases and not apis:
            print('[SKIPPED] Changed backend is unavailable on this host; remote CI must verify it')
        for api in apis:
            phases.extend((('build', api, 'Debug'), ('build', api, 'Release'), ('runtime', api, 'Debug')))
        for phase, api, config in phases:
            command = [sys.executable, '-B', str(script), 'run', '--phase', phase, '--api', api,
                       '--config', config, '--root', str(source), '--revision', revision,
                       '--build-dir', str(cache / 'builds' / build_name('build' if phase == 'runtime' else phase, api, config)),
                       '--changes-file', str(selected),
                       '--fuzz-seconds', str(args.fuzz_seconds), '--seed', str(args.seed)]
            # Never inject the user's build/_deps into a pushed revision: CMake must honor that tree's GIT_TAGs.
            result = subprocess.run(command, env=env)
            if result.returncode:
                return result.returncode
        return 0
    finally:
        lock.rmdir()


def pre_push(args):
    normalized = {}
    for local, remote in push_updates(sys.stdin.read()):
        revision = resolve_commit(args.root, local)
        if revision in normalized and normalized[revision] != remote:
            normalized[revision] = '0' * len(revision)
        else:
            normalized[revision] = remote
    for revision, remote in normalized.items():
        args.revision, args.base = revision, remote
        code = full(args)
        if code:
            print('[FAIL] Push stopped. The remote has not received this update.', file=sys.stderr)
            return code
    return 0


def changes(args):
    revision = args.revision if args.changes_file else resolve_commit(args.root, args.revision)
    impact = (read_impact(args.changes_file, revision) if args.changes_file
              else selection.plan(changed_paths(args.root, revision, args.base)))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(dict(impact, revision=revision), indent=2), encoding='utf-8')
    outputs = dict(selection.github_outputs(impact), selection=dict(impact, revision=revision))
    for name, value in outputs.items():
        line = name + '=' + json.dumps(value, separators=(',', ':'))
        print(line)
        if os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
                output.write(line + '\n')
    print('Selection: ' + impact['reason'])
    return 0


def replay(args):
    saved = args.case.resolve()
    data = json.loads(saved.read_text(encoding='utf-8'))
    if data.get('version') != 1 or Path(data['case']).name != data['case']:
        raise CiError('Invalid replay description', 'BLOCKED')
    build = Path(data['build']).resolve()
    repository = Path(git(args.root, 'rev-parse', '--show-toplevel')).resolve()
    source = Path(data['source_root']).resolve()
    if not build.is_relative_to(repository) or not source.is_relative_to(repository):
        raise CiError('Replay build/source are outside this repository; rebuild the recorded revision here', 'BLOCKED')
    _, programs = read_inventory(build, data['config'])
    runners = [p for p in programs if p['name'] == data['runner'] and p['kind'] == 'cpu-check']
    if len(runners) != 1:
        raise CiError('Replay binary missing; rebuild the recorded revision first', 'BLOCKED')
    if file_digest(runners[0]['binary']) != data['binary_sha256'] or source_digest(source) != data['source_sha256']:
        raise CiError('Replay binary or source assets changed; rebuild the recorded revision before replaying', 'BLOCKED')
    case = saved.parent / data['case']
    env = setup_environment(args.root.resolve())
    return subprocess.run([runners[0]['binary'], '--replay', str(case)], cwd=case.parent, env=env).returncode


def install_hook(args):
    root = args.root.resolve()
    configured = git(root, 'config', '--get', 'core.hooksPath') if subprocess.run(
        ['git', '-C', str(root), 'config', '--get', 'core.hooksPath'], capture_output=True).returncode == 0 else None
    if configured:
        raise CiError('Custom core.hooksPath is configured; use its pre-push hook to call ci.py pre-push. It was not changed.', 'BLOCKED')
    directory = Path(git(root, 'rev-parse', '--path-format=absolute', '--git-path', 'hooks'))
    directory.mkdir(parents=True, exist_ok=True)
    path, previous = directory / 'pre-push', directory / 'pre-push.before-dy-ci'
    marker = '# dy-engine push-only CI'
    if path.is_file() and marker in path.read_text(encoding='utf-8', errors='replace'):
        print('[PASS] pre-push hook already installed; no pre-commit hook was installed')
        return 0
    if previous.exists():
        raise CiError('An earlier hook backup exists; refusing to replace it', 'BLOCKED')
    if path.exists():
        had_previous = True
    else:
        had_previous = False
    # Quote for POSIX shell used by Git for Windows too; root comes from Git at execution time.
    python = str(Path(sys.executable)).replace('\\', '/').replace("'", "'\"'\"'")
    content = '#!/bin/sh\n' + marker + '\n' + 'set -eu\n'
    content += 'root=$(git rev-parse --show-toplevel)\n'
    content += 'input=$(mktemp)\ntrap \'rm -f "$input"\' EXIT HUP INT TERM\ncat >"$input"\n'
    if had_previous:
        content += 'previous=$(git rev-parse --git-path hooks/pre-push.before-dy-ci)\n"$previous" "$@" <"$input"\n'
    content += f"exec_python='{python}'\n"
    content += '"$exec_python" -B "$root/.github/ci/ci.py" pre-push --root "$root" <"$input"\n'
    pending = directory / ('pre-push.dy-ci-' + uuid.uuid4().hex)
    with pending.open('x', encoding='utf-8', newline='\n') as output:
        output.write(content)
    pending.chmod(0o755)
    try:
        if had_previous:
            path.rename(previous)
        pending.replace(path)
    except OSError:
        if had_previous and previous.exists() and not path.exists():
            previous.rename(path)
        raise
    finally:
        if pending.exists():
            pending.unlink()
    print('[PASS] Installed pre-push only. Existing hook preserved; no pre-commit hook installed.')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('full', 'pre-push', 'run'):
        command = commands.add_parser(name)
        command.add_argument('--root', type=Path, default=ROOT)
        command.add_argument('--api', choices=(*APIS, 'auto') if name != 'run' else APIS, default='auto' if name != 'run' else 'null')
        command.add_argument('--revision', required=name == 'full')
        command.add_argument('--base')
        command.add_argument('--fuzz-seconds', type=float, default=3)
        command.add_argument('--seed', type=int, default=1729)
        if name == 'run':
            command.add_argument('--changes-file', type=Path, help='Revision-bound push selection evidence, generated automatically')
            command.add_argument('--dependencies', type=Path, help='Explicit working-copy verification only; never used by pre-push')
            command.add_argument('--phase', choices=('build', 'cpu', 'runtime'), required=True)
            command.add_argument('--config', choices=('Debug', 'Release'), default='Debug')
            command.add_argument('--build-dir', type=Path)
    command = commands.add_parser('replay')
    command.add_argument('--root', type=Path, default=ROOT)
    command.add_argument('--case', type=Path, required=True)
    command = commands.add_parser('install-hook')
    command.add_argument('--root', type=Path, default=ROOT)
    command = commands.add_parser('changes')
    command.add_argument('--root', type=Path, default=ROOT)
    command.add_argument('--revision', required=True)
    command.add_argument('--base')
    command.add_argument('--changes-file', type=Path)
    command.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    if hasattr(args, 'fuzz_seconds') and not (0 <= args.fuzz_seconds <= 60):
        parser.error('--fuzz-seconds must be finite and between 0 and 60')
    if hasattr(args, 'seed') and not (0 <= args.seed <= 0xffffffff):
        parser.error('--seed must fit an unsigned 32-bit integer')
    try:
        return {'full': full, 'pre-push': pre_push, 'run': run_phase, 'replay': replay, 'install-hook': install_hook, 'changes': changes}[args.command](args)
    except CiError as error:
        print(f'[{error.status}] {error}', file=sys.stderr)
        return 2 if error.status == 'BLOCKED' else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f'[BLOCKED] {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
