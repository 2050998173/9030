# 代码审查记录

本文件记录两轮独立审查（编译正确性 + 运行语义）发现的问题与处理结果。

审查方式是**纯静态分析**：本项目的开发环境没有可用的 shell（`pwsh` 一律以
`0xC0000142 STATUS_DLL_INIT_FAILED` 退出，连 Node 也启动不了），也没有真机，
因此**代码从未被编译过，也从未在设备上验证过**。下面的结论都来自逐行阅读与
对 AOSP bionic / Magisk / NDK r30 头文件的交叉核对。

> **已经用 NDK r30 的头文件逐条核对过的事实**（不再是猜测）：
>
> | 事项 | 结论 |
> |---|---|
> | `DT_PLTREL` | 已定义（`linux/elf.h:71`） |
> | `R_AARCH64_JUMP_SLOT` | 1026（`bits/elf_common.h:1062`）= 代码常量 |
> | `R_ARM_JUMP_SLOT` | 22（`bits/elf_common.h:1093`）= 代码常量 |
> | `ELF64_R_SYM` / `ELF32_R_SYM` / `ELF*_R_TYPE` | 均已定义（`linux/elf.h:128-131`） |
> | `libc++.a` / `libc++abi.a` / `libc++_static.a` | API 21–37 全部齐全 |
> | `android/dlext.h`、`sys/utsname.h`、`sys/socket.h`、`sys/mman.h` | 均存在 |
>
> 仍未被编译器验证的只剩：C++ 语法/类型层面的错误（若有），以及运行期行为。

> 审查者曾把 `src/plt_hook.cpp`、`src/hook_file.cpp` 报为"修改未落地"，
> 经逐字节复核是**误报**（读到的是编辑前的缓存）。已修正的项不在此列。

---

## 已修复（按严重程度）

### 致命：整个模块变成空操作
`src/zygisk_module.cpp` — `preAppSpecialize` 在 `config_parse()` **之前**调用
`app_matches()` 做名单过滤。此时 `SpoofDb` 还是空的（`app_count_ == 0`，
`app_matches()` 恒返回 false），于是每次都提前 return：不取配置、不解析、
不打补丁，`armed_` 永远为 false，`postAppSpecialize` 也是空转。
**模块会完整加载但什么都不做。**

→ 名单过滤移到 `config_parse()` 之后；开头只取进程名。

### 致命：arm32 上一个 hook 都装不上（静默）
`src/plt_hook.cpp` — `r_info` 的拆包宏按"结构体类型"硬编码
（`Elf_Rela`→`ELF64_R_*`、`Elf_Rel`→`ELF32_R_*`），而类型是按 `__LP64__` 选的。
armeabi-v7a 上 `Elf32_Rela.r_info` 是 32 位，`ELF64_R_SYM` 等于右移 32 位 →
**恒为 0**，所有重定位都解析到 `sym[0]`；`ELF64_R_TYPE` 又等于整个 `r_info`，
永远不等于 `R_ARM_JUMP_SLOT(22)`。结果是 **32 位设备上一个 GOT 槽都不会被替换，
但日志仍然打印"PLT 补丁完成"**。

另外 JMPREL 表的 RELA/REL 判定用的是 `pltrelsz % sizeof(...)`，而同一 ABI 下
两个 `sizeof` 并不总能区分（arm32：`Elf32_Rela` 12 字节 vs `Elf32_Rel` 8 字节），
大小为 24 倍数的 `.rel.plt` 会被误判成 RELA 而静默跳过。

→ 新增按 `__LP64__` 匹配的 `HW80_R_SYM` / `HW80_R_TYPE`，四处统一使用；
新增读取 `DT_PLTREL`，用它而不是靠大小猜来分派。

### 致命：伪造的 `prop_info` 会让属性读成空串
`src/hook_property.cpp` — bionic 的 `prop_info.serial` **高 8 位编码值的长度**。
原先硬编码 `serial = 2`，长度被解成 0，于是
`SystemProperties.find(k).get()` / `Handle.get()` 这类调用路径会拿到
**空字符串** —— 比不 hook 还糟。而 `hook_property_find` 默认开启。

→ serial 改为 `(value_len << 24) | 2`；`__system_property_read_callback`
转发时也同步修正长度编码；`__system_property_read` 的返回值改为 `len + 1`
（bionic 约定），并按调用方真实缓冲区大小（`name[32]` / `value[92]`）拷贝。

### 严重：伪造 `/proc/cpuinfo` 里带着注释和重复的 Hardware 行
- `config/proc_cpuinfo.txt` 开头的 13 行 `#` 说明注释会被**原样送进**伪装的
  `/proc/cpuinfo`。真实内核不会输出注释 —— 一眼就能看出是伪造的。
- 模板末尾已有 `Hardware/Revision/Serial`，`install_cpuinfo()` 又追加了一遍
  `kEmbeddedCpuInfoHead`，同一段出现两次。

→ 新增 `append_without_comments()` 过滤 `#` 行；模板里删掉重复段；
拼接时检测已有 `Hardware` 行则不再追加。（注释仍可从 README 查阅。）

### 严重：`dlopen` 重扫在构造函数场景下失效
新库的构造函数在**原始 dlopen 内部**执行，而构造函数里常会再 dlopen 别的库。
此时 `g_scan_depth` 已置位，嵌套的 `scan()` 因 `ScanGuard` 不生效而立即返回 0，
于是"被构造函数 dlopen 出来的库"永远补不上补丁。

→ 调用原始 dlopen 前先清 `g_scan_depth`，返回后恢复再统一补扫一次。

