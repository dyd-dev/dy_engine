"""DY_CI_OBSERVER_LIVE=1 enables actual Windows or Xvfb GUI checks."""
import ctypes
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, call, patch

import observe
from checks import check_observation


class WindowsActivationChecks(unittest.TestCase):
    def setUp(self):
        self.desktop = observe.Windows.__new__(observe.Windows)
        self.desktop.u, self.desktop.k = Mock(), Mock()
        self.desktop.pid = Mock(return_value=100)
        self.desktop.u.GetForegroundWindow.return_value = 20
        self.desktop.u.GetWindowThreadProcessId.return_value = 200
        self.desktop.k.GetCurrentThreadId.return_value = 300
        helper = patch.object(observe.subprocess, 'run')
        self.helper = helper.start()
        self.addCleanup(helper.stop)

    def test_wrong_owner_does_not_activate_or_attach(self):
        self.desktop.pid.return_value = 999
        self.desktop.activate(10, 100)
        self.assertEqual(self.desktop.u.mock_calls, [])
        self.assertEqual(self.desktop.k.mock_calls, [])
        self.helper.assert_not_called()

    def test_fast_path_does_not_attach(self):
        self.desktop.u.GetForegroundWindow.return_value = 10
        self.desktop.activate(10, 100)
        self.desktop.u.SetForegroundWindow.assert_called_once_with(10)
        self.desktop.u.AttachThreadInput.assert_not_called()
        self.desktop.u.BringWindowToTop.assert_not_called()
        self.desktop.k.GetCurrentThreadId.assert_not_called()
        self.helper.assert_not_called()

    def test_parent_runs_only_hidden_timeout_bounded_helper(self):
        self.desktop.activate(10, 100)
        self.helper.assert_called_once_with(
            [sys.executable, '-B', str(Path(observe.__file__).resolve()),
             '--activate-owned-window', '10', '100'],
            timeout=2, check=True, capture_output=True, text=True,
            creationflags=getattr(observe.subprocess, 'CREATE_NO_WINDOW', 0))
        self.desktop.u.AttachThreadInput.assert_not_called()
        self.desktop.u.BringWindowToTop.assert_not_called()

    def test_helper_timeout_is_unavailable(self):
        self.helper.side_effect = observe.subprocess.TimeoutExpired('helper', 2)
        with self.assertRaisesRegex(observe.Unavailable, 'timed out after 2 seconds'):
            self.desktop.activate(10, 100)
        self.desktop.u.AttachThreadInput.assert_not_called()

    def test_helper_failure_is_unavailable(self):
        self.helper.side_effect = observe.subprocess.CalledProcessError(2, 'helper', stderr='ownership changed')
        with self.assertRaisesRegex(observe.Unavailable, 'ownership changed'):
            self.desktop.activate(10, 100)

    def test_helper_rechecks_owner_before_attaching(self):
        self.desktop.pid.return_value = 999
        self.assertFalse(self.desktop._activate_attached(10, 100))
        self.assertEqual(self.desktop.u.mock_calls, [])
        self.assertEqual(self.desktop.k.mock_calls, [])

    def test_retry_raises_only_owned_window_and_detaches(self):
        self.desktop.u.SetForegroundWindow.return_value = 0
        self.desktop.u.AttachThreadInput.return_value = 1
        self.desktop._activate_attached(10, 100)
        self.desktop.u.GetWindowThreadProcessId.assert_called_once_with(20, None)
        self.desktop.u.BringWindowToTop.assert_called_once_with(10)
        self.desktop.u.SetForegroundWindow.assert_called_once_with(10)
        self.assertEqual(self.desktop.u.AttachThreadInput.call_args_list,
                         [call(300, 200, True), call(300, 200, False)])

    def test_retry_exception_still_detaches(self):
        self.desktop.u.AttachThreadInput.return_value = 1
        self.desktop.u.BringWindowToTop.side_effect = RuntimeError('activation failed')
        with self.assertRaisesRegex(RuntimeError, 'activation failed'):
            self.desktop._activate_attached(10, 100)
        self.assertEqual(self.desktop.u.AttachThreadInput.call_args_list,
                         [call(300, 200, True), call(300, 200, False)])

    def test_failed_attach_is_not_detached(self):
        self.desktop.u.AttachThreadInput.return_value = 0
        self.desktop._activate_attached(10, 100)
        self.desktop.u.AttachThreadInput.assert_called_once_with(300, 200, True)

    def test_changed_owner_is_not_activated_again(self):
        self.desktop.pid.side_effect = [100, 999, 999]
        self.desktop.u.AttachThreadInput.return_value = 1
        self.desktop._activate_attached(10, 100)
        self.desktop.u.SetForegroundWindow.assert_not_called()
        self.desktop.u.BringWindowToTop.assert_not_called()
        self.assertEqual(self.desktop.u.AttachThreadInput.call_args_list,
                         [call(300, 200, True), call(300, 200, False)])


