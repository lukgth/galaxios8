/*
 * Galaxy S8 Mali-G71 (Bifrost, r0p0 MP20) driver, milestone 2a: the same
 * "one job" test the loader runs (shim/dreamlte/pmic.c, runs c29/c30b), now
 * with the MMU and caches on, from the kernel.
 *
 * Boot arg s8gpu=1 enables it. Sequence, register map and page-table format
 * follow Linux panfrost (panfrost_regs.h, panfrost_gpu.c) and Samsung's
 * pmucal g3d_on (pmucal_local_exynos8895.h):
 *
 *   power: PMU EMBEDDED_G3D_CONFIGURATION 0x164840e0 = 0xf, wait STATUS
 *          0x164840e4 == 0xf (with the PLL and MUX bits, only if it is off)
 *   clock: read back PLL_G3D; ACPM owns the rate (the loader asks for
 *          260 MHz through dvfs_g3d, ACPM id 4)
 *   GPU:   soft reset, L2 -> shaders -> tiler power-up, one address space
 *          with our own AArch64-format 4 KiB page tables, one WRITE_VALUE
 *          job on slot 0, polled completion
 *
 * The GPU has no coherency with the CPU on this SoC (COHERENCY_FEATURES 0,
 * recorded in c30b), so every buffer is flushed to the point of coherency
 * before a job runs.
 */
#if defined(ARM64_BOARD_CONFIG_A5XELTE) && defined(DREAMLTE)

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" void flush_dcache(vm_offset_t addr, unsigned count, int phys);

#define S8_MALI_BASE		0x13900000
#define S8_MALI_SIZE		0x5000
#define S8_CMU_G3D		0x13800000
#define S8_G3D_PLL_CON0		(S8_CMU_G3D + 0x140)
#define S8_G3D_PLL_CON2		(S8_CMU_G3D + 0x148)
#define S8_G3D_MUX_USER		(S8_CMU_G3D + 0x100)
#define S8_G3D_PLL_LOCK		(S8_CMU_G3D + 0x180)
#define S8_PMU_G3D_CONF		0x164840e0
#define S8_PMU_G3D_STAT		0x164840e4

/* GPU_CONTROL */
#define GPU_ID			0x000
#define L2_FEATURES		0x004
#define CORE_FEATURES		0x008
#define TILER_FEATURES		0x00c
#define MEM_FEATURES		0x010
#define MMU_FEATURES		0x014
#define AS_PRESENT		0x018
#define JS_PRESENT		0x01c
#define GPU_INT_RAWSTAT		0x020
#define GPU_INT_CLEAR		0x024
#define GPU_INT_MASK		0x028
#define GPU_CMD			0x030
#define GPU_STATUS		0x034
#define GPU_FAULTSTATUS		0x03c
#define GPU_FAULTADDRESS_LO	0x040
#define SHADER_PRESENT		0x100
#define TILER_PRESENT		0x110
#define L2_PRESENT		0x120
#define SHADER_READY		0x140
#define TILER_READY		0x150
#define L2_READY		0x160
#define SHADER_PWRON		0x180
#define TILER_PWRON		0x190
#define L2_PWRON		0x1a0
#define SHADER_PWROFF		0x1c0
#define TILER_PWROFF		0x1d0
#define L2_PWROFF		0x1e0
#define L2_PWRTRANS		0x220
#define TILER_CONFIG		0xf08
#define COHERENCY_FEATURES	0x300

/* JOB_CONTROL: slot 0 at 0x1800, one slot every 0x80 */
#define JOB_INT_RAWSTAT		0x1000
#define JOB_INT_CLEAR		0x1004
#define JOB_INT_MASK		0x1008
#define JOB_IRQ_JS_STATE	0x1020
#define JS0			0x1800
#define JS_STATUS		0x24
#define JS_HEAD_NEXT_LO		0x40
#define JS_HEAD_NEXT_HI		0x44
#define JS_AFFINITY_NEXT_LO	0x50
#define JS_AFFINITY_NEXT_HI	0x54
#define JS_CONFIG_NEXT		0x58
#define JS_COMMAND_NEXT		0x60

