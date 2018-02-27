#include <cstdio>

#include "haxm.h"

void printRegs(struct vcpu_state_t *regs) {
	printf("EAX = %08x   EBX = %08x   ECX = %08x   EDX = %08x   ESI = %08x   EDI = %08x  EFER = %08x\n", regs->_eax, regs->_ebx, regs->_ecx, regs->_edx, regs->_esi, regs->_edi, regs->_efer);
	printf("CR0 = %08x   CR2 = %08x   CR3 = %08x   CR4 = %08x   ESP = %08x   EBP = %08x   GDT = %08x:%04x\n", regs->_cr0, regs->_cr2, regs->_cr3, regs->_cr4, regs->_esp, regs->_ebp, regs->_gdt.base, regs->_gdt.limit);
	printf("DR0 = %08x   DR1 = %08x   DR2 = %08x   DR3 = %08x   DR6 = %08x   DR7 = %08x   IDT = %08x:%04x\n", regs->_dr0, regs->_dr1, regs->_dr2, regs->_dr3, regs->_dr6, regs->_dr7, regs->_idt.base, regs->_idt.limit);
	printf(" CS = %04x   DS = %04x   ES = %04x   FS = %04x   GS = %04x   SS = %04x   TR = %04x   PDE = %08x   LDT = %08x:%04x\n", regs->_cs.selector, regs->_ds.selector, regs->_es.selector, regs->_fs.selector, regs->_gs.selector, regs->_ss.selector, regs->_tr.selector, regs->_pde, regs->_ldt.base, regs->_ldt.limit);
	printf("EIP = %08x   EFLAGS = %08x\n", regs->_eip, regs->_eflags);
}

void printFPURegs(struct fx_layout *fpu) {
	printf("FCW =     %04x   FSW =     %04x   FTW =       %02x   FOP =     %04x   MXCSR = %08x\n", fpu->fcw, fpu->fsw, fpu->ftw, fpu->fop, fpu->mxcsr);
	printf("FIP = %08x   FCS =     %04x   FDP = %08x   FDS = %08x    mask = %08x\n", fpu->fip, fpu->fcs, fpu->fdp, fpu->fds, fpu->mxcsr_mask);
	for (int i = 0; i < 8; i++) {
		printf("  ST%d = %010x   %+.20Le\n", i, *(uint64_t *)&fpu->st_mm[i], *(long double *)&fpu->st_mm[i]);
	}
	for (int i = 0; i < 8; i++) {
		printf(" XMM%d = %08x %08x %08x %08x     %+.7e %+.7e %+.7e %+.7e     %+.15le %+.15le\n", i,
			*(uint32_t *)&fpu->mmx_1[i][0], *(uint32_t *)&fpu->mmx_1[i][4], *(uint32_t *)&fpu->mmx_1[i][8], *(uint32_t *)&fpu->mmx_1[i][12],
			*(float *)&fpu->mmx_1[i][0], *(float *)&fpu->mmx_1[i][4], *(float *)&fpu->mmx_1[i][8], *(float *)&fpu->mmx_1[i][12],
			*(double *)&fpu->mmx_1[i][0],  *(double *)&fpu->mmx_1[i][8]
		);
	}
}

#define DO_MANUAL_JMP

