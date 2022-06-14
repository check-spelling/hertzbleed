#include <math.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "../util/freq-utils.h"
#include "../util/rapl-utils.h"
#include "../util/util.h"

volatile static int attacker_core_ID;

#define TIME_BETWEEN_MEASUREMENTS 1000000L // 1 millisecond

#define STACK_SIZE 16384

struct args_t {
	uint64_t iters;
	int selector;
};

uint64_t binary_uint64(int hamming)
{
	// Length always 64
	uint64_t final_uint = 0;
	for (int i = 0; i < hamming; i++) {
		final_uint = final_uint + (uint64_t)(pow(2, i) * 1);
	}
	return final_uint;
}

static __attribute__((noinline)) int victim(void *varg)
{
	struct args_t *arg = varg;
	int selector = arg->selector;
	uint64_t operand_small[8];

	for (int i = 0; i < 8; i++) {
		operand_small[i] = binary_uint64(selector);
		// operand_small[i] = binary_uint64(i);
		// operand_small[i] = binary_uint64(8 - i);
	}

	uint64_t full_operand = (operand_small[7] << 56) |
							(operand_small[6] << 48) |
							(operand_small[5] << 40) |
							(operand_small[4] << 32) |
							(operand_small[3] << 24) |
							(operand_small[2] << 16) |
							(operand_small[1] << 8) |
							(operand_small[0] << 0);

	asm volatile(
		"mov %0,%%rbx\n\t" // set register to operand
		"mov %0,%%rcx\n\t" // set register to operand
		"mov %0,%%rdx\n\t" // set register to operand
		"mov %0,%%rsi\n\t" // set register to operand
		"mov %0,%%rdi\n\t" // set register to operand
		"mov %0,%%r8\n\t"  // set register to operand
		"mov %0,%%r9\n\t"  // set register to operand
		"mov %0,%%r10\n\t" // set register to operand
		"mov %0,%%r11\n\t" // set register to operand
		"mov %0,%%r12\n\t" // set register to operand
		"mov %0,%%r13\n\t" // set register to operand
		"mov %0,%%r14\n\t" // set register to operand
		"mov %0,%%r15\n\t" // set register to operand

		".align 64\n\t"
		"loop:\n\t"

		"or %0,%%rbx\n\t"
		"or %0,%%rcx\n\t"
		"or %0,%%rdx\n\t"
		"or %0,%%rsi\n\t"
		"or %0,%%rdi\n\t"
		"or %0,%%r8\n\t"
		"or %0,%%r9\n\t"
		"or %0,%%r10\n\t"
		"or %0,%%r11\n\t"
		"or %0,%%r12\n\t"
		"or %0,%%r13\n\t"
		"or %0,%%r14\n\t"
		"or %0,%%r15\n\t"

		"or %0,%%rbx\n\t"
		"or %0,%%rcx\n\t"
		"or %0,%%rdx\n\t"
		"or %0,%%rsi\n\t"
		"or %0,%%rdi\n\t"
		"or %0,%%r8\n\t"
		"or %0,%%r9\n\t"
		"or %0,%%r10\n\t"
		"or %0,%%r11\n\t"
		"or %0,%%r12\n\t"
		"or %0,%%r13\n\t"
		"or %0,%%r14\n\t"
		"or %0,%%r15\n\t"

		"jmp loop\n\t"
		:
		: "r"(full_operand)
		: "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15");

	return 0;
}

