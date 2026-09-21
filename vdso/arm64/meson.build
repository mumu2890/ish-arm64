# ARM64 VDSO Build Configuration
# macOS ld64 不支持 ELF 链接，直接用 stub

vdso = custom_target('vdso-arm64-stub',
    output: 'libvdso.so.elf',
    command: ['touch', '@OUTPUT@'])