class ObserverChecks(unittest.TestCase):
    def activation_failure(self, exit_code, output='', grace_exit=None):
        process = Mock(pid=100, returncode=None)
        process.poll.return_value = None
        def wait_for_exit(timeout):
            if grace_exit is None and process.wait.call_count == 1:
                raise observe.subprocess.TimeoutExpired('fixture', timeout)
            process.returncode = grace_exit if grace_exit is not None else 0
            process.poll.return_value = process.returncode
            return process.returncode
        process.wait.side_effect = wait_for_exit
        desktop = Mock()
        desktop.find.return_value = 10
        def fail_activation(hwnd, pid):
            process.returncode = exit_code
            process.poll.return_value = exit_code
            raise observe.Unavailable('Owned window disappeared during activation')
        desktop.activate.side_effect = fail_activation
        with patch.object(observe, 'desktop_for', return_value=desktop), \
             patch.object(observe.subprocess, 'Popen', return_value=process), \
             patch.object(Path, 'mkdir'), \
             patch.object(Path, 'open', return_value=io.BytesIO(output.encode())):
            result = observe.observe(Path('unused'), [], Path.cwd(), {}, Path('unused'))
        process.kill.assert_not_called()
        return result, process, desktop

    def test_activation_race_preserves_exit_77_for_declared_feature_check(self):
        marker = 'Unsupported: this RHI backend has no Compute dispatch/storage-buffer implementation.'
        result, process, desktop = self.activation_failure(77, marker)
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['exit_code'], 77)
        self.assertTrue(result['natural_exit'])
        self.assertFalse(result['forced_termination'])
        desktop.close.assert_not_called()
        process.wait.assert_not_called()
        profile = dict(kind='visible', seconds=3, unsupported_markers=[marker])
        self.assertEqual(check_observation(profile, result)['status'], 'UNSUPPORTED')
        self.assertEqual(check_observation(dict(kind='visible', seconds=3), result)['status'], 'FAIL')

    def test_activation_race_exit_zero_does_not_pass_graphical_checks(self):
        result, _, desktop = self.activation_failure(0)
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['exit_code'], 0)
        self.assertTrue(result['natural_exit'])
        self.assertEqual(result['captures'], [])
        self.assertEqual(check_observation(dict(kind='visible', seconds=3), result)['status'], 'BLOCKED')
        desktop.close.assert_not_called()

    def test_activation_waits_for_natural_exit_after_window_disappears(self):
        result, process, desktop = self.activation_failure(None, grace_exit=77)
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['exit_code'], 77)
        self.assertTrue(result['natural_exit'])
        self.assertFalse(result['forced_termination'])
        process.wait.assert_called_once_with(timeout=2)
        desktop.close.assert_not_called()

    def test_live_activation_failure_stays_blocked_after_cleanup(self):
        result, process, desktop = self.activation_failure(None)
        self.assertEqual(result['status'], 'BLOCKED')
        self.assertIn('disappeared during activation', result['message'])
        self.assertFalse(result['natural_exit'])
        self.assertEqual(result['exit_code'], 0)  # Graceful cleanup is not a successful observation.
        desktop.close.assert_called_once_with(10, 100)
        self.assertEqual(process.wait.call_args_list, [call(timeout=2), call(timeout=2)])

    def test_unavailable_display_is_blocked_without_launch(self):
        with patch.object(observe, 'desktop_for', side_effect=observe.Unavailable('No interactive display')):
            with patch.object(observe.subprocess, 'Popen') as launch:
                result = observe.observe(Path('unused'), [], Path.cwd(), {}, Path('unused'))
                self.assertEqual(result['status'], 'BLOCKED')
                launch.assert_not_called()

    @unittest.skipUnless(os.environ.get('DY_CI_OBSERVER_LIVE') == '1',
                         'Requires explicitly enabled Windows desktop or Xvfb display')
    def test_real_window_rgb_close_crash_timeout_and_finite_exit(self):
        scratch_root = Path(__file__).resolve().parents[2] / 'build-ci' / 'observer'
        scratch_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='dy-observer-', dir=scratch_root) as scratch:
            directory = Path(scratch)
            def run(mode, timeout=8):
                return observe.observe(Path(sys.executable), [str(Path(__file__).resolve()), '--fixture', mode],
                                       Path.cwd(), dict(os.environ), directory / mode, seconds=0.8, timeout=timeout, warmup=0.2)
            normal = run('normal')
            self.assertEqual(normal['status'], 'PASS', normal)
            self.assertFalse(normal['forced_termination'])
            self.assertFalse(normal['natural_exit'])
            self.assertTrue(normal['responsive'])
            self.assertGreaterEqual(len(normal['captures']), 2)
            self.assertGreaterEqual(normal['capture_times'][0], 0.6)
            for capture in normal['captures']:
                data = capture.read_bytes().split(b'\n', 3)
                self.assertEqual(data[:3], [b'P6', b'160 120', b'255'])
                self.assertEqual(data[3], bytes((192, 64, 128)) * (160 * 120))
            crashed = run('crash')
            self.assertEqual(crashed['status'], 'FAIL', crashed)
            self.assertEqual(crashed['exit_code'], 7)
            timed_out = run('ignore-close', timeout=2)
            self.assertEqual(timed_out['status'], 'FAIL', timed_out)
            self.assertTrue(timed_out['forced_termination'])
            finite = run('finite')
            self.assertEqual(finite['status'], 'PASS', finite)
            self.assertTrue(finite['natural_exit'])
            self.assertFalse(finite['window_observed'])
            self.assertIn('finite output', finite['output'])


