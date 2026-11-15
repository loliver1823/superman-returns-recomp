import os, re, collections
d = r"tools/regen/ppc_out"
ctxs = collections.Counter()
ex = {}
pat = re.compile(r"(.{0,28})\(base \+ ")
bare = re.compile(r"(?<!\()base \+ ")
for name in os.listdir(d):
    if not (name.startswith("ppc_recomp.") and name.endswith(".cpp")):
        continue
    for ln in open(os.path.join(d, name), encoding="utf-8", errors="replace"):
        for m in pat.finditer(ln):
            key = m.group(1).strip()
            ctxs["PAREN::" + key] += 1
            ex.setdefault("PAREN::" + key, ln.strip()[:170])
        for m in bare.finditer(ln):
            ctxs["BARE"] += 1
            ex.setdefault("BARE", ln.strip()[:170])
for k, v in ctxs.most_common(40):
    print(f"{v:8d}  {k}")
    print(f"          {ex[k]}")
