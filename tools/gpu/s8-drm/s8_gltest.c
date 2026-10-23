/*
 * s8_gltest: draw a triangle with Mesa's OSMesa on the phone, offscreen,
 * and write the result as a PPM to /var/log/s8-gltest.ppm.
 *
 * It proves two things on the device: Mesa's arm64-iOS build loads and runs,
 * and its renderer produces the expected pixels. With softpipe the renderer
 * is the CPU; with panfrost it is the Mali (tools/gpu/README.md).
 *
 * Prints the renderer name and a checksum of the image on stdout, which
 * launchd writes to /var/log/s8-gltest.out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/osmesa.h>
#include <GL/gl.h>

#define W 256
#define H 256

static void
save_ppm(const char *path, const unsigned char *px)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int y = H - 1; y >= 0; y--) {	/* OSMesa rows go up */
		for (int x = 0; x < W; x++) {
			const unsigned char *p = px + (y * W + x) * 4;
			fwrite(p, 1, 3, f);	/* BGRA -> RGB, drop alpha */
		}
	}
	fclose(f);
}

int
main(void)
{
	OSMesaContext ctx;
	unsigned char *buf;
	const char *ver, *rend;
	unsigned sum = 0;

	buf = calloc(1, (size_t)W * H * 4);
	if (buf == NULL) {
		return 1;
	}
	ctx = OSMesaCreateContextExt(OSMESA_RGBA, 24, 0, 0, NULL);
	if (ctx == NULL) {
		printf("s8_gltest: OSMesaCreateContextExt failed\n");
		return 1;
	}
	if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H)) {
		printf("s8_gltest: OSMesaMakeCurrent failed\n");
		return 1;
	}
	ver = (const char *)glGetString(GL_VERSION);
	rend = (const char *)glGetString(GL_RENDERER);
	printf("s8_gltest: GL_VERSION %s\n", ver != NULL ? ver : "(null)");
	printf("s8_gltest: GL_RENDERER %s\n", rend != NULL ? rend : "(null)");

	glViewport(0, 0, W, H);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glColor3f(1.0f, 0.0f, 0.0f);
	glVertex2f(-0.8f, -0.8f);
	glColor3f(0.0f, 1.0f, 0.0f);
	glVertex2f(0.8f, -0.8f);
	glColor3f(0.0f, 0.0f, 1.0f);
	glVertex2f(0.0f, 0.8f);
	glEnd();
	glFinish();

	for (size_t i = 0; i < (size_t)W * H * 4; i++) {
		sum = sum * 31u + buf[i];
	}
	printf("s8_gltest: checksum 0x%08x\n", sum);
	/* The centre of the triangle must not be black. */
	{
		unsigned char *c = buf + ((H / 2) * W + W / 2) * 4;
		printf("s8_gltest: centre BGRA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
	}
	save_ppm("/var/log/s8-gltest.ppm", buf);
	OSMesaDestroyContext(ctx);
	free(buf);
	printf("s8_gltest: done\n");
	return 0;
}
