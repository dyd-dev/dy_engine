"""DY_CI_OBSERVER_LIVE=1 enables actual Windows or Xvfb GUI checks."""
import ctypes
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import observe


class ObserverChecks(unittest.TestCase):
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
