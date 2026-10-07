#!/usr/bin/env node
/**
 * 不依赖 CMake / Ninja 的构建脚本。
 *
 * 为什么需要它: NDK r30 已经不再自带 cmake / ninja, 只装了 NDK 的用户
 * 没法直接跑 build.sh / build.ps1。这个脚本只用 NDK 自带的 clang++ 和
 * Node 自身的 zlib, 直接编译 + 链接 + 打包成可刷入的 zip。
 *
 * 用法:
 *   node tools/build.js
 *   node tools/build.js --ndk "C:\\path\\to\\android-ndk-r30"
 *   node tools/build.js --abi all      # arm64-v8a + armeabi-v7a (默认 arm64)
 *   node tools/build.js --abi every    # 四个 ABI 全编(arm + x86, 模拟器/云机需要)
 *   node tools/build.js --abi x86_64   # 单个 ABI
 *
 * 注意: 模块只能加载与目标进程 ABI 匹配的那一个 so。如果目标环境是
 * x86/x86_64(模拟器、云手机、部分手游助手), 就必须编出对应 ABI,
 * 否则模块会"静默不加载"。
 *
 * 产物: dist/HuaweiMate80ProSpoof-<version>.zip
 */
'use strict';

const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT = path.resolve(__dirname, '..');

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------
function parseArgs(argv) {
  const out = { ndk: null, abi: 'arm64-v8a', keep: false };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--ndk') out.ndk = argv[++i];
    else if (a === '--abi') out.abi = argv[++i];
    else if (a === '--keep') out.keep = true;
    else if (a === '--help' || a === '-h') {
      console.log(fs.readFileSync(__filename, 'utf8').split('*/')[0]);
      process.exit(0);
    }
  }
  return out;
}

// ABI -> clang target triple
const ABI_TRIPLE = {
  'arm64-v8a': 'aarch64-linux-android',
  'armeabi-v7a': 'armv7a-linux-androideabi',
  x86_64: 'x86_64-linux-android',
  x86: 'i686-linux-android',
};

// --abi 的取值: all=两个 arm(真机最常用) / every=四个全要 / 或单个 ABI
const ABI_SETS = {
  all: ['arm64-v8a', 'armeabi-v7a'],
  every: ['arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86'],
  arm: ['arm64-v8a', 'armeabi-v7a'],
  x86: ['x86_64', 'x86'],
};

const API = 28; // minSdkVersion
const REQUIRED_HEADERS = [
  'android/dlext.h',
  'sys/utsname.h',
  'sys/socket.h',
  'sys/mman.h',
  'elf.h',
  'jni.h',
];

// ---------------------------------------------------------------------------
// 找到 NDK
// ---------------------------------------------------------------------------
function looksLikeNdk(dir) {
  if (!dir || !fs.existsSync(dir)) return false;
  const sp = path.join(dir, 'source.properties');
  const clang = path.join(dir, 'toolchains', 'llvm', 'prebuilt', 'windows-x86_64', 'bin', 'clang.exe');
  const clangUnix = path.join(dir, 'toolchains', 'llvm', 'prebuilt', 'linux-x86_64', 'bin', 'clang');
  return (
    (fs.existsSync(sp) && fs.readFileSync(sp, 'utf8').includes('Android NDK')) &&
    (fs.existsSync(clang) || fs.existsSync(clangUnix))
  );
}