### 中等：`__system_property_find` 伪造表存在数据竞争
原实现在**应用线程**里惰性分配记录并 `++g_fake_prop_count`，违背了
`spoof_db.hpp` 里"构建期单线程写入、运行期只读"的约定。两个线程同时
`SystemProperties.find()` 不同 key 时可能拿到同一条记录。

→ 改为在 `hook_install_property_find()`（仍是单线程阶段）一次性预建全部记录，
用 `__ATOMIC_RELEASE` 发布计数；运行期 `fake_prop_for()` 变成纯只读查表。

### 中等：ELF 解析的防御性校验不足
→ 增加 `e_type ∈ {ET_DYN, ET_EXEC}`、`e_phentsize == sizeof(Elf_Phdr)`、
程序头表落在映射范围内的检查；首个 `PT_LOAD` 的 `p_offset != 0` 时直接放弃
（此时 `bias = base - p_offset` 这个假设不成立）。

### 中等：seen 表的 check-then-act 竞争
`LibCollector::visit` 先在一把锁里查表、再在另一把锁里登记，两个并发 `dlopen`
可能同时判定"没见过"。→ 查表与登记合并到同一个临界区。

### 编译错误：`char *const argv[]` 不能赋值
`src/hook_misc.cpp` — `char *const argv[]` 调整为 `char *const *`，
`argv[i]` 的类型是 `char *const`（指针本身 const），赋值非法。这是**唯一的
无条件编译失败**。→ 用 `const_cast<char **>(argv)` 取可写视图（运行期安全，
调用方传进来的确实是可写数组）。

### 其它已修
- `src/hook_jni.cpp` — `const bool exists = (mid != nullptr) || env->ExceptionCheck();`
  逻辑反了（`GetStaticMethodID` 失败时**既**返回 null **又**抛异常），
  该判断恒为真、跳过分支是死代码。→ 改为只看 `mid != nullptr`。
- `src/config.cpp` — `install_cpuinfo` 的缓冲区按 `body_len + head_len + 1` 分配，
  追加表头时 `'\0'` 会写到分配区外 1 字节。→ 分配 `+ 2`。
- `src/util.cpp` — 删掉未使用的 `append_str()`（`-Wunused-function`）。
- `CMakeLists.txt` — 删掉对 NDK 无意义的 `-static-libstdc++`；
  补注释说明**不能**设 `ANDROID_STL=none`（函数内静态对象需要
  `__cxa_guard_acquire/release`，来自 libc++abi）。

---

## 未修复（有意保留，已知边界）

| 项 | 原因 |
|---|---|
| `fgetc` / `getc` / `getline` / `fscanf` 未拦截 | 它们在 FILE 层内部经 `__srefill` 直读 fd，与已 hook 的 `fread`/`fgets` 混用时可能读到真实内容。要彻底解决需在 `open` 时把 fd 换成 memfd，代价与风险都更大。日常读 `/proc/cpuinfo` 极少用这些接口。 |
| `close` / `dup` / `dup2` / `dup3` 未 hook | fd 号被复用后理论上可能对无关文件返回伪装内容。补 hook 会扩大改动面，且这套路径在目标应用里未观察到；记为已知边界。 |
| `fread`/`fgets` 不更新 `FILE` 的 EOF 标志与位置 | `feof()`/`ftell()` 与实际不符。只影响这两个接口的元信息，不影响内容。 |
| `printf` 百分号后无格式化符时可能重复输出该字符 | 只影响模块自己的调试日志（`LOGD`），默认 `debug=0` 时完全不执行。 |
| `__system_property_find` 依赖 bionic `prop_info` 布局 | 该布局十年未变，但仍属实现细节。已按 128 字节/固定偏移实现并默认开启；若某天出问题，把配置里 `hook_property_find` 设为 `0` 即可退回只 hook `get`/`read`/`read_callback`。 |
| `ro.build.version.sdk` 伪装 | 会决定 Java 层 `SDK_INT`。若设得比真机低很多，应用会走低版本兼容分支。配置里已就地注明"出问题就删掉这一行"。 |
| root 隐藏 / `maps` 清理 / TEE 证明 | 不在本模块职责范围，见 README 的"已知边界"。 |

---

## 审查已确认正确的部分

- `include/zygisk.hpp` 与上游 API v5 **逐字段一致**（`module_abi`/`api_table`
  字段顺序与类型、枚举值、两个 `REGISTER_*` 宏、`entry_impl`）。
- `api->hookJniNativeMethods` 确实会把原实现写回 `methods[i].fnPtr`，
  且签名不匹配时写 `nullptr` —— 读取循环两种情况都处理了。
- 四个 `native_get*` 都是 `@FastNative`（调用约定与普通 JNI 方法一致），
  带 `(J…)` 句柄参数的 `@CriticalNative` 重载**没有**被 hook，不会崩。
- 打包流程正确：`zygisk/<abi>.so`（文件名即 ABI 名）确实是 Zygisk 的查找约定；
  `customize.sh` 的 `SKIPUNZIP=1` + `ASH_STANDALONE=1` + `$MODPATH` 兜底、
  `ARCH`→ABI 映射、`module.prop` 六个必填字段、zip 根目录布局均正确。
- arena 容量充足：默认配置 213 条属性 + 各虚拟文件缓冲 ≈ **150 KB / 512 KB**
  （`cpuinfo=@device` 时 ≈ 249 KB），约 2 倍余量。
- `include` 依赖清单完整；除 `zygisk.hpp`/`profile_data.hpp` 外无任何 C++ 标准库头。
