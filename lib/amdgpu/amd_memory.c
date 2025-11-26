/* SPDX-License-Identifier: MIT
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 */

#include "amd_memory.h"
#include "amd_PM4.h"
#include "amd_ip_blocks.h"
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <inttypes.h>

/**
 *
 * @param device_handle
 * @param size
 * @param alignment
 * @param type
 * @param flags
 * @param vmc_addr
 * @param va_handle
 * @return
 */
 amdgpu_bo_handle
 gpu_mem_alloc(amdgpu_device_handle device_handle,
				      uint64_t size,
				      uint64_t alignment,
				      uint32_t type,
				      uint64_t alloc_flags,
				      uint64_t *vmc_addr,
				      amdgpu_va_handle *va_handle)
{
	struct amdgpu_bo_alloc_request req = {
		.alloc_size = size,
		.phys_alignment = alignment,
		.preferred_heap = type,
		.flags = alloc_flags,
	};
	amdgpu_bo_handle buf_handle;
	int r;

	r = amdgpu_bo_alloc(device_handle, &req, &buf_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_alloc(device_handle,
				  amdgpu_gpu_va_range_general,
				  size, alignment, 0, vmc_addr,
				  va_handle, 0);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(buf_handle, 0, size, *vmc_addr, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	return buf_handle;
}

 /**
  *
  * @param dev
  * @param size
  * @param alignment
  * @param heap
  * @param flags
  * @param bo
  * @return
  */
int
amdgpu_bo_alloc_wrap(amdgpu_device_handle dev, unsigned size,
		     unsigned alignment, unsigned heap, uint64_t flags,
		     amdgpu_bo_handle *bo)
{
	amdgpu_bo_handle buf_handle;
	int r;
	struct amdgpu_bo_alloc_request req = {
		.alloc_size = size,
		.phys_alignment = alignment,
		.preferred_heap = heap,
		.flags = flags,
	};

	r = amdgpu_bo_alloc(dev, &req, &buf_handle);
	if (r)
		return r;

	*bo = buf_handle;

	return 0;
}

 /**
  *
  * @param bo
  * @param va_handle
  * @param vmc_addr
  * @param size
  */
 void
 gpu_mem_free(amdgpu_bo_handle bo,
			 amdgpu_va_handle va_handle,
			 uint64_t vmc_addr,
			 uint64_t size)
{
	int r;

	r = amdgpu_bo_va_op(bo, 0, size, vmc_addr, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_free(va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_free(bo);
	igt_assert_eq(r, 0);
}

/**
 *
 * @param dev
 * @param size
 * @param alignment
 * @param heap
 * @param flags
 * @param bo
 * @param cpu
 * @param mc_address
 * @param va_handle
 * @return
 */
int
amdgpu_bo_alloc_and_map(amdgpu_device_handle dev, unsigned size,
			unsigned alignment, unsigned heap, uint64_t flags,
			amdgpu_bo_handle *bo, void **cpu, uint64_t *mc_address,
			amdgpu_va_handle *va_handle)
{
	struct amdgpu_bo_alloc_request request = {
		.alloc_size = size,
		.phys_alignment = alignment,
		.preferred_heap = heap,
		.flags = flags,
	};
	amdgpu_bo_handle buf_handle;
	amdgpu_va_handle handle;
	uint64_t vmc_addr;
	int r;

	r = amdgpu_bo_alloc(dev, &request, &buf_handle);
	if (r)
		return r;

	r = amdgpu_va_range_alloc(dev,
				  amdgpu_gpu_va_range_general,
				  size, alignment, 0, &vmc_addr,
				  &handle, 0);
	if (r)
		goto error_va_alloc;

	r = amdgpu_bo_va_op(buf_handle, 0, size, vmc_addr, 0, AMDGPU_VA_OP_MAP);
	if (r)
		goto error_va_map;

	r = amdgpu_bo_cpu_map(buf_handle, cpu);
	if (r)
		goto error_cpu_map;

	*bo = buf_handle;
	*mc_address = vmc_addr;
	*va_handle = handle;

	return 0;

error_cpu_map:
	amdgpu_bo_cpu_unmap(buf_handle);

error_va_map:
	amdgpu_bo_va_op(buf_handle, 0, size, vmc_addr, 0, AMDGPU_VA_OP_UNMAP);

error_va_alloc:
	amdgpu_bo_free(buf_handle);
	return r;
}

int
amdgpu_bo_alloc_and_map_sync(amdgpu_device_handle dev, unsigned int size,
			     unsigned int alignment, unsigned int heap, uint64_t flags,
			     uint64_t mapping_flags, amdgpu_bo_handle *bo, void **cpu,
			     uint64_t *mc_address, amdgpu_va_handle *va_handle,
			     uint32_t timeline_syncobj_handle, uint64_t point, bool sync)
{
	if (sync)
		return amdgpu_bo_alloc_and_map_uq(dev, size, alignment, heap, flags,
						  mapping_flags, bo, cpu,
						  mc_address, va_handle,
						  timeline_syncobj_handle, point);
	else
		return amdgpu_bo_alloc_and_map(dev, size, alignment, heap, flags,
					       bo, cpu, mc_address, va_handle);
}

int
amdgpu_bo_alloc_and_map_raw(amdgpu_device_handle dev, unsigned size,
			unsigned alignment, unsigned heap, uint64_t alloc_flags,
			uint64_t mapping_flags, amdgpu_bo_handle *bo, void **cpu,
			uint64_t *mc_address, amdgpu_va_handle *va_handle)
{
	struct amdgpu_bo_alloc_request request = {};
	amdgpu_bo_handle buf_handle;
	amdgpu_va_handle handle;
	uint64_t vmc_addr;
	int r;

	request.alloc_size = size;
	request.phys_alignment = alignment;
	request.preferred_heap = heap;
	request.flags = alloc_flags;

	r = amdgpu_bo_alloc(dev, &request, &buf_handle);
	if (r)
		return r;

	r = amdgpu_va_range_alloc(dev,
				  amdgpu_gpu_va_range_general,
				  size, alignment, 0, &vmc_addr,
				  &handle, 0);
	if (r)
		goto error_va_alloc;

	r = amdgpu_bo_va_op_raw(dev, buf_handle, 0,  ALIGN(size, getpagesize()), vmc_addr,
				   AMDGPU_VM_PAGE_READABLE |
				   AMDGPU_VM_PAGE_WRITEABLE |
				   AMDGPU_VM_PAGE_EXECUTABLE |
				   mapping_flags,
				   AMDGPU_VA_OP_MAP);
	if (r)
		goto error_va_map;

	r = amdgpu_bo_cpu_map(buf_handle, cpu);
	if (r)
		goto error_cpu_map;

	*bo = buf_handle;
	*mc_address = vmc_addr;
	*va_handle = handle;

	return 0;

 error_cpu_map:
	amdgpu_bo_cpu_unmap(buf_handle);

 error_va_map:
	amdgpu_bo_va_op(buf_handle, 0, size, vmc_addr, 0, AMDGPU_VA_OP_UNMAP);

 error_va_alloc:
	amdgpu_bo_free(buf_handle);
	return r;
}

/**
 *
 * @param bo
 * @param va_handle
 * @param mc_addr
 * @param size
 */
void
amdgpu_bo_unmap_and_free(amdgpu_bo_handle bo, amdgpu_va_handle va_handle,
			 uint64_t mc_addr, uint64_t size)
{
	amdgpu_bo_cpu_unmap(bo);
	amdgpu_bo_va_op(bo, 0, size, mc_addr, 0, AMDGPU_VA_OP_UNMAP);
	amdgpu_va_range_free(va_handle);
	amdgpu_bo_free(bo);
}

/**
 *
 * @param dev
 * @param bo1
 * @param bo2
 * @param list
 * @return
 */
int
amdgpu_get_bo_list(amdgpu_device_handle dev, amdgpu_bo_handle bo1,
		   amdgpu_bo_handle bo2, amdgpu_bo_list_handle *list)
{
	amdgpu_bo_handle resources[] = {bo1, bo2};

	return amdgpu_bo_list_create(dev, bo2 ? 2 : 1, resources, NULL, list);
}

/**
 * MULTI FENCE
 * @param device
 * @param wait_all
 */
void amdgpu_command_submission_multi_fence_wait_all(amdgpu_device_handle device,
						    bool wait_all)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle ib_result_handle, ib_result_ce_handle;
	void *ib_result_cpu, *ib_result_ce_cpu;
	uint64_t ib_result_mc_address, ib_result_ce_mc_address;
	struct amdgpu_cs_request ibs_request[2] = {};
	struct amdgpu_cs_ib_info ib_info[2];
	struct amdgpu_cs_fence fence_status[2] = {};
	uint32_t *ptr;
	uint32_t expired;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle, va_handle_ce;
	int r;
	int i, ib_cs_num = 2;

	r = amdgpu_cs_ctx_create(device, &context_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ib_result_handle, &ib_result_cpu,
				    &ib_result_mc_address, &va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ib_result_ce_handle, &ib_result_ce_cpu,
				    &ib_result_ce_mc_address, &va_handle_ce);
	igt_assert_eq(r, 0);

	r = amdgpu_get_bo_list(device, ib_result_handle,
			       ib_result_ce_handle, &bo_list);
	igt_assert_eq(r, 0);

	memset(ib_info, 0, 2 * sizeof(struct amdgpu_cs_ib_info));

	/* IT_SET_CE_DE_COUNTERS valid for gfx 6-10 */
	ptr = ib_result_ce_cpu;
	ptr[0] = PACKET3(PACKET3_SET_CE_DE_COUNTERS, 0);
	ptr[1] = 0;
	ptr[2] = PACKET3(PACKET3_INCREMENT_CE_COUNTER, 0);
	ptr[3] = 1;
	ib_info[0].ib_mc_address = ib_result_ce_mc_address;
	ib_info[0].size = 4;
	ib_info[0].flags = AMDGPU_IB_FLAG_CE;

	ptr = ib_result_cpu;
	ptr[0] = PACKET3(PACKET3_WAIT_ON_CE_COUNTER, 0);
	ptr[1] = 1;//Conditional Surface Sync for wrapping CE buffers.
	ib_info[1].ib_mc_address = ib_result_mc_address;
	ib_info[1].size = 2;

	for (i = 0; i < ib_cs_num; i++) {
		ibs_request[i].ip_type = AMDGPU_HW_IP_GFX;
		ibs_request[i].number_of_ibs = 2;
		ibs_request[i].ibs = ib_info;
		ibs_request[i].resources = bo_list;
		ibs_request[i].fence_info.handle = NULL;
	}

	r = amdgpu_cs_submit(context_handle, 0,ibs_request, ib_cs_num);

	igt_assert_eq(r, 0);

	for (i = 0; i < ib_cs_num; i++) {
		fence_status[i].context = context_handle;
		fence_status[i].ip_type = AMDGPU_HW_IP_GFX;
		fence_status[i].fence = ibs_request[i].seq_no;
	}

	r = amdgpu_cs_wait_fences(fence_status, ib_cs_num, wait_all,
				  AMDGPU_TIMEOUT_INFINITE,
				  &expired, NULL);
	igt_assert_eq(r, 0);

	amdgpu_bo_unmap_and_free(ib_result_handle, va_handle,
				 ib_result_mc_address, 4096);

	amdgpu_bo_unmap_and_free(ib_result_ce_handle, va_handle_ce,
				 ib_result_ce_mc_address, 4096);

	r = amdgpu_bo_list_destroy(bo_list);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_ctx_free(context_handle);
	igt_assert_eq(r, 0);
}

/*
 * Get available GTT size of the GPU node
 * Equivalent to Thunk's hsaKmtAvailableMemory function, using amdgpu_query_heap_info
 * to fetch max allocatable size from GTT domain heap.
 */
uint64_t get_available_system_memory(amdgpu_device_handle dev)
{
	struct amdgpu_heap_info gtt_heap;
	int ret;

	ret = amdgpu_query_heap_info(dev, AMDGPU_GEM_DOMAIN_GTT, 0, &gtt_heap);
	if (ret != 0) {
		fprintf(stderr, "Failed to query VRAM heap: %d\n", ret);
		return 0;
	}

	return gtt_heap.max_allocation;
}

/*
 * Get available VRAM size of the GPU node
 * Equivalent to Thunk's hsaKmtAvailableMemory function, using amdgpu_query_heap_info
 * to fetch max allocatable size from VRAM domain heap.
 */
uint64_t get_available_vram(amdgpu_device_handle dev)
{
	struct amdgpu_heap_info vram_heap;
	int ret;

	ret = amdgpu_query_heap_info(dev, AMDGPU_GEM_DOMAIN_VRAM, 0, &vram_heap);
	if (ret != 0) {
		fprintf(stderr, "Failed to query VRAM heap: %d\n", ret);
		return 0;
	}

	return vram_heap.max_allocation;
}

/**
 * get_gpu_memory_info - Get GPU memory information
 * @device: AMDGPU device handle
 * @vram_size: Output for VRAM usable heap size
 * @cpu_accessible_vram_size: Output for CPU accessible VRAM usable heap size
 * @gtt_size: Output for GTT usable heap size
 *
 * Returns 0 on success, error code on failure.
 */
int get_gpu_memory_info(amdgpu_device_handle device,
			       uint64_t *vram_size,
			       uint64_t *cpu_accessible_vram_size,
			       uint64_t *gtt_size)
{
	struct drm_amdgpu_memory_info mem_info;
	int r;

	r = amdgpu_query_info(device, AMDGPU_INFO_MEMORY, sizeof(mem_info), &mem_info);
	if (r) {
		igt_info("Failed to query GPU memory info, error: %d\n", r);
		return r;
	}

	if (vram_size)
		*vram_size = mem_info.vram.usable_heap_size;

	if (cpu_accessible_vram_size)
		*cpu_accessible_vram_size = mem_info.cpu_accessible_vram.usable_heap_size;

	if (gtt_size)
		*gtt_size = mem_info.gtt.usable_heap_size;

	igt_debug("GPU Memory Info: VRAM=%" PRIu64 " MB, CPU_ACCESSIBLE_VRAM=%" PRIu64 " MB, GTT=%" PRIu64 " MB\n",
		(uint64_t)(mem_info.vram.usable_heap_size / (1024 * 1024)),
		(uint64_t)(mem_info.cpu_accessible_vram.usable_heap_size / (1024 * 1024)),
		(uint64_t)(mem_info.gtt.usable_heap_size / (1024 * 1024)));
	return 0;
}

/**
 * Create a DMA-BUF to encapsulate system memory (using amdgpu_bo_alloc instead of DRM_IOCTL_GEM_CREATE)
 * Workflow: Allocate GPU-accessible BO -> Map to user space -> Copy system memory data -> Export as DMA-BUF
 */
int create_dmabuf(amdgpu_device_handle dev, void *sys_addr, size_t size)
{
	struct amdgpu_bo *bo = NULL;
	struct amdgpu_bo_alloc_request alloc_req = {0};
	void *bo_map = NULL;
	int dmabuf_fd = -1;
	int ret;

	/* 1. Allocate GPU buffer object (replaces DRM_IOCTL_GEM_CREATE) */
	alloc_req.alloc_size = size;
	alloc_req.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;  // Allow CPU access
	alloc_req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;  // Use GTT domain (system memory accessible)
	ret = amdgpu_bo_alloc(dev, &alloc_req, &bo);
	if (ret != 0) {
		igt_warn("amdgpu_bo_alloc failed: %d\n", ret);
		return -1;
	}

	/* 2. Map BO to user space (facilitates copying system memory data) */
	ret = amdgpu_bo_cpu_map(bo, &bo_map);
	if (ret != 0) {
		igt_warn("amdgpu_bo_cpu_map failed: %d\n", ret);
		goto free_bo;
	}

	/* 3. Copy system memory data to GPU BO */
	memcpy(bo_map, sys_addr, size);

	/* 4. Unmap from user space (data copy completed) */
	amdgpu_bo_cpu_unmap(bo);
	bo_map = NULL;

	/* 5. Export BO as DMA-BUF (replaces DRM_IOCTL_PRIME_HANDLE_TO_FD) */
	ret = amdgpu_bo_export(bo, amdgpu_bo_handle_type_dma_buf_fd, (uint32_t *)&dmabuf_fd);
	if (ret != 0) {
		igt_warn("amdgpu_bo_export failed: %d\n", ret);
		goto free_bo;
	}

	/* 6. Free BO handle (DMA-BUF exists independently) */
	amdgpu_bo_free(bo);
	return dmabuf_fd;

free_bo:
	if (bo)
		amdgpu_bo_free(bo);
	return -1;
}

/**
 * virtual_mem_to_gpu_bo - Map user-space virtual memory to GPU buffer object
 * @device: AMDGPU device handle
 * @cpu_ptr: Pointer to user-space memory to map
 * @size: Size of the memory region to map
 * @out_bo: Output pointer for the created GPU buffer handle
 * @out_gpu_va: Output pointer for the GPU virtual address of the mapped buffer
 * @out_va_handle: Output pointer for the VA range handle
 *
 * This function converts a user-space memory region to a DMA-BUF, imports it
 * as a GPU buffer object (BO), and maps it to the GPU's virtual address space.
 * Returns 0 on success, negative error code on failure.
 */
int virtual_mem_to_gpu_bo(amdgpu_device_handle device,
			  void *cpu_ptr,
				 size_t size,
				 amdgpu_bo_handle *out_bo,
				 uint64_t *out_gpu_va,
				 amdgpu_va_handle *out_va_handle)
{
	int r;
	int dmabuf_fd;
	struct amdgpu_bo_import_result import_result = {0};
	uint64_t aligned_size = ALIGN(size, getpagesize());
	uint64_t va_base;
	amdgpu_va_handle va_handle;

	/* Convert user memory to DMA-BUF */
	dmabuf_fd = create_dmabuf(device, cpu_ptr, size);
	if (dmabuf_fd < 0) {
		igt_warn("Failed to create DMA-BUF fd\n");
		return -1;
	}

	/* Import DMA-BUF into AMDGPU as a buffer object */
	r = amdgpu_bo_import(device,
			     amdgpu_bo_handle_type_dma_buf_fd,
			     dmabuf_fd,
			     &import_result);
	close(dmabuf_fd); /* FD no longer needed after import */
	if (r != 0) {
		igt_warn("amdgpu_bo_import failed: %d\n", r);
		return r;
	}

	/* Allocate GPU virtual address range for the buffer */
	r = amdgpu_va_range_alloc(device,
				  amdgpu_gpu_va_range_general,
				 aligned_size,
				 getpagesize(),
				 0,
				 &va_base,
				 &va_handle,
				 0);
	if (r != 0) {
		igt_warn("amdgpu_va_range_alloc failed: %d\n", r);
		amdgpu_bo_free(import_result.buf_handle);
		return r;
	}

	/* Map buffer object to the allocated GPU virtual address */
	r = amdgpu_bo_va_op(import_result.buf_handle,
			    0,
			   aligned_size,
			   va_base,
			   AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE,
			   AMDGPU_VA_OP_MAP);
	if (r != 0) {
		igt_warn("amdgpu_bo_va_op failed: %d\n", r);
		amdgpu_va_range_free(va_handle);
		amdgpu_bo_free(import_result.buf_handle);
		return r;
	}

	/* Output results */
	*out_bo = import_result.buf_handle;
	*out_gpu_va = va_base;
	*out_va_handle = va_handle;

	return 0;
}

/**
 * Virtual memory allocation wrapper
 */
void *virtual_alloc_memory(void *address, unsigned int size, int memProtection)
{
	void *ptr;
	/**
	* Memory protection flags mapping table
	*
	* This array maps logical memory protection combinations to the corresponding
	* POSIX mmap protection flags. The index corresponds to the memory protection
	* type defined in the test framework.
	*
	* Index mapping:
	* 0: MEM_NONE      - No access permissions
	* 1: MEM_READ      - Read-only access
	* 2: MEM_WRITE     - Write-only access
	* 3: MEM_READ_WRITE - Read and write access
	* 4: MEM_EXEC      - Execute-only access
	* 5: MEM_EXEC_READ - Execute and read access
	* 6: MEM_EXEC_WRITE - Execute and write access
	* 7: MEM_EXEC_READ_WRITE - Execute, read and write access
	*
	* Note: Some combinations like write-only (PROT_WRITE) may not be supported
	* on all systems and typically require read permission as well.
	*/
	static int protection_flags[8] = {
	PROT_NONE,                           // 0: No access
	PROT_READ,                           // 1: Read-only
	PROT_WRITE,                          // 2: Write-only (may not work on some systems)
	PROT_READ | PROT_WRITE,              // 3: Read and write
	PROT_EXEC,                           // 4: Execute-only
	PROT_EXEC | PROT_READ,               // 5: Execute and read
	PROT_EXEC | PROT_WRITE,              // 6: Execute and write
	PROT_EXEC | PROT_READ | PROT_WRITE   // 7: Execute, read and write
	};

	ptr = mmap(address, size, protection_flags[memProtection],
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (ptr == MAP_FAILED) {
		igt_warn("mmap failed: %s\n", strerror(errno));
		ptr = NULL;
	}

	return ptr;
}

/**
 * Virtual memory free wrapper
 */
bool virtual_free_memory(void *address, unsigned int size)
{
	if (munmap(address, size) == 0)
		return true;
	else {
		igt_warn("munmap failed: %s\n", strerror(errno));
		return false;
	}
}

/**
 * Wait for specific value in memory with timeout
 */
bool wait_on_value(unsigned int *ptr, unsigned int expected)
{
	const int timeout_ms = 5000; // 5 second timeout
	int elapsed_ms = 0;

	while (*ptr != expected) {
		usleep(1000); // 1ms
		elapsed_ms++;

		if (elapsed_ms > timeout_ms) {
			igt_info("Timeout waiting for value 0x%08X, got 0x%08X\n",
				 expected, *ptr);
			return false;
		}
	}
	return true;
}
