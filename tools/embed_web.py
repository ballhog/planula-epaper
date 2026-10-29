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
html = open(src, encoding="utf-8").read()
assert ')WEBUI"' not in html
out = ("// Generated from web/index.html by tools/embed_web.py at build time. Don't edit.\n"
       "#pragma once\n#include <pgmspace.h>\n"
       'static const char WEB_UI[] PROGMEM = R"WEBUI(' + html + ')WEBUI";\n')
if not os.path.exists(dst) or open(dst, encoding="utf-8").read() != out:
    open(dst, "w", encoding="utf-8").write(out)
    print("[planula] web_ui.h regenerated (%d bytes)" % len(html))
