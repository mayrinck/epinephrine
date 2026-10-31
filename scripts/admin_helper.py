"""Restricted administrative protocol. Never interpret caller text as shell code."""
import glob
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import sys
import tempfile

SAFE_PATH = '/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/sbin:/usr/local/bin'
CONFIGS = {'/etc/sysctl.conf', '/etc/gamemode.ini', '/etc/gamescope/epinephrine.conf'}
SYSFS_NAMES = {'power_dpm_force_performance_level', 'power_dpm_state', 'pp_power_profile_mode', 'power1_cap', 'pwm1', 'pwm1_enable', 'temp1_target', 'fan_zero_rpm_enable', 'fan_zero_rpm_stop_temperature'}


def plan(command):
    """Parse the legacy call sites into a closed set of operations."""
    if command in ('smbus enable', 'smbus disable'):
        return ('smbus', command.endswith('enable'))
    match = re.fullmatch(r"for d in /sys/devices/system/cpu/cpu\*/cpufreq; do printf '%[sd]' '?([a-zA-Z0-9_-]+)'? > \"\$d/(scaling_governor|scaling_min_freq|scaling_max_freq)\" 2>/dev/null; done", command)
    if match:
        return ('cpu', match[2], match[1])
    args = shlex.split(command)
    if args and args[-1] == '2>/dev/null':
        args.pop()
    if args[:3] == ['mkdir', '-p', '/etc/gamescope'] and args[3:4] == ['&&']:
        args = args[4:]
    if args[:2] == ['printf', '%s'] and len(args) == 5 and args[3] == '>':
        if not re.fullmatch(r'[a-zA-Z0-9_ .+-]{1,64}', args[2]):
            raise ValueError('Invalid device value')
        path = Path(args[4]).resolve()
        if not str(path).startswith('/sys/devices/') or path.name not in SYSFS_NAMES:
            raise ValueError('Device attribute not allowed')
        return ('write', str(path), args[2])
    if args and args[0] == 'install':
        if args[1:3] == ['-D', '-m']: args.pop(1)
        if len(args) == 5 and args[1] == '-m' and args[2] in ('644', '0644') and args[4] in CONFIGS:
            return ('config', args[3], args[4])
    package_prefixes = [('apt','install','-y'), ('dnf','install','-y'), ('pacman','-S','--noconfirm'), ('zypper','install','-y'), ('xbps-install','-y')]
    for prefix in package_prefixes:
        if tuple(args[:len(prefix)]) == prefix and len(args) > len(prefix) and all(re.fullmatch(r'[a-z0-9][a-z0-9+.-]*', a) for a in args[len(prefix):]):
            return ('exec', args)
    if re.fullmatch(r'kbdrate -r \d{1,3} -d \d{1,4}', command): return ('exec', args)
    if re.fullmatch(r'setleds [+-](?:num|caps|scroll) 2>/dev/null', command): return ('exec', args)
    if re.fullmatch(r'nvidia-smi -g \d+ -f (?:-1|\d{1,3})', command): return ('exec', args)
    if command in ('nvidia-smi -rgc', 'nvidia-smi -rmc', 'nvidia-smi -rac'): return ('exec', args)
    if re.fullmatch(r'nvidia-smi (?:-(?:i|pl|lgc|lmc|pm|ac|rgc|rmc|rac) [0-9,]+ ?)+', command): return ('exec', args)
    if re.fullmatch(r'sysctl -w vm.swappiness=\d{1,3} vm.vfs_cache_pressure=\d{1,4}', command): return ('exec', args)
    if args == ['dmidecode','-t','memory']: return ('exec', args)
    if command == "dmidecode -t 0 2>/dev/null | grep -i 'rom.*size' | sed 's/.*://;s/^[[:space:]]*//' | head -1": return ('bios',)
    if args and Path(args[0]).name == 'epi-hidraw' and len(args) >= 3:
        if re.fullmatch(r'/dev/hidraw\d+',args[1]) and args[2] in ('get-feature','set-feature','write') and all(re.fullmatch(r'[0-9a-fA-F]+',a) for a in args[3:]):
            return ('hid', ['epi-hidraw', *args[1:]])
    raise ValueError('Administrative operation not allowed')


def trusted_program(program):
    import shutil
    path = Path(shutil.which(program, path=SAFE_PATH) or program).resolve()
    for item in [path, *path.parents]:
        info = item.stat()
        if info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError('Administrative executable must be installed and root-owned')
    if not path.is_file(): raise ValueError('Executable not found')
    return str(path)


def run(args):
    args = [trusted_program(args[0]), *args[1:]]
    result = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env={'PATH': SAFE_PATH, 'LC_ALL':'C'}, timeout=600, check=False)
    return result.returncode, result.stdout.decode(errors='replace')[:1024*1024]