/* MMU: address space 0 at 0x2400 */
#define MMU_INT_RAWSTAT		0x2000
#define MMU_INT_CLEAR		0x2004
#define MMU_INT_MASK		0x2008
#define AS0			0x2400
#define AS_TRANSTAB_LO		0x00
#define AS_TRANSTAB_HI		0x04
#define AS_MEMATTR_LO		0x08
#define AS_MEMATTR_HI		0x0c
#define AS_LOCKADDR_LO		0x10
#define AS_LOCKADDR_HI		0x14
#define AS_COMMAND		0x18
#define AS_FAULTSTATUS		0x1c
#define AS_FAULTADDRESS_LO	0x20
#define AS_FAULTADDRESS_HI	0x24
#define AS_STATUS		0x28
#define AS_TRANSCFG_LO		0x30
#define AS_TRANSCFG_HI		0x34

#define AS_CMD_UPDATE		1
#define AS_CMD_LOCK		2
#define AS_CMD_FLUSH_PT		4
#define AS_CMD_FLUSH_MEM	5

/* AArch64-format LPAE entries, as in the loader (c30b) */
#define LPAE_TABLE		3ULL
#define LPAE_PAGE		(1ULL | 1ULL << 10 | 3ULL << 6 | 2ULL << 8)
#define LPAE_TRANSTAB		(4ULL | 3ULL)	/* READ_INNER, ADRMODE_TABLE */

/* One contiguous block: four page tables, the job chain, two data pages */
#define S8_GPU_BLOCK_SIZE	0x10000
#define S8_GPU_TT_L0		0x0000
#define S8_GPU_TT_L1		0x1000
#define S8_GPU_TT_L2		0x2000
#define S8_GPU_TT_L3		0x3000
#define S8_GPU_JOB		0x4000
#define S8_GPU_DATA		0x5000
#define S8_GPU_WITNESS		0x6000	/* never mapped: must stay untouched */
#define S8_GPU_VA_WINDOW	0x10000000ULL	/* GPU VA of the block's data */

class S8Mali : public IOService {
	OSDeclareDefaultStructors(S8Mali);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;

private:
	volatile uint8_t *fMali;
	volatile uint8_t *fPmu;
	IOBufferMemoryDescriptor *fBlock;
	uint64_t fBlockPhys;
	void *fBlockVa;

	uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(fMali + off); }
	void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(fMali + off) = v; }
	uint64_t rd64(uint32_t off);
	void wr64(uint32_t off, uint64_t v);
	bool waitBits(uint32_t off, uint32_t mask, uint32_t want, unsigned us);
	bool waitBitsAt(volatile uint8_t *base, uint32_t off, uint32_t mask,
	    uint32_t want, unsigned us);
	bool powerUpDomain(void);
	bool gpuReset(void);
	bool gpuPowerUp(void);
	void gpuPowerDown(void);
	void asCommand(uint32_t cmd);
	bool asUpdate(uint64_t transtab);
	bool buildTables(void);
	void writeValueJob(uint32_t off, uint64_t va, uint32_t value);
	bool runJob(uint64_t jc, uint32_t *rawstat, uint32_t *status);
	bool selfTest(void);
};

OSDefineMetaClassAndStructors(S8Mali, IOService);

uint64_t
S8Mali::rd64(uint32_t off)
{
	return (uint64_t)rd(off) | ((uint64_t)rd(off + 4) << 32);
}

void
S8Mali::wr64(uint32_t off, uint64_t v)
{
	wr(off, (uint32_t)v);
	wr(off + 4, (uint32_t)(v >> 32));
}

bool
S8Mali::waitBitsAt(volatile uint8_t *base, uint32_t off, uint32_t mask,
    uint32_t want, unsigned us)
{
	for (unsigned i = 0; i < us; i++) {
		if ((*(volatile uint32_t *)(base + off) & mask) == want) {
			return true;
		}
		IODelay(1);
	}
	return (*(volatile uint32_t *)(base + off) & mask) == want;
}

bool
S8Mali::waitBits(uint32_t off, uint32_t mask, uint32_t want, unsigned us)
{
	return waitBitsAt(fMali, off, mask, want, us);
}

/* Samsung pmucal g3d_on, done only when the domain is still off. */
bool
S8Mali::powerUpDomain(void)
{
	uint32_t stat = *(volatile uint32_t *)fPmu;
	if ((stat & 0xf) == 0xf) {
		IOLog("S8 GPU: G3D domain already on (PMU 0x%x)\n", stat);
		return true;
	}
	volatile uint8_t *cmu = (volatile uint8_t *)ml_io_map(S8_CMU_G3D, 0x1000);
	if (cmu == NULL) {
		IOLog("S8 GPU: cannot map CMU_G3D\n");
		return false;
	}
	*(volatile uint32_t *)(cmu + 0x148) = 0x30000003u;	/* PLL_CON2 */
	*(volatile uint32_t *)(cmu + 0x140) |= 1u << 4;		/* PLL_CON0 enable */
	*(volatile uint32_t *)(cmu + 0x100) |= 1u << 4;		/* MUX_USER */
	*(volatile uint32_t *)(fPmu + 0) = (stat & ~0xfu) | 0xfu;
	bool on = waitBitsAt(fPmu, 0, 0xf, 0xf, 10000);
	IOLog("S8 GPU: G3D domain %s (PMU 0x%x)\n", on ? "on" : "did not come on",
	    *(volatile uint32_t *)fPmu);
	return on;
}

