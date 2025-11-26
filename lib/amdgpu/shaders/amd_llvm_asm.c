/* SPDX-License-Identifier: MIT */
/* minimal runtime ISA assembly via external llvm-mc (optional) */
#include "amd_llvm_asm.h"
#include "lib/amdgpu/amdgpu_asic_addr.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <elf.h>

/* track llvm-mc availability */
static int llvm_mc_available;
static char llvm_mc_path[256];

static int find_llvm_mc(void)
{
	const char *candidates[] = {
		"/opt/rocm/llvm/bin/llvm-mc",
		"/usr/bin/llvm-mc",
		"/usr/local/bin/llvm-mc",
		NULL
	};
	size_t i;

	fprintf(stderr, "amdgpu_llvm_asm: searching llvm-mc\n");
	for (i = 0; candidates[i]; i++) {
		fprintf(stderr, "amdgpu_llvm_asm: check %s -> ", candidates[i]);
		if (access(candidates[i], X_OK) == 0) {
			fprintf(stderr, "found\n");
			snprintf(llvm_mc_path, sizeof(llvm_mc_path), "%s", candidates[i]);
			return 1;
		} else {
			fprintf(stderr, "not found (errno=%d)\n", errno);
		}
	}

	fprintf(stderr, "amdgpu_llvm_asm: llvm-mc not found\n");
	return 0;
}

int amdgpu_llvm_asm_init(void)
{
	if (!llvm_mc_available)
		llvm_mc_available = find_llvm_mc();
	return llvm_mc_available ? 0 : -ENOTSUP;
}

/* shutdown hook (noop) */
void amdgpu_llvm_asm_shutdown(void)
{
    /* No dynamic resources to free */
}

/* format mcpu string from major/minor/step */
void amdgpu_format_mcpu(unsigned int major, unsigned int minor, unsigned int step,
			char *dst, size_t dst_size)
{
	if (!dst || dst_size < 8)
		return;
	snprintf(dst, dst_size, "gfx%u%u%x", major, minor, step & 0xFF);
}

/* map family_id to representative mcpu baseline */
int amdgpu_family_id_to_mcpu(uint32_t family_id, char *dst, size_t dst_size)
{
	if (!dst || dst_size < 8)
		return -EINVAL;
	switch (family_id) {
	case FAMILY_GFX1200:
		snprintf(dst, dst_size, "gfx1200"); break;
	case FAMILY_GFX1150:
		snprintf(dst, dst_size, "gfx1150"); break;
	case FAMILY_GFX1103:
		snprintf(dst, dst_size, "gfx1103"); break;
	case FAMILY_GFX1100:
		snprintf(dst, dst_size, "gfx1100"); break;
	case FAMILY_GFX1037:
		snprintf(dst, dst_size, "gfx1037"); break;
	case FAMILY_GFX1036:
		snprintf(dst, dst_size, "gfx1036"); break;
	case FAMILY_NV:
		snprintf(dst, dst_size, "gfx1030"); break; /* RDNA baseline */
	case FAMILY_AI:
		snprintf(dst, dst_size, "gfx900");  break; /* Vega baseline */
	default:
		snprintf(dst, dst_size, "gfx803");  break; /* GCN fallback */
	}
	return 0;
}

