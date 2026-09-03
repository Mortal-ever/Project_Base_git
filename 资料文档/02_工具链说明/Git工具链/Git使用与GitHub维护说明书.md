# Project_Base Git 使用与 GitHub 维护说明书

适用目录：`D:\Project_Items\Project_Base`。当前分支：`main`。远程：`git@github.com:Mortal-ever/Project_Base_git.git`。

## 初始化

务必在工程目录执行，不要在 `D:\Project_Items` 父目录执行：

```powershell
Set-Location 'D:\Project_Items\Project_Base'
git init
git branch -M main
git remote add origin git@github.com:Mortal-ever/Project_Base_git.git
git rev-parse --show-toplevel
git remote -v
```

根目录必须显示 `D:/Project_Items/Project_Base`。

## 身份和连接

```powershell
git config --global user.name "你的姓名"
git config --global user.email "你的邮箱"
ssh -T git@github.com
git lfs version
```

## 忽略规则

当前 `.gitignore` 排除 `.codex_tmp/`、`tmp/`、`GCC-ARM/build/`、`GCC-ARM/.tools/`、`MDK-ARM/Objects/`、`MDK-ARM/List/` 及 Keil/GCC 中间文件。

```powershell
git check-ignore -v .codex_tmp/example tmp/example GCC-ARM/build/example.o MDK-ARM/Objects/example.o
```

已提交过的缓存从索引移除但保留本地文件：

```powershell
git rm -r --cached --ignore-unmatch -- .codex_tmp tmp GCC-ARM/build MDK-ARM/Objects MDK-ARM/List
git add .gitignore
git commit -m "Ignore local build and agent artifacts"
```

## 首次提交和覆盖远程

```powershell
git status --short
git add --all
git diff --cached --name-status
git commit -m "Initial Project_Base snapshot"
git push --force -u origin main
```

远程只有初始化 README 且明确不保留时才使用 `--force`，因为它会覆盖远程历史。

## 日常维护

```powershell
Set-Location 'D:\Project_Items\Project_Base'
git status --short --branch
git diff
git diff --check
git add --all
git diff --cached --name-status
git commit -m "简短描述本次修改"
git push origin main
git status --short --branch
```

看到 `main...origin/main` 即表示同步。

## 合并合法远程更新

只有确认远程有需要保留的有效修改时才拉取：

```powershell
git fetch origin
git pull --rebase origin main
```

冲突解决后执行 `git add <已解决文件>`、`git rebase --continue`，再推送；放弃合并使用 `git rebase --abort`。如果本地才是正确版本，不要 `pull`，检查后直接强制推送。

## 大文件与 Git LFS

GitHub 普通 Git 单文件限制约为 100 MiB。大型资料使用：

```powershell
git lfs install
git lfs track "资料文档/路径/大型文件.zip"
git add .gitattributes "资料文档/路径/大型文件.zip"
git commit -m "Track large document with Git LFS"
git push origin main
```

用 `git lfs ls-files` 和 `git lfs status` 检查。本工程的超大称重软件压缩包已按用户决定删除，未上传。

## 本次问题原因

Git 根目录曾错误位于 `D:\Project_Items`，并且缓存目录进入旧历史。`.gitignore` 只能阻止未来文件，不能清除历史对象，所以推送停在 `Writing objects`。处理方式是：在 Project_Base 重新初始化，清理缓存规则和历史，删除超限压缩包，再用本地恢复版本强制覆盖远程。

## 排查与安全

```powershell
git rev-parse --show-toplevel
git remote -v
git branch -vv
git log --oneline --decorate -10
git ls-remote origin refs/heads/main
git ls-files | Select-String '(^|/)(\.codex_tmp|tmp|GCC-ARM/build|MDK-ARM/Objects|MDK-ARM/List)(/|$)'
git count-objects -vH
```

禁止目录检查应无输出。不要提交 `.env`、密码、Token、私钥或设备密钥；不要使用 `git reset --hard` 或 `git clean -fd` 清理不确定文件。每次文件变更都登记根目录 `CHANGES.md`。
