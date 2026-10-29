"""PlatformIO pre-build hook: embed web/index.html as src/web_ui.h (PROGMEM).
Edit web/index.html, not web_ui.h."""
import os
try:
    Import("env")  # noqa: F821
    root = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
src = os.path.join(root, "web", "index.html")
dst = os.path.join(root, "src", "web_ui.h")
import gzip
html = open(src, "rb").read()
gz = gzip.compress(html, 9, mtime=0)  # served with Content-Encoding: gzip
rows = [", ".join("0x%02x" % b for b in gz[i:i + 20]) for i in range(0, len(gz), 20)]
out = ("// Generated from web/index.html by tools/embed_web.py at build time. Don't edit.\n"
       "#pragma once\n#include <pgmspace.h>\n#include <stddef.h>\n"
       "static const unsigned char WEB_UI_GZ[] PROGMEM = {\n" + ",\n".join(rows) + "\n};\n"
       "static const size_t WEB_UI_GZ_LEN = %d;  // %d bytes before gzip\n" % (len(gz), len(html)))
if not os.path.exists(dst) or open(dst, encoding="utf-8").read() != out:
    open(dst, "w", encoding="utf-8").write(out)
    print("[planula] web_ui.h regenerated (%d -> %d bytes gzip)" % (len(html), len(gz)))
