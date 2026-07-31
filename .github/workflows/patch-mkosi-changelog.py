#!/usr/bin/env python3
"""Make mkosi's per-package changelog fetch (via `apt-get changelog`) non-fatal.

Debian's changelog mirror occasionally lags the package archive, causing
`apt-get changelog` to 404 for a package version that was just installed.
mkosi currently treats that as a hard build failure; this patches it to
record a placeholder instead, matching mkosi v26's mkosi/manifest.py.
"""

import sys

path = sys.argv[1]
src = open(path).read()

old = """                result = Apt.invoke(
                    self.context,
                    "changelog",
                    ["--quiet", "--quiet", "-o", "Dir=/buildroot", name],
                    stdout=subprocess.PIPE,
                )
                source_package = SourcePackageManifest(source, result.stdout.strip())"""

new = """                try:
                    result = Apt.invoke(
                        self.context,
                        "changelog",
                        ["--quiet", "--quiet", "-o", "Dir=/buildroot", name],
                        stdout=subprocess.PIPE,
                    )
                    changelog = result.stdout.strip()
                except subprocess.CalledProcessError:
                    changelog = "(changelog unavailable)"
                source_package = SourcePackageManifest(source, changelog)"""

assert old in src, "mkosi manifest.py layout changed, update this patch"
open(path, "w").write(src.replace(old, new))