int main() {
	// Allocate memory for the RAM and ROM
	const uint32_t ramSize = 256 * 4096; // 1 MiB
	const uint32_t ramBase = 0x00000000;
	const uint32_t romSize = 16 * 4096; // 64 KiB
	const uint32_t romBase = 0xFFFF0000;

	char *ram;
	ram = (char *)_aligned_malloc(ramSize, 0x1000);
	memset(ram, 0, ramSize);

	char *rom;
	rom = (char *)_aligned_malloc(romSize, 0x1000);
	memset(rom, 0xf4, romSize);

	// Write a simple program to ROM
	{
		uint32_t addr;
		#define emit(buf, code) {memcpy(&buf[addr], code, sizeof(code) - 1); addr += sizeof(code) - 1;}
		
		// Jump to initialization code and define GDT/IDT table pointer
		addr = 0xfff0;
		emit(rom, "\xeb\xc6");                         // [0xfff0] jmp    short 0x1b8
		emit(rom, "\x18\x00\xd8\xff\xff\xff");         // [0xfff2] GDT pointer: 0xffffffd8:0x0018
		
		// GDT/IDT talbe
		addr = 0xffd8;
		emit(rom, "\x00\x00\x00\x00\x00\x00\x00\x00"); // [0xffd8] GDT entry 0: null
		emit(rom, "\xff\xff\x00\x00\x00\x9b\xcf\x00"); // [0xffe0] GDT entry 1: code (full access to 4 GB linear space)
		emit(rom, "\xff\xff\x00\x00\x00\x93\xcf\x00"); // [0xffe8] GDT entry 2: data (full access to 4 GB linear space)

		// Load GDT/IDT tables
		addr = 0xffb8;
		emit(rom, "\x66\x2e\x0f\x01\x16\xf2\xff");     // [0xffb8] lgdt   [cs:0xfff2]
		emit(rom, "\x66\x2e\x0f\x01\x1e\xf2\xff");     // [0xffbf] lidt   [cs:0xfff2]

		// Enter protected mode
		emit(rom, "\x0f\x20\xc0");                     // [0xffc6] mov    eax, cr0
		emit(rom, "\x0c\x01");                         // [0xffc9] or      al, 1
		emit(rom, "\x0f\x22\xc0");                     // [0xffcb] mov    cr0, eax
		#ifdef DO_MANUAL_JMP
		emit(rom, "\xf4")                              // [0xffce] hlt
		#else
		emit(rom, "\x66\xea\x00\xff\xff\xff\x08\x00"); // [0xffce] jmp    dword 0x8:0xffffff00
		#endif
		
		// Prepare memory for paging
		// (based on https://github.com/unicorn-engine/unicorn/blob/master/tests/unit/test_x86_soft_paging.c)
		// 0x1000 = Page directory
		// 0x2000 = Page table (identity map RAM)
		// 0x3000 = Page table (identity map ROM)
		// 0x4000 = Page table (0x10000xxx -> 0x00004xxx)
		// 0x5000 = Data area (first dword reads 0xdeadbeef)

		// Load segment registers
		addr = 0xff00;
		emit(rom, "\x33\xc0");                         // [0xff00] xor    eax, eax
		emit(rom, "\xb0\x10");                         // [0xff02] mov     al, 0x10
		emit(rom, "\x8e\xd8");                         // [0xff04] mov     ds, eax
		emit(rom, "\x8e\xc0");                         // [0xff06] mov     es, eax
		emit(rom, "\x8e\xd0");                         // [0xff08] mov     ss, eax

		// Clear page directory
		emit(rom, "\xbf\x00\x10\x00\x00");             // [0xff0a] mov    edi, 0x1000
		emit(rom, "\xb9\x00\x10\x00\x00");             // [0xff0f] mov    ecx, 0x1000
		emit(rom, "\x31\xc0");                         // [0xff14] xor    eax, eax
		emit(rom, "\xf3\xab");                         // [0xff16] rep    stosd

		// Write 0xdeadbeef at physical memory address 0x5000
		emit(rom, "\xbf\x00\x50\x00\x00");             // [0xff18] mov    edi, 0x5000
		emit(rom, "\xb8\xef\xbe\xad\xde");             // [0xff1d] mov    eax, 0xdeadbeef
		emit(rom, "\x89\x07");                         // [0xff22] mov    [edi], eax

        // Identity map the RAM
		emit(rom, "\xb9\x00\x01\x00\x00");             // [0xff24] mov    ecx, 0x100
		emit(rom, "\xbf\x00\x20\x00\x00");             // [0xff29] mov    edi, 0x2000
		emit(rom, "\xb8\x03\x00\x00\x00");             // [0xff2e] mov    eax, 0x0003
        // aLoop:
		emit(rom, "\xab");                             // [0xff33] stosd
		emit(rom, "\x05\x00\x10\x00\x00");             // [0xff34] add    eax, 0x1000
		emit(rom, "\xe2\xf8");                         // [0xff39] loop   aLoop

        // Identity map the ROM
		emit(rom, "\xb9\x10\x00\x00\x00");             // [0xff3b] mov    ecx, 0x10
		emit(rom, "\xbf\xc0\x3f\x00\x00");             // [0xff40] mov    edi, 0x3fc0
		emit(rom, "\xb8\x03\x00\xff\xff");             // [0xff45] mov    eax, 0xffff0003
        // bLoop:
		emit(rom, "\xab");                             // [0xff4a] stosd
		emit(rom, "\x05\x00\x10\x00\x00");             // [0xff4b] add    eax, 0x1000
		emit(rom, "\xe2\xf8");                         // [0xff50] loop   bLoop

        // Map physical address 0x5000 to virtual address 0x10000000
		emit(rom, "\xbf\x00\x40\x00\x00");             // [0xff52] mov    edi, 0x4000
		emit(rom, "\xb8\x03\x50\x00\x00");             // [0xff57] mov    eax, 0x5003
		emit(rom, "\x89\x07");                         // [0xff5c] mov    [edi], eax

        // Add page tables into page directory
		emit(rom, "\xbf\x00\x10\x00\x00");             // [0xff5e] mov    edi, 0x1000
		emit(rom, "\xb8\x03\x20\x00\x00");             // [0xff63] mov    eax, 0x2003
		emit(rom, "\x89\x07");                         // [0xff68] mov    [edi], eax
		emit(rom, "\xbf\xfc\x1f\x00\x00");             // [0xff6a] mov    edi, 0x1ffc
		emit(rom, "\xb8\x03\x30\x00\x00");             // [0xff6f] mov    eax, 0x3003
		emit(rom, "\x89\x07");                         // [0xff74] mov    [edi], eax
		emit(rom, "\xbf\x00\x11\x00\x00");             // [0xff76] mov    edi, 0x1100
		emit(rom, "\xb8\x03\x40\x00\x00");             // [0xff7b] mov    eax, 0x4003
		emit(rom, "\x89\x07");                         // [0xff80] mov    [edi], eax

        // Load the page directory register
		emit(rom, "\xb8\x00\x10\x00\x00");             // [0xff82] mov    eax, 0x1000
		emit(rom, "\x0f\x22\xd8");                     // [0xff87] mov    cr3, eax

        // Enable paging
		emit(rom, "\x0f\x20\xc0");                     // [0xff8a] mov    eax, cr0
		emit(rom, "\x0d\x00\x00\x00\x80");             // [0xff8d] or     eax, 0x80000000
		emit(rom, "\x0f\x22\xc0");                     // [0xff92] mov    cr0, eax

        // Clear EAX
		emit(rom, "\x31\xc0");                         // [0xff95] xor    eax, eax

        // Load using virtual memory address; EAX = 0xdeadbeef
		emit(rom, "\xbe\x00\x00\x00\x10");             // [0xff97] mov    esi, 0x10000000
		emit(rom, "\x8b\x06");                         // [0xff9c] mov    eax, [esi]

		// Stop
		emit(rom, "\xf4");                             // [0xff9e] hlt

		#undef emit
	}

	Haxm haxm;

	HaxmStatus status = haxm.Initialize();
	switch (status) {
	case HXS_NOT_FOUND: printf("HAXM module not found: %d\n", haxm.GetLastError()); return -1;
	case HXS_INIT_FAILED: printf("Failed to open HAXM device %d\n", haxm.GetLastError()); return -1;
	default: break;
	}

	// Check version
	auto ver = haxm.GetModuleVersion();
	printf("HAXM module loaded\n");
	printf("  Current version: %d\n", ver->cur_version);
	printf("  Compatibility version: %d\n", ver->compat_version);

	// Check capabilities
	auto caps = haxm.GetCapabilities();
	printf("\nCapabilities:\n");
	if (caps->wstatus & HAX_CAP_STATUS_WORKING) {
		printf("  HAXM can be used on this host\n");
		if (caps->winfo & HAX_CAP_FASTMMIO) {
			printf("  HAXM kernel module supports fast MMIO\n");
		}
		if (caps->winfo & HAX_CAP_EPT) {
			printf("  Host CPU supports Extended Page Tables (EPT)\n");
		}
		if (caps->winfo & HAX_CAP_UG) {
			printf("  Host CPU supports Unrestricted Guest (UG)\n");
		}
		if (caps->winfo & HAX_CAP_64BIT_SETRAM) {
			printf("  64-bit RAM functionality is present\n");
		}
		if ((caps->wstatus & HAX_CAP_MEMQUOTA) && caps->mem_quota != 0) {
			printf("    Global memory quota enabled: %lld MB available\n", caps->mem_quota);
		}
	}
	else {
		printf("  HAXM cannot be used on this host\n");
		if (caps->winfo & HAX_CAP_FAILREASON_VT) {
			printf("  Intel VT-x not supported or disabled\n");
		}
		if (caps->winfo & HAX_CAP_FAILREASON_NX) {
			printf("  Intel Execute Disable Bit (NX) not supported or disabled\n");
		}
		return -1;
	}
	
	// ------------------------------------------------------------------------

	// Now let's have some fun!

	// Create a VM
	HaxmVM *vm;
	HaxmVMStatus vmStatus = haxm.CreateVM(&vm);
	switch (vmStatus) {
	case HXVMS_CREATE_FAILED: printf("Failed to create VM: %d\n", haxm.GetLastError()); return -1;
	}

	printf("\nVM created: ID %x\n", vm->ID());
	if (vm->FastMMIOEnabled()) {
		printf("  Fast MMIO enabled\n");
	}

	// Allocate RAM
	vmStatus = vm->AllocateMemory(ram, ramSize, ramBase, HXVM_MEM_RAM);
	switch (vmStatus) {
	case HXVMS_MEM_MISALIGNED: printf("Failed to allocate RAM: host memory block is misaligned\n"); return -1;
	case HXVMS_MEMSIZE_MISALIGNED: printf("Failed to allocate RAM: memory block is misaligned\n"); return -1;
	case HXVMS_ALLOC_MEM_FAILED: printf("Failed to allocate RAM: %d\n", vm->GetLastError()); return -1;
	case HXVMS_SET_MEM_FAILED: printf("Failed to configure RAM: %d\n", vm->GetLastError()); return -1;
	}

	printf("  Allocated %d bytes of RAM at physical address 0x%08x\n", ramSize, ramBase);

	// Allocate ROM
	vmStatus = vm->AllocateMemory(rom, romSize, romBase, HXVM_MEM_ROM);
	switch (vmStatus) {
	case HXVMS_MEM_MISALIGNED: printf("Failed to allocate ROM: host memory block is misaligned\n"); return -1;
	case HXVMS_MEMSIZE_MISALIGNED: printf("Failed to allocate ROM: memory block is misaligned\n"); return -1;
	case HXVMS_ALLOC_MEM_FAILED: printf("Failed to allocate ROM: %d\n", vm->GetLastError()); return -1;
	case HXVMS_SET_MEM_FAILED: printf("Failed to configure ROM: %d\n", vm->GetLastError()); return -1;
	}

	printf("  Allocated %d bytes of ROM at physical address 0x%08x\n", romSize, romBase);

	// Add a virtual CPU to the VM
	HaxmVCPU *vcpu;
	HaxmVCPUStatus vcpuStatus = vm->CreateVCPU(&vcpu);
	switch (vcpuStatus) {
	case HXVCPUS_CREATE_FAILED: printf("Failed to create VCPU for VM: %d\n", vm->GetLastError()); return -1;
	}

	printf("  VCPU created with ID %d\n", vcpu->ID());
	printf("    VCPU Tunnel allocated at 0x%016llx\n", (uint64_t)vcpu->Tunnel());
	printf("    VCPU I/O tunnel allocated at 0x%016llx\n", (uint64_t)vcpu->IOTunnel());
	printf("\n");

	// Get CPU registers
	struct vcpu_state_t regs;
	vcpuStatus = vcpu->GetRegisters(&regs);
	switch (vcpuStatus) {
	case HXVCPUS_FAILED: printf("Failed to get VCPU registers: %d\n", vcpu->GetLastError()); return -1;
	}
	
	// Get FPU registers
	struct fx_layout fpu;
	vcpuStatus = vcpu->GetFPURegisters(&fpu);
	switch (vcpuStatus) {
	case HXVCPUS_FAILED: printf("Failed to get VCPU floating point registers: %d\n", vcpu->GetLastError()); return -1;
	}
	
	printf("\nInitial CPU register state:\n");
	printRegs(&regs);
	//printFPURegs(&fpu);
	printf("\n");

	/*// Manipulate CPU registers
	regs._cx = 0x4321;
	vcpuStatus = vcpu->SetRegisters(&regs);
	switch (vcpuStatus) {
	case HXVCPUS_FAILED: printf("Failed to set VCPU registers: %d\n", vcpu->GetLastError()); return -1;
	}

	printf("\CPU registers after manipulation:\n");
	printRegs(&regs);
	//printFPURegs(&fpu);
	printf("\n");*/

	// The CPU starts in 16-bit real mode.
	// Memory addressing is based on segments and offsets, where a segment is basically a 16-byte offset.

	// Run the CPU!
	vcpu->Run();

	#ifdef DO_MANUAL_JMP
	// Do the jmp dword 0x8:0xffffff00 manually
	vcpu->GetRegisters(&regs);

	// Set basic register data
	regs._cs.selector = 0x0008;
	regs._eip = 0xffffff00;

	// Find GDT entry in memory
	uint64_t gdtEntry;
	if (regs._gdt.base >= ramBase && regs._gdt.base <= ramBase + ramSize - 1) {
		// GDT is in RAM
		gdtEntry = *(uint64_t *)&ram[regs._gdt.base - ramBase + regs._cs.selector];
	}
	else if (regs._gdt.base >= romBase && regs._gdt.base <= romBase + romSize - 1) {
		// GDT is in ROM
		gdtEntry = *(uint64_t *)&rom[regs._gdt.base - romBase + regs._cs.selector];
	}

	// Fill in the rest of the CS info with data from the GDT entry
	regs._cs.ar = ((gdtEntry >> 40) & 0xf0ff);
	regs._cs.base = ((gdtEntry >> 16) & 0xfffff) | (((gdtEntry >> 56) & 0xff) << 20);
	regs._cs.limit = ((gdtEntry & 0xffff) | (((gdtEntry >> 48) & 0xf) << 16));
	if (regs._cs.ar & 0x8000) {
		// 4 KB pages
		regs._cs.limit = (regs._cs.limit << 12) | 0xfff;
	}

	vcpu->SetRegisters(&regs);

	// Run the CPU again!
	vcpu->Run();
	#endif

	// Get CPU status
	auto tunnel = vcpu->Tunnel();
	switch (tunnel->_exit_status) {
	case HAX_EXIT_HLT:
		printf("Emulation exited due to HLT instruction as expected!\n");
		break;
	default:
		printf("Emulation exited for another reason: %d\n", tunnel->_exit_status);
		break;
	}

	// Get CPU registers again
	vcpuStatus = vcpu->GetRegisters(&regs);
	switch (vcpuStatus) {
	case HXVCPUS_FAILED: printf("Failed to get VCPU registers: %d\n", vcpu->GetLastError()); return -1;
	}

	// Get FPU registers again
	vcpuStatus = vcpu->GetFPURegisters(&fpu);
	switch (vcpuStatus) {
	case HXVCPUS_FAILED: printf("Failed to get VCPU floating point registers: %d\n", vcpu->GetLastError()); return -1;
	}

	if (regs._eip == 0xffffff9f && regs._cs.selector == 0x0008) {
		printf("Emulation stopped at the right place!\n");
		if (regs._eax == 0xdeadbeef) {
			printf("And we got the right result!\n");
		}
	}

	printf("\nFinal CPU register state:\n");
	printRegs(&regs);
	//printFPURegs(&fpu);

	_aligned_free(rom);
	_aligned_free(ram);
	
	return 0;
}
