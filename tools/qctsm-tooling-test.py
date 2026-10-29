#!/usr/bin/env python3
"""Verify fixed build coverage and reproducible SDK export without touching user data."""
from __future__ import annotations
import importlib.util
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

SOURCE=Path(__file__).resolve().parents[1]

class ToolingTests(unittest.TestCase):
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
