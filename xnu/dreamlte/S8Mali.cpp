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
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <IOKit/IOTypes.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_A5XELTE) && defined(DREAMLTE)

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" int s8_acpm_gpu_khz(uint32_t khz, uint32_t *ans);	/* SweetCPU.cpp */
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
#define S8_GPU_TT_L2_VA		0x7000
#define S8_GPU_JOB		0x4000
#define S8_GPU_DATA		0x5000
#define S8_GPU_WITNESS		0x6000	/* never mapped: must stay untouched */
#define S8_GPU_VA_WINDOW	0x10000000ULL	/* GPU VA of the block's data */

/* GPU address space: 4 GiB, as panfrost gives each client. Tables: one L0,
 * one L1, four L2 (4 GiB / 1 GiB), L3 pages handed out in 64 KiB chunks. */
#define S8_GPU_VA_BASE		0x00010000ULL
#define S8_GPU_VA_END		0x100000000ULL
#define S8_GPU_L3_CHUNKS	128
#define S8_GPU_L3_PER_CHUNK	16
#define S8_GPU_MAX_BOS		256

struct S8GpuBo {
	uint32_t handle;
	uint32_t flags;
	uint64_t va;
	uint64_t size;
	IOBufferMemoryDescriptor *mem;
	void *vaCpu;
};

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
	bool runJob(uint64_t jc, uint32_t *headerWord, uint32_t *rawstat, unsigned slot);
	bool selfTest(void);

	/* Buffer objects and the 4 GiB GPU address space */
	IOBufferMemoryDescriptor *fTables;
	uint64_t fTablesPhys;
	uint64_t *fL2[4];
	IOBufferMemoryDescriptor *fL3Chunk[S8_GPU_L3_CHUNKS];
	uint64_t fL3Phys[S8_GPU_L3_CHUNKS];
	unsigned fL3Count;
	uint64_t fVaNext;
	S8GpuBo *fBos[S8_GPU_MAX_BOS];
	uint32_t fBoCount;

	bool tablesInit(void);
	uint64_t *l3For(uint64_t va);
	bool mapPage(uint64_t va, uint64_t pa, bool noexec);
	bool flushPageTables(void);
	S8GpuBo *allocBo(uint32_t size, uint32_t flags, uint32_t *handle);
	S8GpuBo *findBo(uint32_t handle);
	bool mapBo(S8GpuBo *bo);
	void freeBo(S8GpuBo *bo);
	bool boTest(void);
	bool uapiTest(void);

public:
	/* User client API (panfrost UAPI numbers) */
	int userGetParam(uint32_t param, uint64_t *value);
	IOReturn userCreateBo(uint32_t size, uint32_t flags, uint32_t *handle, uint64_t *offset);
	IOReturn userFreeBo(uint32_t handle);
	IOReturn userMmapBo(uint32_t handle, uint64_t *offset);
	IOReturn userGetBoOffset(uint32_t handle, uint64_t *offset);
	IOReturn userSubmit(uint64_t jc, uint32_t requirements);
	IOReturn userWaitBo(uint32_t handle, int64_t timeout_ns);
	IOReturn userMadvise(uint32_t handle, uint32_t madv, uint32_t *retained);
	IOMemoryDescriptor *userBoMemory(vm_offset_t type);
	bool userOpen(void);
	void userClose(void);

private:
	IOLock *fLock;
	bool fPowered;
	bool gpuAcquire(void);
	void gpuRelease(void);
	uint64_t vaToCpu(uint64_t va, uint64_t *size);
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
	if (on) {
		/* The PLL alone does not set the rate: the APM firmware owns it. */
		uint32_t ans = 0;
		int rc = s8_acpm_gpu_khz(260000, &ans);
		IOLog("S8 GPU: ACPM g3d 260 MHz rc %d rate %u\n", rc, ans);
	}
	return on;
}

