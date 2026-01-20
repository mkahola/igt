/* Must be same as MAGIC_LOOP_STOP in lib/intel_compute.c */
#define MAGIC_LOOP_STOP 0x12341234

__kernel void loop(volatile __global int *input,
		   __global int *output,
		   unsigned int count)
{
	int i = get_global_id(0);

	while (1) {
		if (input[0] == MAGIC_LOOP_STOP)
			break;
	}
	output[0] = i;
}
