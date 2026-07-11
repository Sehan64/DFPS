#!/usr/bin/env python3
"""Zip a module source directory into an AxManager/KernelSU installer.

Usage: zip_module.py <srcdir> <out.zip>

Files keep their on-disk mode; lifecycle scripts (*.sh) and anything under
system/bin/ are forced to 0755, data files to 0644, so the installer finds
executable scripts at the archive root.
"""
import os
import sys
import zipfile


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: zip_module.py <srcdir> <out.zip>")
    src, out = sys.argv[1], sys.argv[2]
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _dirs, files in os.walk(src):
            for name in sorted(files):
                full = os.path.join(root, name)
                rel = os.path.relpath(full, src)
                info = zipfile.ZipInfo(rel)
                base = os.path.basename(rel)
                mode = 0o755 if (rel.startswith("system/bin/") or base.endswith(".sh")) else 0o644
                info.external_attr = mode << 16
                with open(full, "rb") as fh:
                    z.writestr(info, fh.read())
    print("wrote", out)


if __name__ == "__main__":
    main()