bool
S8Mali::gpuReset(void)
{
	wr(MMU_INT_CLEAR, 0xffffffffu);
	wr(JOB_INT_CLEAR, 0xffffffffu);
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
		/* Left over from an earlier fault; the job status decides. */
		IOLog("S8 GPU: AS fault still 0x%x at 0x%x (STATUS 0x%x)\n",
		    rd(AS0 + AS_FAULTSTATUS), rd(AS0 + AS_FAULTADDRESS_LO),
		    rd(AS0 + AS_STATUS));
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
	uint64_t *l2v = (uint64_t *)((uint8_t *)fBlockVa + S8_GPU_TT_L2_VA);
	uint64_t dataPa = base + S8_GPU_DATA;
	uint64_t jobPa = base + S8_GPU_JOB;

	for (unsigned i = 0; i < 512; i++) {
		l0[i] = l1[i] = l2[i] = l3[i] = l2v[i] = 0;
	}
	l0[0] = (base + S8_GPU_TT_L1) | LPAE_TABLE;
	l1[(dataPa >> 30) & 0x1ff] = (base + S8_GPU_TT_L2) | LPAE_TABLE;
	l2[(dataPa >> 21) & 0x1ff] = (base + S8_GPU_TT_L3) | LPAE_TABLE;
	l3[(dataPa >> 12) & 0x1ff] = dataPa | LPAE_PAGE;
	l3[(jobPa >> 12) & 0x1ff] = jobPa | LPAE_PAGE;
	/* 0x10000000 is in 1 GiB entry 0; the table above serves 0x883... */
	l1[(va >> 30) & 0x1ff] = (base + S8_GPU_TT_L2_VA) | LPAE_TABLE;
	l2v[(va >> 21) & 0x1ff] = (base + S8_GPU_TT_L3) | LPAE_TABLE;
	l3[(va >> 12) & 0x1ff] = dataPa | LPAE_PAGE;
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

/* The job header's first word holds the exception status (1 = DONE), as the
 * loader logged in c30b; JS_STATUS is 0 again once the job left the slot. */
bool
S8Mali::runJob(uint64_t jc, uint32_t *headerWord, uint32_t *rawstat, unsigned slot)
{
	/* Slot 0 runs fragment jobs, slot 1 vertex/tiler and compute. panfrost
	 * picks the slot from the requirements of the submit. */
	uint32_t js = JS0 + slot * 0x80;
	uint32_t donebit = (1u << slot) | 0x10000u;	/* done or MMU fault */
	flush_dcache((vm_offset_t)fBlockVa, S8_GPU_BLOCK_SIZE, 0);
	wr(JOB_INT_CLEAR, 0xffffffffu);
	wr(js + JS_HEAD_NEXT_LO, (uint32_t)jc);
	wr(js + JS_HEAD_NEXT_HI, (uint32_t)(jc >> 32));
	wr(js + JS_AFFINITY_NEXT_LO, slot == 0 ? rd(SHADER_PRESENT) : rd(TILER_PRESENT));
	wr(js + JS_AFFINITY_NEXT_HI, 0);
	wr(js + JS_CONFIG_NEXT, 8u << 16 | 3u << 8 | 3u << 12);
	wr(js + JS_COMMAND_NEXT, 1);			/* START */
	bool done = false;
	for (unsigned i = 0; i < 200000; i++) {		/* 200 ms */
		uint32_t raw = rd(JOB_INT_RAWSTAT);
		if (raw & donebit) {
			done = (raw & (1u << slot)) != 0;
			break;
		}
		IODelay(1);
	}
	*rawstat = rd(JOB_INT_RAWSTAT);
	flush_dcache((vm_offset_t)headerWord, 0x40, 0);
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
	bool done = runJob(jobPa, (uint32_t *)((uint8_t *)fBlockVa + S8_GPU_JOB), &raw, 0);
	status = *(uint32_t *)((uint8_t *)fBlockVa + S8_GPU_JOB);
	IOLog("S8 GPU: job 1 %s RAWSTAT 0x%x header status 0x%x data[4] 0x%x\n",
	    done ? "done" : "TIMEOUT", raw, status, data[4]);
	if (!done || status != 1 || data[4] != 0xc0ffee11u) {
		IOLog("S8 GPU: job 1 failed (expected header status 1 and 0xc0ffee11)\n");
		return false;
	}
	data[8] = 0;
	flush_dcache((vm_offset_t)fBlockVa, S8_GPU_BLOCK_SIZE, 0);
	done = runJob(jobPa + 0x40, (uint32_t *)((uint8_t *)fBlockVa + S8_GPU_JOB + 0x40), &raw, 0);
	status = *(uint32_t *)((uint8_t *)fBlockVa + S8_GPU_JOB + 0x40);
	IOLog("S8 GPU: job 2 %s RAWSTAT 0x%x header status 0x%x data[8] 0x%x witness 0x%x\n",
	    done ? "done" : "TIMEOUT", raw, status, data[8], *witness);
	if (!done || status != 1 || data[8] != 0xc0ffee22u || *witness != 0xdead0000u) {
		IOLog("S8 GPU: job 2 failed (expected 0xc0ffee22 through the VA "
		    "window, witness untouched)\n");
		return false;
	}
	return true;
}

/* ---- buffer objects and the 4 GiB GPU address space (milestone 2b, 1/2) ---- */

bool
S8Mali::tablesInit(void)
{
	fTables = IOBufferMemoryDescriptor::withOptions(
	    kIOMemoryPhysicallyContiguous | kIODirectionOutIn | kIOMemoryMapperNone,
	    0x10000, 0x10000);
	if (fTables == NULL) {
		return false;
	}
	fTablesPhys = fTables->getPhysicalAddress();
	uint8_t *base = (uint8_t *)fTables->getBytesNoCopy();
	uint64_t *l0 = (uint64_t *)base;
	uint64_t *l1 = (uint64_t *)(base + 0x1000);
	for (unsigned i = 0; i < 512; i++) {
		l0[i] = 0;
		l1[i] = 0;
	}
	l0[0] = (fTablesPhys + 0x1000) | LPAE_TABLE;
	for (unsigned i = 0; i < 4; i++) {
		uint64_t *l2 = (uint64_t *)(base + 0x2000 + i * 0x1000);
		for (unsigned j = 0; j < 512; j++) {
			l2[j] = 0;
		}
		fL2[i] = l2;
		l1[i] = (fTablesPhys + 0x2000 + i * 0x1000) | LPAE_TABLE;
	}
	flush_dcache((vm_offset_t)base, 0x10000, 0);
	fVaNext = S8_GPU_VA_BASE;
	return true;
}

uint64_t *
S8Mali::l3For(uint64_t va)
{
	unsigned l2i = (unsigned)((va >> 30) & 3);
	unsigned idx = (unsigned)((va >> 21) & 0x1ff);
	uint64_t entry = fL2[l2i][idx];
	uint8_t *base = (uint8_t *)fTables->getBytesNoCopy();

	if (entry != 0) {
		uint64_t pa = entry & ~0xfffULL;
		for (unsigned c = 0; c < fL3Count; c++) {
			if (pa >= fL3Phys[c] && pa < fL3Phys[c] + 0x10000) {
				unsigned page = (unsigned)((pa - fL3Phys[c]) >> 12);
				return (uint64_t *)((uint8_t *)fL3Chunk[c]->getBytesNoCopy() +
				    page * 0x1000);
			}
		}
		return NULL;
	}
	if (fL3Count >= S8_GPU_L3_CHUNKS) {
		return NULL;
	}
	IOBufferMemoryDescriptor *chunk = IOBufferMemoryDescriptor::withOptions(
	    kIOMemoryPhysicallyContiguous | kIODirectionOutIn | kIOMemoryMapperNone,
	    0x10000, 0x10000);
	if (chunk == NULL) {
		return NULL;
	}
	uint64_t pa = chunk->getPhysicalAddress();
	uint64_t *l3 = (uint64_t *)chunk->getBytesNoCopy();
	for (unsigned i = 0; i < 512; i++) {
		l3[i] = 0;
	}
	flush_dcache((vm_offset_t)l3, 0x1000, 0);
	fL3Chunk[fL3Count] = chunk;
	fL3Phys[fL3Count] = pa;
	fL3Count++;
	fL2[l2i][idx] = pa | LPAE_TABLE;
	flush_dcache((vm_offset_t)base, 0x10000, 0);
	return l3;
}

bool
S8Mali::mapPage(uint64_t va, uint64_t pa, bool noexec)
{
	uint64_t *l3 = l3For(va);
	if (l3 == NULL) {
		return false;
	}
	unsigned i = (unsigned)((va >> 12) & 0x1ff);
	l3[i] = pa | LPAE_PAGE | (noexec ? (1ULL << 54) : 0);
	flush_dcache((vm_offset_t)&l3[i], 8, 0);
	return true;
}

bool
S8Mali::flushPageTables(void)
{
	flush_dcache((vm_offset_t)fTables->getBytesNoCopy(), 0x10000, 0);
	for (unsigned c = 0; c < fL3Count; c++) {
		flush_dcache((vm_offset_t)fL3Chunk[c]->getBytesNoCopy(), 0x10000, 0);
	}
	asCommand(AS_CMD_FLUSH_PT);
	asCommand(AS_CMD_UPDATE);
	return true;
}

S8GpuBo *
S8Mali::allocBo(uint32_t size, uint32_t flags, uint32_t *handle)
{
	uint64_t sz = ((uint64_t)size + 0xfff) & ~0xfffULL;
	if (sz == 0) {
		sz = 0x1000;
	}
	if (fVaNext + sz > S8_GPU_VA_END) {
		return NULL;
	}
	unsigned slot;
	for (slot = 0; slot < S8_GPU_MAX_BOS; slot++) {
		if (fBos[slot] == NULL) {
			break;
		}
	}
	if (slot == S8_GPU_MAX_BOS) {
		return NULL;
	}
	IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::withOptions(
	    kIODirectionOutIn | kIOMemoryMapperNone, (vm_size_t)sz, 1);
	if (mem == NULL) {
		return NULL;
	}
	S8GpuBo *bo = (S8GpuBo *)IOMalloc(sizeof(S8GpuBo));
	if (bo == NULL) {
		mem->release();
		return NULL;
	}
	bo->handle = slot + 1;
	bo->flags = flags;
	bo->va = fVaNext;
	bo->size = sz;
	bo->mem = mem;
	bo->vaCpu = mem->getBytesNoCopy();
	bzero(bo->vaCpu, (size_t)sz);
	fVaNext += sz;
	fBos[slot] = bo;
	*handle = bo->handle;
	return bo;
}

S8GpuBo *
S8Mali::findBo(uint32_t handle)
{
	if (handle == 0 || handle > S8_GPU_MAX_BOS) {
		return NULL;
	}
	return fBos[handle - 1];
}

bool
S8Mali::mapBo(S8GpuBo *bo)
{
	bool noexec = (bo->flags & 1) != 0;		/* PANFROST_BO_NOEXEC */
	for (uint64_t off = 0; off < bo->size; ) {
		IOByteCount len = 0;
		IOPhysicalAddress pa = bo->mem->getPhysicalSegment(off, &len,
		    kIOMemoryMapperNone);
		if (pa == 0 || len == 0) {
			return false;
		}
		for (IOByteCount p = 0; p < len; p += 0x1000) {
			if (!mapPage(bo->va + off + p, pa + p, noexec)) {
				return false;
			}
		}
		off += len;
	}
	return flushPageTables();
}

void
S8Mali::freeBo(S8GpuBo *bo)
{
	if (bo == NULL) {
		return;
	}
	for (uint64_t off = 0; off < bo->size; off += 0x1000) {
		uint64_t *l3 = l3For(bo->va + off);
		if (l3 != NULL) {
			l3[((bo->va + off) >> 12) & 0x1ff] = 0;
		}
	}
	flushPageTables();
	bo->mem->release();
	fBos[bo->handle - 1] = NULL;
	IOFree(bo, sizeof(S8GpuBo));
}

/* Two BOs mapped, one not: the GPU must write into the first through its
 * GPU address, and a write to the unmapped one must fault and recover. */
bool
S8Mali::boTest(void)
{
	uint32_t ha = 0, hb = 0, hc = 0;
	S8GpuBo *a = allocBo(0x1000, 0, &ha);
	S8GpuBo *b = allocBo(0x1000, 0, &hb);
	S8GpuBo *c = allocBo(0x1000, 0, &hc);
	if (a == NULL || b == NULL || c == NULL) {
		IOLog("S8 GPU: BO allocation failed\n");
		return false;
	}
	if (!mapBo(a) || !mapBo(b)) {
		IOLog("S8 GPU: BO mapping failed\n");
		return false;
	}
	IOLog("S8 GPU: BOs A %u VA 0x%llx, B %u VA 0x%llx, C %u VA 0x%llx (unmapped)\n",
	    ha, a->va, hb, b->va, hc, c->va);

	{
		/* write the chain into BO A at offset 0 */
		uint32_t *job = (uint32_t *)a->vaCpu;
		for (unsigned i = 0; i < 16; i++) {
			job[i] = 0;
		}
		uint64_t target = b->va + 0x10;
		job[4] = 1u | 2u << 1 | 1u << 16;
		job[8] = (uint32_t)target;
		job[9] = (uint32_t)(target >> 32);
		job[10] = 6;
		job[12] = 0x600dcafeu;
	}
	flush_dcache((vm_offset_t)a->vaCpu, a->size, 0);
	flush_dcache((vm_offset_t)b->vaCpu, b->size, 0);

	uint32_t raw = 0;
	bool done = runJob(a->va, (uint32_t *)a->vaCpu, &raw, 0);
	flush_dcache((vm_offset_t)b->vaCpu, b->size, 0);
	uint32_t status = *(uint32_t *)a->vaCpu;
	uint32_t got = *(volatile uint32_t *)((uint8_t *)b->vaCpu + 0x10);
	IOLog("S8 GPU: BO job %s RAWSTAT 0x%x header status 0x%x B[0x10] 0x%x\n",
	    done ? "done" : "TIMEOUT", raw, status, got);
	if (!done || status != 1 || got != 0x600dcafeu) {
		IOLog("S8 GPU: BO job failed\n");
		return false;
	}

	/* write to the unmapped BO C: expect an MMU fault */
	{
		uint32_t *job = (uint32_t *)a->vaCpu;
		uint64_t target = c->va + 0x10;
		for (unsigned i = 0; i < 16; i++) {
			job[i] = 0;
		}
		job[4] = 1u | 2u << 1 | 1u << 16;
		job[8] = (uint32_t)target;
		job[9] = (uint32_t)(target >> 32);
		job[10] = 6;
		job[12] = 0xbad0bad0u;
	}
	flush_dcache((vm_offset_t)a->vaCpu, a->size, 0);
	done = runJob(a->va, (uint32_t *)a->vaCpu, &raw, 0);
	uint32_t mmu = rd(MMU_INT_RAWSTAT);
	uint32_t fault = rd(AS0 + AS_FAULTSTATUS);
	uint32_t faddr = rd(AS0 + AS_FAULTADDRESS_LO);
	IOLog("S8 GPU: unmapped job %s RAWSTAT 0x%x MMU int 0x%x fault 0x%x at 0x%x\n",
	    done ? "done (wrong)" : "did not complete", raw, mmu, fault, faddr);

	/* the fault left the slot stopped: reset and re-init before anything else */
	gpuReset();
	gpuPowerUp();
	asUpdate(fTablesPhys | LPAE_TRANSTAB);
	freeBo(a);
	freeBo(b);
	freeBo(c);
	return fault != 0;
}

/* ---- user client: the panfrost UAPI (milestone 2b, 2/2) ---- */

/* Layouts of include/uapi/drm/panfrost_drm.h, Mesa 25.0.7 */
struct s8_pf_create_bo {
	uint32_t size;
	uint32_t flags;
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;
};
struct s8_pf_mmap_bo {
	uint32_t handle;
	uint32_t flags;
	uint64_t offset;
};
struct s8_pf_get_param {
	uint32_t param;
	uint32_t pad;
	uint64_t value;
};
struct s8_pf_get_bo_offset {
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;
};
struct s8_pf_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
struct s8_pf_wait_bo {
	uint32_t handle;
	uint32_t pad;
	int64_t timeout_ns;
};
struct s8_pf_madvise {
	uint32_t handle;
	uint32_t madv;
	uint32_t retained;
};

class S8MaliUserClient : public IOUserClient {
	OSDeclareDefaultStructors(S8MaliUserClient);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **descriptor) APPLE_KEXT_OVERRIDE;
private:
	S8Mali *fGpu;
};
OSDefineMetaClassAndStructors(S8MaliUserClient, IOUserClient);

