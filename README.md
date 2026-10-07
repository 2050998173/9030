# HUAWEI Mate 80 Pro 全面伪装 (Zygisk 模块)

把 Android 手机伪装成 **HUAWEI Mate 80 Pro（SGT-AL50 / 麒麟 9030 Pro）**，只对下面三个应用生效：

| 包名 | 说明 |
|---|---|
| `com.liuzh.deviceinfo` | Device Info（设备信息） |
| `com.maple.detect` | Maple Detector（检测工具） |
| `com.tencent.tmgp.dfm` | 三角洲行动（腾讯） |

其它任何应用（包括系统、微信、支付宝…）**完全不受影响** —— 模块在 `preAppSpecialize`
阶段就按包名分流，不匹配的进程里连模块的代码都不会留下。

> ### ⚠️ 未编译、未实机验证
>
> 本工程是在没有 shell、没有 Android NDK、没有设备的开发环境里写出来的。
> 源码经过两轮独立静态审查（编译正确性 + 运行语义），修掉了包括
> "arm32 上一个 hook 都装不上"、"伪造 prop_info 让属性读成空串"、
> "`app_matches` 在配置加载前调用导致模块整体空操作" 在内的若干致命问题，
> 但**从未被编译器编译过，也从未在任何设备上运行过**。
>
> 首次 `./build.sh` 可能需要小修；首次上机建议先看 `logcat -s HW80Pro`。
> 完整的审查结论与已知边界见 **[REVIEW-NOTES.md](REVIEW-NOTES.md)**。

---

## 一、伪装了什么

### 1. CPU（麒麟 9030 Pro，9 核 14 线程）

| 项目 | 伪装值 |
|---|---|
| SoC | HiSilicon Kirin 9030 Pro（麒麟 9030 Pro） |
| 核心 | 9 核 14 线程：1×2.75GHz 超大核 + 4×2.27GHz 大核 + 4×1.72GHz 小核 |
| 频率节点 | `/sys/devices/system/cpu/*/cpufreq/*` 保持真实（伪造实时频率反而更假） |
| 核数节点 | `possible` / `present` / `online` / `offline` / `kernel_max` 伪装成 9 核 |
| GPU | Maleoon 935（马良 935） |
| `/proc/cpuinfo` | 整体替换为麒麟 9030 Pro 的 9 个 processor 块 + `Hardware: HUAWEI Kirin 9030 Pro`（模板里的 `#` 注释行会被自动剔除，真实内核不输出注释） |
| NPU / ISP | NPU 3.0 / ISP 9.0 |

`/proc/cpuinfo` 的拦截覆盖**所有**读取路径，不只是某一种：

```
Java FileInputStream / RandomAccessFile  ->  open/openat + read/pread64
NDK  fread(buf,1,n,f)                    ->  fopen + fread
NDK  fgets(line,n,f)                     ->  fopen + fgets
NDK  fseek + fread                       ->  fopen + fseek + fread
```

### 2. 系统属性（约 300 条）

覆盖层面：

| 层面 | 手段 | 覆盖对象 |
|---|---|---|
| native | PLT hook `__system_property_get` | 所有 native 代码 |
| native | PLT hook `__system_property_read` | 老 API |
| native | PLT hook `__system_property_read_callback` | API 26+ 新 API |
| native | PLT hook `__system_property_find` | 拿句柄缓存的库（实验性，默认关） |
| Java | 替换 `SystemProperties.native_get*` | `Build.*`、`Settings`、`getprop` 之外的框架读取 |
| 子进程 | 改写 `execve` 参数 | App 里执行 `getprop xxx` 的情况 |

伪装的关键属性分组：

- **产品身份**：`ro.product.brand/manufacturer/model/device/board/name`、
  `ro.product.marketname = HUAWEI Mate 80 Pro`
- **硬件**：`ro.board.platform = kirin9030`、`ro.hardware`、`ro.chipname`、
  `ro.soc.manufacturer = HiSilicon`、`ro.soc.model = Kirin 9030 Pro`、
  `ro.gpu.model = Maleoon 935`、`ro.opengles.version`
- **系统版本**：`ro.build.version.emui = EmotionUI_15.0.0`、
  `ro.build.display.id = SGT-AL50 15.0.0.130(SP1C00E130R5P1)`、
  `ro.build.version.harmonyos`、`ro.build.version.release/sdk`、
  以及**清空** MIUI / HyperOS / ColorOS / OneUI / OriginOS / Magic 等一切非华为痕迹
