"""Print lines where a naive `$` regex and the tokenizer disagree (sanity check only).

Usage: python check_tokens2.py CORPUS NAME [LIMIT]
"""
import re
import sys

import analyze as A

if len(sys.argv) < 3:
    sys.exit(__doc__)
corpus, name = sys.argv[1], sys.argv[2]
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 15
R = A.Repo(name, "", corpus)
shown = 0
for f in R.files:
    if not f.endswith(".gd") or not R.first_party(f):
        continue
    src = R.read(f)
    info = A.parse_gd(src)
    tok_lines = {u["line"] for u in info["uses"] if u["kind"] == "dollar"}
    for i, l in enumerate(src.split("\n"), 1):
        code = l if ('"' in l or "'" in l) else l.split("#")[0]
        if re.search(r"\$[A-Za-z_\"%/']", code) and i not in tok_lines:
            print(f"{f}:{i}: {l.strip()[:120]}")
            shown += 1
            if shown >= limit:
                sys.exit()