bool
S8Mali::gpuReset(void)
{
	wr(GPU_INT_CLEAR, 0xffffffffu);
	wr(GPU_CMD, 1);					/* soft reset */
	bool ok = waitBits(GPU_INT_RAWSTAT, 1u << 8, 1u << 8, 10000);
	wr(GPU_INT_CLEAR, 0xffffffffu);
	if (!ok) {
		IOLog("S8 GPU: reset did not complete (RAWSTAT 0x%x STATUS 0x%x)\n",
		    rd(GPU_INT_RAWSTAT), rd(GPU_STATUS));
	}
	return ok;
}

bool
S8Mali::gpuPowerUp(void)
{
	uint32_t l2 = rd(L2_PRESENT), sh = rd(SHADER_PRESENT), ti = rd(TILER_PRESENT);
	/* G71 r0p0 runs the TMIX errata workaround (Linux panfrost_gpu.c). */
	wr(TILER_CONFIG, rd(TILER_CONFIG) | 1u);

	wr(L2_PWRON, l2);
	bool l2ok = waitBits(L2_READY, l2 ? l2 : 0xffffffffu, l2, 20000);
	wr(SHADER_PWRON, sh);
	bool shok = waitBits(SHADER_READY, sh, sh, 20000);
	wr(TILER_PWRON, ti);
	bool tiok = waitBits(TILER_READY, ti, ti, 20000);
	IOLog("S8 GPU: power L2 0x%x %s, shaders 0x%x %s, tiler 0x%x %s\n",
	    l2, l2ok ? "ready" : "TIMEOUT", sh, shok ? "ready" : "TIMEOUT",
	    ti, tiok ? "ready" : "TIMEOUT");
	return l2ok && shok && tiok;
}

void
S8Mali::gpuPowerDown(void)
{
	wr(SHADER_PWROFF, rd(SHADER_PRESENT));
	wr(TILER_PWROFF, rd(TILER_PRESENT));
	IODelay(100);
	wr(L2_PWROFF, rd(L2_PRESENT));
	waitBits(L2_PWRTRANS, 0xffffffffu, 0, 20000);
}

void
S8Mali::asCommand(uint32_t cmd)
{
	waitBits(AS0 + AS_STATUS, 1, 0, 100000);
	wr(AS0 + AS_COMMAND, cmd);
	waitBits(AS0 + AS_STATUS, 1, 0, 100000);
}

bool
S8Mali::asUpdate(uint64_t transtab)
{
	wr(MMU_INT_CLEAR, 0xffffffffu);
	wr64(AS0 + AS_LOCKADDR_LO, 63);			/* whole VA range */
	asCommand(AS_CMD_LOCK);
	asCommand(AS_CMD_FLUSH_MEM);
	wr64(AS0 + AS_TRANSCFG_LO, 0);			/* legacy (LPAE) mode */
	wr64(AS0 + AS_TRANSTAB_LO, transtab);
	wr64(AS0 + AS_MEMATTR_LO, 0x00888d88u);		/* as panfrost */
	asCommand(AS_CMD_UPDATE);
	if (rd(AS0 + AS_FAULTSTATUS) != 0) {
		IOLog("S8 GPU: AS fault 0x%x at 0x%llx (STATUS 0x%x)\n",
		    rd(AS0 + AS_FAULTSTATUS), rd64(AS0 + AS_FAULTADDRESS_LO),
		    rd(AS0 + AS_STATUS));
		return false;
	}
	return true;
}

/* L0..L3 tables map the block's data page at S8_GPU_VA_WINDOW and the job
 * chain at its physical address; nothing else is mapped. */
