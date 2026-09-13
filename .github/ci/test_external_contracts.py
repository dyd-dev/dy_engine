import tempfile
from pathlib import Path
import unittest

import checks as contracts


class ExternalContractTests(unittest.TestCase):
    def observation(self, folder, color=(255, 0, 0), *, shape=False):
        path = Path(folder) / 'frame.ppm'
        pixels = bytearray(bytes(color) * (100 * 80))
        if shape:
            for y in range(20, 60):
                for x in range(40, 70):
                    i = (y * 100 + x) * 3
                    pixels[i:i + 3] = bytes((0, 255, 0))
        path.write_bytes(b'P6\n100 80\n255\n' + pixels)
        second = Path(folder) / 'frame-02.ppm'
        second.write_bytes(path.read_bytes())
        return dict(status='PASS', exit_code=0, window_observed=True, observed_seconds=3,
                    forced_kill=False, captures=[path, second], output='')

    def test_expected_public_api_pixels_reject_wrong_color(self):
        profile = {'kind': 'pixels', 'seconds': 2, 'pixels': [[0.5, 0.5, 255, 0, 0]], 'tolerance': 8}
        with tempfile.TemporaryDirectory() as folder:
            observed = self.observation(folder)
            self.assertEqual(contracts.check_observation(profile, observed)['status'], 'PASS')
            observed = self.observation(folder, color=(0, 255, 0))
            self.assertEqual(contracts.check_observation(profile, observed)['status'], 'FAIL')

    def test_visible_example_rejects_flat_scene(self):
        profile = {'kind': 'visible', 'seconds': 2, 'region': [0.38, 0.1, 0.9, 0.9]}
        with tempfile.TemporaryDirectory() as folder:
            observed = self.observation(folder, shape=True)
            self.assertEqual(contracts.check_observation(profile, observed)['status'], 'PASS')
            observed = self.observation(folder)
            self.assertEqual(contracts.check_observation(profile, observed)['status'], 'FAIL')

    def test_forced_kill_missing_capture_and_early_exit_never_pass(self):
        profile = {'kind': 'visible', 'seconds': 2}
        with tempfile.TemporaryDirectory() as folder:
            observed = self.observation(folder, shape=True)
            for change in ({'forced_kill': True}, {'captures': []}, {'captures': observed['captures'][:1]}, {'observed_seconds': 0.1},
                           {'window_observed': False}, {'exit_code': 7}):
                self.assertNotEqual(contracts.check_observation(profile, dict(observed, **change))['status'], 'PASS')

    def test_external_capture_unavailable_stays_blocked(self):
        self.assertEqual(contracts.check_observation({'kind': 'visible'},
                         {'status': 'BLOCKED', 'message': 'no display'})['status'], 'BLOCKED')

    def test_callback_order_is_observed_from_original_console_output(self):
        profile = {'kind': 'render-graph'}
        observed = {'status': 'PASS', 'exit_code': 0, 'output':
                    '[VERIFICATION PASSED]\n-> Executing [ShadowPass]\n'
                    '-> Executing [MainForwardPass]\n-> Executing [PostProcessingPass]\n'}
        self.assertEqual(contracts.check_observation(profile, observed)['status'], 'PASS')
        observed['output'] = '[VERIFICATION PASSED]'
        self.assertEqual(contracts.check_observation(profile, observed)['status'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
