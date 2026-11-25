# UE 5.8.4 一键还原

UE 5.8.4 的**完整源码快照**放在一个公开仓库里：

| 项 | 值 |
| --- | --- |
| 仓库 | https://github.com/454812511/UnrealEngine-5.8.4 |
| 源码分支 | `main`（纯 UE 原始路径，不含任何额外文件） |
| 工具分支 | `restore-tooling`（本目录里的还原脚本 + 校验清单） |
| 源提交 | `f45729c0b6f097038e72b52d366a62baeca3275b`（EpicGames/UnrealEngine 的 `release` 分支） |
| 文件数 | 224010 |
| 体积 | 原始内容 2.68 GB；git pack 后约 554 MB |

## 一条命令还原（Windows）

```powershell
powershell -ExecutionPolicy Bypass -Command "git clone --depth 1 -b restore-tooling https://github.com/454812511/UnrealEngine-5.8.4.git $env:TEMP\ue584_tooling; & $env:TEMP\ue584_tooling\restore_ue584_single.ps1 -Target D:\UE5.8.4"
```

## 一条命令还原（Linux / macOS）

```bash
bash -c 'git clone --depth 1 -b restore-tooling https://github.com/454812511/UnrealEngine-5.8.4.git /tmp/ue584_tooling && bash /tmp/ue584_tooling/restore_ue584_single.sh /opt/UE5.8.4'
```

## 脚本做什么

1. `git clone --depth 1` 拉取 `main` 分支到临时目录（clone 时带 `-c core.autocrlf=false`，保证 Windows / Linux 出来的字节完全一致）；
2. `git checkout-index -a -f --prefix=<目标目录>/` 把索引里的文件按**原始路径**展开成源码树；
3. 统计目标目录文件数，与 `REPO.json` 的 `total_files` 比对；
4. 用 `MANIFEST.sha256` 逐文件校验 sha256。

任何一步不符即以非零码退出并打印 `[FAIL]`。

> 为什么用 `checkout-index` 而不是 `git archive`：
> - `git archive` 会遵守 `export-ignore` 规则，UE 源码里有嵌套的 `.gitattributes` 声明了 `export-ignore`，导出会**漏掉 2 个文件**；
> - `git archive` 输出的是 blob 原始字节，不做 eol 转换，而根 `.gitattributes` 规定 `*.bat eol=crlf`，会与校验清单对不上 351 个 `.bat`；
> - `checkout-index` 按工作区规则写出，既不漏文件，又应用了 eol 转换。

## 完整性依据

- 仓库 `main` 分支的树对象与源仓库 `f45729c0` 的树**完全相等**（path + mode + blob sha 逐条比对，224010 条）。
- 每个 blob 就是源仓库里的同一批 git 对象（同一 sha），不是重新生成的副本。
- `MANIFEST.sha256` 是源工作树 224010 个文件的逐文件 sha256。
- 还原脚本跑完后逐文件重算 sha256 并与清单比对，全绿才算通过。

## 还原后

在目标目录运行 `Setup.bat`（Windows）或 `./Setup.sh`（Linux），再用 UBT 生成工程文件。

## 参数

| 脚本 | 参数 | 默认值 |
| --- | --- | --- |
| `restore_ue584_single.ps1` | `-Target` | `D:\UE5.8.4` |
| | `-Owner` | `454812511` |
| | `-Repo` | `UnrealEngine-5.8.4` |
| | `-Work` | `%TEMP%\ue584_clone` |
| | `-SkipHash` | 关（跳过 sha256，只校验文件数） |
| | `-KeepClone` | 关（还原后删除临时 clone 目录） |
| `restore_ue584_single.sh` | 第 1 个位置参数 | `/opt/UE5.8.4` |
| | 环境变量 `OWNER` | `454812511` |
| | 环境变量 `REPO` | `UnrealEngine-5.8.4` |
| | 环境变量 `WORK` | `/tmp/ue584_clone` |

## 手动还原（不想用脚本）

```bash
git -c core.autocrlf=false clone --depth 1 https://github.com/454812511/UnrealEngine-5.8.4.git /tmp/ue
git -C /tmp/ue checkout-index -a -f --prefix=/opt/UE5.8.4/
```

Windows 上也可以直接 clone 后把工作区（除 `.git` 外）复制到目标目录，clone 时同样要带 `-c core.autocrlf=false`。