bool
S8MaliUserClient::start(IOService *provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fGpu = OSDynamicCast(S8Mali, provider);
	if (fGpu == NULL) {
		return false;
	}
	if (!fGpu->userOpen()) {
		return false;
	}
	IOLog("S8 GPU: user client opened\n");
	return true;
}

void
S8MaliUserClient::stop(IOService *provider)
{
	if (fGpu != NULL) {
		fGpu->userClose();
	}
	IOUserClient::stop(provider);
}

IOReturn
S8MaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
    IOMemoryDescriptor **descriptor)
{
	if (fGpu == NULL || descriptor == NULL) {
		return kIOReturnBadArgument;
	}
	IOMemoryDescriptor *mem = fGpu->userBoMemory(type);
	if (mem == NULL) {
		return kIOReturnBadArgument;
	}
	/* Linux maps panfrost BOs write-combined; the client gets the same. */
	*options = kIOMapWriteCombineCache;
	*descriptor = mem;
	return kIOReturnSuccess;
}

IOReturn
S8MaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch __unused, OSObject *target __unused,
    void *reference __unused)
{
	if (args == NULL || fGpu == NULL) {
		return kIOReturnBadArgument;
	}
	void *in = (void *)args->structureInput;
	void *out = (void *)args->structureOutput;
	IOByteCount inSize = args->structureInputSize;
	IOByteCount outSize = args->structureOutputSize;
	IOReturn ret = kIOReturnBadArgument;

	switch (selector) {
	case 0x00: {					/* SUBMIT */
		if (inSize < sizeof(s8_pf_submit)) {
			break;
		}
		s8_pf_submit s;
		if (out != NULL && outSize >= sizeof(s)) {
			bcopy(&s, out, sizeof(s));	/* the shim may pass it in either */
		} else {
			bcopy(in, &s, sizeof(s));
		}
		ret = fGpu->userSubmit(s.jc, s.requirements);
		break;
	}
	case 0x01: {					/* WAIT_BO */
		if (inSize < sizeof(s8_pf_wait_bo)) {
			break;
		}
		s8_pf_wait_bo s;
		bcopy(in, &s, sizeof(s));
		ret = fGpu->userWaitBo(s.handle, s.timeout_ns);
		break;
	}
	case 0x02: {					/* CREATE_BO */
		if (inSize < sizeof(s8_pf_create_bo) || out == NULL ||
		    outSize < sizeof(s8_pf_create_bo)) {
			break;
		}
		s8_pf_create_bo s;
		bcopy(in, &s, sizeof(s));
		ret = fGpu->userCreateBo(s.size, s.flags, &s.handle, &s.offset);
		if (ret == kIOReturnSuccess) {
			bcopy(&s, out, sizeof(s));
		}
		break;
	}
	case 0x03: {					/* MMAP_BO */
		if (inSize < sizeof(s8_pf_mmap_bo) || out == NULL ||
		    outSize < sizeof(s8_pf_mmap_bo)) {
			break;
		}
		s8_pf_mmap_bo s;
		bcopy(in, &s, sizeof(s));
		ret = fGpu->userMmapBo(s.handle, &s.offset);
		if (ret == kIOReturnSuccess) {
			bcopy(&s, out, sizeof(s));
		}
		break;
	}
	case 0x04: {					/* GET_PARAM */
		if (inSize < sizeof(s8_pf_get_param) || out == NULL ||
		    outSize < sizeof(s8_pf_get_param)) {
			break;
		}
		s8_pf_get_param s;
		bcopy(in, &s, sizeof(s));
		ret = fGpu->userGetParam(s.param, &s.value) == 0 ?
		    kIOReturnSuccess : kIOReturnBadArgument;
		if (ret == kIOReturnSuccess) {
			bcopy(&s, out, sizeof(s));
		}
		break;
	}
	case 0x05: {					/* GET_BO_OFFSET */
		if (inSize < sizeof(s8_pf_get_bo_offset) || out == NULL ||
		    outSize < sizeof(s8_pf_get_bo_offset)) {
			break;
		}
		s8_pf_get_bo_offset s;
		bcopy(in, &s, sizeof(s));
		ret = fGpu->userGetBoOffset(s.handle, &s.offset);
		if (ret == kIOReturnSuccess) {
			bcopy(&s, out, sizeof(s));
		}
		break;
	}
	case 0x40: {					/* S8_LOG: a line into the ring */
		char text[241];
		IOByteCount n = inSize < sizeof(text) - 1 ? inSize : sizeof(text) - 1;
		if (in == NULL || n == 0) {
			break;
		}
		bcopy(in, text, n);
		text[n] = 0;
		IOLog("S8 USER: %s\n", text);
		ret = kIOReturnSuccess;
		break;
	}
	case 0x08: {					/* MADVISE */
		if (inSize < sizeof(s8_pf_madvise) || out == NULL ||
		    outSize < sizeof(s8_pf_madvise)) {
			break;
		}
		s8_pf_madvise s;
		bcopy(in, &s, sizeof(s));
		ret = fGpu->userMadvise(s.handle, s.madv, &s.retained);
		if (ret == kIOReturnSuccess) {
			bcopy(&s, out, sizeof(s));
		}
		break;
	}
	default:
		ret = kIOReturnUnsupported;
		break;
	}
	return ret;
}

