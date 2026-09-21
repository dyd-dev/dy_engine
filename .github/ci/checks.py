"""Assertions on external observations; no source rewriting or private engine hooks."""
from collections import Counter
from pathlib import Path
import re
import statistics


GPU_ERROR = re.compile(r'Validation Error|VUID-|DEVICE REMOVED|ERROR: AddressSanitizer|runtime error:|'
                       r'\[D3D12 [^\n]*sev=[01]\b|\b(?:VS|PS) Compile Error:')


def read_ppm(path):
    path = Path(path)
    if not 0 < path.stat().st_size <= 128 * 1024 * 1024:
        raise ValueError('Capture is empty or exceeds 128 MiB')
    data = path.read_bytes()
    header = re.match(rb'P6\n([0-9]+) ([0-9]+)\n255\n', data)
    if not header:
        raise ValueError('Expected an RGB P6 capture')
    width, height = map(int, header.groups())
    pixels = data[header.end():]
    if not (1 <= width <= 16384 and 1 <= height <= 16384) or len(pixels) != width * height * 3:
        raise ValueError('Invalid capture dimensions or truncated pixel data')
    return width, height, pixels


def rgb(frame, x, y):
    width, height, data = frame
    offset = (min(height - 1, max(0, y)) * width + min(width - 1, max(0, x))) * 3
    return tuple(data[offset:offset + 3])


def check_observation(profile, observed):
    checks = []
    def result(status, message):
        return dict(status=status, message=message, checks=checks, coverage=profile.get('coverage', profile.get('kind')))
    def require(condition, name, **data):
        checks.append(dict(name=name, passed=bool(condition), **data))
    output = observed.get('output', '')
    unsupported = profile.get('unsupported_markers', [])
    if (observed.get('status') in ('PASS', 'FAIL') and observed.get('exit_code') == 77
            and not observed.get('forced_termination') and not observed.get('forced_kill')
            and not GPU_ERROR.search(output) and isinstance(unsupported, list)
            and any(isinstance(marker, str) and marker and marker in output for marker in unsupported)):
        require(True, 'declared-feature-exclusion', exit_code=77)
        return result('UNSUPPORTED', 'Example reported an explicitly declared unsupported backend feature')
    if observed.get('status') != 'PASS':
        return result(observed.get('status', 'BLOCKED'), observed.get('message', 'No observation'))
    kind = profile.get('kind')
    expected_exit = 77 if kind == 'capability' else 0
    if observed.get('forced_termination') or observed.get('forced_kill') or observed.get('exit_code') != expected_exit:
        return result('FAIL', 'Abnormal exit or forced termination is not a successful run')
    if GPU_ERROR.search(output):
        return result('FAIL', 'Captured diagnostic output reports a graphics or sanitizer error')
    if kind == 'capability':
        markers = profile.get('markers')
        if profile.get('expected_exit') != 77 or not isinstance(markers, list) or not markers:
            return result('BLOCKED', 'Capability checks require authored exit 77 and diagnostic markers')
        for marker in markers:
            require(isinstance(marker, str) and bool(marker) and marker in output, 'capability-diagnostic', expected=marker)
        if not all(check['passed'] for check in checks):
            return result('FAIL', 'Missing capability or unsupported-feature diagnostic')
        return result('UNSUPPORTED', 'Capability query completed; this example explicitly does not implement the feature')
    elif kind in ('window', 'visible', 'pixels'):
        if not observed.get('window_observed') or observed.get('observed_seconds', 0) < profile.get('seconds', 3) * 0.9:
            return result('BLOCKED', 'Required responsive window observation was not completed')
        if len(observed.get('captures', [])) < 2:
            return result('BLOCKED', 'Two actual client-area captures are required')
        try:
            frames = [read_ppm(path) for path in observed['captures']]
            if len({frame[:2] for frame in frames}) != 1:
                return result('BLOCKED', 'Client size changed during observation')
        except (OSError, ValueError) as error:
            return result('BLOCKED', str(error))
        for index, frame in enumerate(frames):
            if kind == 'visible':
                x0, y0, x1, y1 = profile.get('region', [0.40, 0.12, 0.88, 0.88])
                width, height, _ = frame
                if not (0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1):
                    return result('BLOCKED', 'Invalid observation region')
                # ponytail: bounded samples catch missing scenes, not exact geometry or subpixel defects.
                step = max(1, min(width, height) // 128)
                values = [rgb(frame, x, y) for y in range(int(y0 * height), int(y1 * height), step)
                          for x in range(int(x0 * width), int(x1 * width), step)]
                if not values:
                    return result('BLOCKED', 'Observation region contains no pixel samples')
                dominant = Counter(tuple(channel // 8 for channel in value) for value in values).most_common(1)[0][0]
                different = sum(max(abs(channel - bucket * 8 - 4) for channel, bucket in zip(value, dominant)) > 12
                                for value in values) / len(values)
                require(different >= 0.002, 'visible-scene-' + str(index), non_background_fraction=different,
                        region=[x0, y0, x1, y1])
            elif kind == 'pixels':
                points = profile.get('pixels')
                if not isinstance(points, list) or not points:
                    return result('BLOCKED', 'No authored expected pixel values')
                for point in points:
                    if (not isinstance(point, list) or len(point) != 5
                            or not all(isinstance(value, (int, float)) for value in point)
                            or not 0 <= point[0] <= 1 or not 0 <= point[1] <= 1
                            or not all(0 <= channel <= 255 for channel in point[2:])):
                        return result('BLOCKED', 'Invalid expected pixel specification')
                    tolerance = profile.get('tolerance', 8)
                    if not isinstance(tolerance, (int, float)) or not 0 <= tolerance <= 255:
                        return result('BLOCKED', 'Invalid pixel tolerance')
                    x, y = int(point[0] * frame[0]), int(point[1] * frame[1])
                    neighborhood = [rgb(frame, x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]
                    actual = tuple(int(statistics.median(value[channel] for value in neighborhood)) for channel in range(3))
                    require(max(abs(a - b) for a, b in zip(actual, point[2:])) <= tolerance,
                            'pixel-' + str(index) + '-' + str(point[:2]), actual=actual, expected=point[2:])
        for marker in profile.get('markers', []):
            require(marker in output, 'fixture-evidence', expected=marker)
    else:
        return result('BLOCKED', 'No external check profile for this official example')
    failed = [check['name'] for check in checks if not check['passed']]
    return result('FAIL' if failed else 'PASS', ', '.join(failed) if failed else 'Declared external checks passed')
