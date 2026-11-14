"""Exercise the embedded installer without elevation or driver changes (Windows only)."""
import base64
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "pc/streamer/src/audio/steam_devices.cpp"
source = SOURCE.read_text(encoding="utf-8")
script = re.search(r'LR"PS\((.*?)\)PS"', source, re.S).group(1)
# Keep the real PowerShell control flow, substitute only the native installer.
fake = r'''
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class SteamAudioInstaller {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern void SetLastError(uint code);
    public static bool DiInstallDriverW(IntPtr hwnd, string inf, uint flags, out bool reboot) {
        string name = Path.GetFileName(inf);
        File.AppendAllText(Environment.GetEnvironmentVariable("TEST_CALLS"), name + "\n");
        reboot = Environment.GetEnvironmentVariable("TEST_REBOOT") == name;
        if (Environment.GetEnvironmentVariable("TEST_FAIL") == name) {
            SetLastError(5);
            return false;
        }
        return true;
    }
}
'''
test_script = re.sub(r"(?s)(Add-Type -TypeDefinition @'\n).*?(\n'@)",
                     lambda m: m[1] + fake.strip() + m[2], script)
assert test_script != script
ps = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
speaker = "SteamStreamingSpeakers.inf"
mic = "SteamStreamingMicrophone.inf"
cases = [
    ("success", [speaker, mic], "", "", 0, [speaker, mic]),
    ("missing package", [], "", "", 2, []),
    ("missing microphone", [speaker], "", "", 2, []),
    ("speaker failure", [speaker, mic], speaker, "", 5, [speaker]),
    ("microphone failure", [speaker, mic], mic, "", 5, [speaker, mic]),
    ("speaker needs reboot", [speaker, mic], "", speaker, 3010, [speaker, mic]),
    ("microphone needs reboot", [speaker, mic], "", mic, 3010, [speaker, mic]),
]
with tempfile.TemporaryDirectory(prefix="rincast-steam-audio-") as tmp:
    base = Path(tmp)
    for label, files, fail, reboot, expected_code, expected_calls in cases:
        case = base / label
        drivers = case / "Steam/drivers/Windows10/x64"
        drivers.mkdir(parents=True)
        for name in files:
            (drivers / name).touch()
        calls = case / "calls.txt"
        env = dict(os.environ, TEST_CALLS=str(calls), TEST_FAIL=fail, TEST_REBOOT=reboot)
        env["CommonProgramFiles(x86)"] = str(case)
        encoded = base64.b64encode(test_script.encode("utf-16-le")).decode("ascii")
        result = subprocess.run([str(ps), "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
                                env=env, capture_output=True, timeout=30)
        actual_calls = calls.read_text().splitlines() if calls.exists() else []
        assert result.returncode == expected_code, (label, result.returncode, result.stderr)
        assert actual_calls == expected_calls, (label, actual_calls)
        print(f"PASS: {label}")
print("7 installer scenarios passed; no drivers installed.")
