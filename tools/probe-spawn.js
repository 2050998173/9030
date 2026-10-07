// 一次性探针: 在这个沙箱里能否真的启动一个进程?
// 背景: pwsh 每次启动都以 0xC0000142 (STATUS_DLL_INIT_FAILED) 死掉,
// 所以先只验证"spawn 一个 .exe 并拿到退出码"这条最小路径。
//
// 用法: node tools/probe-spawn.js
const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const NDK = process.argv[2] || 'C:\\Users\\Administrator\\Desktop\\android-ndk-r30-windows\\android-ndk-r30';
const BIN = path.join(NDK, 'toolchains', 'llvm', 'prebuilt', 'windows-x86_64', 'bin');
const CLANG = path.join(BIN, 'clang.exe');

function report(label, exe, args) {
  process.stdout.write(`\n=== ${label} ===\n  exe: ${exe}\n`);
  if (!fs.existsSync(exe)) {
    process.stdout.write('  结果: 可执行文件不存在\n');
    return;
  }
  let st = null;
  try { st = fs.statSync(exe); } catch (e) { /* ignore */ }
  if (st) process.stdout.write(`  大小: ${st.size} 字节\n`);

  const r = spawnSync(exe, args, { encoding: 'utf8', timeout: 60000 });
  process.stdout.write(`  error: ${r.error ? `${r.error.code || ''} ${r.error.message}` : '(无)'}\n`);
  process.stdout.write(`  status: ${r.status}  signal: ${r.signal}\n`);
  const out = ((r.stdout || '') + (r.stderr || '')).trim();
  if (out) process.stdout.write(`  输出:\n${out.split('\n').map((l) => '    ' + l).join('\n')}\n`);
}

report('clang --version', CLANG, ['--version']);
report('node 自身 (对照)', process.execPath, ['-e', 'console.log("node ok", process.version)']);

// NDK 版本文件
const sp = path.join(NDK, 'source.properties');
process.stdout.write(`\n=== source.properties ===\n`);
try {
  process.stdout.write(fs.readFileSync(sp, 'utf8'));
} catch (e) {
  process.stdout.write(`  读取失败: ${e.message}\n`);
}
