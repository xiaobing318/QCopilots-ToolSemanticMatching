#!/usr/bin/env python3
"""Run installed programs and real text embeddings from relocated user packages.

All profiles and installation roots are unique. A missing GPU is recorded as
unavailable hardware, never as a passed GPU inference test.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import queue
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import zipfile

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
PROGRAMS = ('qctsm-gui', 'qctsm-service', 'qctsm-worker', 'qctsm-cli')
WINDOWS_PRODUCT_KEY = r'Software\QCopilots\qcopilots-tool-semantic-matching'
WINDOWS_UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\qcopilots-tool-semantic-matching'
WINDOWS_MENU = 'QCopilots Tool Semantic Matching'


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def external(path):
    path = Path(path).expanduser().resolve()
    if path == SOURCE or SOURCE in path.parents or path == Path(path.anchor):
        raise RuntimeError(f'Expected external dedicated path: {path}')
    return path


def run(command, *, env=None, timeout=180):
    print('+ ' + subprocess.list2cmdline([str(item) for item in command]), flush=True)
    result = subprocess.run([str(item) for item in command], env=env, timeout=timeout,
                            check=False, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {result.stdout[-4000:]}\n{result.stderr[-4000:]}')
    return result


def resolve_tree(root):
    suffix = '.exe' if os.name == 'nt' else ''
    found = list(root.rglob('qctsm-cli' + suffix))
    found = [item for item in found if item.is_file() and not item.is_symlink()]
    if len(found) != 1:
        raise RuntimeError(f'Expected one installed CLI under {root}, found {found}')
    binary = found[0].parent
    for name in PROGRAMS:
        candidate = binary / (name + suffix)
        if not candidate.is_file() or candidate.is_symlink() or root.resolve() not in candidate.resolve().parents:
            raise RuntimeError(f'Missing or redirected installed program: {candidate}')
    identities = list(root.rglob('build-identity.json'))
    if len(identities) != 1:
        raise RuntimeError('Distribution must contain one build-identity.json')
    return binary, json.loads(identities[0].read_text(encoding='utf-8'))


def clean_environment(profile, binary):
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('QT_', 'QML_', 'GGML_', 'HIP_', 'ROCM_', 'CUDA_')) or key in ('LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'PYTHONPATH'):
            env.pop(key, None)
    env.update(QT_QPA_PLATFORM='offscreen', XDG_CONFIG_HOME=str(profile/'config'),
               XDG_DATA_HOME=str(profile/'data'), XDG_CACHE_HOME=str(profile/'cache'),
               APPDATA=str(profile/'roaming'), LOCALAPPDATA=str(profile/'local'),
               QCOPILOTS_TOOL_SEMANTIC_MATCHING_TEST_ROOT=str(profile/'tests'))
    if os.name == 'nt':
        windows = Path(os.environ.get('SystemRoot', r'C:\Windows'))
        env['PATH'] = os.pathsep.join([str(binary), str(windows/'System32'), str(windows)])
    else:
        env['PATH'] = os.pathsep.join([str(binary), '/usr/bin', '/bin', '/usr/sbin', '/sbin'])
    return env


def worker_request(program, models, env, request, *, backend='cpu'):
    process = subprocess.Popen([str(program), '--model-dir', str(models), '--backend', backend],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env=env, text=True, encoding='utf-8', errors='replace')
    lines = queue.Queue()
    errors = []
    def read_stdout():
        for line in process.stdout:
            lines.put(line)
        lines.put(None)
    def read_stderr():
        for line in process.stderr:
            errors.append(line)
            if len(errors) > 100:
                del errors[:50]
    stdout_thread = threading.Thread(target=read_stdout, daemon=True)
    stderr_thread = threading.Thread(target=read_stderr, daemon=True)
    stdout_thread.start()
    stderr_thread.start()
    try:
        process.stdin.write(json.dumps(request, ensure_ascii=False) + '\n')
        process.stdin.flush()
        deadline = time.monotonic() + 180
        while True:
            line = lines.get(timeout=max(0.1, deadline-time.monotonic()))
            if line is None:
                raise RuntimeError('Worker exited before replying: ' + ''.join(errors))
            result = json.loads(line)
            if result.get('id') == request['id']:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('Worker response timeout')
        process.stdin.write(json.dumps({'id':'package-quit', 'operation':'quit'}) + '\n')
        process.stdin.flush()
        process.wait(timeout=30)
        if process.returncode:
            raise RuntimeError('Worker quit failed: ' + ''.join(errors))
        return result
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        for stream in (process.stdin, process.stdout, process.stderr):
            if stream:
                stream.close()
        stdout_thread.join(timeout=2)
        stderr_thread.join(timeout=2)


def smoke(root, models, work, expected_commit=None):
    binary, identity = resolve_tree(root)
    if expected_commit and (identity.get('candidateCommit') != expected_commit or identity.get('workingTreeModified') is not False):
        raise RuntimeError('Installed candidate identity differs from required clean commit')
    if os.environ.get('GITHUB_ACTIONS') == 'true':
        if (identity.get('runId') != os.environ.get('GITHUB_RUN_ID') or
                str(identity.get('attempt')) != os.environ.get('GITHUB_RUN_ATTEMPT') or
                identity.get('variant') != os.environ.get('QCOPILOTS_TOOL_SEMANTIC_MATCHING_VARIANT')):
            raise RuntimeError('Installed package run, attempt, or variant differs from this execution')
    suffix = '.exe' if os.name == 'nt' else ''
    gpu_payload = verify_gpu_payload(root, binary, identity)
    initial = {name:digest(binary/(name+suffix)) for name in PROGRAMS}
    env = clean_environment(work, binary)
    for name in PROGRAMS:
        run([binary/(name+suffix), '--version'], env=env, timeout=30)
    run([binary/('qctsm-gui'+suffix), '--smoke-test'], env=env, timeout=30)
    run([binary/('qctsm-service'+suffix), '--smoke-test', '--profile', work/'service'], env=env, timeout=30)
    result = worker_request(binary/('qctsm-worker'+suffix), models, env,
        {'id':'package-embedding', 'operation':'embed', 'input':[
            'task: search result | query: 道路缓冲区',
            'title: Buffer | text: Creates buffer polygons around input vector features.']})
    if result.get('status') != 'succeeded' or result.get('dimensions') != 768:
        raise RuntimeError('Real model inference failed: ' + json.dumps(result, ensure_ascii=False))
    vectors = result.get('data', [])
    if len(vectors) != 2:
        raise RuntimeError('Worker did not preserve batch count')
    for vector in vectors:
        if len(vector) != 768 or not all(isinstance(x, (float, int)) and math.isfinite(x) for x in vector):
            raise RuntimeError('Worker returned invalid real embedding')
        if abs(math.sqrt(sum(x*x for x in vector))-1) > 0.002:
            raise RuntimeError('Embedding is not L2 normalized')
    if {name:digest(binary/(name+suffix)) for name in PROGRAMS} != initial:
        raise RuntimeError('Installed programs changed during verification')
    runtime = result.get('runtime', {})
    if runtime.get('backend') not in ('cpu', 'CPU'):
        raise RuntimeError('CPU smoke did not confirm actual CPU execution')
    # Query backend enumeration separately. GPU availability is evidence, not a
    # requirement imposed on ordinary hosted CPU-only runners.
    devices = worker_request(binary/('qctsm-worker'+suffix), models, env,
                             {'id':'package-devices', 'operation':'devices'},backend='auto')
    if devices.get('status') != 'succeeded':
        raise RuntimeError('Installed backend discovery failed: '+json.dumps(devices))
    if devices.get('gpu_load_error_kind') == 'package':
        raise RuntimeError('Installed GPU package is invalid: '+json.dumps(devices))
    return {'passed':True, 'identity':identity, 'programSha256':initial,
            'realModel':{'passed':True, 'dimensions':768, 'count':2, 'runtime':runtime},
            'deviceInventory':devices, 'gpuPayload':gpu_payload, 'gpuInference':{'executed':False,
                'reason':'Package smoke explicitly uses CPU. This report provides no GPU inference acceptance evidence.'}}


def safe_member(name):
    path = PurePosixPath(name.replace('\\', '/'))
    if path.is_absolute() or '..' in path.parts or any(':' in x for x in path.parts):
        raise RuntimeError(f'Unsafe archive path: {name}')
    return path


def verify_gpu_payload(root, binary, identity):
    backend = identity.get('backend')
    if backend == 'cpu':
        return {'backend':'cpu','gpuPayloadRequired':False}
    name = ('ggml-'+backend+'.dll') if os.name == 'nt' else ('libggml-'+backend+'.so')
    modules = [path for path in root.rglob(name) if path.is_file()]
    if not modules:
        raise RuntimeError('GPU package does not contain its selected backend: '+name)
    if backend == 'metal':
        return {'backend':backend,'gpuPayloadRequired':True,'moduleSha256':digest(modules[0]),
                'systemRuntime':'Metal framework, verified by macOS bundle dependency inspection'}
    report_path = binary/'qctsm-gpu-runtime.json'
    manifest = json.loads(report_path.read_text(encoding='utf-8'))
    if (manifest.get('variant') != identity.get('variant') or manifest.get('backend') != backend or
            manifest.get('verification',{}).get('dependencyClosure') != 'verified'):
        raise RuntimeError('GPU runtime closure evidence does not match the package variant')
    prefix = binary.parent.resolve()
    entries = manifest.get('files',[])
    if not entries or not any(entry.get('role') == 'backend' for entry in entries):
        raise RuntimeError('GPU closure omits the actual backend module')
    seen = set()
    for entry in entries:
        relative = safe_member(entry['path'])
        target = prefix/relative
        if str(relative) in seen or prefix not in target.resolve().parents:
            raise RuntimeError('GPU closure has a duplicate or redirected runtime file')
        seen.add(str(relative))
        if not target.is_file() or target.stat().st_size != entry['size'] or digest(target) != entry['sha256']:
            raise RuntimeError('Installed GPU runtime is missing or changed: '+str(relative))
    resources = manifest.get('resources',[])
    if not resources:
        raise RuntimeError('GPU package has no original runtime license resources')
    for resource in resources:
        for entry in resource['files']:
            target = binary/safe_member(entry['source'])
            if binary.resolve() not in target.resolve().parents or digest(target) != entry['sha256']:
                raise RuntimeError('GPU runtime license is missing or changed')
    return {'backend':backend,'gpuPayloadRequired':True,'dependencyClosure':'verified',
            'verifiedRuntimeFiles':len(entries),'verifiedLicenseResources':len(resources)}


def windows_registry_installations():
    import winreg
    found = []
    for hive_name,hive in (('HKCU',winreg.HKEY_CURRENT_USER),('HKLM',winreg.HKEY_LOCAL_MACHINE)):
        for view in (winreg.KEY_WOW64_64KEY,winreg.KEY_WOW64_32KEY):
            for key in (WINDOWS_PRODUCT_KEY,WINDOWS_UNINSTALL_KEY):
                try:
                    with winreg.OpenKey(hive,key,0,winreg.KEY_READ|view) as handle:
                        value_name = '' if key == WINDOWS_PRODUCT_KEY else 'InstallLocation'
                        try:
                            location = winreg.QueryValueEx(handle,value_name)[0]
                        except FileNotFoundError:
                            location = None
                        found.append({'hive':hive_name,'key':key,'location':location})
                except FileNotFoundError:
                    continue
    return found


def require_no_windows_installation():
    found = windows_registry_installations()
    menus = [Path(os.environ[name])/'Microsoft/Windows/Start Menu/Programs'/WINDOWS_MENU
             for name in ('APPDATA','ProgramData') if os.environ.get(name)]
    default = Path(os.environ.get('LOCALAPPDATA',str(Path.cwd())))/'Programs/qctsm'
    if found or default.exists() or any(path.exists() for path in menus):
        raise RuntimeError('Installer verification refuses to overwrite an existing QCTSM installation, registry key, or Start menu directory')


def remove_owned_windows_installation(install_root):
    for entry in windows_registry_installations():
        location = entry['location']
        if entry['hive'] != 'HKCU' or not isinstance(location,str) or Path(location).resolve() != install_root.resolve():
            raise RuntimeError('Installation ownership changed. Refusing to run an uninstaller against another registration')
    uninstallers = list(install_root.glob('*ninstall*.exe')) if install_root.exists() else []
    if len(uninstallers) > 1:
        raise RuntimeError('Installer produced ambiguous uninstallers')
    if uninstallers:
        run([uninstallers[0],'/S','_?='+str(install_root)],timeout=120)
    if any(install_root.rglob('qctsm-cli.exe')) or windows_registry_installations():
        raise RuntimeError('Uninstaller left application files or product registration')


def extract(package, destination):
    if package.name.endswith('.zip'):
        with zipfile.ZipFile(package) as archive:
            for member in archive.infolist():
                safe_member(member.filename)
                if (member.external_attr >> 16) & 0o170000 == 0o120000:
                    raise RuntimeError('ZIP symlinks are not accepted')
            archive.extractall(destination)
    elif package.name.endswith(('.tar.gz', '.tgz')):
        with tarfile.open(package, 'r:gz') as archive:
            for member in archive.getmembers():
                safe_member(member.name)
                if member.issym() or member.islnk():
                    target = PurePosixPath(member.name).parent / member.linkname
                    # Data filter below additionally validates resolved targets.
                    if PurePosixPath(member.linkname).is_absolute():
                        raise RuntimeError('Absolute archive link')
            archive.extractall(destination, filter='data')
    else:
        raise RuntimeError(f'Unsupported portable package: {package.name}')


def installer_checks(directory, models, scratch, expected_commit):
    records = []
    if os.name == 'nt':
        require_no_windows_installation()
        installers = sorted(directory.glob('qctsm-*.exe'))
        setups = sorted(directory.glob('qctsm-*-setup.zip'))
        if len(installers)+len(setups) != 1:
            raise RuntimeError('Expected exactly one NSIS installer or verified external-payload setup')
        installer = installers[0] if installers else None
        if setups:
            external_payload = scratch/'setup-payload'
            extract(setups[0], external_payload)
            installer = external_payload/'setup.exe'
        install_root = scratch/'installed'
        package = (installers or setups)[0]
        package_digest = digest(package)
        try:
            run([installer, '/S', '/D='+str(install_root)], timeout=180)
            report = smoke(install_root, models, scratch/'installed-profile', expected_commit)
            if digest(package) != package_digest:
                raise RuntimeError('Installer package changed during verification')
            records.append({'package':package.name,'sha256':package_digest,**report})
        finally:
            remove_owned_windows_installation(install_root)
    elif sys.platform.startswith('linux'):
        packages = list(directory.glob('qctsm-*.deb'))
        if len(packages) != 1:
            raise RuntimeError('Expected exactly one DEB')
        package_name = run(['dpkg-deb', '-f', packages[0], 'Package']).stdout.strip()
        package_version = run(['dpkg-deb', '-f', packages[0], 'Version']).stdout.strip()
        if package_name != 'qctsm':
            raise RuntimeError('Unexpected DEB package name')
        prior = subprocess.run(['dpkg-query', '-W', '-f=${Status}', package_name], capture_output=True, text=True)
        if prior.returncode == 0 or Path('/opt/qctsm').exists():
            raise RuntimeError('Refusing to replace an existing user installation during verification')
        sudo = [] if os.geteuid() == 0 else ['sudo', '-n']
        package_digest = digest(packages[0])
        try:
            run([*sudo, 'dpkg', '-i', packages[0]], timeout=180)
            report = smoke(Path('/opt/qctsm'), models, scratch/'installed-profile', expected_commit)
            if digest(packages[0]) != package_digest:
                raise RuntimeError('DEB changed during verification')
            records.append({'package':packages[0].name,'sha256':package_digest,**report})
        finally:
            installed = subprocess.run(['dpkg-query','-W','-f=${Version}',package_name],capture_output=True,text=True)
            if installed.returncode == 0:
                if installed.stdout.strip() != package_version:
                    raise RuntimeError('Package ownership changed. Refusing to remove a different installed version')
                run([*sudo, 'dpkg', '-r', package_name], timeout=120)
            if Path('/opt/qctsm/bin/qctsm-cli').exists():
                raise RuntimeError('DEB removal left application executable')
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path)
    parser.add_argument('--package', type=Path)
    parser.add_argument('--packages-dir', type=Path)
    parser.add_argument('--installers-dir', type=Path)
    parser.add_argument('--model-dir', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--expected-commit', default=os.environ.get('QCOPILOTS_TOOL_SEMANTIC_MATCHING_CANDIDATE_HEAD'))
    args = parser.parse_args()
    report_path = external(args.report)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    result = {'passed':False, 'checks':[]}
    try:
        with tempfile.TemporaryDirectory(prefix='qctsm-package-', dir=report_path.parent) as temporary:
            scratch = Path(temporary)
            if args.root:
                result['checks'].append({'kind':'installed-stage', **smoke(external(args.root), args.model_dir, scratch/'stage-profile', args.expected_commit)})
            packages = [args.package] if args.package else []
            if args.packages_dir:
                packages += [item for item in args.packages_dir.iterdir()
                             if item.name.endswith(('.zip','.tar.gz','.dmg')) and not item.name.endswith('-setup.zip')]
            for index, package in enumerate(packages):
                original_digest = digest(package)
                target = scratch/str(index)
                target.mkdir()
                if package.suffix == '.dmg':
                    mount = target/'mount'
                    run(['hdiutil','attach','-nobrowse','-readonly','-mountpoint',mount,package])
                    try:
                        apps = list(mount.glob('*.app'))
                        if len(apps) != 1:
                            raise RuntimeError('DMG must contain one application bundle')
                        shutil.copytree(apps[0],target/apps[0].name,symlinks=True)
                    finally:
                        run(['hdiutil','detach',mount])
                else:
                    extract(package,target)
                result['checks'].append({'kind':'portable-package','package':package.name,
                    'sha256':original_digest,**smoke(target,args.model_dir,scratch/f'profile-{index}',args.expected_commit)})
                if digest(package) != original_digest:
                    raise RuntimeError('Package changed during verification')
            if args.installers_dir:
                result['installers'] = installer_checks(args.installers_dir,args.model_dir,scratch,args.expected_commit)
            if not result['checks'] and not result.get('installers'):
                raise RuntimeError('No package verification was requested')
            result['passed'] = True
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        report_path.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(result,ensure_ascii=False).replace('::',r'\u003a\u003a'))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print('qctsm package verification: '+str(error),file=sys.stderr)
        raise SystemExit(1)
