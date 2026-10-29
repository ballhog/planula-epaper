"""PlatformIO pre-build hook: embed a git-derived firmware version.

Injects two macros the OTA client uses:
  FW_VERSION  — human display string, e.g. "ef37ecf" or "v1.2-3-gef37ecf-dirty"
  FW_BUILD    — monotonic int (git commit count) for "is the manifest newer?" compares

Falls back to "unknown"/0 if git isn't available on the build host.
"""
import subprocess
Import("env")  # noqa: F821  (PlatformIO injects this)


def _git(args, default):
    try:
        return subprocess.check_output(
            ["git"] + args, cwd=env["PROJECT_DIR"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        return default


desc  = _git(["describe", "--tags", "--always", "--dirty"], "unknown")
build = _git(["rev-list", "--count", "HEAD"], "0")
print(f"[planula-epaper] FW_VERSION={desc}  FW_BUILD={build}")

env.Append(BUILD_FLAGS=[f'-DFW_VERSION=\\"{desc}\\"', f'-DFW_BUILD={build}'])
