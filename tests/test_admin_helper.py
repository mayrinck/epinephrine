import importlib.util
from pathlib import Path
import unittest
import tempfile
from unittest.mock import patch
import io
spec=importlib.util.spec_from_file_location('admin',Path(__file__).resolve().parents[1]/'scripts/admin_helper.py')
admin=importlib.util.module_from_spec(spec);spec.loader.exec_module(admin)
class AdminTests(unittest.TestCase):
 def test_reject_shell(self):
  for cmd in ['bash /tmp/script','sh -c id','apt install -y steam; id','apt install -y $(id)','printf %s x > /etc/passwd','install -m 644 /tmp/x /etc/shadow','nvidia-smi -pl 80; reboot']:
   with self.subTest(cmd=cmd),self.assertRaises(ValueError):admin.plan(cmd)
 def test_expected_commands(self):
  for cmd in ['apt install -y gamemode','dmidecode -t memory 2>/dev/null','kbdrate -r 30 -d 250','setleds +caps 2>/dev/null','sysctl -w vm.swappiness=10 vm.vfs_cache_pressure=100','smbus enable',"install -D -m 644 '/tmp/epi-config' '/etc/gamemode.ini'",'for d in /sys/devices/system/cpu/cpu*/cpufreq; do printf \'%s\' \'performance\' > "$d/scaling_governor" 2>/dev/null; done']:
   with self.subTest(cmd=cmd): self.assertTrue(admin.plan(cmd))
 def test_untrusted_executable(self):
  with self.assertRaises((ValueError,FileNotFoundError)):admin.trusted_program('/tmp/not-installed')
 def test_atomic_config_and_symlink_refusal(self):
  with tempfile.TemporaryDirectory() as root:
   target=Path(root)/'config'; admin.write_config(target,b'first'); admin.write_config(target,b'second')
   self.assertEqual(target.read_bytes(),b'second')
   alias=Path(root)/'alias';alias.symlink_to(target)
   with self.assertRaises(ValueError):admin.write_config(alias,b'changed')
   self.assertEqual(target.read_bytes(),b'second')
 def test_protocol_never_executes_unrecognized_commands(self):
  incoming=io.StringIO('SYNC:1234ABCD bash /tmp/evil\nexit\n')
  output=io.StringIO()
  with patch.object(admin.sys,'stdin',incoming),patch.object(admin.sys,'stdout',output):admin.serve()
  self.assertIn('EXIT:1234ABCD:1',output.getvalue())
if __name__=='__main__':unittest.main()
