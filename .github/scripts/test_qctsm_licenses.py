#!/usr/bin/env python3
"""Verify actual collected notices and reject modified or incomplete copies."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('qctsm_licenses', SOURCE/'tools/qctsm-license-materials.py')
licenses = importlib.util.module_from_spec(spec)
spec.loader.exec_module(licenses)


def expect_rejected(directory, label):
    try:
        licenses.verify_materials(directory)
    except (ValueError, OSError):
        return
    raise RuntimeError('Modified license payload was accepted: '+label)


def remove_with_inventory(directory, relative):
    (directory/relative).unlink()
    manifest = directory/licenses.MANIFEST
    document = json.loads(manifest.read_text(encoding='utf-8'))
    document['files'] = [entry for entry in document['files'] if entry['path'] != relative]
    manifest.write_text(json.dumps(document,indent=2)+'\n',encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--materials',required=True,type=Path)
    args = parser.parse_args()
    original = args.materials.resolve()
    verified = licenses.verify_materials(original)
    with tempfile.TemporaryDirectory(prefix='qctsm-license-test-',dir=original.parent) as temporary:
        parent = Path(temporary)
        changed = parent/'changed'
        shutil.copytree(original,changed)
        with (changed/'llama/LICENSE').open('ab') as stream:
            stream.write(b'\nUnexpected modification\n')
        expect_rejected(changed,'changed upstream notice bytes')
        missing = parent/'missing'
        shutil.copytree(original,missing)
        remove_with_inventory(missing,'llama/vendor/cpp-httplib/LICENSE')
        expect_rejected(missing,'missing cpp-httplib license, even after editing inventory')
        missing_qt = parent/'missing-qt'
        shutil.copytree(original,missing_qt)
        remove_with_inventory(missing_qt,'qt/sbom/qtbase-6.12.0.spdx.json')
        expect_rejected(missing_qt,'missing Qt module SPDX document')
    print(json.dumps({'passed':True,'actualMaterials':verified,'rejectionCases':3}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