- **指纹**：`ro.build.fingerprint` 及 `bootimage/system/vendor/odm/product/system_ext`
  六路指纹全部统一为 `HUAWEI/SGT-AL50/HWSGT:15/...`
- **启动与安全**：`ro.boot.verifiedbootstate = green`、`ro.boot.flash.locked = 1`、
  `ro.boot.vbmeta.device_state = locked`、`ro.secureboot.lockstate = locked`、
  `ro.debuggable = 0`、`ro.build.tags = release-keys`
- **华为专有**：`ro.config.hw_*`（约 90 条）、`hw_sc.build.*`、`const.product.*`
- **内存**：`/proc/meminfo` 的 `MemTotal` 改成 16GB 机型对应值

### 3. uname / 内核标识（默认关闭）

配置里 `spoof_uname=1` 可开启。默认关闭的原因：真实内核版本与伪装值如果差得太远，
反而会成为"这台机器被改过"的证据。

---

## 二、安装

### 前置要求

- 已 root，且 Zygisk 可用：**Magisk v27+（`versionCode` ≥ 27000）**、
  或 KernelSU / APatch 的 Zygisk 实现
- Android 9 (API 28) 及以上，arm64 或 armeabi-v7a

> **版本不匹配时模块会「静默失效」**：Zygisk API v5 要求 Magisk `versionCode`
> ≥ 27000。官方说明是"新版 Magisk 永远能加载按旧 API 编译的模块"，反过来不成立 ——
> 在 Magisk 26.x 上这个模块**既不会加载也不会报错**，目标应用里毫无变化。
> 排查时先确认 `adb shell magisk -v`。若必须在 Magisk 26.x 上用，需要把
> `include/zygisk.hpp` 换成 API v4 版本，并去掉 `hookJniNativeMethods` 的调用。

### 直接用编好的 zip

1. 到 `dist/` 或 GitHub Actions 的 artifact 里拿 `HuaweiMate80ProSpoof-1.0.0.zip`
2. 在 Magisk / KernelSU / APatch 里"从本地安装"
3. 重启手机

### 自己编译

**最省事：双击运行（推荐给不想碰命令行的）**

把工程根目录下的 **`一键构建-复制到桌面.cmd`** 复制到桌面，双击它。
它会自动找 Node 和 NDK、调用构建、跑完停住窗口不关（默认编四个 ABI）。

> **不要双击 `clang++.exe`。** 它是命令行程序，双击会"闪退"——那是它正常
> 打印完用法就退出，不是崩溃。而且交叉编译必须带
> `--target=aarch64-linux-android28` 和 `--sysroot=...` 两个参数，
> 光双击 exe 什么都编不出来。要手动验证它能不能跑，请在**已经开着的**终端里：
>
> ```powershell
> & "C:\Users\Administrator\Desktop\android-ndk-r30-windows\android-ndk-r30\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" --version
> ```

**推荐：只装了 NDK 就能编（不需要 CMake / Ninja）**

NDK r30 起已经**不再自带 cmake / ninja**，所以提供了一个纯 Node 的构建脚本，
它直接用 NDK 自带的 `clang++.exe` 编译链接，再用 Node 自带的 zlib 打包：

```bash
node tools/build.js --ndk "C:\Users\Administrator\Desktop\android-ndk-r30-windows\android-ndk-r30"
node tools/build.js --abi every      # 四个 ABI 全编（arm + x86）
node tools/build.js --abi all        # 只编两个 arm
node tools/build.js                  # 不传 --ndk 时自动在常见位置搜索
```

> **ABI 一定要选对**：模块只会加载与目标进程 ABI 匹配的那一个
> `zygisk/<abi>.so`。如果跑在模拟器 / 云机 / 手游助手上（大概率是 x86/x86_64），
> 就必须用 `--abi every` 编出对应 ABI，否则模块**不会加载也不报错**。

它会自动做这几件事，失败时给出可复现的完整命令行：

1. 定位 NDK 并读出 `Pkg.Revision`
2. 体检 sysroot 里的关键头文件（`android/dlext.h`、`sys/utsname.h`、`elf.h`、`jni.h` …）
3. 用 Python 生成内嵌配置 `profile_data.cpp`
4. 逐个 ABI 编译 10 个源文件 → 链接 `libhw80pro.so`
5. 用 `llvm-nm` 检查 `zygisk_module_entry` / `zygisk_companion_entry` **是否真的导出**
   （漏掉这两个符号模块就是死的，这一步能提前发现）
