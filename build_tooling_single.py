"""Build and push the `restore-tooling` branch for the single UE 5.8.4 repo.

The branch is an orphan (root commit) holding only tooling files:
  restore_ue584_single.sh / .ps1  一键还原脚本
  README_ue584_restore.md          使用说明
  REPO.json                        仓库元信息（文件数、源提交等）
  MANIFEST.sha256                  224010 个文件的逐文件 sha256

The `main` branch of the repo stays 100% pure UE source (no extra files), so
`git checkout-index` reproduces the original tree byte for byte.

Usage:
  python3 build_tooling_single.py <bare-git-dir> <tooling-src-dir> <manifest> <repojson>
"""
import json
import os
import shutil
import subprocess
import sys

REPO = sys.argv[1]
SRCDIR = os.path.abspath(sys.argv[2])
MANIFEST = sys.argv[3]
REPOJSON = sys.argv[4]

MSG = """UE 5.8.4 restore tooling

One-command restore + integrity check for the single UnrealEngine-5.8.4 repo.
"""


def git(args, env=None, cwd=None):
    r = subprocess.run(["git"] + args, env=env, cwd=cwd,
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"git {args}: {r.stderr[:2000]}")
    return r.stdout


stage = os.path.join(os.path.dirname(os.path.abspath(REPO)), "_tooling_stage")
shutil.rmtree(stage, ignore_errors=True)
os.makedirs(stage, exist_ok=True)
for f in os.listdir(SRCDIR):
    src = os.path.join(SRCDIR, f)
    if os.path.isfile(src):
        shutil.copy2(src, os.path.join(stage, f))
shutil.copy2(MANIFEST, os.path.join(stage, "MANIFEST.sha256"))
shutil.copy2(REPOJSON, os.path.join(stage, "REPO.json"))

env = dict(os.environ)
env["GIT_DIR"] = REPO
env["GIT_WORK_TREE"] = stage
env["GIT_AUTHOR_NAME"] = "UE 5.8.4 snapshot"
env["GIT_AUTHOR_EMAIL"] = "snapshot@local"
env["GIT_COMMITTER_NAME"] = "UE 5.8.4 snapshot"
env["GIT_COMMITTER_EMAIL"] = "snapshot@local"
env["GIT_AUTHOR_DATE"] = "2026-11-25T12:00:00+0000"
env["GIT_COMMITTER_DATE"] = "2026-11-25T12:00:00+0000"

# orphan branch: no parent, so the tooling tree never mixes with the UE tree
git(["add", "-A", "."], env=env, cwd=stage)
tree = git(["write-tree"], env=env).strip()
commit = git(["commit-tree", tree, "-m", MSG], env=env).strip()
git(["update-ref", "refs/heads/restore-tooling", commit], env=env)
print("tooling commit:", commit)

# push the tooling branch to GitHub
meta = json.load(open(REPOJSON, encoding="utf-8"))
remote = f"https://github.com/{meta['owner']}/{meta['repo']}.git"
penv = dict(os.environ)
penv["GIT_ASKPASS"] = "/root/.gh/askpass.sh"
penv["GIT_TERMINAL_PROMPT"] = "0"
r = subprocess.run(["git", "-C", REPO, "push", "--progress", remote,
                    "refs/heads/restore-tooling:refs/heads/restore-tooling"],
                   env=penv, capture_output=True, text=True)
if r.returncode != 0:
    print("[FAIL] push restore-tooling failed:")
    print(r.stderr[-3000:])
    sys.exit(1)
print("[OK] restore-tooling pushed to", remote)
print(git(["-C", REPO, "ls-tree", "-r", "--name-only", "restore-tooling"]))
