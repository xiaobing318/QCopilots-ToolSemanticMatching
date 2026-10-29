#!/usr/bin/env python3
"""Run the same native workflow locally and in the eleven GitHub variants."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[2]
PREFIX = 'QCOPILOTS_TOOL_SEMANTIC_MATCHING_'
spec = importlib.util.spec_from_file_location('qctsm_artifacts', SOURCE/'.github/scripts/qctsm-artifact-selection.py')
artifact_selection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifact_selection)
CONTAINER_CMAKE_VERSION = '3.31.10'
CONTAINER_CMAKE_WHEEL_SHA256 = '2f766bb46367e5e0559fa33184653754bce044583a06014dcaebf8e6dff8a1f1'


def environment_path(name):
    value = os.environ.get(name)
    if not value or any(char in value for char in '\r\n'):
        raise RuntimeError('Required CI path is missing or invalid: '+name)
    path = Path(value).resolve()
    if path == SOURCE or SOURCE in path.parents:
        raise RuntimeError('CI outputs must be outside source: '+str(path))
    return path


def candidate_head():
    event_path = os.environ.get('GITHUB_EVENT_PATH')
    event = json.loads(Path(event_path).read_text(encoding='utf-8')) if event_path else {}
    head = event.get('pull_request', {}).get('head', {}).get('sha') or os.environ.get('GITHUB_SHA')
    if not isinstance(head, str) or not re.fullmatch('[0-9a-f]{40}', head):
        raise RuntimeError('CI needs the exact event source commit')
    if os.environ.get(PREFIX+'CANDIDATE_HEAD', head) != head:
        raise RuntimeError('Configured candidate does not match the event')
    return head


def run(command, *, env=None, capture=False):
    command = [str(part) for part in command]
    print('+ '+subprocess.list2cmdline(command), flush=True)
    return subprocess.run(command, cwd=SOURCE, env=env, check=True, text=True,
                          encoding='utf-8', errors='replace', capture_output=capture)


def clean_source():
    head = run(['git', 'rev-parse', 'HEAD'], capture=True).stdout.strip()
    dirty = run(['git', 'status', '--porcelain'], capture=True).stdout.strip()
    if head != candidate_head() or dirty:
        raise RuntimeError('Native CI requires the matching, clean source checkout')


def variant():
    identifier = os.environ.get(PREFIX+'CI_ID')
    inventory = artifact_selection.variants()
    if identifier not in inventory:
        raise RuntimeError('Unknown CI variant: '+str(identifier))
    return inventory[identifier]


def variant_root():
    return environment_path(PREFIX+'BUILD_ROOT')/variant()['id']


def reports():
    return variant_root()/'reports/Release'


def matrix():
    rows = list(artifact_selection.variants().values())
    if len(rows) != 11 or sum(len(row['generators']) for row in rows) != 21:
        raise RuntimeError('Expected eleven variants and twenty-one user packages')
    document = {'include':[{**row, 'container':row.get('container', '')} for row in rows]}
    encoded = json.dumps(document, separators=(',', ':'))
    print(encoded)
    if os.environ.get('GITHUB_OUTPUT'):
        with environment_path('GITHUB_OUTPUT').open('a', encoding='utf-8') as output:
            print('matrix='+encoded, file=output)


def initialize():
    clean_source()
    selected = variant()
    attempt = os.environ.get('GITHUB_RUN_ATTEMPT', '')
    run_id = os.environ.get('GITHUB_RUN_ID', '')
    if not re.fullmatch('[1-9][0-9]*', attempt) or not re.fullmatch('[1-9][0-9]*', run_id):
        raise RuntimeError('CI run ID and attempt must be positive integers')
    temporary = environment_path('RUNNER_TEMP')
    parent = Path(tempfile.mkdtemp(prefix='qctsm-', dir=temporary))
    root = parent/selected['id']
    directories = {PREFIX+'BUILD_ROOT':parent, PREFIX+'VARIANT_ROOT':root,
        PREFIX+'QT_ROOT':parent/'sdk/qt',
        PREFIX+'MODEL_DIR':temporary/'qctsm-model-cache/gemma',
        PREFIX+'TEST_ROOT':root/'runs/Release'}
    for directory in directories.values():
        directory.mkdir(parents=True, exist_ok=True)
    report_dir = root/'reports/Release'
    report_dir.mkdir(parents=True)
    identity = {'schemaVersion':1, 'candidateHead':candidate_head(), 'runId':run_id,
        'runAttempt':int(attempt), 'variant':selected['id'], 'source':str(SOURCE),
        'directories':{key:str(value) for key,value in directories.items()}}
    (report_dir/'runner-identity.json').write_text(json.dumps(identity,indent=2)+'\n', encoding='utf-8')
    with environment_path('GITHUB_ENV').open('a',encoding='utf-8') as output:
        for key,value in directories.items():
            print(f'{key}={value}',file=output)
        print(f'{PREFIX}PYTHON={sys.executable}',file=output)
    print(json.dumps(identity,indent=2))


def toolchain():
    selected = variant()
    if (platform.system() != 'Linux' or selected['arch'] != 'x64' or selected['backend'] != 'cuda'
            or not selected.get('container') or os.environ.get('GITHUB_ACTIONS') != 'true'
            or os.environ.get(PREFIX+'CI_CONTAINER') != '1'
            or os.environ.get(PREFIX+'CI_CONTAINER_IMAGE') != selected['container']):
        raise RuntimeError('Locked CMake bootstrap is only for the selected disposable CUDA container')
    bootstrap = variant_root()/'bootstrap'
    bootstrap.mkdir(parents=True,exist_ok=True)
    target = bootstrap/('cmake-'+CONTAINER_CMAKE_VERSION)
    if target.exists():
        raise RuntimeError('Locked toolchain destination must be fresh')
    requirements = bootstrap/'cmake-requirements.txt'
    requirements.write_text(f'cmake=={CONTAINER_CMAKE_VERSION} --hash=sha256:{CONTAINER_CMAKE_WHEEL_SHA256}\n',encoding='utf-8')
    run([sys.executable,'-m','pip','--isolated','--disable-pip-version-check','install',
         '--index-url','https://pypi.org/simple','--only-binary=:all:','--require-hashes',
         '--no-deps','--no-cache-dir','--target',target,'--requirement',requirements])
    binary = target/'cmake/data/bin'
    environment = dict(os.environ)
    environment['PATH'] = str(binary)+os.pathsep+environment.get('PATH','')
    for name in ('cmake','ctest','cpack'):
        expected = binary/name
        if not expected.is_file() or expected.is_symlink() or Path(shutil.which(name,path=environment['PATH']) or '/missing').resolve() != expected.resolve():
            raise RuntimeError('Toolchain did not resolve to the pinned wheel: '+name)
        result = run([expected,'--version'],env=environment,capture=True)
        if result.stdout.splitlines()[0] != f'{name} version {CONTAINER_CMAKE_VERSION}':
            raise RuntimeError('Toolchain version mismatch: '+name)
    with environment_path('GITHUB_PATH').open('a',encoding='utf-8') as output:
        print(binary,file=output)
    print(json.dumps({'cmakeVersion':CONTAINER_CMAKE_VERSION,'wheelSha256':CONTAINER_CMAKE_WHEEL_SHA256}))


def dev(action, *arguments):
    entry = ['pwsh','-NoProfile','-File',SOURCE/'qctsm-dev.ps1'] if os.name == 'nt' else ['bash',SOURCE/'qctsm-dev.sh']
    command = entry+[action,'--build-root',environment_path(PREFIX+'BUILD_ROOT'),
        '--variant',variant()['id'],'--qt-root',environment_path(PREFIX+'QT_ROOT'),
        '--model-dir',environment_path(PREFIX+'MODEL_DIR'),'--config','Release','--jobs','3',*arguments]
    run(command)


def configure():
    clean_source()
    dev('configure','--cmake-arg=-D'+PREFIX+'CANDIDATE_COMMIT='+candidate_head(),
        '--cmake-arg=-D'+PREFIX+'CANDIDATE_ATTEMPT='+os.environ['GITHUB_RUN_ATTEMPT'],
        '--cmake-arg=-D'+PREFIX+'CANDIDATE_RUN_ID='+os.environ['GITHUB_RUN_ID'])


def test():
    dev('test')
    root = ET.parse(reports()/'ctest.xml').getroot()
    cases = root.findall('.//testcase')
    if not cases or root.findall('.//failure') or root.findall('.//error') or root.findall('.//skipped'):
        raise RuntimeError('Required CTest cases are absent, failed, or skipped')
    names = [case.attrib.get('name','') for case in cases]
    required = {'qctsm-core','qctsm-http-provider','qctsm-service','qctsm-gui-interaction',
                'qctsm-model-files','qctsm-embedding-real-model','qctsm-worker-protocol',
                'qctsm-quality-real-model','qctsm-quality-contract','qctsm-quality-cold-latency','qctsm-cli-protocol',
                'qctsm-tooling','qctsm-license-materials'}
    missing = required-set(names)
    if missing:
        raise RuntimeError('Required product tests were not executed: '+', '.join(sorted(missing)))
    print(json.dumps({'passed':True,'testCount':len(cases),'testNames':names}))
    quality_files = list((variant_root()/'runs/Release/quality').glob('*/gis-quality.json'))
    if len(quality_files) != 1:
        raise RuntimeError('Fresh CI execution requires one inspectable real-model quality report')
    quality = json.loads(quality_files[0].read_text(encoding='utf-8'))
    if quality.get('passed') is not True or quality.get('query_count',0) < 50 or quality.get('recall_at_8',0) < .9:
        raise RuntimeError('Real-model retrieval acceptance report did not meet the required dataset and recall')
    evidence = {key:quality.get(key) for key in ('dataset_id','dataset_revision','query_count',
        'catalog_sha256','queries_sha256','recall_at_8','mrr_at_8','context_size',
        'latency_seconds','warm_repeat_seconds','query_transport','cli_http_parity','failures')}
    cold_files = list((variant_root()/'runs/Release/quality').glob('*/cold-latency.json'))
    if len(cold_files) != 1:
        raise RuntimeError('Fresh CI execution requires one independent cold/warm latency report')
    cold = json.loads(cold_files[0].read_text(encoding='utf-8'))
    if cold.get('passed') is not True or cold.get('ranking_payload_parity') is not True:
        raise RuntimeError('Independent cold/warm query verification did not pass')
    evidence['cold_warm'] = {key:cold.get(key) for key in ('cold_scope','cold_query_seconds',
        'warm_query_seconds','cold_recall','ranking_payload_parity','prepare_graceful_shutdown','restart_graceful_shutdown')}
    evidence['gpu_test_output'] = [line.strip() for case in cases
        for line in case.findtext('system-out','').splitlines() if 'GPU inference tested=' in line]
    (reports()/'acceptance-summary.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'acceptance':evidence},ensure_ascii=False,indent=2))
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with environment_path('GITHUB_STEP_SUMMARY').open('a',encoding='utf-8') as summary:
            print(f'Validated {quality["query_count"]} frozen real-model queries. '
                  f'Recall@8={quality["recall_at_8"]:.4f}, MRR@8={quality["mrr_at_8"]:.4f}.',file=summary)
            print('\n```json\n'+json.dumps(evidence,ensure_ascii=False,indent=2)+'\n```',file=summary)


def package():
    dev('package')
    dev('verify-package')


def products():
    selected = variant()
    verified = {}
    for filename in ('package.json', 'installers.json') if selected['os'] != 'macos' else ('package.json',):
        report = json.loads((reports()/filename).read_text(encoding='utf-8'))
        if report.get('passed') is not True:
            raise RuntimeError('Package verification did not pass: '+filename)
        for entry in report.get('checks',[])+report.get('installers',[]):
            if entry.get('package') and entry.get('passed') is True:
                verified[entry['package']] = entry.get('sha256')
    version = artifact_selection.project_version(SOURCE)
    alternatives = artifact_selection.package_name_options(selected['id'],version)
    allowed = set().union(*alternatives)
    files = sorted((variant_root()/'pkg/Release').glob('qctsm-*'))
    if {path.name for path in files} not in alternatives:
        raise RuntimeError('Unexpected package set: '+', '.join(path.name for path in files))
    records = []
    for path in files:
        if not path.is_file() or path.is_symlink() or path.name not in allowed or not 0 < path.stat().st_size <= artifact_selection.MAX_ASSET_BYTES:
            raise RuntimeError('Invalid raw user package: '+path.name)
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda:stream.read(1024*1024),b''):
                digest.update(block)
        if verified.get(path.name) != digest.hexdigest():
            raise RuntimeError('Final package differs from its successfully verified bytes: '+path.name)
        records.append({'name':path.name,'path':str(path),'bytes':path.stat().st_size,'sha256':digest.hexdigest()})
    report = {'candidateHead':candidate_head(),'runId':os.environ['GITHUB_RUN_ID'],
              'attempt':int(os.environ['GITHUB_RUN_ATTEMPT']),'variant':selected['id'],'packages':records}
    (reports()/'products.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))
    with environment_path('GITHUB_OUTPUT').open('a',encoding='utf-8') as output:
        for index in range(2):
            item = records[index] if index < len(records) else {}
            print(f'package_{index+1}_name={item.get("name","")}',file=output)
            print(f'package_{index+1}_path={item.get("path","")}',file=output)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with environment_path('GITHUB_STEP_SUMMARY').open('a',encoding='utf-8') as summary:
            print('| Package | Bytes | SHA-256 |\n|---|---:|---|',file=summary)
            for item in records:
                print(f'| {item["name"]} | {item["bytes"]} | `{item["sha256"]}` |',file=summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['matrix','initialize','toolchain','bootstrap','doctor','configure',
        'build','models','test','install','package','installers','products'])
    action = parser.parse_args().action
    specific = {'matrix':matrix,'initialize':initialize,'toolchain':toolchain,'configure':configure,
                'test':test,'package':package,'products':products}
    if action == 'build':
        clean_source()
    if action in specific:
        specific[action]()
    else:
        dev(action)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print('qctsm native CI: '+str(error),file=sys.stderr)
        raise SystemExit(1)