6. 打包成 zip（DEFLATE，Node 自带 zlib，不依赖 `zip` 命令）

**备选：有 CMake + Ninja 时**

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
./build.sh                       # Linux / macOS / MSYS
ABIS=arm64-v8a ./build.sh        # 只编 arm64
```

```powershell
.\build.ps1 -Ndk "C:\path\to\android-ndk-r30"
```

**完全不用本地工具链**：推送到 GitHub 后，
`.github/workflows/build.yml` 会在线编译并附到 Release。

产物统一在 `dist/HuaweiMate80ProSpoof-<version>.zip`。

### 已用 NDK r30 核对过的事实

| 事项 | 结论 |
|---|---|
| `Pkg.Revision` | 30.0.16248370 (r30)，平台范围 API 21–37，`android-28` 可用 |
| `DT_PLTREL` | 已定义（`linux/elf.h:71`）—— 用于分派 RELA/REL |
| `R_AARCH64_JUMP_SLOT` | 1026（`bits/elf_common.h:1062`），与代码里的常量一致 |
| `R_ARM_JUMP_SLOT` | 22（`bits/elf_common.h:1093`），与代码里的常量一致 |
| `ELF64_R_SYM` / `ELF32_R_SYM` | 均已定义（`linux/elf.h:128-131`） |
| `libc++.a` / `libc++abi.a` / `libc++_static.a` | 每个 API level 21–37 都齐全 |
| `android/dlext.h`、`sys/utsname.h`、`sys/socket.h`、`sys/mman.h` | 均存在 |

---

## 三、配置

安装后配置在 **`/data/adb/hw80pro/hw80pro.conf`**（安装时不会覆盖已有配置，
新版默认配置会存成 `hw80pro.conf.new`）。

```ini
# 只对这三个应用生效
apps=com.liuzh.deviceinfo
apps=com.maple.detect
apps=com.tencent.tmgp.dfm

# /proc/cpuinfo 的来源
cpuinfo=@file          # 用 /data/adb/hw80pro/proc_cpuinfo.txt（默认）
# cpuinfo=@device      # 读真机 cpuinfo, 只替换型号名/核数（推荐实机核对后使用）
# cpuinfo=@none        # 不处理

meminfo_memtotal_kb=15872000
hook_property_find=1
hook_exec_getprop=1
patch_sys_cpu=1
spoof_uname=0
debug=0

# 任意系统属性都可以在这里加/改，例如:
ro.product.model=SGT-AL50
ro.build.fingerprint=HUAWEI/SGT-AL50/HWSGT:15/HUAWEISGT-AL00/15.0.0.130:user/release-keys
```

约定：

- `key=value` 新增或覆盖一条属性；**值留空 = 不伪装这一项**
- 想恢复某项为真机真实值，把那一行删掉或把值清空
- 改完必须**重启目标应用**（或重启手机）才生效
- 属性值上限 91 字节（Android 限制），超长会被截断并打警告

### ⚠️ 需要你用真机核对的字段

下面这些值公开资料无法 100% 确认（各代华为写法不同）。它们不影响模块运行，
但**想要"全面伪装"不露馅，建议核对后改成真机值**：

```bash
# 在你的手机上执行
adb shell getprop | grep -E 'fingerprint|display.id|incremental|board.platform|ro.hardware'
adb shell cat /proc/cpuinfo
```

| 配置项 | 当前默认值 | 说明 |
|---|---|---|
| `ro.build.fingerprint` 等 6 条指纹 | `HUAWEI/SGT-AL50/HWSGT:15/...` | 编译号、版本号需按真机改 |
| `ro.build.display.id` / `ro.build.version.incremental` | `SGT-AL50 15.0.0.130(SP1C00E130R5P1)` | 同上 |
| `ro.board.platform` / `ro.hardware` | `kirin9030` | 华为各代写法不同 |
| `/proc/cpuinfo` 的 `CPU part` / `Features` / `CPU revision` | `0xd84` / `0xd85` … | 建议用 `cpuinfo=@device`，或把真机输出覆盖进 `proc_cpuinfo.txt` |
| `ro.build.version.security_patch` | `2025-11-01` | 按真机 EMUI/HarmonyOS 版本改 |
| 序列号 | **不伪装** | 改它容易与"设置-关于手机"对不上 |

---

## 四、验证

伪装**只对被 hook 的进程生效**，所以在终端里敲 `getprop` 看到的仍是真机值 —— 这是正常的。

正确验证方式：

1. 打开 `com.liuzh.deviceinfo`，看"设备信息 / CPU"页：
   应显示 `HUAWEI Mate 80 Pro`、`Kirin 9030 Pro`、8/9 核、`HarmonyOS/EMUI 15`
2. 打开 `com.maple.detect`，看设备指纹相关条目
3. 打开 `com.tencent.tmgp.dfm`，进设置里的"设备信息"

看模块日志：

```bash
adb shell "su -c 'logcat -s HW80Pro'"
```

日志会打印：命中哪个进程、加载了多少条属性、打了多少个 PLT 补丁、
Java 层替换了几个 native 方法。

点管理器里本模块的"操作"按钮（`action.sh`）会生成一份状态报告：
`/data/local/tmp/hw80pro_status.txt`。

---

## 五、原理与实现

```
preAppSpecialize (zygote 上下文, 还没被沙箱限制)
  ├─ 取进程名 -> 不在 apps= 名单里就直接 return (Zygisk 卸载模块)
  ├─ connectCompanion() 让 root 进程读 /data/adb/hw80pro/hw80pro.conf
  ├─ 解析配置 -> 把属性表和虚拟文件内容复制进静态 arena
  ├─ dlsym 解析所有原始函数指针
  ├─ 注册 hook 规则
  └─ plt_hook_apply():
       ├─ hook android_dlopen_ext / dlopen -> 以后加载的库也会补上
       └─ 解析 /proc/self/maps 里每个 ELF 的 PT_DYNAMIC
          -> DT_JMPREL / .rela.plt -> 直接改写目标符号的 GOT 槽
