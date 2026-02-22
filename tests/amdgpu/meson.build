
amdgpu_progs = []
amdgpu_deps = test_deps
if libdrm_amdgpu.found()
	amdgpu_progs += [ 'amd_abm',
			  'amd_assr',
			  'amd_basic',
			  'amd_basic_ex',
			  'amd_basic_multi_gpu',
			  'amd_bo',
			  'amd_bypass',
			  'amd_color',
                          'amd_cross_gpu_sync',
			  'amd_cp_dma_misc',
			  'amd_cs_nop',
			  'amd_cursor_overlay',
			  'amd_deadlock',
			  'amd_dp_dsc',
			  'amd_freesync_video_mode',
			  'amd_fuzzing',
			  'amd_hotplug',
			  'amd_gang_cs' ,
			  'amd_ilr',
			  'amd_info',
			  'amd_jpeg_dec',
			  'amd_jpeg_queue',
			  'amd_link_settings',
			  'amd_max_bpc',
			  'amd_mem_leak',
			  'amd_mode_switch',
			  'amd_module_load',
			  'amd_multidisplay_modeset',
			  'amd_pci_unplug',
			  'amd_plane',
			  'amd_prime',
			  'amd_psr',
			  'amd_ras',
			  'amd_replay',
			  'amd_security',
			  'amd_uvd_dec',
			  'amd_uvd_enc',
			  'amd_vce_enc',
			  'amd_vcn',
			  'amd_vcn_queue',
			  'amd_vm',
			  'amd_vrr_range',
			  'amd_mall',
			  'amd_odm',
			  'amd_subvp',
			  'amd_vpe',
			  'amd_mem',
			  'amd_remote_mem',
			]
	if libdrm_amdgpu.version().version_compare('> 2.4.97')
		amdgpu_progs +=[ 'amd_syncobj', ]
	else
		warning('libdrm <= 2.4.97 found, amd_syncobj test not applicable')
	endif
	if libdrm_amdgpu.version().version_compare('> 2.4.99')
		amdgpu_progs +=[ 'amd_dispatch',]
	else
		warning('libdrm <= 2.4.99 found, amdgpu_cs_query_reset_state2 not applicable')
	endif
	if libdrm_amdgpu.version().version_compare('> 2.4.109')
		amdgpu_progs +=[ 'amd_pstate', ]
	else
		warning('libdrm <= 2.4.109 found, amd_pstate test not applicable')
	endif
	if libdrm_amdgpu.version().version_compare('> 2.4.104')
		amdgpu_progs +=[ 'amd_queue_reset',]
	else
		warning('libdrm <= 2.4.104 found, amd_queue_reset test not applicable')
	endif
	amdgpu_deps += libdrm_amdgpu
endif

foreach prog : amdgpu_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : amdgpu_deps,
				       install_dir : amdgpudir,
				       install_rpath : amdgpudir_rpathdir,
				       install : true)
	test_list += join_paths('amdgpu', prog)
endforeach