bool
S8Mali::buildTables(void)
{
	uint64_t base = fBlockPhys;
	uint64_t va = S8_GPU_VA_WINDOW;
	uint64_t *l0 = (uint64_t *)((uint8_t *)fBlockVa + S8_GPU_TT_L0);
	uint64_t *l1 = (uint64_t *)((uint8_t *)fBlockVa + S8_GPU_TT_L1);
	uint64_t *l2 = (uint64_t *)((uint8_t *)fBlockVa + S8_GPU_TT_L2);
	uint64_t *l3 = (uint64_t *)((uint8_t *)fBlockVa + S8_GPU_TT_L3);
	uint64_t dataPa = base + S8_GPU_DATA;
	uint64_t jobPa = base + S8_GPU_JOB;

	for (unsigned i = 0; i < 512; i++) {
		l0[i] = l1[i] = l2[i] = l3[i] = 0;
	}
	l0[0] = (base + S8_GPU_TT_L1) | LPAE_TABLE;
	l1[(dataPa >> 30) & 0x1ff] = (base + S8_GPU_TT_L2) | LPAE_TABLE;
	l2[(dataPa >> 21) & 0x1ff] = (base + S8_GPU_TT_L3) | LPAE_TABLE;
	l3[(dataPa >> 12) & 0x1ff] = dataPa | LPAE_PAGE;
	l3[(va >> 12) & 0x1ff] = dataPa | LPAE_PAGE;
	l3[(jobPa >> 12) & 0x1ff] = jobPa | LPAE_PAGE;
	return true;
}

/* Mesa genxml v6 WRITE_VALUE: Is 64b, Write value, index 1, VA, Immediate 32 */
void
S8Mali::writeValueJob(uint32_t off, uint64_t va, uint32_t value)
{
	uint32_t *job = (uint32_t *)((uint8_t *)fBlockVa + off);
	for (unsigned i = 0; i < 16; i++) {
		job[i] = 0;
	}
	job[4] = 1u | 2u << 1 | 1u << 16;
	job[8] = (uint32_t)va;
	job[9] = (uint32_t)(va >> 32);
	job[10] = 6;					/* Immediate 32 */
	job[12] = value;
	job[13] = 0;
}

bool
S8Mali::runJob(uint64_t jc, uint32_t *rawstat, uint32_t *status)
{
	flush_dcache((vm_offset_t)fBlockVa, S8_GPU_BLOCK_SIZE, 0);
	wr(JOB_INT_CLEAR, 0xffffffffu);
	wr(JS0 + JS_HEAD_NEXT_LO, (uint32_t)jc);
	wr(JS0 + JS_HEAD_NEXT_HI, (uint32_t)(jc >> 32));
	wr(JS0 + JS_AFFINITY_NEXT_LO, rd(SHADER_PRESENT));
	wr(JS0 + JS_AFFINITY_NEXT_HI, 0);
	wr(JS0 + JS_CONFIG_NEXT, 8u << 16 | 3u << 8 | 3u << 12);
	wr(JS0 + JS_COMMAND_NEXT, 1);			/* START */
	bool done = false;
	for (unsigned i = 0; i < 200000; i++) {		/* 200 ms */
		uint32_t raw = rd(JOB_INT_RAWSTAT);
		if (raw & 0x10001) {
			done = true;
			break;
		}
		IODelay(1);
	}
	*rawstat = rd(JOB_INT_RAWSTAT);
	*status = rd(JS0 + JS_STATUS);
	flush_dcache((vm_offset_t)fBlockVa, S8_GPU_BLOCK_SIZE, 0);
	return done;
}