postAppSpecialize (应用沙箱内, classloader 已就绪)
  └─ api->hookJniNativeMethods("android/os/SystemProperties", ...)
```

### 为什么自己写 PLT hook 而不用 `api->pltHookRegister()`

Zygisk 的 `pltHookRegister` + `pltHookCommit` 只对**调用那一刻已经映射**的库打补丁。
Zygisk 的 Api 在 `postAppSpecialize` 之后就失效，而那时应用自己的 `.so`（包括
`libandroid_runtime.so`）还没 `dlopen` 进来。所以本模块自己实现：

1. 解析 `/proc/self/maps`，得到每个已映射 ELF 的 `(dev, inode, base)`
2. 读它的 `PT_DYNAMIC`，拿到 `DT_SYMTAB` / `DT_STRTAB` / `DT_JMPREL` / `DT_PLTRELSZ`
3. 遍历 `.rela.plt`（arm32 是 `.rel.plt`），按符号名匹配
4. 把对应 GOT 槽原子地改成我们的实现
5. 同时 hook `android_dlopen_ext` / `__loader_android_dlopen_ext` / `dlopen`，
   之后加载的库加载完立刻补打一遍

因为 `libandroid_runtime.so` 是应用进程里才 dlopen 的，第 5 步是 Java 层能拦住的
关键。整个过程只改 GOT 槽，不写指令段，不需要 `mprotect`，可逆且不会破坏代码。

### 隐私 / 安全

- 不联网，不做任何上报
- 不修改任何系统分区文件，不写持久化 hook（只在目标进程内存里生效，进程退出即消失）
- 模块数据只放在 `/data/adb/hw80pro/`（权限 0700）

---

## 六、已知边界（做不到的事）

请务必了解，避免误以为"能过一切检测"：

| 项目 | 说明 |
|---|---|
| **root 隐藏** | 本模块**不隐藏 root**。`com.maple.detect` 仍可能检测到 Magisk / Zygisk / su / 挂载痕迹。要过这类检测请配合 Shamiko（配 DenyList）、Zygisk Next 等 |
| 终端里的 `getprop` | 那是另一个进程，不会被 hook。属正常现象 |
| `system()` / `popen("cat /proc/cpuinfo")` | 通过 shell 起子进程读文件，文件内容替换管不到（只有 `getprop` 已被特殊处理） |
| `/proc/self/maps`、`/proc/self/mountinfo` | 不做处理（Magisk 自身的 unmount 机制会处理一部分） |
| 其它未列出的 `/proc`、`/sys` 节点 | 只改了 `cpuinfo`、`meminfo`、`cpu/possible|present|online|offline|kernel_max` |
| `/sys/.../cpufreq` 实时频率 | 故意不改。伪造静态频率与真实负载对不上，反而更容易被识别 |
| 硬件级校验 | 反作弊若读 IMEI、传感器指纹、TEE/Attestation 证书链、GPU 渲染器字符串，本模块无法伪装。TEE 证明的硬件指纹是**无法伪造**的 |
| 游戏反作弊 | 改机行为本身可能违反游戏用户协议，`com.tencent.tmgp.dfm` 有内核级反作弊，请自行评估风险 |

---

## 七、目录结构

```
├── module.prop                 Magisk 模块描述
├── customize.sh                安装脚本(按架构裁剪 zygisk/<abi>.so)
├── post-fs-data.sh / service.sh / uninstall.sh / action.sh
├── webroot/                    WebUI(KernelSU/APatch, 无框架纯静态)
├── config/
│   ├── hw80pro.conf            默认伪装配置(约 300 条属性)
│   └── proc_cpuinfo.txt        麒麟 9030 Pro 的 cpuinfo 模板
├── include/zygisk.hpp          Zygisk API v5 (来自 zygisk-module-sample)
├── src/
│   ├── zygisk_module.cpp       模块入口 / 流程编排
│   ├── companion.{hpp,cpp}     root companion 进程(读配置)
│   ├── config.{hpp,cpp}        配置解析 + cpuinfo 运行时改写
│   ├── spoof_db.{hpp,cpp}      伪装数据库(无锁定长表 + arena)
│   ├── plt_hook.{hpp,cpp}      自研 PLT/GOT hook 引擎
│   ├── hook_property.cpp       系统属性拦截(4 个 native 符号)
│   ├── hook_file.cpp           虚拟文件内容替换(fd/FILE* 双路径)
│   ├── hook_jni.cpp            Java 层 native 方法替换
│   ├── hook_misc.cpp           uname 伪装 + getprop 子进程改写
│   └── util.{hpp,cpp}          日志 / 字符串 / maps 解析(raw syscall)
├── tools/
│   ├── build.js                **推荐的构建脚本**: 直接用 NDK 的 clang++, 不需要 CMake/Ninja
│   ├── embed_profile.py        把配置嵌进 profile_data.cpp
│   └── probe-spawn.js          诊断: 当前环境能否启动子进程
├── build.cmd                   Windows 一键构建(双击用; 本身结尾不暂停)
├── 一键构建-复制到桌面.cmd      ★ 复制到桌面双击, 跑完会停住窗口
├── CMakeLists.txt / build.sh / build.ps1   备选构建方式(需要 CMake + Ninja)
├── REVIEW-NOTES.md             两轮代码审查的结论与已知边界
└── .github/workflows/build.yml 在线构建
```

安装后的模块目录长这样（Zygisk 要求库文件名就是 ABI 名）：

```
/data/adb/modules/hw80pro_spoof
├── module.prop
├── zygisk
│   └── arm64-v8a.so          # 只保留本机 ABI
├── action.sh / service.sh / post-fs-data.sh / uninstall.sh
└── webroot/
```

---

## 八、排错

| 现象 | 处理 |
|---|---|
| 应用里没任何变化 | `logcat -s HW80Pro` 看有没有"命中目标进程"。没有 = Zygisk 没加载模块 |
| 日志里说 "Java 层属性拦截没有生效" | 系统 `SystemProperties` 方法签名与预期不符（改过 ROM？），native 层拦截仍然有效 |
| 日志里说 "X 未解析到原始实现" | 该符号在当前系统不存在，对应 hook 不会生效，其它功能不受影响 |
| 打不开某个应用 / 闪退 | 把该应用从 `apps=` 里删掉，或把 `debug=0`、逐段注释配置定位是哪条属性 |
| `ro.debuggable` / `ro.build.tags` 导致异常 | 配置里注释掉那一小段（它们会影响系统与 App 行为） |
| 指纹不对 | 用真机 `getprop ro.build.fingerprint` 覆盖配置里那 7 条 `*fingerprint` |
| 模块装了但状态栏没变化 | 正常。属性伪装只在该应用进程内可见，不影响系统 UI |

卸载：管理器里卸载模块即可，配置目录 `/data/adb/hw80pro` 默认保留（`uninstall.sh`
里那行 `rm -rf` 取消注释即可连配置一起删）。

---

## 九、免责声明

本项目仅用于**技术研究、设备信息展示与兼容性测试**。

- 伪造设备信息可能违反某些应用（尤其游戏）的用户协议，可能导致账号被封禁
- 请勿用于欺诈、绕过风控、刷单、虚假注册等用途
- 使用本模块产生的任何后果由使用者自行承担