def write_config(path, contents):
    target = Path(path)
    if target.is_symlink() or target.parent.is_symlink(): raise ValueError('Symlink destination refused')
    if target.exists() and not target.is_file(): raise ValueError('Invalid destination')
    target.parent.mkdir(parents=True, exist_ok=True)
    mode = stat.S_IMODE(target.stat().st_mode) if target.exists() else 0o644
    fd, temporary = tempfile.mkstemp(prefix='.epinephrine-', dir=target.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            os.fchmod(stream.fileno(), mode)
            stream.write(contents)
            stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)


def execute(operation):
    kind, *values = operation
    if kind in ('exec','hid'): return run(values[0])
    if kind == 'bios':
        code, output = run(['dmidecode','-t','0'])
        return code, next((line.split(':',1)[-1].strip() for line in output.splitlines() if re.search('rom.*size', line, re.I)), '')
    if kind == 'cpu':
        attribute, value = values
        paths = glob.glob('/sys/devices/system/cpu/cpu[0-9]*/cpufreq/'+attribute)
        if not paths: raise ValueError('CPU frequency control unavailable')
        for path in paths:
            if attribute == 'scaling_governor':
                available = Path(path).with_name('scaling_available_governors').read_text().split()
                if value not in available: raise ValueError('Governor unavailable')
            elif not value.isdigit(): raise ValueError('Invalid frequency')
            Path(path).write_text(value)
        return 0, ''
    if kind == 'write':
        Path(values[0]).write_text(values[1]); return 0, ''
    if kind == 'config':
        source, dest = values
        fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd,'rb') as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid != int(os.environ.get('PKEXEC_UID','-1')) or info.st_size > 1024*1024:
                raise ValueError('Invalid configuration source')
            contents = stream.read(1024*1024+1)
        if dest == '/etc/sysctl.conf':
            text = contents.decode()
            original = Path(dest).read_text() if Path(dest).exists() else ''
            for key, limit in [('vm.swappiness', 200), ('vm.vfs_cache_pressure', 10000)]:
                match = re.search(r'^\s*'+re.escape(key)+r'\s*=\s*(\d+)\s*$', text, re.M)
                if not match or int(match[1]) > limit: raise ValueError('Invalid memory setting')
                line = key+'='+match[1]
                original, count = re.subn(r'^\s*'+re.escape(key)+r'\s*=.*$', line, original, flags=re.M)
                if not count: original += '\n'+line+'\n'
            contents = original.encode()
        write_config(dest, contents)
        return 0, ''
    if kind == 'smbus':
        path = Path('/etc/default/grub')
        original = path.read_text()
        if path.is_symlink(): raise ValueError('Symlink GRUB configuration refused')
        content = re.sub(r'\s*acpi_enforce_resources=lax', '', original)
        if values[0]:
            content, count = re.subn(r'^(GRUB_CMDLINE_LINUX_DEFAULT=")(.*)(")$', r'\1\2 acpi_enforce_resources=lax\3', content, flags=re.M)
            if not count: content += '\nGRUB_CMDLINE_LINUX_DEFAULT="quiet splash acpi_enforce_resources=lax"\n'
        write_config(path, content.encode())
        try:
            import shutil
            if shutil.which('update-grub', path=SAFE_PATH): result = run(['update-grub'])
            elif shutil.which('grub2-mkconfig',path=SAFE_PATH): result = run(['grub2-mkconfig','-o','/boot/grub2/grub.cfg'])
            else: result = run(['grub-mkconfig','-o','/boot/grub/grub.cfg'])
            if result[0]: write_config(path, original.encode())
            return result
        except Exception:
            write_config(path, original.encode()); raise
    raise ValueError('Unknown operation')


def serve():
    for line in sys.stdin:
        line = line.rstrip('\n')
        if line == 'exit': break
        sync = line.startswith('SYNC:')
        tag = 'invalid'
        try:
            if sync:
                tag, command = line[5:].split(' ',1)
                if not re.fullmatch(r'[0-9A-Fa-f]{8}', tag): raise ValueError('Invalid request tag')
            elif line.startswith('ASYNC:'): command = line[6:]
            else: continue
            if len(command) > 65536: raise ValueError('Request too large')
            code, output = execute(plan(command))
        except Exception as error:
            code, output = 1, str(error)
        if sync:
            print(output, flush=True)
            print('EXIT:'+tag+':'+str(code), flush=True)
        elif code:
            print(output, file=sys.stderr, flush=True)


if __name__ == '__main__':
    if os.geteuid() != 0: raise SystemExit('Administrative helper requires root')
    serve()
