import json, sys
from pathlib import Path
d = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
print("== " + Path(sys.argv[1]).stem)
bg = d.get("background"); pal = d.get("palette") or []
if bg: print("  bg " + str(bg))
if pal: print("  palette " + ", ".join((p.get("color","?") + " " + str(round(float(p.get("share",0))*100)) + "%") if isinstance(p, dict) else str(p) for p in pal[:5]))
lay = d.get("layout") or d.get("regions") or []
if isinstance(lay, dict): lay = lay.get("blocks") or []
rows = []
for it in lay[:14]:
    if isinstance(it, dict):
        b = it.get("box") or it.get("bbox")
        rows.append("    " + str(it.get("type","?")) + " " + str(b) + " " + str(it.get("fill","")) )
print("\n".join(rows))
tx = d.get("texts") or d.get("text") or []
if isinstance(tx, dict): tx = tx.get("items") or []
for t in tx[:12]:
    if isinstance(t, dict):
        print("    txt " + str(t.get("text",""))[:44] + " | " + str(t.get("box")) + " | " + str(t.get("size_px") or t.get("size")) )
iss = d.get("issues") or []
print("  issues " + str(len(iss)))
for i in iss[:8]:
    print("    ! " + (i.get("kind","") if isinstance(i,dict) else str(i)) + " " + str(i.get("detail","") if isinstance(i,dict) else "")[:90])
