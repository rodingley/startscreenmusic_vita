@ Import stubs for SceSysmem's user functions (library 0x37FE725A), under our
@ own names: vitasdk's stub library for them differs between SDK versions.
@ NIDs from vita-headers' db (SceSysmem.yml).
.arm
.section .vitalink.fstubs.SceSysmem,"ax",%progbits
.align 4

.global ssm_sceKernelAllocMemBlock
.type ssm_sceKernelAllocMemBlock, %function
ssm_sceKernelAllocMemBlock:
.word 0x00000000
.word 0x37FE725A
.word 0xB9D5EBDE
.inst 0xE320F000
.size ssm_sceKernelAllocMemBlock, 16

.global ssm_sceKernelFreeMemBlock
.type ssm_sceKernelFreeMemBlock, %function
ssm_sceKernelFreeMemBlock:
.word 0x00000000
.word 0x37FE725A
.word 0xA91E15EE
.inst 0xE320F000
.size ssm_sceKernelFreeMemBlock, 16

.global ssm_sceKernelGetMemBlockBase
.type ssm_sceKernelGetMemBlockBase, %function
ssm_sceKernelGetMemBlockBase:
.word 0x00000000
.word 0x37FE725A
.word 0xB8EF5818
.inst 0xE320F000
.size ssm_sceKernelGetMemBlockBase, 16

.global ssm_sceKernelGetFreeMemorySize
.type ssm_sceKernelGetFreeMemorySize, %function
ssm_sceKernelGetFreeMemorySize:
.word 0x00000000
.word 0x37FE725A
.word 0x87CC580B
.inst 0xE320F000
.size ssm_sceKernelGetFreeMemorySize, 16

.section .note.GNU-stack,"",%progbits
