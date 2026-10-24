/*
 * s8_test: the first user process on the S8 that talks to its GPU.
 *
 * It opens the S8Mali user client, asks for the GPU model, creates two
 * buffer objects, maps them, submits a WRITE_VALUE job through the GPU
 * address of the first that writes into the second, and checks the word.
 *
 * Output goes to stdout and to /var/log/s8-gpu-test.txt, because the phone
 * has no console while iOS runs.
 */
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

/* The panfrost structures of include/uapi/drm/panfrost_drm.h */
struct pf_create_bo {
	uint32_t size;
	uint32_t flags;
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;
};
struct pf_mmap_bo {
	uint32_t handle;
	uint32_t flags;
	uint64_t offset;
};
struct pf_get_param {
	uint32_t param;
	uint32_t pad;
	uint64_t value;
};
struct pf_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
struct pf_wait_bo {
	uint32_t handle;
	uint32_t pad;
	int64_t timeout_ns;
};
struct pf_madvise {
	uint32_t handle;
	uint32_t madv;
	uint32_t retained;
};

enum {
	PF_SUBMIT = 0x00,
	PF_WAIT_BO = 0x01,
	PF_CREATE_BO = 0x02,
	PF_MMAP_BO = 0x03,
	PF_GET_PARAM = 0x04,
	PF_GET_BO_OFFSET = 0x05,
	PF_MADVISE = 0x08,
};

static FILE *out;
static io_connect_t conn;

/* S8Mali selector 0x80 puts a line into the kernel log ring, which is the
 * only place readable from TWRP without pulling the whole root. */
#define PF_LOG 0x80

static void
say(const char *fmt, ...)
{
	static int cons = -1;
	char line[240];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	printf("%s", line);
	fflush(stdout);
	if (out != NULL) {
		fprintf(out, "%s", line);
		fflush(out);
	}
	/* /dev/console is the fallback: its writes reach the kernel log ring
	 * even when the user client cannot be opened. */
	if (cons == -1) {
		cons = open("/dev/console", O_WRONLY);
		if (cons < 0) {
			cons = -2;
		}
	}
	if (cons >= 0) {
		write(cons, line, strlen(line));
	}
	if (conn != 0) {
		IOConnectCallStructMethod(conn, PF_LOG, line, strlen(line), NULL, NULL);
	}
}

static int
call(uint32_t selector, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t r = IOConnectCallStructMethod(conn, selector, data, size,
	    data, &returned);
	return r == KERN_SUCCESS ? 0 : -1;
}

static uint32_t *
map_bo(uint32_t handle, uint64_t gpu_va)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t size = 0x1000;

	if (IOConnectMapMemory64(conn, (uint32_t)gpu_va, mach_task_self(),
	    &address, &size, kIOMapAnywhere) != KERN_SUCCESS) {
		return NULL;
	}
	(void)handle;
	return (uint32_t *)(uintptr_t)address;
}

int
main(void)
{
	io_service_t service;
	struct pf_get_param gp;
	struct pf_create_bo a, b;
	struct pf_mmap_bo ma, mb;
	struct pf_submit sub;
	struct pf_wait_bo wait;
	struct pf_madvise adv;
	uint32_t *cpu_a, *cpu_b;

	out = fopen("/var/log/s8-gpu-test.txt", "w");
	say("s8_test: start\n");

	service = IOServiceGetMatchingService(kIOMainPortDefault,
	    IOServiceMatching("S8Mali"));
	if (service == 0) {
		say("s8_test: no S8Mali service (boot with s8gpu=1)\n");
		return 1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
		say("s8_test: IOServiceOpen failed (is s8gpu=1 set?)\n");
		return 1;
	}
	say("s8_test: user client open\n");

	memset(&gp, 0, sizeof(gp));
	gp.param = 0;					/* GPU_PROD_ID */
	if (call(PF_GET_PARAM, &gp, sizeof(gp)) != 0) {
		say("s8_test: GET_PARAM failed\n");
		return 1;
	}
	say("s8_test: GPU_PROD_ID 0x%llx\n", (unsigned long long)gp.value);

	memset(&a, 0, sizeof(a));
	a.size = 0x1000;
	if (call(PF_CREATE_BO, &a, sizeof(a)) != 0) {
		say("s8_test: CREATE_BO A failed\n");
		return 1;
	}
	memset(&b, 0, sizeof(b));
	b.size = 0x1000;
	if (call(PF_CREATE_BO, &b, sizeof(b)) != 0) {
		say("s8_test: CREATE_BO B failed\n");
		return 1;
	}
	say("s8_test: A handle %u VA 0x%llx, B handle %u VA 0x%llx\n",
	    a.handle, (unsigned long long)a.offset, b.handle,
	    (unsigned long long)b.offset);

	memset(&ma, 0, sizeof(ma));
	ma.handle = a.handle;
	if (call(PF_MMAP_BO, &ma, sizeof(ma)) != 0) {
		say("s8_test: MMAP_BO A failed\n");
		return 1;
	}
	memset(&mb, 0, sizeof(mb));
	mb.handle = b.handle;
	if (call(PF_MMAP_BO, &mb, sizeof(mb)) != 0) {
		say("s8_test: MMAP_BO B failed\n");
		return 1;
	}
	cpu_a = map_bo(a.handle, ma.offset);
	cpu_b = map_bo(b.handle, mb.offset);
	if (cpu_a == NULL || cpu_b == NULL) {
		say("s8_test: mapping the buffers failed\n");
		return 1;
	}
	say("s8_test: A at %p, B at %p\n", cpu_a, cpu_b);

	/* Mesa genxml v6 WRITE_VALUE: 64-bit target, immediate 32 value. */
	memset(cpu_a, 0, 0x1000);
	cpu_a[4] = 1u | 2u << 1 | 1u << 16;
	cpu_a[8] = (uint32_t)(b.offset + 0x40);
	cpu_a[9] = (uint32_t)((b.offset + 0x40) >> 32);
	cpu_a[10] = 6;
	cpu_a[12] = 0x5a5a1234u;

	memset(&sub, 0, sizeof(sub));
	sub.jc = a.offset;
	sub.requirements = 1;				/* PANFROST_JD_REQ_FS */
	if (call(PF_SUBMIT, &sub, sizeof(sub)) != 0) {
		say("s8_test: SUBMIT failed\n");
		return 1;
	}
	say("s8_test: B[0x40] = 0x%x (want 0x5a5a1234)\n", cpu_b[0x10]);

	memset(&wait, 0, sizeof(wait));
	wait.handle = b.handle;
	say("s8_test: WAIT_BO %s\n", call(PF_WAIT_BO, &wait, sizeof(wait)) == 0 ?
	    "ok" : "failed");

	memset(&adv, 0, sizeof(adv));
	adv.handle = b.handle;
	adv.madv = 0;					/* WILLNEED */
	if (call(PF_MADVISE, &adv, sizeof(adv)) == 0) {
		say("s8_test: MADVISE retained %u\n", adv.retained);
	}

	IOServiceClose(conn);
	say("s8_test: done\n");
	return cpu_b[0x10] == 0x5a5a1234u ? 0 : 2;
}
