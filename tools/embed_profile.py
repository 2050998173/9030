#!/usr/bin/env python3
"""把 config/ 下的文本文件转成 C++ 源码, 编译进 libhw80pro.so。

故意不用 CMake 的 file(READ) + configure_file: 那样要处理分号转义和
路径反斜杠, 在 Windows 上很容易出错。raw string 字面量最稳。

生成: <out_dir>/profile_data.cpp   (定义 const StrRef kDefaultConf / kDefaultCpuInfo)
"""
import os
import sys

HEADER = '''// 由 tools/embed_profile.py 自动生成, 请勿手工修改。
// 源文件: config/hw80pro.conf, config/proc_cpuinfo.txt
#include "profile_data.hpp"

namespace hw80 {

'''

FOOTER = '''
} // namespace hw80
'''


def emit(out_dir: str, conf_path: str, cpuinfo_path: str) -> None:
    with open(conf_path, 'r', encoding='utf-8') as f:
        conf = f.read()
    with open(cpuinfo_path, 'r', encoding='utf-8') as f:
        cpuinfo = f.read()

    for name, text in (('kDefaultConf', conf), ('kDefaultCpuInfo', cpuinfo)):
        if ')#' in text:
            raise SystemExit(f'{name}: 内容里不允许出现 ")#" 序列 (会破坏 raw string)')

    body = []
    for name, text in (('kDefaultConf', conf), ('kDefaultCpuInfo', cpuinfo)):
        body.append('namespace {\nconstexpr char ' + name + '_data[] = R"#(' + text + ')#";\n}\n')
        body.append('const StrRef ' + name + '{' + name + '_data, sizeof(' + name +
                    '_data) - 1};\n\n')

    os.makedirs(out_dir, exist_ok=True)
    out_file = os.path.join(out_dir, 'profile_data.cpp')
    with open(out_file, 'w', encoding='utf-8', newline='\n') as f:
        f.write(HEADER)
        f.write(''.join(body))
        f.write(FOOTER)
    print(f'[embed] {out_file}  ({len(conf)} B conf, {len(cpuinfo)} B cpuinfo)')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit('usage: embed_profile.py <out_dir> <conf> <cpuinfo>')
    emit(sys.argv[1], sys.argv[2], sys.argv[3])