function findNdk(explicit) {
  const candidates = [];
  const push = (p) => { if (p) candidates.push(p); };

  push(explicit);
  push(process.env.ANDROID_NDK_HOME);
  push(process.env.ANDROID_NDK_ROOT);

  // 常见位置 + 一层子目录(压缩包解压后常多一层 android-ndk-rXX)
  const bases = [
    process.env.ANDROID_HOME && path.join(process.env.ANDROID_HOME, 'ndk'),
    process.env.ANDROID_SDK_ROOT && path.join(process.env.ANDROID_SDK_ROOT, 'ndk'),
    process.env.LOCALAPPDATA && path.join(process.env.LOCALAPPDATA, 'Android', 'Sdk', 'ndk'),
    'C:\\Android\\Sdk\\ndk',
    'C:\\Android',
    path.join(process.env.USERPROFILE || 'C:\\', 'Desktop'),
    path.join(process.env.USERPROFILE || 'C:\\', 'Downloads'),
    '/usr/local/lib/android/sdk/ndk',
    '/opt/android-ndk',
  ].filter(Boolean);

  for (const base of bases) {
    if (!fs.existsSync(base)) continue;
    push(base);
    let entries = [];
    try { entries = fs.readdirSync(base); } catch { /* ignore */ }
    for (const e of entries) {
      const full = path.join(base, e);
      push(full);
      // 常见解压结果: <某目录>/android-ndk-r30-windows/android-ndk-r30/
      let sub = [];
      try { sub = fs.readdirSync(full); } catch { /* ignore */ }
      for (const s of sub) {
        const deeper = path.join(full, s);
        push(deeper);
        if (/ndk/i.test(s)) {
          let sub2 = [];
          try { sub2 = fs.readdirSync(deeper); } catch { /* ignore */ }
          for (const s2 of sub2) push(path.join(deeper, s2));
        }
      }
    }
  }

  for (const c of candidates) {
    try { if (looksLikeNdk(c)) return path.resolve(c); } catch { /* ignore */ }
  }
  return null;
}

// ---------------------------------------------------------------------------
// zip 写入(STORED + DEFLATE, 用 Node 自带 zlib)
// ---------------------------------------------------------------------------
const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();

function crc32(buf) {
  let c = 0 ^ -1;
  for (let i = 0; i < buf.length; i++) c = (c >>> 8) ^ CRC_TABLE[(c ^ buf[i]) & 0xff];
  return (c ^ -1) >>> 0;
}

function u16(v) { const b = Buffer.alloc(2); b.writeUInt16LE(v >>> 0); return b; }
function u32(v) { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); return b; }

function writeZip(zipPath, files) {
  const local = [];
  const central = [];
  let offset = 0;

  for (const f of files) {
    const raw = f.data;
    const crc = crc32(raw);
    const method = raw.length > 0 ? 8 : 0; // 8 = deflate(0 长度必须 store)
    const body = method === 8 ? zlib.deflateRawSync(raw, { level: 9 }) : raw;
    // 压缩没变小就用 STORED
    const useDeflate = method === 8 && body.length < raw.length;
    const finalBody = useDeflate ? body : raw;
    const finalMethod = useDeflate ? 8 : 0;

    const name = Buffer.from(f.name.replace(/\\/g, '/'), 'utf8');
    const lh = Buffer.concat([
      u32(0x04034b50), u16(20), u16(0x0800), u16(finalMethod),
      u16(0), u16(0),                    // time, date (固定值, 保证可复现)
      u32(crc), u32(finalBody.length), u32(raw.length),
      u16(name.length), u16(0),
      name,
    ]);
    local.push(lh, finalBody);

    central.push(Buffer.concat([
      u32(0x02014b50), u16(20), u16(20), u16(0x0800), u16(finalMethod),
      u16(0), u16(0),
      u32(crc), u32(finalBody.length), u32(raw.length),
      u16(name.length), u16(0), u16(0),   // name, extra, comment
      u16(0), u16(0), u32(0),             // disk, int attr, ext attr
      u32(offset),
      name,
    ]));
    offset += lh.length + finalBody.length;
  }

  const centralBuf = Buffer.concat(central);
  const eocd = Buffer.concat([
    u32(0x06054b50), u16(0), u16(0),
    u16(files.length), u16(files.length),
    u32(centralBuf.length), u32(offset), u16(0),
  ]);

  const out = Buffer.concat([...local, centralBuf, eocd]);
  fs.mkdirSync(path.dirname(zipPath), { recursive: true });
  fs.writeFileSync(zipPath, out);
  return out.length;
}

