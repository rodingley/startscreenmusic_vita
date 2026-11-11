/* SceAppMgr's _sceAppMgrGetIdByName (library 0x8AF17416, function 0xC311CC17,
   vita-headers' db), declared here so the build doesn't depend on the
   vitasdk version's appmgr.h. Same layout as vitasdk's own stubs. */

	.arm
	.section .vitalink.fstubs.SceAppMgr,"ax",%progbits

	.align 4
	.global ssm_sceAppMgrGetIdByName
	.type ssm_sceAppMgrGetIdByName, %function
ssm_sceAppMgrGetIdByName:
	.word 0x00000000
	.word 0x8AF17416
	.word 0xC311CC17
	.inst 0xE320F000
	.size ssm_sceAppMgrGetIdByName, 16

	.section .note.GNU-stack,"",%progbits