int amdgpu_llvm_assemble(const char *mcpu,
			 const char *isa_text,
			 uint8_t *out_buf,
			 size_t out_buf_size,
			 size_t *out_size)
{
	int fd_src = -1, fd_obj = -1;
	int r = 0;
	ssize_t wlen;
	char cmd[1024];
	FILE *f;
	long sz;
	uint8_t *obj_data = NULL;
	const Elf64_Ehdr *eh;
	const Elf64_Shdr *sh_base;
	const Elf64_Shdr *sh_str;
	const char *strtab;
	const Elf64_Shdr *text_sec = NULL;
	int i;
	char src_path[256];
	char obj_path[256];
	char template_buf[256];
	const char *dirs[3];
	int d;

	if (!out_size)
	return -EINVAL;
	*out_size = 0;
	if (!mcpu || !isa_text || !out_buf)
	return -EINVAL;
	if (!llvm_mc_available && amdgpu_llvm_asm_init())
	return -ENOTSUP;

	dirs[0] = "/tmp";
	dirs[1] = "/var/tmp";
	dirs[2] = NULL;
	/* Try to add $HOME/tmp if exists */
	{
		const char *home = getenv("HOME");

		if (home) {
			struct stat st;
			char home_try[256];

			snprintf(home_try, sizeof(home_try), "%s/tmp", home);
			if (stat(home_try, &st) == 0 && S_ISDIR(st.st_mode)) {
				dirs[2] = home_try; /* Override last slot */
			}
		}
	}

	fd_src = -1;
	for (d = 0; dirs[d]; ++d) {
		snprintf(template_buf, sizeof(template_buf), "%s/amdgpu_isaXXXXXX", dirs[d]);
		fd_src = mkstemp(template_buf);
		if (fd_src >= 0) {
			/* Use mkstemp-created file directly (extension not required) */
			strncpy(src_path, template_buf, sizeof(src_path));
			src_path[sizeof(src_path) - 1] = '\0';
			break;
		}
	}
	if (fd_src < 0)
		return -errno;

	wlen = write(fd_src, isa_text, strlen(isa_text));
	if (wlen < 0 || (size_t)wlen != strlen(isa_text)) {
		r = -EIO;
		goto cleanup;
	}

	close(fd_src);
	fd_src = -1;

	/* Create object temp file */
	for (d = 0; dirs[d]; ++d) {
		snprintf(template_buf, sizeof(template_buf), "%s/amdgpu_objXXXXXX", dirs[d]);
		fd_obj = mkstemp(template_buf);
		if (fd_obj >= 0) {
			strncpy(obj_path, template_buf, sizeof(obj_path));
			obj_path[sizeof(obj_path) - 1] = '\0';
			break;
		}
	}
	if (fd_obj < 0) {
		r = -errno;
		goto cleanup;
	}
	close(fd_obj);
	fd_obj = -1;

	snprintf(cmd, sizeof(cmd),
		 "%s -triple amdgcn-amd-amdhsa -mcpu=%s -filetype=obj %s -o %s 2>/dev/null",
	llvm_mc_path, mcpu, src_path, obj_path);
	r = system(cmd);
	if (r != 0) {
		r = -EFAULT;
		goto cleanup;
	}

	f = fopen(obj_path, "rb");
	if (!f) {
		r = -errno;
		goto cleanup;
	}

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		r = -EIO;
		goto cleanup;
	}
	sz = ftell(f);
	if (sz < 0) {
		fclose(f);
		r = -EIO;
		goto cleanup;
	}

	rewind(f);
	obj_data = malloc(sz);
	if (!obj_data) {
		fclose(f);
		r = -ENOMEM;
		goto cleanup;
	}
	if (fread(obj_data, 1, sz, f) != (size_t)sz) {
		free(obj_data);
		fclose(f);
		r = -EIO;
		goto cleanup;
	}
	fclose(f);

	if ((size_t)sz < sizeof(Elf64_Ehdr)) {
		r = -EINVAL;
		goto cleanup;
	}

	eh = (const Elf64_Ehdr *)obj_data;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E') {
		r = -EINVAL;
		goto cleanup;
	}

	sh_base = (const Elf64_Shdr *)(obj_data + eh->e_shoff);
	sh_str  = &sh_base[eh->e_shstrndx];
	strtab = (const char *)(obj_data + sh_str->sh_offset);
	for (i = 0; i < (int)eh->e_shnum; i++) {
		const Elf64_Shdr *sec = &sh_base[i];
		const char *name = strtab + sec->sh_name;

		if (strcmp(name, ".text") == 0) {
			text_sec = sec;
			break;
		}
	}
	if (!text_sec) {
		r = -EINVAL;
		goto cleanup;
	}

	if (text_sec->sh_size > out_buf_size)	{
		r = -ENOSPC;
		goto cleanup;
	}

	memcpy(out_buf, obj_data + text_sec->sh_offset, text_sec->sh_size);
	*out_size = text_sec->sh_size;

cleanup:
	if (fd_src >= 0)
		close(fd_src);
	if (fd_obj >= 0)
		close(fd_obj);
	if (src_path[0])
		unlink(src_path);
	if (obj_path[0])
		unlink(obj_path);
	if (obj_data)
		free(obj_data);

	return r;
}
