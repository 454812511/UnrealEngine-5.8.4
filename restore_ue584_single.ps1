<#
    restore_ue584_single.ps1
    ---------------------------------------------------------------------------
    从单个 GitHub 仓库还原 UE 5.8.4 源码树，并做完整性校验。

    用法（一条命令）:
        git clone --depth 1 -b restore-tooling https://github.com/454812511/UnrealEngine-5.8.4.git tooling
        powershell -ExecutionPolicy Bypass -File tooling\restore_ue584_single.ps1 -Target D:\UE5.8.4

    参数:
        -Target     还原目标目录（默认 D:\UE5.8.4）
        -Owner      GitHub 账号（默认 454812511）
        -Repo       仓库名（默认 UnrealEngine-5.8.4）
        -Work       临时 clone 目录（默认 %TEMP%\ue584_clone）
        -SkipHash   跳过 sha256 逐文件校验（只做文件数校验）
        -KeepClone  还原后保留临时 clone 目录
#>
[CmdletBinding()]
param(
    [string]$Target = "D:\UE5.8.4",
    [string]$Owner = "454812511",
    [string]$Repo = "UnrealEngine-5.8.4",
    [string]$Work = "$env:TEMP\ue584_clone",
    [switch]$SkipHash,
    [switch]$KeepClone
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$metaFile = Join-Path $scriptDir "REPO.json"
$manifestFile = Join-Path $scriptDir "MANIFEST.sha256"

$total = 224010
if (Test-Path $metaFile) {
    $total = [int]((Get-Content $metaFile -Raw | ConvertFrom-Json).total_files)
}

Write-Host "=============================================="
Write-Host " UE 5.8.4 源码还原（单仓库）"
Write-Host " 来源仓库 : https://github.com/$Owner/$Repo.git"
Write-Host " 目标目录 : $Target"
Write-Host "=============================================="

# ---------------------------------------------------------------- 1. clone
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Path $Work | Out-Null
New-Item -ItemType Directory -Path $Target -Force | Out-Null

Write-Host "[1/3] clone ..."
$dst = Join-Path $Work "clone"
# core.autocrlf=false 必须在 clone 之前生效：这样 checkout 出来的字节在
# Windows / Linux 上完全一致（.gitattributes 的 eol=crlf/eol=lf 规则仍照常生效，
# 其余文件保持 blob 原样）。
# --no-checkout：不先把 2.68 GB 工作区落盘，直接从索引展开到目标目录。
& git -c core.autocrlf=false clone --depth 1 --no-checkout "https://github.com/$Owner/$Repo.git" $dst
if ($LASTEXITCODE -ne 0) { throw "clone 失败: $Repo" }

# ---------------------------------------------------------------- 2. expand
Write-Host "[2/3] checkout-index 展开源码树 ..."
& git -C $dst read-tree HEAD
if ($LASTEXITCODE -ne 0) { throw "read-tree 失败" }
# prefix 必须是正斜杠并以 / 结尾，git 才能正确解析 Windows 路径。
$prefix = ($Target -replace '\\', '/')
if (-not $prefix.EndsWith('/')) { $prefix += '/' }
& git -C $dst checkout-index -a -f --prefix="$prefix"
if ($LASTEXITCODE -ne 0) { throw "checkout-index 失败" }

# ---------------------------------------------------------------- 3. verify
Write-Host "[3/3] 校验 ..."
$actual = (Get-ChildItem -LiteralPath $Target -Recurse -File -Force | Measure-Object).Count
Write-Host ""
Write-Host "文件数: 期望 $total  实际 $actual"
if ($actual -ne $total) {
    Write-Host "[FAIL] 文件数不符" -ForegroundColor Red
    exit 1
}

if (-not $SkipHash) {
    if (-not (Test-Path $manifestFile)) { throw "缺少 MANIFEST.sha256: $manifestFile" }
    Write-Host "校验 sha256 (224k 文件, 约需数分钟) ..."
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $bad = 0
    $n = 0
    foreach ($line in [System.IO.File]::ReadLines($manifestFile)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $expect = $line.Substring(0, 64)
        $rel = $line.Substring(66)
        $f = Join-Path $Target ($rel -replace '/', '\')
        if (-not (Test-Path -LiteralPath $f)) {
            $bad++
            if ($bad -le 20) { Write-Host "  缺失: $rel" -ForegroundColor Red }
            continue
        }
        $fs = [System.IO.File]::OpenRead($f)
        $got = [System.BitConverter]::ToString($sha.ComputeHash($fs)).Replace('-', '').ToLower()
        $fs.Close()
        if ($got -ne $expect) {
            $bad++
            if ($bad -le 20) { Write-Host "  校验失败: $rel" -ForegroundColor Red }
        }
        $n++
    }
    if ($bad -gt 0) {
        Write-Host "[FAIL] $bad / $n 个文件校验不通过" -ForegroundColor Red
        exit 1
    }
    Write-Host "sha256 全部通过 ($n 个文件)" -ForegroundColor Green
}

Write-Host ""
Write-Host "[PASS] 还原完成并通过完整性校验: $Target" -ForegroundColor Green
Write-Host "下一步: 在该目录运行 Setup.bat，然后生成项目文件。"

if (-not $KeepClone) {
    Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
}
