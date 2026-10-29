#!/usr/bin/env python3
"""Verify fixed build coverage and reproducible SDK export without touching user data."""
from __future__ import annotations
import importlib.util
import hashlib
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

SOURCE=Path(__file__).resolve().parents[1]

class ToolingTests(unittest.TestCase):
    def test_pinned_source_manifest_and_qt_runtime_closure(self):
        dependencies=json.loads((SOURCE/'dependencies.lock.json').read_text())
        llama=dependencies['llama.cpp']
        self.assertEqual(hashlib.sha256((SOURCE/llama['sourceManifest']).read_bytes()).hexdigest(),
                         llama['sourceManifestSha256'])
        qt=json.loads((SOURCE/'tools/qt-packages.lock.json').read_text())
        self.assertIn('icu',{item['module'] for item in qt['platforms']['linux']['archives']})
        for platform in qt['platforms'].values():
            self.assertTrue({'qtbase','qttools'} <= {item['module'] for item in platform['archives']})
            self.assertFalse({'qtpdf','qtmultimedia'} & {item['module'] for item in platform['archives']})

    def test_native_matrix_is_complete(self):
        variants=json.loads((SOURCE/'config/build-variants.json').read_text())['variants']
        self.assertEqual(len(variants),11)
        self.assertEqual(len({x['id'] for x in variants}),11)
        self.assertEqual(sum(len(x['generators']) for x in variants),21)
        self.assertEqual({x['backend'] for x in variants},{'cpu','vulkan','hip','cuda','metal'})

    def test_sdk_archive_is_reproducible_and_has_no_runtime(self):
        spec=importlib.util.spec_from_file_location('qctsm_export',SOURCE/'tools/qctsm-export-core.py')
        module=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(prefix='qctsm-sdk-test-') as temporary:
            first=module.export(Path(temporary)/'first',True)
            second=module.export(Path(temporary)/'second',True)
            self.assertEqual(first['archive_sha256'],second['archive_sha256'])
            with tarfile.open(Path(temporary)/'first'/first['archive']) as archive:
                names=archive.getnames()
                self.assertIn('src/core/retrieval.h',names)
                self.assertIn('src/provider_http/http_embedding_provider.h',names)
                self.assertFalse(any(name.startswith(('src/runtime/','src/app/','src/worker/')) for name in names))
                self.assertTrue(all(not name.startswith('/') and '..' not in Path(name).parts for name in names))

if __name__=='__main__':
    unittest.main()
