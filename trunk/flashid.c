/*
 * Утилита проверки функционирования JTAG-интерфейса
 * для микроконтроллеров Elvees Multicore.
 * 1) Читает и проверяет идентификатор процессора.
 * 2) Читает код изготовителя FLASH-памяти.
 * 3) Читает идентификатор типа микросхемы FLASH-памяти.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "multicore.h"

multicore_t *multicore;
int debug;

int main (int argc, char **argv)
{
	unsigned mfcode, devcode;
	int n;

	multicore_init ();
	while ((n = getopt(argc, argv, "D")) != -1) {
		switch (n) {
		case 'D':
			++debug;
			break;
		default:
			fprintf (stderr, "Incorrect option: `%c'\n", n);
			goto usage;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 0) {
		fprintf (stderr, "Incorrect number of arguments: %d\n", argc);
usage:		printf ("\nUsage:\n");
		printf ("\tflashid [-D]\n");
		printf ("Options:\n");
		printf ("\t-D\t- print debugging information\n");
		exit (0);
	}
	multicore = multicore_open ();
	if (! multicore) {
		fprintf (stderr, "Error detecting device -- check cable!\n");
		exit (1);
	}
	printf ("CPU code = 0x%08x (%s)\n", multicore_idcode (multicore),
		multicore_cpu_name (multicore));
	multicore_flash_id (multicore, &mfcode, &devcode);
	printf ("Flash manufacturer code = 0x%08x\n", mfcode);
	printf ("Flash device code = 0x%08x (%s)\n", devcode,
		multicore_flash_name (multicore));
	multicore_close (multicore);
	return 0;
}
