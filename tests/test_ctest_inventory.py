"""Source-only synthetic CTest inventory regression (requires CMake and CTest)."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'ctest_inventory.py'
spec = importlib.util.spec_from_file_location('ctest_inventory', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class InventoryTest(unittest.TestCase):
    def test_disabled_and_absent_are_distinct(self):
        doc = {'kind': 'ctestInfo', 'version': {'major': 1}, 'tests': [
            {'name': 'disabled', 'properties': [{'name': 'DISABLED', 'value': True}]},
            {'name': 'present'}]}
        report = module.inventory(doc, {'optional': ('disabled', 'present', 'absent')})
        self.assertEqual(report['groups']['optional'], {'absent': ['absent'], 'disabled': ['disabled']})
        self.assertFalse(report['execution_verified'])

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ctest'), 'requires CMake and CTest')
    def test_optional_prerequisite_omission(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.24)
project(inventory NONE)
enable_testing()
add_test(NAME baseline COMMAND ${CMAKE_COMMAND} -E true)
option(HAVE_OPTIONAL "Synthetic prerequisite" OFF)
if(HAVE_OPTIONAL)
  add_test(NAME optional COMMAND ${CMAKE_COMMAND} -E true)
endif()
''', encoding='utf-8')
            for enabled in ('OFF', 'ON'):
                build = root / enabled
                subprocess.run(['cmake', '-S', str(root), '-B', str(build),
                                '-DHAVE_OPTIONAL=' + enabled], check=True, capture_output=True)
                run = subprocess.run(['ctest', '--test-dir', str(build), '--output-on-failure'],
                                     check=True, capture_output=True)
                self.assertIn(b'100% tests passed', run.stdout)
                raw = subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1'])
                report = module.inventory(json.loads(raw), {'synthetic': ('optional',)})
                self.assertEqual(report['groups']['synthetic']['absent'], ['optional'] if enabled == 'OFF' else [])
                self.assertEqual(report['registered_count'], 1 if enabled == 'OFF' else 2)


if __name__ == '__main__':
    unittest.main()
