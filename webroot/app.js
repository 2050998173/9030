/*
 * WebUI 脚本。刻意不依赖任何打包器 / 框架 —— 直接静态文件即可用。
 *
 * 不同管理器暴露的 shell 桥不一样, 这里做一次兼容:
 *   KernelSU / APatch : window.ksu.exec(cmd, opts)  (回调整形式, 也有 Promise 版本)
 *   Magisk (v27+)     : 需要官方 WebUI 的 index.js, 本模块不附带, 因此退化为
 *                       "只读提示", 不报错。
 */
(function () {
  'use strict';

  var STATUS_FILE = '/data/local/tmp/hw80pro_status.txt';
  var DATA_DIR = '/data/adb/hw80pro';

  var statusEl = document.getElementById('status');
  var hintEl = document.getElementById('exec-hint');
  var subtitleEl = document.getElementById('subtitle');

  // ---------------------------------------------------------------- shell 桥
  function ksuExec(cmd) {
    return new Promise(function (resolve, reject) {
      if (!window.ksu || typeof window.ksu.exec !== 'function') {
        reject(new Error('NO_BRIDGE'));
        return;
      }
      try {
        window.ksu.exec(cmd, JSON.stringify({ cwd: '/' }), function (code, stdout, stderr) {
          if (code === 0 || (stdout && stdout.length)) {
            resolve({ code: code, stdout: stdout || '', stderr: stderr || '' });
          } else {
            reject(new Error(stderr || ('exit ' + code)));
          }
        });
      } catch (e) {
        reject(e);
      }
    });
  }

  function hasBridge() {
    return !!(window.ksu && typeof window.ksu.exec === 'function');
  }

  function setStatus(text) {
    statusEl.textContent = text;
  }

  function loadStatus() {
    if (!hasBridge()) {
      setStatus(
        '当前环境没有可用的 WebUI shell 桥, 无法自动读取状态。\n\n' +
        '你仍然可以:\n' +
        '  1. 在管理器里点击本模块的"操作"按钮, 会生成状态报告\n' +
        '  2. 用 adb / 终端直接查看: cat ' + STATUS_FILE + '\n'
      );
      return;
    }
    setStatus('正在读取…');
    ksuExec('cat ' + STATUS_FILE + ' 2>/dev/null || echo "尚未生成报告, 请点上面的按钮"')
      .then(function (r) { setStatus(r.stdout || '(空)'); })
      .catch(function (e) { setStatus('读取失败: ' + e.message); });
  }

  function runReport() {
    if (!hasBridge()) {
      setStatus('当前环境不支持执行 shell, 请在管理器里点击模块的"操作"按钮。');
      return;
    }
    setStatus('正在生成报告…');
    var modDir = '/data/adb/modules/hw80pro_spoof';
    ksuExec('sh ' + modDir + '/action.sh')
      .then(function (r) { setStatus(r.stdout || '(无输出)'); })
      .catch(function (e) { setStatus('执行失败: ' + e.message); });
  }

  document.getElementById('btn-refresh').addEventListener('click', loadStatus);
  document.getElementById('btn-run').addEventListener('click', runReport);

  if (hasBridge()) {
    hintEl.textContent = '已检测到 KernelSU / APatch WebUI 接口。';
    subtitleEl.textContent = 'Zygisk 模块 · WebUI 可用';
    loadStatus();
  } else {
    hintEl.textContent = '未检测到 WebUI shell 接口(仅提示, 不影响模块功能)。';
    subtitleEl.textContent = 'Zygisk 模块 · 只读模式';
    loadStatus();
  }
})();
