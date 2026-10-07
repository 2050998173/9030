# 用云端编译（不需要本地任何工具链）

本机执行不了 NDK 里的程序时，走这条路：**让 GitHub 的服务器帮你编译**，
你只负责下载成品。不受杀毒软件、沙箱、受控文件夹访问影响。

一切都在 [`.github/workflows/build.yml`](.github/workflows/build.yml) 里配好了：
在 Ubuntu 上装 NDK r27c，编 **arm64-v8a + armeabi-v7a**，打包成可刷入的 zip。

---

## 一次性准备

你需要一个 GitHub 账号，以及本机装了 Git（`git --version` 能打印版本）。

在项目目录打开终端（在资源管理器地址栏输入 `cmd` 回车），依次执行：

```bat
cd /d "D:\Backup\Documents\deepseek-harness\default-workspace\HuaweiMate80ProSpoof"

git init
git add .
git commit -m "HUAWEI Mate 80 Pro spoof module"
git branch -M main
```

然后去 GitHub 网页上**新建一个空仓库**（不要勾选 README / .gitignore / License，
否则 push 会冲突）。假设你的用户名是 `YOURNAME`、仓库名是 `hw80pro`：

```bat
git remote add origin https://github.com/YOURNAME/hw80pro.git
git push -u origin main
```

> **push 卡住或超时**是常见情况（网络原因）。多试几次，或者配置代理：
> ```bat
> git config --global http.proxy http://127.0.0.1:7890
> ```
> （端口换成你自己代理的；不需要时用 `git config --global --unset http.proxy` 取消）

## 拿成品

1. 打开仓库页面 → 顶部 **Actions** 标签
2. 左边选 **build**，会看到一个正在跑（或已完成）的任务
3. 等它变绿（约 3–5 分钟）
4. 点进这次运行 → 页面底部 **Artifacts** → 下载 `HuaweiMate80ProSpoof-zip`
5. 解压得到 `HuaweiMate80ProSpoof-1.0.0.zip`，传到手机上用管理器"从本地安装"

如果需要重新编译：Actions → build → 右侧 **Run workflow**。

> 想打 tag 发 Release（可选，只影响你自己，不影响构建）：
> ```bat
> git tag v1.0.0
> git push origin v1.0.0
> ```

---

## 换成 Gitee / 国内平台

如果 GitHub 的 push 一直失败，还有两条路：

1. **仍然用 GitHub，但改用网页上传**（见下一节）—— 完全不需要 git 和命令行。
2. **换到别的机器编译**：把整个 `HuaweiMate80ProSpoof` 目录复制到任何一台
   能正常执行程序的机器上（Windows 或 Linux/macOS 都行），装好 NDK + Node 后
   执行 `node tools/build.js`，产物在 `dist/`。

---

## 没有 git？用浏览器上传（完全不需要命令行）

GitHub 的网页支持直接上传文件夹。

### 1. 先让 .github 文件夹可见

Windows 资源管理器默认不显示以点开头的文件夹，而流水线配置就在 `.github` 里。
打开项目目录 → 顶部「查看」→ 勾选「**隐藏的项目**」。

### 2. 网页上创建仓库

github.com → 右上角 **+** → **New repository** → 填名字（例如 `hw80pro`）→
选 **Public** → **不要**勾选 "Add a README file" / .gitignore / license →
**Create repository**。

### 3. 上传

在新建好的仓库页面点 **uploading an existing file**（或 **Add file → Upload files**），
然后把项目目录里的**所有内容**拖进去 —— 包括：

```
.github        <-- 必须上传, CI 配置在这里
src  include  config  tools  webroot
module.prop  customize.sh  action.sh  service.sh  post-fs-data.sh
uninstall.sh  CMakeLists.txt  README.md  REVIEW-NOTES.md  ACTIONS.md
.gitignore  .gitattributes
```

**不要上传**（中间产物，几百 MB）：

```
build/        dist/        *.zip
```

> `.gitignore` 里已经排除了 `build/`、`dist/`、`*.zip`，但网页上传不看 `.gitignore`，
> 所以要手动避开这几个。

然后点 **Commit changes**。

### 4. 拿成品

跟命令行方式一样：**Actions** 标签 → 等任务变绿（3–5 分钟）→
运行页面底部 **Artifacts** → 下载 `HuaweiMate80ProSpoof-zip`。

---

## 验证这次编译是好的

CI 日志里会打印两行关键检查（`tools/build.js` 加的）：

```
  符号检查: zygisk_module_entry, zygisk_companion_entry 均已导出
```

**这两行必须出现。** Zygisk 是按这两个符号名找模块入口的，漏掉任何一个，
模块会装上去但完全不工作。如果 CI 日志里出现 `[警告] ... 没有导出 ...`，
把日志发我。

---

## 我也可以本地编，但需要你在场

如果你想在这台机器上编，请先解决"NDK 里的 exe 无法执行"的问题：

1. 把 NDK 移出桌面（桌面受 Windows"受控文件夹访问"保护）：
   ```bat
   robocopy "C:\Users\Administrator\Desktop\android-ndk-r30-windows\android-ndk-r30" "D:\android-ndk-r30" /E /NFL /NDL /NJH /NJS /NP
   "D:\android-ndk-r30\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe" --version
   ```
   能打印版本号就成功了，然后：
   ```bat
   node tools/build.js --ndk "D:\android-ndk-r30"
   ```

2. 若仍零输出，检查**杀毒软件拦截日志**，把 NDK 目录加入白名单，
   或临时关闭"受控文件夹访问"（Windows 安全中心 → 病毒和威胁防护 → 勒索软件防护）。
