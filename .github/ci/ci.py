"""Build the common examples application and check process startup on each platform."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]


class CiError(RuntimeError):
    def __init__(self, message, status='FAIL'):
        super().__init__(message)
        self.status = status


def tool(name, env):
    path = shutil.which(name, path=env.get('PATH'))
    if not path:
        raise CiError('Required tool is missing: ' + name)
    return path


def terminate(process):
    if process.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    process.wait(timeout=10)


class Report:
    def __init__(self, directory):
        self.directory = directory
        directory.mkdir(parents=True, exist_ok=True)

    def add(self, name, status, message):
        print(f'[{status}] {name}: {message}', flush=True)

    def command(self, name, arguments, cwd, timeout=3600, env=None):
        log = self.directory / (name + '.log')
        with log.open('w', encoding='utf-8') as output:
            output.write(json.dumps([str(value) for value in arguments]) + '\n')
            output.flush()
            process = subprocess.Popen([str(value) for value in arguments], cwd=cwd, env=env,
                                       stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=os.name != 'nt')
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                terminate(process)
                raise CiError(f'{name} timed out; see {log}')
        text = log.read_text(encoding='utf-8', errors='replace')
        if code:
            print('\n'.join(text.splitlines()[-30:]), flush=True)
            if env and env.get('GITHUB_ACTIONS') == 'true':
                lines = text.splitlines()
                errors = [index for index, line in enumerate(lines) if re.search(
                    r'\b(?:fatal\s+)?error(?:\s+[A-Z]+\d+)?\s*:|CMake Error|^FAILED:',
                    line, re.IGNORECASE)]
                context = sorted({index for error in errors
                                  for index in range(max(0, error - 3), min(len(lines), error + 3))})
                details = [lines[index] for index in context] if context else lines[-30:]
                details = '\n'.join(line if len(line) <= 700 else line[:250] + ' ... ' + line[-400:]
                                    for line in details[:20])
                details = details.replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')
                print(f'::error title={name} failed::{details}', flush=True)
            raise CiError(f'{name} exited {code}; see {log}')
        return text

    def launch(self, name, arguments, cwd, env):
        log = self.directory / (name + '.log')
        with log.open('w', encoding='utf-8') as output:
            process = subprocess.Popen([str(value) for value in arguments], cwd=cwd, env=env,
                                       stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=os.name != 'nt')
            try:
                try:
                    code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    code = None
                text = log.read_text(encoding='utf-8', errors='replace')
                returned = re.search(r'Application exited with code (-?\d+)', text)
                if code not in (None, 0) or (returned and int(returned[1]) != 0):
                    print(text, flush=True)
                    raise CiError(f'{name} failed during startup; see {log}')
                message = 'Exited with code 0' if code == 0 else 'Running after 5 seconds; CI will stop it'
                self.add(name, 'PASS', message)
            finally:
                terminate(process)


def desktop_build(root, build, platform_name, api, config, report, env):
    options = ['cmake', '-S', str(root), '-B', str(build),
               '-DDY_BUILD_EXAMPLES=ON', '-DDY_EXTEND_MODEL=ON',
               '-DDY_EXTEND_ADVANCED_RENDERING=ON', '-DDY_EXTEND_IMGUI=ON']
    if platform_name == 'windows':
        options += ['-G', 'Visual Studio 17 2022', '-A', 'x64',
                    '-DUSE_D3D12=' + ('ON' if api == 'd3d12' else 'OFF'),
                    '-DUSE_VULKAN=' + ('ON' if api == 'vulkan' else 'OFF')]
    else:
        options += ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=' + config]
        if platform_name == 'linux':
            options += ['-DDY_LINUX_WINDOW_SYSTEM=X11']
    report.command('configure', options, root, env=env)
    report.command('build', ['cmake', '--build', str(build), '--config', config,
                            '--parallel', '4'], root, env=env)
    report.add('build', 'PASS', platform_name + ' ' + api + ' ' + config)


def desktop_runtime(build, platform_name, config, report, env):
    binary = build / config / 'visualizer.exe' if platform_name == 'windows' else build / 'visualizer'
    if not binary.is_file():
        raise CiError('Build the examples application first: ' + str(binary))
    report.launch('examples-startup', [binary], binary.parent, env)


def mobile_build(root, build, platform_name, config, report, env):
    output = build / 'output'
    output.mkdir(parents=True, exist_ok=True)
    if platform_name == 'android':
        settings = (root / 'android/app/build.gradle').read_text(encoding='utf-8')
        def setting(name):
            match = re.search(r'\b' + name + r'\s+[\'"]?([A-Za-z0-9_.]+)', settings)
            if not match:
                raise CiError(f'Android {name} is missing from app/build.gradle', 'BLOCKED')
            return match[1]
        cmake_version = re.search(r"\bversion\s+'([0-9.]+)'", settings)
        if not cmake_version:
            raise CiError('Android CMake version is not declared', 'BLOCKED')
        report.command('android-sdk', [tool('sdkmanager', env), 'platform-tools',
                       'platforms;android-' + setting('compileSdk'),
                       'build-tools;' + setting('buildToolsVersion'), 'ndk;' + setting('ndkVersion'),
                       'cmake;' + cmake_version[1]], root, env=env)
        wrapper = ([str(root / 'android/gradlew.bat')] if os.name == 'nt'
                   else ['bash', str(root / 'android/gradlew')])
        report.command('android-' + config, [*wrapper, '--no-daemon', '--stacktrace',
                       ':app:assemble' + config], root / 'android', timeout=4800, env=env)
        packages = list((root / 'android/app/build/outputs/apk' / config.lower()).glob('*.apk'))
        if len(packages) != 1:
            raise CiError('Expected exactly one APK containing all configured Android ABIs')
        shutil.copy2(packages[0], output / 'application.apk')
    else:
        if sys.platform != 'darwin':
            raise CiError('iOS builds require Xcode on macOS', 'BLOCKED')
        project = root / 'ios/AdvancedMetalIOS/AdvancedMetalIOS.xcodeproj'
        settings = (project / 'project.pbxproj').read_text(encoding='utf-8')
        deployment = re.search(r'IPHONEOS_DEPLOYMENT_TARGET\s*=\s*([^;]+);', settings)
        if not deployment:
            raise CiError('The Xcode project does not declare its iOS deployment target', 'BLOCKED')
        for sdk in ('iphonesimulator', 'iphoneos'):
            sdk_root = subprocess.run(['xcrun', '--sdk', sdk, '--show-sdk-path'], env=env,
                                      capture_output=True, text=True, check=True, timeout=30).stdout.strip()
            report.command('ios-configure-' + sdk, ['cmake', '-S', str(root / 'ios'),
                           '-B', str(root / 'build' / f'ios-{sdk}-arm64-{config}'), '-G', 'Ninja',
                           '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_SYSROOT=' + sdk_root,
                           '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_BUILD_TYPE=' + config,
                           '-DCMAKE_OSX_DEPLOYMENT_TARGET=' + deployment[1].strip()], root, env=env)
        derived = build / 'DerivedData'
        report.command('ios-' + config, [tool('xcodebuild', env), '-project', str(project),
                       '-scheme', 'Examples', '-configuration', config, '-sdk', 'iphonesimulator',
                       '-destination', 'generic/platform=iOS Simulator', '-derivedDataPath', str(derived),
                       'ARCHS=arm64', 'ONLY_ACTIVE_ARCH=YES', 'CODE_SIGNING_ALLOWED=NO', 'build'],
                       root, timeout=4800, env=env)
        apps = list((derived / 'Build/Products' / (config + '-iphonesimulator')).glob('*.app'))
        if len(apps) != 1:
            raise CiError('Expected exactly one simulator application from Xcode')
        report.command('ios-package', ['ditto', '-c', '-k', '--keepParent', str(apps[0]),
                       str(output / 'application.zip')], root, env=env)
        report.command('ios-device-' + config, ['xcodebuild', '-project', str(project),
                       '-scheme', 'Examples', '-configuration', config, '-sdk', 'iphoneos',
                       '-destination', 'generic/platform=iOS', '-derivedDataPath', str(build / 'DeviceData'),
                       'ARCHS=arm64', 'ONLY_ACTIVE_ARCH=NO', 'CODE_SIGNING_ALLOWED=NO', 'build'],
                       root, timeout=4800, env=env)
    report.add('mobile-package', 'PASS', f'{platform_name} {config}: {output}')



def mobile_artifact(path, filename):
    if path is None:
        raise CiError('Mobile runtime requires --artifact from its build job')
    path = path.resolve()
    path = path / filename if path.is_dir() else path
    if not path.is_file():
        raise CiError('Missing application package: ' + str(path))
    return path


def android_runtime(root, build, artifact, report, env):
    adb = tool('adb', env)
    device = []

    def command(*arguments, required=True):
        result = subprocess.run([adb, *device, *arguments], env=env, capture_output=True,
                                text=True, timeout=30)
        if required and result.returncode:
            raise CiError('adb: ' + result.stderr.strip())
        return result.stdout

    devices = [line.split()[0] for line in command('devices').splitlines()
               if len(line.split()) == 2 and line.split()[1] == 'device']
    serial = env.get('ANDROID_SERIAL')
    if serial and serial not in devices:
        raise CiError('ANDROID_SERIAL is not an authorized, online device')
    if not serial and len(devices) != 1:
        raise CiError('Connect one Android device, or set ANDROID_SERIAL')
    device = ['-s', serial or devices[0]]
    settings = (root / 'android/app/build.gradle').read_text(encoding='utf-8')
    package = re.search(r'\bapplicationId\s+[\'"]([^\'"]+)', settings)[1]
    report.command('android-install', [adb, *device, 'install', '-r',
                   str(mobile_artifact(artifact, 'application.apk'))], root, env=env)
    pid = None

    def foreground():
        activity = command('shell', 'dumpsys', 'activity', 'activities')
        return any(package + '/' in line and
                   ('topResumedActivity' in line or 'mResumedActivity' in line)
                   for line in activity.splitlines())

    try:
        command('shell', 'am', 'force-stop', package)
        launched = command('shell', 'am', 'start', '-W', '-n', package + '/android.app.NativeActivity')
        if 'Error:' in launched:
            raise CiError(launched)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            pids = command('shell', 'pidof', package, required=False).split()
            if len(pids) == 1 and foreground():
                pid = pids[0]
                break
            time.sleep(.25)
        if not pid:
            raise CiError('Android application did not enter the foreground')
        time.sleep(5)
        if not foreground() or pid not in command('shell', 'pidof', package, required=False).split():
            raise CiError('Android application stopped during startup')
        report.add('android-startup', 'PASS', 'Application remains in the foreground after 5 seconds')
    finally:
        if pid:
            (report.directory / 'android-startup.log').write_text(
                command('logcat', '-d', '--pid=' + pid, '-v', 'threadtime', required=False),
                encoding='utf-8')
        command('shell', 'am', 'force-stop', package, required=False)


def ios_runtime(root, build, artifact, report, env):
    if sys.platform != 'darwin':
        raise CiError('iOS simulator execution requires macOS')
    app_dir = build / ('application-' + uuid.uuid4().hex)
    report.command('ios-unpack', ['ditto', '-x', '-k',
                   str(mobile_artifact(artifact, 'application.zip')), str(app_dir)], root, env=env)
    apps = list(app_dir.glob('*.app'))
    if len(apps) != 1:
        raise CiError('Expected exactly one simulator application')
    with (apps[0] / 'Info.plist').open('rb') as stream:
        package = plistlib.load(stream)['CFBundleIdentifier']
    xcrun = tool('xcrun', env)

    def simctl(*arguments, required=True):
        result = subprocess.run([xcrun, 'simctl', *arguments], env=env, capture_output=True,
                                text=True, timeout=180)
        if required and result.returncode:
            raise CiError('simctl: ' + result.stderr.strip())
        return result.stdout.strip()

    runtimes = [runtime for runtime in json.loads(simctl('list', 'runtimes', '-j'))['runtimes']
                if runtime.get('isAvailable') and '.iOS-' in runtime['identifier']]
    if not runtimes:
        raise CiError('Install an iOS simulator runtime in Xcode')
    runtime = max(runtimes, key=lambda value: tuple(map(int, value['version'].split('.'))))
    version = sum(int(part) << shift for part, shift in
                  zip((runtime['version'].split('.') + ['0', '0'])[:3], (16, 8, 0)))
    types = [kind for kind in json.loads(simctl('list', 'devicetypes', '-j'))['devicetypes']
             if re.fullmatch(r'iPhone [0-9]+', kind['name'])
             and kind.get('minRuntimeVersion', 0) <= version <= kind.get('maxRuntimeVersion', 0xffffffff)]
    if not types:
        raise CiError('No compatible iPhone simulator device type is installed')
    kind = max(types, key=lambda value: int(value['name'].split()[1]))
    udid = simctl('create', 'dy-ci-' + uuid.uuid4().hex[:12], kind['identifier'], runtime['identifier'])
    try:
        simctl('boot', udid)
        simctl('bootstatus', udid, '-b')
        simctl('install', udid, str(apps[0]))
        report.launch('ios-startup', [xcrun, 'simctl', 'launch', '--console', udid, package], root, env)
    finally:
        simctl('terminate', udid, package, required=False)
        simctl('shutdown', udid, required=False)
        simctl('delete', udid, required=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('linux', 'windows', 'macos', 'android', 'ios'), required=True)
    parser.add_argument('--phase', choices=('build', 'runtime'), required=True)
    parser.add_argument('--api', choices=('vulkan', 'd3d12', 'metal'))
    parser.add_argument('--config', choices=('Debug', 'Release'), default='Debug')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--artifact', type=Path)
    args = parser.parse_args()
    api = args.api or ('metal' if args.platform in ('macos', 'ios') else 'vulkan')
    if args.platform != 'windows' and api != ('metal' if args.platform in ('macos', 'ios') else 'vulkan'):
        parser.error('The selected API does not match this platform')
    if args.platform == 'windows' and api == 'metal':
        parser.error('Metal is unavailable on Windows')
    build, env = args.build_dir.resolve(), os.environ.copy()
    report = Report(build / 'ci-logs')
    try:
        if args.phase == 'build':
            if args.platform in ('android', 'ios'):
                mobile_build(ROOT, build, args.platform, args.config, report, env)
            else:
                desktop_build(ROOT, build, args.platform, api, args.config, report, env)
        elif args.platform == 'android':
            android_runtime(ROOT, build, args.artifact, report, env)
        elif args.platform == 'ios':
            ios_runtime(ROOT, build, args.artifact, report, env)
        else:
            desktop_runtime(build, args.platform, args.config, report, env)
    except (CiError, OSError, ValueError, KeyError, IndexError, subprocess.SubprocessError) as error:
        print('[FAIL] ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
