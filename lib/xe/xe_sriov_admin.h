/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2025 Intel Corporation. All rights reserved.
 */

#ifndef __XE_SRIOV_ADMIN_H__
#define __XE_SRIOV_ADMIN_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "xe_sriov_provisioning.h" /* for enum xe_sriov_sched_priority */

struct igt_sysfs_choice;

bool xe_sriov_admin_is_present(int pf_fd);

int  __xe_sriov_admin_set_exec_quantum_ms(int pf_fd, unsigned int vf_num, uint32_t eq_ms);
void  xe_sriov_admin_set_exec_quantum_ms(int pf_fd, unsigned int vf_num, uint32_t eq_ms);
int  __xe_sriov_admin_get_exec_quantum_ms(int pf_fd, unsigned int vf_num, uint32_t *eq_ms);
uint32_t xe_sriov_admin_get_exec_quantum_ms(int pf_fd, unsigned int vf_num);
int  __xe_sriov_admin_set_preempt_timeout_us(int pf_fd, unsigned int vf_num, uint32_t pt_us);
void  xe_sriov_admin_set_preempt_timeout_us(int pf_fd, unsigned int vf_num, uint32_t pt_us);
int  __xe_sriov_admin_get_preempt_timeout_us(int pf_fd, unsigned int vf_num, uint32_t *pt_us);
uint32_t xe_sriov_admin_get_preempt_timeout_us(int pf_fd, unsigned int vf_num);
int __xe_sriov_admin_set_sched_priority_string(int pf_fd, unsigned int vf_num,
					       const char *prio);
int __xe_sriov_admin_get_sched_priority_choice(int pf_fd, unsigned int vf_num,
					       struct igt_sysfs_choice *choice);
int __xe_sriov_admin_set_sched_priority(int pf_fd, unsigned int vf_num,
					enum xe_sriov_sched_priority prio);
void xe_sriov_admin_set_sched_priority(int pf_fd, unsigned int vf_num,
				       enum xe_sriov_sched_priority prio);
int __xe_sriov_admin_get_sched_priority(int pf_fd, unsigned int vf_num,
					enum xe_sriov_sched_priority *prio,
					unsigned int *prio_mask);
enum xe_sriov_sched_priority
xe_sriov_admin_get_sched_priority(int pf_fd, unsigned int vf_num,
				  unsigned int *prio_mask);

int  __xe_sriov_admin_bulk_set_exec_quantum_ms(int pf_fd, uint32_t eq_ms);
void  xe_sriov_admin_bulk_set_exec_quantum_ms(int pf_fd, uint32_t eq_ms);
int  __xe_sriov_admin_bulk_set_preempt_timeout_us(int pf_fd, uint32_t pt_us);
void  xe_sriov_admin_bulk_set_preempt_timeout_us(int pf_fd, uint32_t pt_us);
int  __xe_sriov_admin_bulk_set_sched_priority_string(int pf_fd, const char *prio);
void  xe_sriov_admin_bulk_set_sched_priority_string(int pf_fd, const char *prio);
int __xe_sriov_admin_bulk_set_sched_priority(int pf_fd,
					     enum xe_sriov_sched_priority prio);
void xe_sriov_admin_bulk_set_sched_priority(int pf_fd,
					    enum xe_sriov_sched_priority prio);

int  __xe_sriov_admin_vf_stop(int pf_fd, unsigned int vf_num);
void  xe_sriov_admin_vf_stop(int pf_fd, unsigned int vf_num);

int  __xe_sriov_admin_restore_defaults(int pf_fd, unsigned int vf_num);
void  xe_sriov_admin_restore_defaults(int pf_fd, unsigned int vf_num);
int  __xe_sriov_admin_bulk_restore_defaults(int pf_fd);
void  xe_sriov_admin_bulk_restore_defaults(int pf_fd);

#endif /* __XE_SRIOV_ADMIN_H__ */