uint64_t
S8Mali::vaToCpu(uint64_t va, uint64_t *size)
{
	for (unsigned i = 0; i < S8_GPU_MAX_BOS; i++) {
		S8GpuBo *bo = fBos[i];
		if (bo != NULL && va >= bo->va && va < bo->va + bo->size) {
			if (size != NULL) {
				*size = bo->size - (va - bo->va);
			}
			return (uint64_t)((uint8_t *)bo->vaCpu + (va - bo->va));
		}
	}
	return 0;
}

bool
S8Mali::gpuAcquire(void)
{
	if (fPowered) {
		return true;
	}
	if (!powerUpDomain() || !gpuReset() || !gpuPowerUp()) {
		return false;
	}
	if (fTables == NULL && !tablesInit()) {
		return false;
	}
	if (!asUpdate(fTablesPhys | LPAE_TRANSTAB)) {
		return false;
	}
	for (unsigned i = 0; i < S8_GPU_MAX_BOS; i++) {
		if (fBos[i] != NULL && !mapBo(fBos[i])) {
			return false;
		}
	}
	fPowered = true;
	return true;
}

void
S8Mali::gpuRelease(void)
{
	if (fPowered) {
		gpuPowerDown();
		fPowered = false;
	}
}

bool
S8Mali::userOpen(void)
{
	if (fLock == NULL) {
		fLock = IOLockAlloc();
	}
	if (fLock == NULL) {
		return false;
	}
	IOLockLock(fLock);
	bool ok = gpuAcquire();
	IOLockUnlock(fLock);
	return ok;
}

