#!/usr/bin/env python3
"""Reproducible native build workflow. All generated material lives outside the checkout."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
PREFIX = 'QCOPILOTS_TOOL_SEMANTIC_MATCHING_'

def external(path):
    path = Path(path).expanduser().resolve()
    if path == SOURCE or SOURCE in path.parents:
        raise RuntimeError(f'Generated path must be outside source: {path}')
    return path

def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()

def run(command, *, cwd=None, env=None, capture=False):
    command = [str(x) for x in command]
    print('+ ' + subprocess.list2cmdline(command), flush=True)
    return subprocess.run(command, cwd=cwd, env=env, check=True, text=True,
                          encoding='utf-8', errors='replace', capture_output=capture)

def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))

def write_json(path, data):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

def variant_info(identifier=None):
    system = {'Windows':'windows', 'Linux':'linux', 'Darwin':'macos'}[platform.system()]
    arch = {'amd64':'x64', 'x86_64':'x64', 'aarch64':'arm64', 'arm64':'arm64'}[platform.machine().lower()]
    default = f'{system}-{arch}-' + ('metal' if system == 'macos' else 'cpu')
    identifier = identifier or os.environ.get(PREFIX+'VARIANT') or default
    variants = {x['id']:x for x in read_json(SOURCE/'config/build-variants.json')['variants']}
    if identifier not in variants:
        raise RuntimeError('Unknown variant: '+identifier)
    variant = variants[identifier]
    if variant['os'] != system or variant['arch'] != arch:
        raise RuntimeError(f'{identifier} is not a native variant for {system}-{arch}')
    return variant

def qt_kit(root):
    root = external(root)
    if (root/'lib/cmake/Qt6/Qt6Config.cmake').is_file():
        return root
    system = {'Windows':'windows','Linux':'linux','Darwin':'macos'}[platform.system()]
    return root / read_json(SOURCE/'tools/qt-packages.lock.json')['platforms'][system]['kit']

class Workflow:
    def __init__(self, args):
        self.args = args
        self.variant = variant_info(args.variant)
        self.base = external(args.build_root)
        self.root = self.base / self.variant['id']
        self.build = self.root / args.config
        self.stage = self.root / 'stage' / args.config
        self.packages = self.root / 'pkg' / args.config
        self.reports = self.root / 'reports' / args.config
        self.models = external(args.model_dir or os.environ.get(PREFIX+'MODEL_DIR') or self.base/'models')
        self.sdk = external(args.sdk_root or self.base/'sdk'/self.variant['id'])
        self.qtroot = external(args.qt_root or os.environ.get(PREFIX+'QT_ROOT') or self.base/'sdk/qt')
        self.qt = qt_kit(self.qtroot)
        self.gpu_report = self.reports/'gpu-sdk.json'
        self.env = dict(os.environ)
        self.env['PYTHONDONTWRITEBYTECODE'] = '1'
        self.env[PREFIX+'MODEL_DIR'] = str(self.models)
        self.env[PREFIX+'TEST_ROOT'] = str(self.root/'runs'/args.config)
        self.env[PREFIX+'VARIANT'] = self.variant['id']
        self.env['QT_QPA_PLATFORM'] = self.env.get('QT_QPA_PLATFORM', 'offscreen')
        self.env['PATH'] = str(self.qt/'bin') + os.pathsep + str(self.build/'bin') + os.pathsep + self.env.get('PATH','')
        if platform.system() == 'Linux':
            self.env['LD_LIBRARY_PATH'] = str(self.qt/'lib') + os.pathsep + str(self.build/'lib') + os.pathsep + self.env.get('LD_LIBRARY_PATH','')
        self.root.mkdir(parents=True, exist_ok=True)
        self.reports.mkdir(parents=True, exist_ok=True)
        marker = self.root/'.qctsm-build-root.json'
        if marker.exists() and Path(read_json(marker)['source']).resolve() != SOURCE:
            raise RuntimeError('Build root belongs to another source checkout')
        write_json(marker, {'schemaVersion':1,'source':str(SOURCE),'variant':self.variant['id']})

    def gpu(self, install=False):
        command = [sys.executable,'-B',SOURCE/'tools/qctsm-gpu-sdk.py','--variant',self.variant['id'],
                   '--sdk-root',self.sdk,'--build-root',self.root,'--output',self.gpu_report]
        if install:
            command += ['--install']
        run(command, env=self.env)
        report = read_json(self.gpu_report)
        self.env.update(report.get('environment',{}))
        return report

    def bootstrap(self):
        if not (self.qt/'lib/cmake/Qt6/Qt6Config.cmake').is_file():
            helper_python = sys.executable
            if importlib.util.find_spec('py7zr') is None:
                import venv
                venv_root = self.root/'bootstrap/python'
                venv.create(venv_root, with_pip=True)
                helper_python = str(venv_root/('Scripts/python.exe' if os.name=='nt' else 'bin/python'))
                run([helper_python,'-m','pip','install','py7zr==1.1.3'],env=self.env)
            run([helper_python,'-B',SOURCE/'tools/qctsm-bootstrap-qt.py','--qt-root',self.qtroot,
                 '--cache-dir',self.base/'sdk/qt-downloads'],env=self.env)
        self.gpu(install=True)
        if os.name == 'nt':
            run([sys.executable,'-B',SOURCE/'tools/qctsm-bootstrap-nsis.py','--output-dir',
                 self.root/'bootstrap/nsis','--cache-dir',self.base/'sdk/nsis-downloads'],env=self.env)

    def doctor(self):
        tools = {}
        for name in ('cmake','ctest','cpack','ninja'):
            found = shutil.which(name)
            if not found:
                raise RuntimeError('Required developer program missing: '+name)
            tools[name] = {'path':found,'version':run([found,'--version'],env=self.env,capture=True).stdout.splitlines()[0]}
        if not (self.qt/'lib/cmake/Qt6/Qt6Config.cmake').is_file():
            raise RuntimeError(f'Qt SDK missing: {self.qt}. Run bootstrap or pass --qt-root.')
        report = self.gpu()
        write_json(self.reports/'doctor.json', {'variant':self.variant,'python':sys.executable,
                   'qt':str(self.qt),'tools':tools,'gpu_sdk':report,
                   'gpu_hardware_tested':False})

    def configure(self):
        gpu = self.gpu()
        command = ['cmake','-S',SOURCE,'-B',self.build,'-G','Ninja',
            '-DCMAKE_BUILD_TYPE='+self.args.config,'-DCMAKE_PREFIX_PATH='+str(self.qt),
            '-DCMAKE_INSTALL_PREFIX='+str(self.stage),'-DCPACK_PACKAGE_DIRECTORY='+str(self.packages),
            '-DPython3_EXECUTABLE='+sys.executable,
            '-D'+PREFIX+'VARIANT='+self.variant['id'],
            '-D'+PREFIX+'DEPENDENCY_ROOT='+str(self.root/'deps'/self.args.config),
            '-D'+PREFIX+'TEST_ROOT='+str(self.root/'runs'/self.args.config),
            '-D'+PREFIX+'MODEL_DIR='+str(self.models),
            '-D'+PREFIX+'CORE_ONLY='+('ON' if self.args.core_only else 'OFF')]
        for key,value in gpu.get('cmakeCache',{}).items():
            command.append('-D'+key+'='+str(value))
        command += self.args.cmake_arg
        run(command,env=self.env)

    def build_all(self):
        if self.gpu_report.exists():
            self.env.update(read_json(self.gpu_report).get('environment',{}))
        run(['cmake','--build',self.build,'--config',self.args.config,'--parallel',self.args.jobs],env=self.env)

    def download_models(self):
        self.models.mkdir(parents=True,exist_ok=True)
        lock = read_json(SOURCE/'config/qctsm-models.lock.json')
        entries = lock['models'][0]['artifacts']
        for item in entries:
            destination = self.models/item['name']
            if destination.exists() and digest(destination) == item['sha256']:
                continue
            if destination.exists():
                raise RuntimeError('Existing model hash differs from lock: '+str(destination))
            for attempt in range(3):
                fd, tmp = tempfile.mkstemp(prefix='.qctsm-model-',dir=self.models)
                try:
                    print(f'Downloading {item["name"]}, attempt {attempt + 1}', flush=True)
                    with os.fdopen(fd,'wb') as stream, urllib.request.urlopen(item['url'],timeout=120) as response:
                        if response.status != 200:
                            raise RuntimeError(f'Model download requires HTTP 200, got {response.status}')
                        shutil.copyfileobj(response,stream,length=1024*1024)
                    actual_size, actual_hash = Path(tmp).stat().st_size, digest(tmp)
                    if actual_size != item['size'] or actual_hash != item['sha256']:
                        raise RuntimeError(f'Downloaded model identity mismatch: {item["name"]}, bytes={actual_size}, sha256={actual_hash}')
                    os.replace(tmp,destination)
                    break
                except (OSError, RuntimeError) as error:
                    if attempt == 2:
                        raise
                    print(f'{error}. Retrying the pinned URL.', flush=True)
                    time.sleep(2)
                finally:
                    Path(tmp).unlink(missing_ok=True)
        write_json(self.reports/'models.json', {'files':[{'name':x['name'],'sha256':digest(self.models/x['name'])} for x in entries]})

    def test(self):
        run(['ctest','--test-dir',self.build,'--build-config',self.args.config,'--output-on-failure',
             '--no-tests=error','--output-junit',self.reports/'ctest.xml'],env=self.env)
        import xml.etree.ElementTree as ET
        tree = ET.parse(self.reports/'ctest.xml')
        skipped = tree.findall('.//skipped')
        if skipped:
            raise RuntimeError(f'{len(skipped)} required CTest cases were skipped')

    def install(self):
        run(['cmake','--install',self.build,'--config',self.args.config,'--prefix',self.stage],env=self.env)

    def package(self):
        self.packages.mkdir(parents=True,exist_ok=True)
        nsis = self.root/'bootstrap/nsis'
        if os.name=='nt' and nsis.exists():
            candidates = list(nsis.rglob('makensis.exe'))
            if candidates:
                self.env['PATH'] = str(candidates[0].parent)+os.pathsep+self.env['PATH']
        if os.name == 'nt':
            command = [sys.executable,'-B',SOURCE/'tools/qctsm-package-windows.py','--build-dir',self.build,
                       '--package-dir',self.packages,'--config',self.args.config,'--variant',self.variant['id']]
            compiler = shutil.which('makensis.exe',path=self.env['PATH'])
            if compiler:
                command += ['--nsis',compiler]
            run(command,env=self.env)
            run(['cpack','--config',self.build/'CPackConfig.cmake','-C',self.args.config,'-B',self.packages,'-G','ZIP'],env=self.env)
        else:
            run(['cpack','--config',self.build/'CPackConfig.cmake','-C',self.args.config,'-B',self.packages],env=self.env)

    def verify(self, installers=False):
        command = [sys.executable,'-B',SOURCE/'tools/qctsm-verify-package.py','--root',self.stage,
                   '--model-dir',self.models,'--report',self.reports/('installers.json' if installers else 'package.json')]
        if installers:
            command += ['--installers-dir',self.packages]
        else:
            command += ['--packages-dir',self.packages]
        run(command,env=self.env)

    def products(self):
        spec = importlib.util.spec_from_file_location('qctsm_package_selection',SOURCE/'.github/scripts/qctsm-artifact-selection.py')
        selection = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(selection)
        files = sorted(p for p in self.packages.iterdir() if p.is_file() and
                       any(p.name.endswith(suffix) for suffix in ('.exe','.zip','.deb','.tar.gz','.dmg')))
        names = {p.name for p in files}
        if names not in selection.package_name_options(self.variant['id'],selection.project_version(SOURCE)):
            raise RuntimeError(f'Unexpected package set for {self.variant["id"]}: {sorted(names)}')
        found = [{'name':p.name,'path':str(p),'sha256':digest(p),'size':p.stat().st_size} for p in files]
        write_json(self.reports/'products.json',{'variant':self.variant['id'],'products':found})
        print(json.dumps({'products':found},ensure_ascii=False))
        return found

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage',choices=['bootstrap','doctor','configure','build','models','test','install','package','verify-package','installers','products','check'])
    parser.add_argument('--build-root',default=os.environ.get(PREFIX+'BUILD_ROOT',str(SOURCE.parent.parent/'github-repos-compilations/QCopilots-ToolSemanticMatching')))
    parser.add_argument('--variant')
    parser.add_argument('--qt-root')
    parser.add_argument('--sdk-root')
    parser.add_argument('--model-dir')
    parser.add_argument('--config',default='Release',choices=['Release','Debug','RelWithDebInfo'])
    parser.add_argument('--jobs',default=str(min(os.cpu_count() or 2,8)))
    parser.add_argument('--core-only',action='store_true')
    parser.add_argument('--cmake-arg',action='append',default=[])
    args=parser.parse_args()
    workflow=Workflow(args)
    actions={'bootstrap':workflow.bootstrap,'doctor':workflow.doctor,'configure':workflow.configure,
             'build':workflow.build_all,'models':workflow.download_models,'test':workflow.test,
             'install':workflow.install,'package':workflow.package,'verify-package':workflow.verify,
             'installers':lambda:workflow.verify(True),'products':workflow.products}
    for stage in (['doctor','configure','build','models','test','install','package','verify-package','installers','products'] if args.stage=='check' else [args.stage]):
        actions[stage]()
    return 0

if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (OSError,ValueError,KeyError,RuntimeError,subprocess.CalledProcessError) as error:
        print('qctsm developer workflow: '+str(error),file=sys.stderr)
        raise SystemExit(1)