// Collects traces
static __attribute__((noinline)) int monitor(void *in)
{
	static int rept_index = 0;

	struct args_t *arg = (struct args_t *)in;

	// Pin monitor to a single CPU
	pin_cpu(attacker_core_ID);

	// Set filename
	// The format is, e.g., ./out/all_02_2330.out
	// where 02 is the selector and 2330 is an index to prevent overwriting files
	char output_filename[64];
	sprintf(output_filename, "./out/all_%02d_%06d.out", arg->selector, rept_index);
	rept_index += 1;

	// Prepare output file
	FILE *output_file = fopen((char *)output_filename, "w");
	if (output_file == NULL) {
		perror("output file");
	}

	// Prepare
	double energy, prev_energy = rapl_msr(attacker_core_ID, PP0_ENERGY);
	struct freq_sample_t freq_sample, prev_freq_sample = frequency_msr_raw(attacker_core_ID);

	// Collect measurements
	for (uint64_t i = 0; i < arg->iters; i++) {

		// Wait before next measurement
		nanosleep((const struct timespec[]){{0, TIME_BETWEEN_MEASUREMENTS}}, NULL);

		// Collect measurement
		energy = rapl_msr(attacker_core_ID, PP0_ENERGY);
		freq_sample = frequency_msr_raw(attacker_core_ID);

		// Store measurement
		uint64_t aperf_delta = freq_sample.aperf - prev_freq_sample.aperf;
		uint64_t mperf_delta = freq_sample.mperf - prev_freq_sample.mperf;
		uint32_t khz = (maximum_frequency * aperf_delta) / mperf_delta;
		fprintf(output_file, "%.15f %" PRIu32 "\n", energy - prev_energy, khz);

		// Save current
		prev_energy = energy;
		prev_freq_sample = freq_sample;
	}

	// Clean up
	fclose(output_file);
	return 0;
}

int main(int argc, char *argv[])
{
	// Check arguments
	if (argc != 4) {
		fprintf(stderr, "Wrong Input! Enter: %s <ntasks> <samples> <outer>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	// Read in args
	int ntasks;
	struct args_t arg;
	int outer;
	sscanf(argv[1], "%d", &ntasks);
	if (ntasks < 0) {
		fprintf(stderr, "ntasks cannot be negative!\n");
		exit(1);
	}
	sscanf(argv[2], "%" PRIu64, &(arg.iters));
	sscanf(argv[3], "%d", &outer);
	if (outer < 0) {
		fprintf(stderr, "outer cannot be negative!\n");
		exit(1);
	}

	// Open the selector file
	FILE *selectors_file = fopen("input.txt", "r");
	if (selectors_file == NULL)
		perror("fopen error");

	// Read the selectors file line by line
	int num_selectors = 0;
	int selectors[100];
	size_t len = 0;
	ssize_t read = 0;
	char *line = NULL;
	while ((read = getline(&line, &len, selectors_file)) != -1) {
		if (line[read - 1] == '\n')
			line[--read] = '\0';

		// Read selector
		sscanf(line, "%d", &(selectors[num_selectors]));
		num_selectors += 1;
	}

	// Set the scheduling priority to high to avoid interruptions
	// (lower priorities cause more favorable scheduling, and -20 is the max)
	setpriority(PRIO_PROCESS, 0, -20);

	// Prepare up monitor/attacker
	attacker_core_ID = 0;
	set_frequency_units(attacker_core_ID);
	frequency_msr_raw(attacker_core_ID);
	set_rapl_units(attacker_core_ID);
	rapl_msr(attacker_core_ID, PP0_ENERGY);

	// Allocate memory for the threads
	char *tstacks = mmap(NULL, (ntasks + 1) * STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	// Run experiment once for each selector
	for (int i = 0; i < outer * num_selectors; i++) {

		// Set alternating selector
		arg.selector = selectors[i % num_selectors];

		// Start victim threads
		int tids[ntasks];
		for (int tnum = 0; tnum < ntasks; tnum++) {
			tids[tnum] = clone(&victim, tstacks + (ntasks - tnum) * STACK_SIZE, CLONE_VM | SIGCHLD, &arg);
		}

		// Start the monitor thread
		clone(&monitor, tstacks + (ntasks + 1) * STACK_SIZE, CLONE_VM | SIGCHLD, (void *)&arg);

		// Join monitor thread
		wait(NULL);

		// Kill victim threads
		for (int tnum = 0; tnum < ntasks; tnum++) {
			syscall(SYS_tgkill, tids[tnum], tids[tnum], SIGTERM);

			// Need to join o/w the threads remain as zombies
			// https://askubuntu.com/a/427222/1552488
			wait(NULL);
		}
	}

	// Clean up
	munmap(tstacks, (ntasks + 1) * STACK_SIZE);
}
