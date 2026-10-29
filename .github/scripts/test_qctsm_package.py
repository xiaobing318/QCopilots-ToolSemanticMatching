#!/usr/bin/env python3
"""Package validation must reject unsafe extraction and altered GPU payloads."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('qctsm_package_verify',Path(__file__).resolve().parents[2]/'tools/qctsm-verify-package.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class PackageChecks(unittest.TestCase):
    def test_archive_parent_traversal_is_rejected_before_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root/'bad.zip'
            with zipfile.ZipFile(package,'w') as output:
                output.writestr('../escaped.txt','invalid')
            with self.assertRaisesRegex(RuntimeError,'Unsafe archive path'):
                verify.extract(package,root/'extracted')
            self.assertFalse((root/'escaped.txt').exists())

    def test_existing_windows_registration_blocks_installation(self):
        with mock.patch.object(verify,'windows_registry_installations',return_value=[{'location':'existing'}]):
            with self.assertRaisesRegex(RuntimeError,'existing QCTSM installation'):
                verify.require_no_windows_installation()

    def test_existing_windows_menu_blocks_installation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            menu = root/'Microsoft/Windows/Start Menu/Programs'/verify.WINDOWS_MENU
            menu.mkdir(parents=True)
            with mock.patch.dict(os.environ,{'APPDATA':str(root),'LOCALAPPDATA':str(root),'ProgramData':str(root)}), mock.patch.object(verify,'windows_registry_installations',return_value=[]):
                with self.assertRaisesRegex(RuntimeError,'Start menu'):
                    verify.require_no_windows_installation()

    def test_uninstaller_refuses_changed_registration_ownership(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with mock.patch.object(verify,'windows_registry_installations',return_value=[{'hive':'HKCU','location':str(root/'another')}]), mock.patch.object(verify,'run') as run:
                with self.assertRaisesRegex(RuntimeError,'ownership changed'):
                    verify.remove_owned_windows_installation(root/'owned')
                run.assert_not_called()

    def test_gpu_backend_missing_is_a_failure_even_when_cpu_would_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(RuntimeError,'selected backend'):
                verify.verify_gpu_payload(root,root/'bin',{'backend':'vulkan'})

    def test_gpu_closure_requires_exact_installed_bytes_and_licenses(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root/'bin'
            binary.mkdir()
            module = binary/('ggml-vulkan.dll' if os.name == 'nt' else 'libggml-vulkan.so')
            module.write_bytes(b'unit-test-library')
            license = binary/'licenses/vendor.txt'
            license.parent.mkdir()
            license.write_text('license',encoding='utf-8')
            identity = {'backend':'vulkan','variant':'test-vulkan'}
            manifest = {**identity,'verification':{'dependencyClosure':'verified'},
                'files':[{'path':module.relative_to(root).as_posix(),'role':'backend','size':module.stat().st_size,'sha256':verify.digest(module)}],
                'resources':[{'files':[{'source':'licenses/vendor.txt','sha256':verify.digest(license)}]}]}
            (binary/'qctsm-gpu-runtime.json').write_text(json.dumps(manifest),encoding='utf-8')
            self.assertEqual(verify.verify_gpu_payload(root,binary,identity)['verifiedRuntimeFiles'],1)
            module.write_bytes(b'tampered-library')
            with self.assertRaisesRegex(RuntimeError,'missing or changed'):
                verify.verify_gpu_payload(root,binary,identity)


if __name__ == '__main__':
    unittest.main()