def fixture(mode):
    if mode == 'crash':
        os._exit(7)
    if mode == 'finite':
        print('finite output')
        return
    if sys.platform.startswith('linux'):
        return x11_fixture(mode)
    import tkinter
    window = tkinter.Tk()
    window.title('dy_engine external observer RGB fixture')
    window.geometry('160x120+80+80')
    window.configure(background='#c04080')
    if mode == 'ignore-close':
        window.protocol('WM_DELETE_WINDOW', lambda: None)
    window.mainloop()


def x11_fixture(mode):
    desktop = observe.X11(dict(os.environ))
    x, display, root = desktop.x, desktop.display, desktop.root
    ptr, ulong, integer = ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int
    for name, args, result in (
        ('XCreateSimpleWindow', [ptr, ulong, integer, integer, ctypes.c_uint, ctypes.c_uint,
                                 ctypes.c_uint, ulong, ulong], ulong),
        ('XChangeProperty', [ptr, ulong, ulong, ulong, integer, integer, ptr, integer], integer),
        ('XSetWMProtocols', [ptr, ulong, ctypes.POINTER(ulong), integer], integer),
        ('XMapWindow', [ptr, ulong], integer),
        ('XNextEvent', [ptr, ctypes.POINTER(observe.XEvent)], integer),
    ):
        fn = getattr(x, name)
        fn.argtypes, fn.restype = args, result
    window = x.XCreateSimpleWindow(display, root, 80, 80, 160, 120, 0, 0, 0xc04080)
    pid = ulong(os.getpid())
    x.XChangeProperty(display, window, desktop.atoms['_NET_WM_PID'], 6, 32, 0, ctypes.byref(pid), 1)
    protocols = (ulong * 2)(desktop.atoms['_NET_WM_PING'], desktop.atoms['WM_DELETE_WINDOW'])
    x.XSetWMProtocols(display, window, protocols, 2)
    x.XMapWindow(display, window)
    x.XFlush(display)
    event = observe.XEvent()
    while True:
        x.XNextEvent(display, ctypes.byref(event))
        if event.client.type != 33:
            continue
        if event.client.data[0] == desktop.atoms['_NET_WM_PING'] and event.client.window == window:
            event.client.window = root
            x.XSendEvent(display, root, False, 1 << 19, ctypes.byref(event))
            x.XFlush(display)
        elif event.client.data[0] == desktop.atoms['WM_DELETE_WINDOW'] and mode != 'ignore-close':
            break
    desktop.finish()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--fixture':
        fixture(sys.argv[2])
    else:
        unittest.main()