bool
S8Mali::selfTest(void)
{
	uint32_t id = rd(GPU_ID);
	IOLog("S8 GPU: ID 0x%08x (product 0x%x r%up%u), L2_FEATURES 0x%x CORE 0x%x "
	    "TILER 0x%x MEM 0x%x MMU 0x%x AS 0x%x JS 0x%x SHADERS 0x%x COH 0x%x\n",
	    id, (id >> 16) & 0xffff, (id >> 12) & 0xf, (id >> 8) & 0xf,
	    rd(L2_FEATURES), rd(CORE_FEATURES), rd(TILER_FEATURES), rd(MEM_FEATURES),
	    rd(MMU_FEATURES), rd(AS_PRESENT), rd(JS_PRESENT), rd(SHADER_PRESENT),
	    rd(COHERENCY_FEATURES));

	if (!gpuReset() || !gpuPowerUp()) {
		return false;
	}
	buildTables();
	if (!asUpdate(fBlockPhys | LPAE_TRANSTAB)) {
		return false;
	}

	uint64_t base = fBlockPhys;
	uint64_t dataPa = base + S8_GPU_DATA;
	uint64_t jobPa = base + S8_GPU_JOB;
	uint32_t *witness = (uint32_t *)((uint8_t *)fBlockVa + S8_GPU_WITNESS);
	uint32_t *data = (uint32_t *)((uint8_t *)fBlockVa + S8_GPU_DATA);

	*witness = 0xdead0000u;
	writeValueJob(S8_GPU_JOB, dataPa + 0x10, 0xc0ffee11u);
	/* second chain: VA S8_GPU_VA_WINDOW + 0x10 must reach the same page */
	writeValueJob(S8_GPU_JOB + 0x40, S8_GPU_VA_WINDOW + 0x20, 0xc0ffee22u);

	uint32_t raw = 0, status = 0;
	bool done = runJob(jobPa, &raw, &status);
	IOLog("S8 GPU: job 1 %s RAWSTAT 0x%x JS_STATUS 0x%x data[4] 0x%x\n",
	    done ? "done" : "TIMEOUT", raw, status, data[4]);
	if (!done || status != 1 || data[4] != 0xc0ffee11u) {
		IOLog("S8 GPU: job 1 failed (expected status 1, 0xc0ffee11)\n");
		return false;
	}
	data[8] = 0;
	flush_dcache((vm_offset_t)fBlockVa, S8_GPU_BLOCK_SIZE, 0);
	done = runJob(jobPa + 0x40, &raw, &status);
	IOLog("S8 GPU: job 2 %s RAWSTAT 0x%x JS_STATUS 0x%x data[8] 0x%x witness 0x%x\n",
	    done ? "done" : "TIMEOUT", raw, status, data[8], *witness);
	if (!done || status != 1 || data[8] != 0xc0ffee22u || *witness != 0xdead0000u) {
		IOLog("S8 GPU: job 2 failed (expected 0xc0ffee22 through the VA "
		    "window, witness untouched)\n");
		return false;
	}
	return true;
}

bool
S8Mali::start(IOService *provider)
{
	uint32_t enable = 0;
	if (!PE_parse_boot_argn("s8gpu", &enable, sizeof(enable)) || enable == 0) {
		return false;
	}
	if (!IOService::start(provider)) {
		return false;
	}
	fMali = NULL;
	fPmu = NULL;
	fBlock = NULL;
	fBlockVa = NULL;
	fBlockPhys = 0;

	fMali = (volatile uint8_t *)ml_io_map(S8_MALI_BASE, S8_MALI_SIZE);
	fPmu = (volatile uint8_t *)ml_io_map(S8_PMU_G3D_CONF, 0x10);
	if (fMali == NULL || fPmu == NULL) {
		IOLog("S8 GPU: cannot map the registers\n");
		return false;
	}

	fBlock = IOBufferMemoryDescriptor::withOptions(
	    kIOMemoryPhysicallyContiguous | kIODirectionOutIn | kIOMemoryMapperNone,
	    S8_GPU_BLOCK_SIZE, S8_GPU_BLOCK_SIZE);
	if (fBlock == NULL || fBlock->getPhysicalAddress() == 0) {
		IOLog("S8 GPU: no contiguous memory block\n");
		return false;
	}
	fBlockVa = fBlock->getBytesNoCopy();
	fBlockPhys = fBlock->getPhysicalAddress();
	IOLog("S8 GPU: block VA %p PA 0x%llx\n", fBlockVa, fBlockPhys);

	if (!powerUpDomain()) {
		return false;
	}
	volatile uint8_t *cmu = (volatile uint8_t *)ml_io_map(S8_CMU_G3D, 0x200);
	uint32_t pll = *(volatile uint32_t *)(cmu + 0x140);
	uint32_t m = (pll >> 16) & 0x3ff, p = (pll >> 8) & 0x3f, s = pll & 7;
	uint32_t mhz = p ? (uint32_t)((26ULL * m / p) >> s) : 0;
	IOLog("S8 GPU: PLL_G3D 0x%x = %u MHz (ACPM owns the rate)\n", pll, mhz);

	bool ok = selfTest();
	if (ok) {
		IOLog("S8 GPU: milestone 2a passed: a WRITE_VALUE job ran from XNU\n");
	}
	asUpdate(0);
	gpuPowerDown();
	setProperty("s8gpu-selftest", ok ? "passed" : "failed");
	return ok;
}

void
S8Mali::stop(IOService *provider)
{
	IOService::stop(provider);
}

void
S8Mali::free(void)
{
	if (fBlock != NULL) {
		fBlock->release();
		fBlock = NULL;
	}
	IOService::free();
}

#endif /* ARM64_BOARD_CONFIG_A5XELTE && DREAMLTE */