function walk(dir, base = dir, acc = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, e.name);
    if (e.isDirectory()) walk(full, base, acc);
    else acc.push({ name: path.relative(base, full).replace(/\\/g, '/'), full });
  }
  return acc;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
function fail(msg) {
  console.error(`\n[错误] ${msg}\n`);
  process.exit(1);
}

function main() {
  const args = parseArgs(process.argv);
  console.log('=== HUAWEI Mate 80 Pro 伪装模块 — 构建 ===\n');

  const ndk = findNdk(args.ndk);
  if (!ndk) {
    fail(
      '找不到 Android NDK。请用 --ndk <路径> 指定, 或设置环境变量 ANDROID_NDK_HOME。\n' +
      '  NDK 必须包含 source.properties 与 toolchains/llvm/prebuilt/<host>/bin/clang。'
    );
  }
  const spText = fs.readFileSync(path.join(ndk, 'source.properties'), 'utf8');
  const rev = (spText.match(/Pkg\.Revision\s*=\s*(.+)/) || [])[1] || 'unknown';
  console.log(`NDK: ${ndk}  (revision ${rev.trim()})`);

  // host tag
  const isWin = process.platform === 'win32';
  const hostTag =
    process.platform === 'win32' ? 'windows-x86_64'
    : process.platform === 'darwin' ? 'darwin-x86_64'
    : 'linux-x86_64';
  const binDir = path.join(ndk, 'toolchains', 'llvm', 'prebuilt', hostTag, 'bin');
  const sysroot = path.join(ndk, 'toolchains', 'llvm', 'prebuilt', hostTag, 'sysroot');
  const clang = path.join(binDir, isWin ? 'clang.exe' : 'clang');
  if (!fs.existsSync(clang)) fail(`找不到 clang: ${clang}`);
  if (!fs.existsSync(sysroot)) fail(`找不到 sysroot: ${sysroot}`);

  // 关键头文件体检 —— 给出明确报错而不是让 clang 抛一堆找不到头文件
  const incDir = path.join(sysroot, 'usr', 'include');
  const missing = REQUIRED_HEADERS.filter((h) => !fs.existsSync(path.join(incDir, h)));
  if (missing.length) fail(`NDK sysroot 缺少头文件: ${missing.join(', ')}\n  (${incDir})`);
  console.log('头文件体检: 通过');

  const abis = ABI_SETS[args.abi] || [args.abi];
  for (const a of abis) if (!ABI_TRIPLE[a]) fail(`不支持的 ABI: ${a}`);

  // 编译器路径: 直接用 clang++.exe, 而不是 aarch64-linux-android28-clang++.cmd。
  // .cmd 是批处理包装, Node 在 Windows 上 spawn 它需要 shell:true, 很容易出
  // "返回 0 但什么都没做" 这种诡异现象; 反正我们用 --target= 显式指定目标,
  // 不依赖包装脚本。
  const compiler = path.join(binDir, isWin ? 'clang++.exe' : 'clang++');
  if (!fs.existsSync(compiler)) fail(`找不到编译器: ${compiler}`);

  // ---- 0. 编译器自检 ----
  // 先确认这个 clang++ 真的能跑、真的能写出文件。
  // 之前的症状是"返回码 0、零输出、零产物" —— 必须早点发现,
  // 而不是等到 11 个文件都"编译成功"之后才在链接处爆出莫名其妙的错误。
  {
    const probeDir = path.join(ROOT, 'build', 'probe');
    fs.mkdirSync(probeDir, { recursive: true });
    const probeSrc = path.join(probeDir, 'probe.cpp');
    const probeObj = path.join(probeDir, 'probe.o');
    fs.writeFileSync(probeSrc, 'int hw80_probe() { return 42; }\n', 'utf8');
    fs.rmSync(probeObj, { force: true });

    console.log('\n=== 编译器自检 ===');
    const vr = spawnSync(compiler, ['--version'], { encoding: 'utf8', timeout: 60000 });
    const verOut = ((vr.stdout || '') + (vr.stderr || '')).trim();
    if (verOut) console.log('  ' + verOut.split('\n')[0]);
    else console.log('  (--version 没有任何输出 — 编译器可能无法正常运行)');

    const pr = spawnSync(compiler, [
      `--target=${ABI_TRIPLE[abis[0]]}${API}`,
      `--sysroot=${sysroot}`,
      '-std=c++20', '-c', probeSrc, '-o', probeObj,
    ], { encoding: 'utf8', timeout: 120000 });
    const prOut = ((pr.stdout || '') + (pr.stderr || '')).trim();
    if (prOut) console.log(prOut.split('\n').map((l) => '    ' + l).join('\n'));

    const probeOk = fs.existsSync(probeObj) && fs.statSync(probeObj).size > 0;
    if (!probeOk) {
      console.error('\n[错误] 编译器自检失败: 无法产出一个最简单的 .o 文件。');
      console.error(`  编译器     : ${compiler}`);
      console.error(`  返回码     : ${pr.status}   signal: ${pr.signal}`);
      console.error(`  spawn 错误 : ${pr.error ? `${pr.error.code || ''} ${pr.error.message}` : '(无)'}`);
      console.error(`  编译器输出 : ${prOut ? '(见上)' : '(空 — 编译器什么都没打印)'}`);
      console.error(`  探查源文件 : ${probeSrc}`);
      console.error('\n  这通常意味着编译器进程没有真正执行 (被安全软件拦截、缺少运行库,');
      console.error('  或者 exe 本身损坏)。请手工执行这一条以确认:\n');
      console.error(`  "${compiler}" --version\n`);
      process.exit(1);
    }
    console.log(`  自检通过 (.o ${fs.statSync(probeObj).size} 字节)\n`);
  }

  // ---- 1. 生成内嵌配置 ----
  const genDir = path.join(ROOT, 'build', 'generated');
  fs.mkdirSync(genDir, { recursive: true });
  const py = findPython();
  if (!py) fail('找不到 python3/python, 无法生成内嵌配置(profile_data.cpp)。');
  const gen = spawnSync(
    py,
    [
      path.join(ROOT, 'tools', 'embed_profile.py'),
      genDir,
      path.join(ROOT, 'config', 'hw80pro.conf'),
      path.join(ROOT, 'config', 'proc_cpuinfo.txt'),
    ],
    { stdio: 'inherit' }
  );
  if (gen.status !== 0) fail('生成 profile_data.cpp 失败');

  // ---- 2. 逐个 ABI 编译 ----
  const SOURCES = [
    'zygisk_module.cpp', 'companion.cpp', 'config.cpp', 'spoof_db.cpp', 'util.cpp',
    'plt_hook.cpp', 'hook_property.cpp', 'hook_file.cpp', 'hook_misc.cpp', 'hook_jni.cpp',
  ].map((f) => path.join(ROOT, 'src', f));
  SOURCES.push(path.join(genDir, 'profile_data.cpp'));

  const outputs = {};
  for (const abi of abis) {
    const triple = ABI_TRIPLE[abi];
    const target = `${triple}${API}`;

    console.log(`\n=== 编译 ${abi} (${target}) ===`);
    const objDir = path.join(ROOT, 'build', abi);
    fs.mkdirSync(objDir, { recursive: true });

    const common = [
      `--target=${target}`,
      `--sysroot=${sysroot}`,
      '-std=c++20',
      '-O2',
      '-DNDEBUG',
      '-fno-exceptions',
      '-fno-rtti',
      '-fvisibility=hidden',
      '-fvisibility-inlines-hidden',
      '-ffunction-sections',
      '-fdata-sections',
      '-Wall',
      '-Wextra',
      '-Wno-unused-parameter',
      `-I${path.join(ROOT, 'include')}`,
      `-I${path.join(ROOT, 'src')}`,
    ];

    const objects = [];
    let failed = false;
    for (const src of SOURCES) {
      const obj = path.join(objDir, path.basename(src).replace(/\.cpp$/, '.o'));
      objects.push(obj);
      const args = [...common, '-c', src, '-o', obj];
      console.log(`  CC ${path.basename(src)}`);
      fs.rmSync(obj, { force: true });
      const r = spawnSync(compiler, args, { encoding: 'utf8', timeout: 300000 });
      const out = ((r.stdout || '') + (r.stderr || '')).trim();
      if (out) console.log(out.split('\n').map((l) => '      ' + l).join('\n'));

      if (r.error) {
        fail(`无法启动编译器: ${r.error.code || ''} ${r.error.message}\n  命令: "${compiler}"`);
      }
      // 关键断言: 返回 0 不代表编出来了。子进程被拦截 / 空跑时,
      // 退出码同样是 0, 只有检查产物才能发现。
      const objOk = fs.existsSync(obj) && fs.statSync(obj).size > 0;
      if (r.status !== 0 || !objOk) {
        console.error(`\n[错误] 编译 ${src} 失败。`);
        console.error(`  返回码     : ${r.status}   signal: ${r.signal}`);
        console.error(`  产出 .o    : ${objOk ? '有' : '无'}`);
        console.error(`  编译器输出 : ${out ? '(见上)' : '(空 — 编译器什么都没打印)'}`);
        console.error(`  编译器     : ${compiler}`);
        if (r.error) console.error(`  spawn 错误 : ${r.error.code || ''} ${r.error.message}`);
        console.error('\n  请在 cmd 里手工执行下面这条, 看编译器到底有没有反应:\n');
        console.error(`  "${compiler}" --version\n`);
        failed = true;
        break;
      }
      process.stdout.write(`      -> ${(fs.statSync(obj).size / 1024).toFixed(1)} KB\n`);
    }
    if (failed) process.exit(1);

    const soPath = path.join(ROOT, 'build', abi, 'libhw80pro.so');
    const linkArgs = [
      `--target=${target}`,
      `--sysroot=${sysroot}`,
      '-shared',
      '-o', soPath,
      ...objects,
      // 注意: 不要加 -static-libstdc++。NDK 里没有 libstdc++, 该标志是
      // GCC 兼容残留, 在 NDK 上是 no-op 且可能干扰驱动; 需要的 libc++ 由
      // clang 驱动自动按 -stdlib= 默认值链接。
      '-Wl,--gc-sections',
      '-Wl,-z,now',
      '-Wl,-z,relro',
      '-Wl,-z,max-page-size=16384',
      '-llog', '-ldl', '-landroid',
    ];

    console.log(`  LINK libhw80pro.so`);
    fs.rmSync(soPath, { force: true });

    // 加 -v: 万一失败, 输出里会包含完整的库搜索路径, 便于定位。
    const linkArgsVerbose = [...linkArgs, '-v'];
    const lr = spawnSync(compiler, linkArgsVerbose, { encoding: 'utf8', timeout: 600000 });
    const linkOut = ((lr.stdout || '') + (lr.stderr || '')).trim();
    if (linkOut) console.log(linkOut.split('\n').map((l) => '    ' + l).join('\n'));

    if (lr.error) {
      fail(`无法启动链接器: ${lr.error.code || ''} ${lr.error.message}`);
    }
    if (lr.status !== 0) {
      console.error(`\n[错误] 链接器返回 ${lr.status}。完整命令(在项目根目录执行):\n`);
      console.error(`  "${compiler}" ${linkArgsVerbose.map((s) => (/[\\ ]/.test(s) ? `"${s}"` : s)).join(' ')}\n`);
      process.exit(1);
    }
    if (!fs.existsSync(soPath)) {
      console.error('\n[错误] 链接器返回 0 但没有生成输出文件, 这不应该发生。');
      console.error(`  期望路径: ${soPath}`);
      console.error(`  目录内容: ${fs.existsSync(path.dirname(soPath)) ? fs.readdirSync(path.dirname(soPath)).join(', ') : '(目录不存在)'}`);
      console.error(`  命令行长度: ${linkArgsVerbose.join(' ').length} 字符\n`);
      process.exit(1);
    }
    const size = fs.statSync(soPath).size;
    console.log(`  完成: ${soPath} (${(size / 1024).toFixed(1)} KB)`);
    outputs[abi] = soPath;

    // 检查是否导出了 zygisk 入口符号
    const nm = path.join(binDir, isWin ? 'llvm-nm.exe' : 'llvm-nm');
    if (fs.existsSync(nm)) {
      const nr = spawnSync(nm, ['-D', '--defined-only', soPath], { encoding: 'utf8' });
      const syms = (nr.stdout || '');
      const need = ['zygisk_module_entry', 'zygisk_companion_entry'];
      for (const s of need) {
        if (!syms.includes(s)) {
          console.error(`\n[警告] ${abi} 的 so 里没有导出 ${s} —— 模块不会被 Zygisk 加载!`);
        }
      }
      if (need.every((s) => syms.includes(s))) console.log(`  符号检查: ${need.join(', ')} 均已导出`);
    }
  }

  // ---- 3. 打包 ----
  const version = (fs.readFileSync(path.join(ROOT, 'module.prop'), 'utf8')
    .match(/^version=(.+)$/m) || [])[1].trim();

  const stage = path.join(ROOT, 'build', 'stage');
  fs.rmSync(stage, { recursive: true, force: true });
  fs.mkdirSync(path.join(stage, 'zygisk'), { recursive: true });

  const copyIf = (rel, destRel = rel) => {
    const s = path.join(ROOT, rel);
    if (!fs.existsSync(s)) return;
    const d = path.join(stage, destRel);
    fs.mkdirSync(path.dirname(d), { recursive: true });
    fs.cpSync(s, d, { recursive: true });
  };
  for (const f of ['module.prop', 'customize.sh', 'post-fs-data.sh', 'service.sh',
                   'uninstall.sh', 'action.sh', 'webroot']) copyIf(f);
  copyIf('config/hw80pro.conf', 'config/hw80pro.conf');
  copyIf('config/proc_cpuinfo.txt', 'config/proc_cpuinfo.txt');
  for (const abi of abis) {
    fs.copyFileSync(outputs[abi], path.join(stage, 'zygisk', `${abi}.so`));
  }

  const files = walk(stage).map((f) => ({ name: f.name, data: fs.readFileSync(f.full) }));
  const zipPath = path.join(ROOT, 'dist', `HuaweiMate80ProSpoof-${version}.zip`);
  const zipSize = writeZip(zipPath, files);

  console.log(`\n=== 完成 ===`);
  console.log(`  ${zipPath}  (${(zipSize / 1024).toFixed(1)} KB)`);
  console.log(`  打包内容:`);
  for (const f of files) console.log(`    ${f.name}`);
  console.log(`\n把它传到手机上, 在 Magisk / KernelSU / APatch 里"从本地安装", 然后重启。`);
}

function findPython() {
  // 这里只需要一个能跑 embed_profile.py 的 Python。优先用调用方给的 PYTHON,
  // 其次 PATH, 最后回退到 DSH 运行时自带的便携 Python(用户机器上通常没有
  // 全局安装 Python, 但 DSH 一定带一个)。
  const home = process.env.USERPROFILE || process.env.HOME || '';
  const bundled = home
    ? path.join(home, '.dsh', 'dsh-runtimes', 'dsh-primary-runtime',
                'dependencies', 'python', 'python.exe')
    : null;

  const cands = [
    process.env.PYTHON,
    process.platform === 'win32' ? 'python' : 'python3',
    process.platform === 'win32' ? 'python3' : 'python',
    process.env.LOCALAPPDATA && path.join(process.env.LOCALAPPDATA, 'Programs', 'Python',
                                          'Python312', 'python.exe'),
    bundled,
  ].filter(Boolean);

  for (const c of cands) {
    try {
      const r = spawnSync(c, ['-c', 'print(1)'], { encoding: 'utf8' });
      if (!r.error && r.status === 0) return c;
    } catch { /* 继续试下一个 */ }
  }
  return null;
}

main();
