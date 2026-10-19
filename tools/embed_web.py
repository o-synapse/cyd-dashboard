"""PlatformIO pre-build step: gzip web/index.html into src/web_index.h."""
import gzip
import os

Import("env")  # noqa: F821  (provided by PlatformIO)

proj = env.subst("$PROJECT_DIR")  # noqa: F821
src = os.path.join(proj, "web", "index.html")
dst = os.path.join(proj, "src", "web_index.h")

data = gzip.compress(open(src, "rb").read(), 9, mtime=0)
body = ",".join(str(b) for b in data)
out = (
    "// Generated from web/index.html by tools/embed_web.py - do not edit\n#pragma once\n#include <pgmspace.h>\n"
    f"const size_t WEB_INDEX_GZ_LEN = {len(data)};\n"
    f"const uint8_t WEB_INDEX_GZ[] PROGMEM = {{{body}}};\n"
)
if not os.path.exists(dst) or open(dst).read() != out:
    open(dst, "w").write(out)
    print(f"Embedded web UI ({len(data)} bytes gzipped)")