void
S8Mali::userClose(void)
{
	if (fLock == NULL) {
		return;
	}
	IOLockLock(fLock);
	gpuRelease();
	IOLockUnlock(fLock);
}

int
S8Mali::userGetParam(uint32_t param, uint64_t *value)
{
	static unsigned logged;
	uint32_t id = rd(GPU_ID);

	switch (param) {
	case 0:						/* GPU_PROD_ID */
		/* Linux panfrost reports the model id (0x6000 for G71), not the
		 * whole product word (this chip: 0x60a0). */
		*value = (id >> 16) & 0xff00;
		break;
	case 1:						/* JS_FEATURES13 */
	case 2:						/* JS_FEATURES14 */
	case 3:						/* JS_FEATURES15 */
		*value = 0;
		break;
	case 4:						/* NR_CORE_GROUPS */
		*value = 1;
		break;
	case 5:						/* THREAD_TLS_ALLOC */
	case 6:						/* AFBC_FEATURES */
		*value = 0;
		break;
	case 7:						/* SYSTEM_TIMESTAMP */
		*value = mach_absolute_time();
		break;
	case 8:						/* SYSTEM_TIMESTAMP_FREQUENCY */
		*value = 0;
		break;
	default:
		return -22;
	}
	if (logged++ < 16) {
		IOLog("S8 GPU: GET_PARAM %u = 0x%llx\n", param, *value);
	}
	return 0;
}

