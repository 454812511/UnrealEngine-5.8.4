#!/usr/bin/env bash
# restore_ue584_single.sh
# 从单个 GitHub 仓库还原 UE 5.8.4 源码树（Linux / macOS）
#
# 用法:
#   git clone --depth 1 -b restore-tooling https://github.com/454812511/UnrealEngine-5.8.4.git tooling
#   bash tooling/restore_ue584_single.sh /opt/UE5.8.4
set -euo pipefail

TARGET="${1:-/opt/UE5.8.4}"
OWNER="${OWNER:-454812511}"
REPO="${REPO:-UnrealEngine-5.8.4}"
WORK="${WORK:-/tmp/ue584_clone}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=============================================="
echo " UE 5.8.4 源码还原（单仓库）"
echo " 来源仓库 : https://github.com/$OWNER/$REPO.git"
echo " 目标目录 : $TARGET"
echo "=============================================="

command -v git >/dev/null || { echo "需要 git"; exit 1; }

TOTAL=224010
if [ -f "$SCRIPT_DIR/REPO.json" ]; then
    TOTAL=$(python3 -c "
import json
print(json.load(open('$SCRIPT_DIR/REPO.json'))['total_files'])
" 2>/dev/null || echo 224010)
fi

rm -rf "$WORK"
mkdir -p "$WORK" "$TARGET"

echo "[1/3] clone ..."
# core.autocrlf=false 必须在 clone 之前生效：这样 checkout 出来的字节在
# Windows / Linux 上完全一致（.gitattributes 的 eol=crlf/eol=lf 规则仍照常生效，
# 其余文件保持 blob 原样）。
# --no-checkout：不要把 2.68 GB 的工作区先落盘，直接从索引展开到目标目录，
# 临时占用只有 .git 里的 pack（约 554 MB）。
git -c core.autocrlf=false clone --depth 1 --no-checkout --quiet \
    "https://github.com/$OWNER/$REPO.git" "$WORK/clone"

echo "[2/3] checkout-index 展开源码树 ..."
# checkout-index 把索引里的文件按工作区规则（eol 转换）写到目标目录：
# 不含 .git，也不受 export-ignore 影响。
git -C "$WORK/clone" read-tree HEAD
git -C "$WORK/clone" checkout-index -a -f --prefix="$TARGET/"

rm -rf "$WORK"

echo "[3/3] 校验 ..."
actual=$(find "$TARGET" -type f | wc -l)
echo ""
echo "文件数: 期望 $TOTAL  实际 $actual"
if [ "$actual" -ne "$TOTAL" ]; then
    echo "[FAIL] 文件数不符" >&2
    exit 1
fi

if [ -f "$SCRIPT_DIR/MANIFEST.sha256" ]; then
    echo "校验 sha256 (224k 文件, 约需数分钟) ..."
    (cd "$TARGET" && sha256sum -c --quiet "$SCRIPT_DIR/MANIFEST.sha256")
    echo "[PASS] sha256 全部通过"
fi

echo "[PASS] 还原完成: $TARGET"
echo "下一步: 在该目录运行 ./Setup.sh，再用 UBT 生成工程文件。"
