"""Observe an unmodified example from outside its process (no engine test hooks)."""
import ctypes
import ctypes.util
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import sys
import time


class Unavailable(RuntimeError):
    pass


class Windows:
    """Capture an owned, foreground, unobscured client region only."""

    def __init__(self):
        if sys.platform != 'win32':
            raise Unavailable(f'No native external window observer for {sys.platform}; '
                              'macOS capture permissions and window ownership are not implemented')
        self.u = ctypes.WinDLL('user32', use_last_error=True)
        self.g = ctypes.WinDLL('gdi32', use_last_error=True)
        self.enum_proc = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        signatures = {
            'EnumWindows': ([self.enum_proc, w.LPARAM], w.BOOL),
            'GetWindowThreadProcessId': ([w.HWND, ctypes.POINTER(w.DWORD)], w.DWORD),
            'IsWindowVisible': ([w.HWND], w.BOOL), 'IsIconic': ([w.HWND], w.BOOL),
            'GetForegroundWindow': ([], w.HWND), 'SetForegroundWindow': ([w.HWND], w.BOOL),
            'GetClientRect': ([w.HWND, ctypes.POINTER(w.RECT)], w.BOOL),
            'GetWindowRect': ([w.HWND, ctypes.POINTER(w.RECT)], w.BOOL),
            'ClientToScreen': ([w.HWND, ctypes.POINTER(w.POINT)], w.BOOL),
            'GetDC': ([w.HWND], w.HDC), 'ReleaseDC': ([w.HWND, w.HDC], ctypes.c_int),
            'SendMessageTimeoutW': ([w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT,
                                    w.UINT, ctypes.POINTER(ctypes.c_size_t)], w.LPARAM),
            'PostMessageW': ([w.HWND, w.UINT, w.WPARAM, w.LPARAM], w.BOOL),
            'OpenInputDesktop': ([w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
            'CloseDesktop': ([w.HANDLE], w.BOOL),
            'GetThreadDesktop': ([w.DWORD], w.HANDLE),
            'GetUserObjectInformationW': ([w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD,
                                          ctypes.POINTER(w.DWORD)], w.BOOL),
            'GetSystemMetrics': ([ctypes.c_int], ctypes.c_int),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.u, name)
            fn.argtypes, fn.restype = args, result
        for name, args, result in (
            ('CreateCompatibleDC', [w.HDC], w.HDC),
            ('CreateCompatibleBitmap', [w.HDC, ctypes.c_int, ctypes.c_int], w.HBITMAP),
            ('SelectObject', [w.HDC, w.HANDLE], w.HANDLE),
            ('DeleteObject', [w.HANDLE], w.BOOL), ('DeleteDC', [w.HDC], w.BOOL),
            ('BitBlt', [w.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                        w.HDC, ctypes.c_int, ctypes.c_int, w.DWORD], w.BOOL),
            ('GetDIBits', [w.HDC, w.HBITMAP, w.UINT, w.UINT, w.LPVOID, w.LPVOID, w.UINT], ctypes.c_int),
        ):
            fn = getattr(self.g, name)
            fn.argtypes, fn.restype = args, result
        # Keep client coordinates and capture pixels in the same physical units.
        self.u.SetProcessDPIAware()
        desktop = self.u.OpenInputDesktop(0, False, 1)
        if not desktop:
            raise Unavailable('No accessible interactive Windows input desktop')
        try:
            def name(handle):
                buffer, needed = ctypes.create_unicode_buffer(256), w.DWORD()
                if not self.u.GetUserObjectInformationW(handle, 2, buffer, ctypes.sizeof(buffer),
                                                        ctypes.byref(needed)):
                    raise Unavailable('Cannot identify the interactive Windows desktop')
                return buffer.value
            thread_id = ctypes.WinDLL('kernel32').GetCurrentThreadId()
            if name(desktop) != name(self.u.GetThreadDesktop(thread_id)):
                raise Unavailable('Observer is not on the current interactive Windows desktop')
        finally:
            self.u.CloseDesktop(desktop)

    def pid(self, hwnd):
        pid = w.DWORD()
        self.u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        return pid.value

    def windows(self):
        found = []
        callback = self.enum_proc(lambda hwnd, _: found.append(hwnd) is None)
        if not self.u.EnumWindows(callback, 0):
            raise Unavailable('Cannot enumerate native Windows windows')
        return found

    def find(self, pid):
        for hwnd in self.windows():
            if self.pid(hwnd) == pid and self.u.IsWindowVisible(hwnd) and not self.u.IsIconic(hwnd):
                rect = w.RECT()
                if self.u.GetClientRect(hwnd, ctypes.byref(rect)) and rect.right > 0 and rect.bottom > 0:
                    return hwnd
        return None

    def activate(self, hwnd, pid):
        if self.pid(hwnd) == pid:
            self.u.SetForegroundWindow(hwnd)

    def finish(self):
        pass

    def responsive(self, hwnd, pid):
        if self.pid(hwnd) != pid:
            return False
        result = ctypes.c_size_t()
        return bool(self.u.SendMessageTimeoutW(hwnd, 0, 0, 0, 0x2 | 0x20, 300,
                                              ctypes.byref(result)))

    def close(self, hwnd, pid):
        return self.pid(hwnd) == pid and bool(self.u.PostMessageW(hwnd, 0x10, 0, 0))

    def region(self, hwnd, pid):
        if self.pid(hwnd) != pid or not self.u.IsWindowVisible(hwnd) or self.u.IsIconic(hwnd):
            raise Unavailable('Owned example window is no longer visible')
        if self.u.GetForegroundWindow() != hwnd:
            raise Unavailable('Owned example window is not foreground; screen capture withheld')
        rect, origin = w.RECT(), w.POINT()
        if not self.u.GetClientRect(hwnd, ctypes.byref(rect)) or not self.u.ClientToScreen(hwnd, ctypes.byref(origin)):
            raise Unavailable('Cannot locate example client region')
        width, height = rect.right, rect.bottom
        x, y = origin.x, origin.y
        vx, vy, vw, vh = (self.u.GetSystemMetrics(i) for i in (76, 77, 78, 79))
        if width <= 0 or height <= 0 or x < vx or y < vy or x + width > vx + vw or y + height > vy + vh:
            raise Unavailable('Example client region is outside the active desktop')
        found = False
        for other in self.windows():
            if other == hwnd:
                found = True
                break
            if self.u.IsWindowVisible(other) and not self.u.IsIconic(other):
                above = w.RECT()
                if not self.u.GetWindowRect(other, ctypes.byref(above)):
                    raise Unavailable('Cannot prove example client region is unobscured')
                if above.left < x + width and above.right > x and above.top < y + height and above.bottom > y:
                    raise Unavailable('Another window overlaps the example; capture withheld')
        if not found:
            raise Unavailable('Owned example window disappeared before capture')
        return x, y, width, height

    def capture(self, hwnd, pid, path):
        region = self.region(hwnd, pid)
        _, _, width, height = region
        source = self.u.GetDC(hwnd)
        dc = self.g.CreateCompatibleDC(source) if source else None
        bitmap = self.g.CreateCompatibleBitmap(source, width, height) if dc else None
        previous = self.g.SelectObject(dc, bitmap) if bitmap else None
        try:
            if not source or not dc or not bitmap or not previous:
                raise Unavailable('Windows client capture resource allocation failed')
            if not self.g.BitBlt(dc, 0, 0, width, height, source, 0, 0, 0x00CC0020):
                raise Unavailable('Windows client BitBlt failed')
            self.g.SelectObject(dc, previous)
            previous = None
            # BITMAPINFOHEADER, top-down 32-bit BGRX; no dependencies or row padding.
            import struct
            info = ctypes.create_string_buffer(struct.pack('<IiiHHIIiiII', 40, width, -height,
                                                          1, 32, 0, 0, 0, 0, 0, 0))
            pixels = ctypes.create_string_buffer(width * height * 4)
            if self.g.GetDIBits(dc, bitmap, 0, height, pixels, info, 0) != height:
                raise Unavailable('Windows client pixels could not be read')
            # Reject focus, geometry, ownership or overlap changes during capture.
            if self.region(hwnd, pid) != region:
                raise Unavailable('Example moved during capture; image discarded')
            raw = pixels.raw
            rgb = bytearray(width * height * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
            path.write_bytes(f'P6\n{width} {height}\n255\n'.encode('ascii') + rgb)
        finally:
            if previous:
                self.g.SelectObject(dc, previous)
            if bitmap:
                self.g.DeleteObject(bitmap)
            if dc:
                self.g.DeleteDC(dc)
            if source:
                self.u.ReleaseDC(hwnd, source)


class XEvent(ctypes.Union):
    class Client(ctypes.Structure):
        _fields_ = [('type', ctypes.c_int), ('serial', ctypes.c_ulong), ('send_event', ctypes.c_int),
                    ('display', ctypes.c_void_p), ('window', ctypes.c_ulong),
                    ('message_type', ctypes.c_ulong), ('format', ctypes.c_int),
                    ('data', ctypes.c_long * 5)]
    _fields_ = [('client', Client), ('pad', ctypes.c_long * 24)]


class XAttributes(ctypes.Structure):
    _fields_ = [(name, kind) for name, kind in (
        ('x', ctypes.c_int), ('y', ctypes.c_int), ('width', ctypes.c_int), ('height', ctypes.c_int),
        ('border_width', ctypes.c_int), ('depth', ctypes.c_int), ('visual', ctypes.c_void_p),
        ('root', ctypes.c_ulong), ('class_', ctypes.c_int), ('bit_gravity', ctypes.c_int),
        ('win_gravity', ctypes.c_int), ('backing_store', ctypes.c_int), ('backing_planes', ctypes.c_ulong),
        ('backing_pixel', ctypes.c_ulong), ('save_under', ctypes.c_int), ('colormap', ctypes.c_ulong),
        ('map_installed', ctypes.c_int), ('map_state', ctypes.c_int), ('all_event_masks', ctypes.c_long),
        ('your_event_mask', ctypes.c_long), ('do_not_propagate_mask', ctypes.c_long),
        ('override_redirect', ctypes.c_int), ('screen', ctypes.c_void_p))]


class XImage(ctypes.Structure):
    _fields_ = [(name, ctypes.c_int) for name in ('width', 'height', 'xoffset', 'format')]
    _fields_ += [('data', ctypes.c_void_p)]
    _fields_ += [(name, ctypes.c_int) for name in ('byte_order', 'bitmap_unit', 'bitmap_bit_order',
                                                 'bitmap_pad', 'depth', 'bytes_per_line', 'bits_per_pixel')]
    _fields_ += [(name, ctypes.c_ulong) for name in ('red_mask', 'green_mask', 'blue_mask')]


class X11:
    """X11/Xvfb observation; window-manager reparenting is explicitly unsupported."""

    def __init__(self, env):
        if not env.get('DISPLAY'):
            raise Unavailable('DISPLAY is unset; an X11 display such as Xvfb is required')
        library = ctypes.util.find_library('X11')
        if not library:
            raise Unavailable('libX11 is unavailable for external window observation')
        self.x = ctypes.CDLL(library)
        ptr, ulong, integer = ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int
        signatures = {
            'XOpenDisplay': ([ctypes.c_char_p], ptr), 'XCloseDisplay': ([ptr], integer),
            'XDefaultRootWindow': ([ptr], ulong), 'XInternAtom': ([ptr, ctypes.c_char_p, integer], ulong),
            'XQueryTree': ([ptr, ulong, ctypes.POINTER(ulong), ctypes.POINTER(ulong),
                            ctypes.POINTER(ctypes.POINTER(ulong)), ctypes.POINTER(ctypes.c_uint)], integer),
            'XGetWindowAttributes': ([ptr, ulong, ctypes.POINTER(XAttributes)], integer),
            'XGetWindowProperty': ([ptr, ulong, ulong, ctypes.c_long, ctypes.c_long, integer, ulong,
                                    ctypes.POINTER(ulong), ctypes.POINTER(integer), ctypes.POINTER(ulong),
                                    ctypes.POINTER(ulong), ctypes.POINTER(ptr)], integer),
            'XGetWMProtocols': ([ptr, ulong, ctypes.POINTER(ctypes.POINTER(ulong)), ctypes.POINTER(integer)], integer),
            'XFree': ([ptr], integer), 'XRaiseWindow': ([ptr, ulong], integer),
            'XSelectInput': ([ptr, ulong, ctypes.c_long], integer),
            'XSendEvent': ([ptr, ulong, integer, ctypes.c_long, ctypes.POINTER(XEvent)], integer),
            'XCheckTypedWindowEvent': ([ptr, ulong, integer, ctypes.POINTER(XEvent)], integer),
            'XFlush': ([ptr], integer), 'XSync': ([ptr, integer], integer),
            'XGrabServer': ([ptr], integer), 'XUngrabServer': ([ptr], integer),
            'XGetImage': ([ptr, ulong, integer, integer, ctypes.c_uint, ctypes.c_uint, ulong, integer],
                          ctypes.POINTER(XImage)),
            'XDestroyImage': ([ctypes.POINTER(XImage)], integer),
            'XSetErrorHandler': ([ptr], ptr),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.x, name)
            fn.argtypes, fn.restype = args, result
        self.display = self.x.XOpenDisplay(env['DISPLAY'].encode())
        if not self.display:
            raise Unavailable('Cannot connect to the specified X11 display')
        self.error = False
        def error_handler(_display, _event):
            self.error = True
            return 0
        self.error_handler = ctypes.CFUNCTYPE(integer, ptr, ptr)(error_handler)
        self.old_error_handler = self.x.XSetErrorHandler(self.error_handler)
        self.root = self.x.XDefaultRootWindow(self.display)
        self.atoms = {name: self.x.XInternAtom(self.display, name.encode(), False)
                      for name in ('_NET_WM_PID', 'WM_PROTOCOLS', '_NET_WM_PING', 'WM_DELETE_WINDOW')}
        self.x.XSelectInput(self.display, self.root, 1 << 19)

    def finish(self):
        self.x.XCloseDisplay(self.display)
        self.x.XSetErrorHandler(self.old_error_handler)

    def children(self, window):
        root, parent, count = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_uint()
        children = ctypes.POINTER(ctypes.c_ulong)()
        if not self.x.XQueryTree(self.display, window, ctypes.byref(root), ctypes.byref(parent),
                                ctypes.byref(children), ctypes.byref(count)):
            return []
        try:
            return list(children[:count.value])
        finally:
            if children:
                self.x.XFree(children)

    def pid(self, window):
        kind, count, remaining, fmt = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_int()
        data = ctypes.c_void_p()
        self.x.XGetWindowProperty(self.display, window, self.atoms['_NET_WM_PID'], 0, 1, False, 6,
                                 ctypes.byref(kind), ctypes.byref(fmt), ctypes.byref(count),
                                 ctypes.byref(remaining), ctypes.byref(data))
        try:
            return ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))[0] if data and fmt.value == 32 and count.value == 1 else None
        finally:
            if data:
                self.x.XFree(data)

    def attributes(self, window):
        attributes = XAttributes()
        if not self.x.XGetWindowAttributes(self.display, window, ctypes.byref(attributes)):
            raise Unavailable('X11 example window disappeared')
        return attributes

    def find(self, pid):
        for window in reversed(self.children(self.root)):
            if self.pid(window) == pid and self.attributes(window).map_state == 2:
                return window
            for child in self.children(window):
                if self.pid(child) == pid:
                    raise Unavailable('Reparented X11 windows are not supported; run this check in Xvfb without a window manager')
        return None

    def activate(self, window, pid):
        if self.pid(window) == pid:
            self.x.XRaiseWindow(self.display, window)
            self.x.XFlush(self.display)

    def send(self, window, protocol, token=0):
        protocols, count = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_int()
        if not self.x.XGetWMProtocols(self.display, window, ctypes.byref(protocols), ctypes.byref(count)):
            raise Unavailable('X11 example does not expose window-manager protocols')
        try:
            if self.atoms[protocol] not in protocols[:count.value]:
                raise Unavailable(f'X11 example does not support {protocol}; external responsiveness/close cannot be verified')
        finally:
            self.x.XFree(protocols)
        event = XEvent()
        event.client.type, event.client.display, event.client.window = 33, self.display, window
        event.client.message_type, event.client.format = self.atoms['WM_PROTOCOLS'], 32
        event.client.data[:3] = (self.atoms[protocol], token, window)
        sent = self.x.XSendEvent(self.display, window, False, 0, ctypes.byref(event))
        self.x.XFlush(self.display)
        return bool(sent)

    def responsive(self, window, pid):
        if self.pid(window) != pid:
            return False
        token = time.monotonic_ns() & 0x7fffffff
        self.send(window, '_NET_WM_PING', token)
        deadline, event = time.monotonic() + 0.3, XEvent()
        while time.monotonic() < deadline:
            while self.x.XCheckTypedWindowEvent(self.display, self.root, 33, ctypes.byref(event)):
                if (event.client.message_type == self.atoms['WM_PROTOCOLS'] and
                        list(event.client.data[:3]) == [self.atoms['_NET_WM_PING'], token, window]):
                    return True
            time.sleep(0.005)
        return False

    def close(self, window, pid):
        return self.pid(window) == pid and self.send(window, 'WM_DELETE_WINDOW')

    def capture(self, window, pid, path):
        self.x.XGrabServer(self.display)
        pixels = None
        try:
            self.error = False
            if self.pid(window) != pid:
                raise Unavailable('X11 window ownership changed')
            a, root = self.attributes(window), self.attributes(self.root)
            left, top = a.x + a.border_width, a.y + a.border_width
            if a.map_state != 2 or left < 0 or top < 0 or left + a.width > root.width or top + a.height > root.height:
                raise Unavailable('X11 example is not fully visible on the display')
            windows = self.children(self.root)
            if window not in windows:
                raise Unavailable('X11 example is not a direct client window')
            for other in windows[windows.index(window) + 1:]:
                b = self.attributes(other)
                if (b.map_state == 2 and b.x < left + a.width and b.x + b.width + 2 * b.border_width > left
                        and b.y < top + a.height and b.y + b.height + 2 * b.border_width > top):
                    raise Unavailable('Another X11 window overlaps the example; capture withheld')
            pixels = self.x.XGetImage(self.display, window, 0, 0, a.width, a.height,
                                      ctypes.c_ulong(-1).value, 2)
            self.x.XSync(self.display, False)
            if not pixels or self.error:
                raise Unavailable('X11 client image could not be read')
            info = pixels.contents
            if info.bits_per_pixel != 32 or (info.red_mask, info.green_mask, info.blue_mask) != (0xff0000, 0xff00, 0xff):
                raise Unavailable('X11 capture requires a standard 24/32-bit RGB visual')
            raw = ctypes.string_at(info.data, info.bytes_per_line * info.height)
            rows = b''.join(raw[y * info.bytes_per_line:y * info.bytes_per_line + info.width * 4]
                            for y in range(info.height))
            rgb = bytearray(info.width * info.height * 3)
            indices = (2, 1, 0) if info.byte_order == 0 else (1, 2, 3)
            for output, source in enumerate(indices):
                rgb[output::3] = rows[source::4]
            header = f'P6\n{info.width} {info.height}\n255\n'.encode('ascii')
        finally:
            if pixels:
                self.x.XDestroyImage(pixels)
            self.x.XUngrabServer(self.display)
            self.x.XFlush(self.display)
        path.write_bytes(header + rgb)


def desktop_for(env):
    return X11(env) if sys.platform.startswith('linux') else Windows()


def observe(binary: Path, arguments: list[str], cwd: Path, env: dict, directory: Path,
            seconds: float = 3, timeout: float = 120, warmup: float = 0) -> dict:
    """Launch, observe, capture and gracefully close only this process's native window.

    PASS describes process/window observation, not GPU validation or image correctness.
    A finite process may exit successfully without a window; callers must require
    captures and sufficient observed_seconds when judging graphical examples.
    """
    result = dict(status='FAIL', message='', output='', captures=[], capture_times=[], exit_code=None,
                  observed_seconds=0.0, window_observed=False, natural_exit=False,
                  forced_termination=False, responsive=False, capture_method='Win32 client BitBlt')
    if seconds <= 0 or timeout <= 0 or warmup < 0 or warmup + seconds >= timeout:
        result['message'] = 'Positive observation time and nonnegative warmup must fit inside timeout'
        return result
    try:
        desktop = desktop_for(env)
        result['capture_method'] = 'X11 client XGetImage' if isinstance(desktop, X11) else 'Win32 client BitBlt'
    except Unavailable as error:
        result.update(status='BLOCKED', message=str(error))
        return result
    directory = Path(directory)
    process, hwnd, observed_at, closed_at, unresponsive_at = None, None, None, None, None
    started = time.monotonic()
    try:
        directory.mkdir(parents=True, exist_ok=True)
        with (directory / 'process.log').open('w+b') as log:
            try:
                process = subprocess.Popen([str(binary), *arguments], cwd=cwd, env=env,
                                           stdout=log, stderr=subprocess.STDOUT)
                while True:
                    now = time.monotonic()
                    code = process.poll()
                    if code is not None:
                        result.update(exit_code=code, natural_exit=closed_at is None)
                        if result['message']:
                            break
                        result.update(status='PASS' if code == 0 else 'FAIL',
                                      message=('Exited cleanly after WM_CLOSE' if closed_at else
                                               'Process exited naturally; graphical coverage requires caller checks')
                                      if code == 0 else f'Process exited with code {code}')
                        break
                    if now - started >= timeout or (closed_at and now - closed_at >= 5):
                        result.update(status='FAIL', message='Example timed out or did not close after WM_CLOSE')
                        break
                    if closed_at:
                        time.sleep(0.05)
                        continue
                    if not hwnd:
                        hwnd = desktop.find(process.pid)
                        if hwnd:
                            result['window_observed'] = True
                            desktop.activate(hwnd, process.pid)  # Only the newly launched process.
                    if hwnd:
                        if not desktop.responsive(hwnd, process.pid):
                            # A window exists before shader/device initialization finishes.
                            if observed_at is not None:
                                unresponsive_at = unresponsive_at or now
                                if now - unresponsive_at >= 3:
                                    result.update(status='FAIL', message='Example window stopped responding')
                                    desktop.close(hwnd, process.pid)
                                    closed_at = now
                            time.sleep(0.05)
                            continue
                        unresponsive_at = None
                        observed_at = observed_at if observed_at is not None else now
                        # Original examples may process messages before their first GPU present.
                        result['observed_seconds'] = max(0.0, now - observed_at - warmup)
                        result['responsive'] = True
                        elapsed = result['observed_seconds']
                        if (not result['captures'] and elapsed >= min(0.75, seconds / 2)) or elapsed >= seconds:
                            capture = directory / f'frame-{len(result["captures"]):02}.ppm'
                            desktop.capture(hwnd, process.pid, capture)
                            result['captures'].append(capture)
                            result['capture_times'].append(time.monotonic() - started)
                        if elapsed >= seconds:
                            if not desktop.close(hwnd, process.pid):
                                result.update(status='FAIL', message='Could not deliver WM_CLOSE to owned window')
                            closed_at = now
                    time.sleep(0.05)
            except Unavailable as error:
                result.update(status='BLOCKED', message=str(error))
            except OSError as error:
                result.update(status='FAIL', message=f'Example launch/observation failed: {error}')
            finally:
                if process is not None and process.poll() is None:
                    if hwnd:
                        try:
                            desktop.close(hwnd, process.pid)
                        except Unavailable:
                            pass
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()  # Popen retains the original process handle on Windows.
                        process.wait(timeout=5)
                        result['forced_termination'] = True
                        if result['status'] == 'PASS':
                            result.update(status='FAIL', message='Forced termination is not a passing observation')
                if process is not None:
                    result['exit_code'] = process.returncode
                log.seek(0)
                result['output'] = log.read().decode('utf-8', errors='replace')
    except OSError as error:
        result.update(status='FAIL', message=f'Cannot write observer output: {error}')
    finally:
        desktop.finish()
    return result