IOReturn
S8Mali::userCreateBo(uint32_t size, uint32_t flags, uint32_t *handle, uint64_t *offset)
{
	if (fLock == NULL || size == 0) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	S8GpuBo *bo = allocBo(size, flags, handle);
	IOReturn ret = kIOReturnNoMemory;
	if (bo != NULL && (!fPowered || mapBo(bo))) {
		*offset = bo->va;
		static unsigned logged;
		if (logged++ < 16) {
			IOLog("S8 GPU: user BO %u size %u flags %u VA 0x%llx\n",
			    *handle, size, flags, bo->va);
		}
		ret = kIOReturnSuccess;
	} else if (bo != NULL) {
		freeBo(bo);
	}
	IOLockUnlock(fLock);
	return ret;
}

IOReturn
S8Mali::userFreeBo(uint32_t handle)
{
	if (fLock == NULL) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	S8GpuBo *bo = findBo(handle);
	if (bo != NULL) {
		freeBo(bo);
	}
	IOLockUnlock(fLock);
	return bo != NULL ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn
S8Mali::userMmapBo(uint32_t handle, uint64_t *offset)
{
	S8GpuBo *bo = findBo(handle);
	if (bo == NULL) {
		return kIOReturnBadArgument;
	}
	*offset = bo->va;
	return kIOReturnSuccess;
}

IOReturn
S8Mali::userGetBoOffset(uint32_t handle, uint64_t *offset)
{
	return userMmapBo(handle, offset);
}

IOReturn
S8Mali::userSubmit(uint64_t jc, uint32_t requirements)
{
	if (fLock == NULL) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	IOReturn ret = kIOReturnBadArgument;
	uint64_t size = 0;
	uint64_t cpu = vaToCpu(jc, &size);
	if (cpu != 0 && gpuAcquire()) {
		for (unsigned i = 0; i < S8_GPU_MAX_BOS; i++) {
			if (fBos[i] != NULL) {
				flush_dcache((vm_offset_t)fBos[i]->vaCpu,
				    (unsigned)fBos[i]->size, 0);
			}
		}
		uint32_t raw = 0;
		unsigned slot = (requirements & 1u) ? 0 : 1;	/* PANFROST_JD_REQ_FS */
		bool done = runJob(jc, (uint32_t *)cpu, &raw, slot);
		for (unsigned i = 0; i < S8_GPU_MAX_BOS; i++) {
			if (fBos[i] != NULL) {
				flush_dcache((vm_offset_t)fBos[i]->vaCpu,
				    (unsigned)fBos[i]->size, 0);
			}
		}
		uint32_t status = *(volatile uint32_t *)cpu;
		static uint32_t s8_submit_ok;
		if (done && status == 1) {
			if ((s8_submit_ok++ % 256) == 0) {
				IOLog("S8 GPU: submit ok %u, job 0x%llx slot %u\n",
				    s8_submit_ok, jc, slot);
			}
			ret = kIOReturnSuccess;
		} else {
			IOLog("S8 GPU: submit status 0x%x RAWSTAT 0x%x fault 0x%x at 0x%x "
			    "(job 0x%llx)\n", status, raw, rd(AS0 + AS_FAULTSTATUS),
			    rd(AS0 + AS_FAULTADDRESS_LO), jc);
			/* A fault leaves the slot and the MMU stopped: start again so
			 * the next submit has a working GPU. Buffers stay mapped. */
			if (gpuReset() && gpuPowerUp()) {
				asUpdate(fTablesPhys | LPAE_TRANSTAB);
				for (unsigned i = 0; i < S8_GPU_MAX_BOS; i++) {
					if (fBos[i] != NULL) {
						mapBo(fBos[i]);
					}
				}
			}
			ret = kIOReturnTimeout;
		}
	}
	IOLockUnlock(fLock);
	return ret;
}

IOReturn
S8Mali::userWaitBo(uint32_t handle, int64_t timeout_ns)
{
	(void)timeout_ns;
	return findBo(handle) != NULL ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn
S8Mali::userMadvise(uint32_t handle, uint32_t madv, uint32_t *retained)
{
	(void)madv;
	if (findBo(handle) == NULL) {
		return kIOReturnBadArgument;
	}
	*retained = 1;					/* pages are wired and kept */
	return kIOReturnSuccess;
}

IOMemoryDescriptor *
S8Mali::userBoMemory(vm_offset_t type)
{
	S8GpuBo *bo = findBo((uint32_t)type);
	if (bo == NULL) {
		for (unsigned i = 0; i < S8_GPU_MAX_BOS; i++) {
			if (fBos[i] != NULL && fBos[i]->va == (uint64_t)type) {
				bo = fBos[i];
				break;
			}
		}
	}
	if (bo == NULL) {
		return NULL;
	}
	bo->mem->retain();
	return bo->mem;
}

/* The GPU has its own TMU (tmuctrl_2 at 0x10084000). Sensor 0: code in
 * CURRENT_TEMP at 0x40, trim at 0, two-point 25/85 C, as the CPU TMU. */
static int
s8_gpu_temp(void)
{
	volatile uint8_t *tmu = (volatile uint8_t *)ml_io_map(0x10084000, 0x1000);
	if (tmu == NULL) {
		return 0;
	}
	uint32_t two_point = (*(volatile uint32_t *)tmu >> 23) & 1;
	uint32_t trim = *(volatile uint32_t *)(tmu + 0);
	int e1 = (int)(trim & 0x1ff), e2 = (int)((trim >> 9) & 0x1ff);
	int code = (int)((*(volatile uint32_t *)(tmu + 0x40)) & 0x1ff);
	if (e1 == 0 || (two_point && e2 <= e1)) {
		return 0;
	}
	int t = two_point ? (code - e1) * (85 - 25) / (e2 - e1) + 25 : code - e1 + 25;
	return (t < 5 || t > 125) ? 0 : t;
}

/* Exercise the calls the user client exposes, without a user process:
 * create two BOs, submit a chain through the GPU address of the first that
 * writes into the second, wait, advise and free. Boot arg s8gpu=3. */
bool
S8Mali::uapiTest(void)
{
	uint32_t ha = 0, hb = 0;
	uint64_t va = 0, va2 = 0;

	if (!userOpen()) {
		IOLog("S8 GPU: UAPI open failed\n");
		return false;
	}
	if (userCreateBo(0x2000, 0, &ha, &va) != kIOReturnSuccess ||
	    userCreateBo(0x1000, 0, &hb, &va2) != kIOReturnSuccess) {
		IOLog("S8 GPU: UAPI create failed\n");
		return false;
	}
	uint64_t cpuA = vaToCpu(va, NULL), cpuB = vaToCpu(va2, NULL);
	if (cpuA == 0 || cpuB == 0) {
		IOLog("S8 GPU: UAPI address lookup failed\n");
		return false;
	}
	uint32_t *job = (uint32_t *)cpuA;
	uint64_t target = va2 + 0x40;
	for (unsigned i = 0; i < 16; i++) {
		job[i] = 0;
	}
	job[4] = 1u | 2u << 1 | 1u << 16;
	job[8] = (uint32_t)target;
	job[9] = (uint32_t)(target >> 32);
	job[10] = 6;
	job[12] = 0x5a5a1234u;

	IOReturn ret = userSubmit(va, 0);
	uint32_t got = *(volatile uint32_t *)((uint8_t *)cpuB + 0x40);
	uint32_t retained = 0;
	userWaitBo(hb, 0);
	userMadvise(hb, 0, &retained);
	userFreeBo(ha);
	userFreeBo(hb);
	IOLog("S8 GPU: UAPI submit 0x%x -> B[0x40] 0x%x retained %u\n",
	    ret, got, retained);
	return ret == kIOReturnSuccess && got == 0x5a5a1234u;
}

/* IOResources matching runs more than once during boot; the self test runs
 * on the first instance only. */
static bool s8_gpu_ran;

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
	if (s8_gpu_ran) {
		return true;
	}
	s8_gpu_ran = true;
	fMali = NULL;
	fPmu = NULL;
	fBlock = NULL;
	fBlockVa = NULL;
	fBlockPhys = 0;
	fTables = NULL;
	fTablesPhys = 0;
	fL3Count = 0;
	fBoCount = 0;
	fLock = NULL;
	fPowered = false;

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

	IOLog("S8 GPU: TMU %d C before the test\n", s8_gpu_temp());
	bool ok = selfTest();
	IOLog("S8 GPU: TMU %d C after the test\n", s8_gpu_temp());
	if (ok) {
		IOLog("S8 GPU: milestone 2a passed: a WRITE_VALUE job ran from XNU\n");
	}
	setProperty(kIOUserClientClassKey, "S8MaliUserClient");
	bool bo_ok = false;
	if (ok && tablesInit() && asUpdate(fTablesPhys | LPAE_TRANSTAB)) {
		bo_ok = boTest();
		if (bo_ok) {
			IOLog("S8 GPU: milestone 2b (1/2) passed: BOs and a fault\n");
		}
	}
	if (bo_ok && enable >= 3) {
		bool uapi_ok = uapiTest();
		IOLog("S8 GPU: UAPI test %s\n", uapi_ok ? "passed" : "failed");
	}
	if (!fTables) {
		asUpdate(0);
	}
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
