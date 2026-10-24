# 创建全新的 Public 仓库（仅命令说明，未执行）

本目录不带旧 `.git`。不要把它覆盖回旧仓库，也不要直接将旧仓库改为 Public。
旧历史中发现过凭据；创建全新历史并不能撤销旧凭据，维护者仍应自行更换仍在使用的旧值。

需要 GitHub CLI（本次整理环境未安装 gh）；可从 https://cli.github.com/ 安装，或手动在 GitHub 建立空的 Public 仓库。
以下 PowerShell 必须在本源码包的根目录执行；若已有 `.git` 或目标仓库已存在，先停下核实，不覆盖、不强推。

```powershell
if (Test-Path .git) { throw 'Directory already has Git history; inspect before continuing.' }
python tools/verify_release.py
if ($LASTEXITCODE -ne 0) { throw 'Payload verification failed.' }
git config --get user.name
git config --get user.email
# 如果未设置身份，先填写本人真实身份；不要编造其他人的姓名或邮箱。
git init -b main
Import-Csv SOURCE_MANIFEST.csv | ForEach-Object { git add -- $_.path }
git add -- SOURCE_MANIFEST.csv
git diff --cached --check
git diff --cached --stat
git status
# 人工确认暂存内容后：
git commit -m "release: publish Nexus S1 competition source"
gh auth login --hostname github.com --git-protocol https --web
gh auth setup-git
$owner = (gh api user --jq .login).Trim()
$repo = "$owner/Nexus-S1-Competition-2026"
gh repo create $repo --public --source . --remote origin
# 仅在创建成功后：
git push -u origin main
gh repo view $repo --json nameWithOwner,visibility,url
```

`gh repo create` 与 `git push` 分开，无 --force、无旧历史迁入。若仓库名已被占用，创建会失败；选择另一个全新名称后再继续。
提交前清单只能对应当前发布快照；后续修改源码应重新运行验证并更新清单，不能绕过哈希错误。
命令来源：https://cli.github.com/manual/gh_repo_create 与 https://docs.github.com/en/repositories/creating-and-managing-repositories/creating-a-new-repository 。
