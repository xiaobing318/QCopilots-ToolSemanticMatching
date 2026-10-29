#!/usr/bin/env python3
"""Export a reproducible, QGIS-independent retrieval SDK source archive."""
from __future__ import annotations
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

SOURCE = Path(__file__).resolve().parents[1]
SDK_CMAKE = '''cmake_minimum_required(VERSION 3.16)
# This SDK intentionally does not change the parent project's compiler or Qt.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network)
if(NOT TARGET SQLite3::SQLite3 AND NOT TARGET SQLite::SQLite3)
  find_package(SQLite3 REQUIRED)
endif()
if(TARGET SQLite3::SQLite3 AND NOT TARGET SQLite::SQLite3)
  add_library(SQLite::SQLite3 ALIAS SQLite3::SQLite3)
endif()
if(TARGET SQLite::SQLite3 AND NOT TARGET SQLite3::SQLite3)
  add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
add_subdirectory(src/core)
add_subdirectory(src/provider_http)
'''

def git(*args):
    result = subprocess.run(['git','-C',str(SOURCE),*args],capture_output=True,text=True,encoding='utf-8')
    return result.stdout.strip() if result.returncode==0 else ''

def source_bytes(name, commit, dirty):
    if commit and not dirty:
        # Use the committed blob, not a checkout transformed by core.autocrlf.
        # This makes the same source commit produce the same SDK on every OS.
        return subprocess.check_output(['git','-C',str(SOURCE),'show',commit+':'+name])
    return (SOURCE/name).read_bytes().replace(b'\r\n',b'\n')

def export(output, allow_dirty=False):
    output=Path(output).resolve()
    if output==SOURCE or SOURCE in output.parents:
        raise ValueError('SDK exports must be outside the source checkout')
    commit=git('rev-parse','HEAD')
    dirty=bool(git('status','--porcelain'))
    if (not commit or dirty) and not allow_dirty:
        raise ValueError('Export requires a clean committed checkout. Use --allow-dirty only for local development.')
    files={}
    for prefix in ('src/core','src/provider_http'):
        directory=SOURCE/prefix
        if not (directory/'CMakeLists.txt').is_file():
            raise ValueError('Missing SDK module: '+prefix)
        for path in sorted(directory.rglob('*')):
            if path.is_file() and not path.is_symlink():
                name=path.relative_to(SOURCE).as_posix()
                files[name]=source_bytes(name,commit,dirty)
    for name in ('LICENSE','THIRD_PARTY_NOTICES.md','docs/integration-contract.md'):
        files[name]=source_bytes(name,commit,dirty)
    files['CMakeLists.txt']=SDK_CMAKE.encode()
    manifest={'schema_version':1,'interface_version':1,'version':'0.1.0',
              'repository':'https://github.com/xiaobing318/QCopilots-ToolSemanticMatching',
              'commit':commit or 'UNCOMMITTED','dirty':dirty or not bool(commit),
              'files':{name:hashlib.sha256(content).hexdigest() for name,content in sorted(files.items())}}
    files['qctsm-core-manifest.json']=(json.dumps(manifest,ensure_ascii=False,sort_keys=True,indent=2)+'\n').encode()
    output.mkdir(parents=True,exist_ok=True)
    name='qctsm-core-0.1.0-'+(commit[:12] if commit else 'development')+('-dirty' if dirty else '')+'.tar.gz'
    archive=output/name
    with archive.open('wb') as raw, gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as compressed:
        with tarfile.open(fileobj=compressed,mode='w') as tar:
            for path,content in sorted(files.items()):
                info=tarfile.TarInfo(path)
                info.size=len(content)
                info.mode=0o644
                info.mtime=0
                tar.addfile(info,io.BytesIO(content))
    manifest['archive']=name
    manifest['archive_sha256']=hashlib.sha256(archive.read_bytes()).hexdigest()
    (output/'qctsm-core-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,sort_keys=True,indent=2)+'\n',encoding='utf-8')
    return manifest

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir',required=True,type=Path)
    parser.add_argument('--allow-dirty',action='store_true')
    args=parser.parse_args()
    print(json.dumps(export(args.output_dir,args.allow_dirty),ensure_ascii=False,indent=2))
    return 0

if __name__=='__main__':
    raise SystemExit(main())
