# ARM64 VDSO Build Configuration
# The VDSO gets inserted into the address space of emulated processes, so it
# needs to be compiled as an aarch64 ELF shared object. Use the LLVM cross
# toolchain (clang + lld) which can target aarch64 from macOS.

clang = find_program('/opt/homebrew/opt/llvm/bin/clang', '/usr/local/opt/llvm/bin/clang', 'clang')
check_cc = find_program('./check-cc-arm64.sh')
result = run_command(check_cc, clang, check: false)
if result.returncode() != 0
    message('\n' + result.stdout() + result.stderr())
    if build_machine.system() == 'darwin'
        install_msg = 'Install LLVM through Homebrew (brew install llvm)'
    else
        install_msg = 'Install Clang and LLD using your package manager'
    endif
    error('Could not find usable VDSO compiler. ' + install_msg)
endif
find_program('/opt/homebrew/opt/lld/bin/ld.lld', '/usr/local/opt/lld/bin/ld.lld')
vdso_compiler = [clang, '-target', 'aarch64-linux-gnu', '-fuse-ld=lld', '-B/opt/homebrew/opt/lld/bin/']

vdso = custom_target('vdso-arm64', input: ['vdso.S', 'vdso.c', 'vdso.lds'], output: 'libvdso.so.elf',
    command: vdso_compiler + ['-o', '@OUTPUT@', '@INPUT0@', '@INPUT1@',
        '-nostdlib', '-Wl,-T,@INPUT2@', '-Wl,--hash-style,sysv', '-shared', '-fPIC']
        + get_option('vdso_c_args').split())
