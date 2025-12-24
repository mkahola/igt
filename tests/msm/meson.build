msm_progs = [
	'msm_bo',
	'msm_mapping',
	'msm_recovery',
	'msm_shrink',
	'msm_submit',
	'msm_submitoverhead',
]

foreach prog : msm_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : test_deps,
				       install_dir : msmdir,
				       install_rpath : msm_rpathdir,
				       install : true)
	test_list += join_paths('msm', prog)
endforeach
