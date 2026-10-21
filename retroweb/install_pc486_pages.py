#!/usr/bin/env python3
"""Install shared pc486 entry HTML under Doom/ and full/ (one source index.html)."""
import argparse
import os
import sys

# From /pc486 (no trailing slash) a relative "Doom/" resolves to /Doom/ at the
# site root. Build the target from pathname so both /pc486 and /pc486/ work.
REDIRECT = """\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>486 DX2-66 Gaming PC</title>
<script>
(function () {
  var p = location.pathname;
  var base = p.charAt(p.length - 1) === "/" ? p : p + "/";
  location.replace(base + "Doom/");
})();
</script>
</head>
<body>
<p><a id="go" href="Doom/">Doom</a></p>
<script>
(function () {
  var p = location.pathname;
  document.getElementById("go").href = (p.charAt(p.length - 1) === "/" ? p : p + "/") + "Doom/";
})();
</script>
</body>
</html>
"""

BASE_TAG = '<base href="../">'


def with_base(html: str) -> str:
    if "<base " in html.lower():
        return html
    lower = html.lower()
    i = lower.find("<head>")
    if i < 0:
        raise SystemExit("install_pc486_pages: no <head> in index.html")
    insert = i + len("<head>")
    return html[:insert] + "\n" + BASE_TAG + html[insert:]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dest", help="pc486 web root (local web/ or staged)")
    ap.add_argument(
        "--page",
        required=True,
        help="source machine HTML (always the editable index.html, never a redirect)",
    )
    ap.add_argument(
        "--redirect-root",
        action="store_true",
        help="write dest/index.html as a redirect to Doom/ (staging only)",
    )
    args = ap.parse_args()
    dest = os.path.abspath(args.dest)
    src = os.path.abspath(args.page)
    if not os.path.isfile(src):
        sys.exit(f"install_pc486_pages: missing {src}")
    html = open(src, encoding="utf-8").read()
    if "location.replace(base + \"Doom/\")" in html or len(html) < 2000:
        sys.exit(f"install_pc486_pages: --page looks like a redirect, not the machine HTML: {src}")
    page = with_base(html)
    for name in ("Doom", "full"):
        d = os.path.join(dest, name)
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, "index.html"), "w", encoding="utf-8").write(page)
    if args.redirect_root:
        open(os.path.join(dest, "index.html"), "w", encoding="utf-8").write(REDIRECT)


if __name__ == "__main__":
    main()
