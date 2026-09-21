import importlib.util, os, sys
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("sfh", os.path.join(here, "strip_file_headers.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
for rel in sys.argv[1:]:
    style = "c" if os.path.splitext(rel)[1].lower() in m.C_EXTS else "hash"
    raw = open(rel, "rb").read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig" if bom else "utf-8")
    new, meta = m.transform(text, style)
    print("=" * 62)
    print(rel, "| header_lines=%d guard=%r bom=%s" % (meta["header_lines"], meta["guard"], bom))
    print("-" * 62)
    print("\n".join(new.split("\n")[:14]))
