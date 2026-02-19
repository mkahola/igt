/* SPDX-License-Identifier: MIT
 * Copyright 2012 Advanced Micro Devices, Inc.
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

#ifndef AMD_COMMAND_SUBMISSION
#define AMD_COMMAND_SUBMISSION

#include "amd_ip_blocks.h"

/**
 * Packet types supported by the command submission library
 */
typedef enum {
    CMD_PACKET_WRITE_LINEAR,
    CMD_PACKET_WRITE_ATOMIC,
    CMD_PACKET_COPY_LINEAR,
    CMD_PACKET_COPY_ATOMIC,
    CMD_PACKET_FENCE,
    CMD_PACKET_TIMESTAMP,
} cmd_packet_type_t;

/**
 * Command submission context structure
 */
typedef struct {
    amdgpu_device_handle device;
    struct amdgpu_ring_context *ring_ctx;
    const struct amdgpu_ip_block_version *ip_block;
    enum amd_ip_block_type ip_type;
    bool initialized;
    bool user_queue;
    bool uses_external_bo;
    uint64_t last_submit_seq; /* Last submission sequence number */
} cmd_context_t;

/**
 * Packet building parameters
 */
typedef struct {
    cmd_packet_type_t type;
    uint64_t src_addr;
    uint64_t dst_addr;
    uint32_t size;
    uint32_t data;
    uint32_t *custom_data;
    uint32_t custom_data_size;
} cmd_packet_params_t;

int amdgpu_test_exec_cs_helper(amdgpu_device_handle device,
				unsigned int ip_type, struct amdgpu_ring_context *ring_context,
				int expect);

void amdgpu_command_submission_write_linear_helper(amdgpu_device_handle device,
						   const struct amdgpu_ip_block_version *ip_block,
						   bool secure, bool user_queue);

void amdgpu_command_submission_write_linear_helper2(amdgpu_device_handle device, unsigned type,
						    bool secure, bool user_queue);

void amdgpu_command_submission_const_fill_helper(amdgpu_device_handle device,
						 const struct amdgpu_ip_block_version *ip_block,
						 bool user_queue);

void amdgpu_command_submission_copy_linear_helper(amdgpu_device_handle device,
						 const struct amdgpu_ip_block_version *ip_block,
						 bool user_queue);

void  amdgpu_command_ce_write_fence(amdgpu_device_handle dev,
					  amdgpu_context_handle ctx);

cmd_context_t* cmd_context_create(amdgpu_device_handle device,
                                    enum amd_ip_block_type ip_type,
                                    uint32_t ring_id,
                                    bool user_queue,
                                    uint32_t write_length,
                                    amdgpu_bo_handle external_bo,
                                    uint64_t external_bo_mc,
                                    volatile uint32_t *external_bo_cpu);
void cmd_context_destroy(cmd_context_t *ctx, bool destroy_external_bo);

int cmd_place_packet(cmd_context_t *ctx, const cmd_packet_params_t *params);

int cmd_submit_packet(cmd_context_t *ctx);

int cmd_place_and_submit_packet(cmd_context_t *ctx, const cmd_packet_params_t *params);

int cmd_wait_completion(cmd_context_t *ctx);

int cmd_submit_write_linear(cmd_context_t *ctx, uint64_t dst_addr, uint32_t size, uint32_t data);

int cmd_submit_copy_linear(cmd_context_t *ctx, uint64_t src_addr, uint64_t dst_addr, uint32_t size);

int cmd_submit_atomic(cmd_context_t *ctx, uint64_t dst_addr, uint32_t data);

bool cmd_ring_available(amdgpu_device_handle device,
                        enum amd_ip_block_type ip_type,
                       uint32_t ring_id,
                       bool user_queue);

uint32_t cmd_get_available_rings(amdgpu_device_handle device,
                                 enum amd_ip_block_type ip_type,
                                bool user_queue);

/**
 * Query HW and populate DMA transfer limits.
 * All fields in the returned struct are in BYTES.
 */
void amdgpu_dma_limits_query(amdgpu_device_handle device,
                             struct amdgpu_dma_limits *limits);

#endif
