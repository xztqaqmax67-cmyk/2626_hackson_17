"""Make a source-only package; explicitly exclude evaluations, data and builds."""
import argparse
from pathlib import Path
import zipfile

p=argparse.ArgumentParser()
p.add_argument("--team",default="dag_chain_solver_v8")
p.add_argument("--out",type=Path,default=Path(__file__).resolve().parents[1]/"dist")
a=p.parse_args()
if not a.team or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in a.team):
    p.error("team must use ASCII letters, digits, underscore or hyphen")
root=Path(__file__).resolve().parents[1]
a.out.mkdir(parents=True,exist_ok=True)
files=[root/"CMakeLists.txt",root/"README.md",root/"SEARCH_LIMITS_zh.md",root/"CHANGES_v8_zh.md",*sorted((root/"src").glob("*"))]
target=a.out/(a.team+".zip")
with zipfile.ZipFile(target,"w",zipfile.ZIP_DEFLATED) as z:
    for file in files:
        if file.is_file():z.write(file,str(Path(a.team)/file.relative_to(root)))
print(target)
