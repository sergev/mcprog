#include <string.h>
#include <time.h>
#include "swinfo.h"

char *label = INFO_LABEL;

sw_info * find_info(char *area, int area_size)
{
	char *found_label;
	found_label = area;
	while (1) {
		found_label = memchr (found_label, label[0], area_size - (found_label - area));
		if (!found_label)
			return 0;
		if (strcmp (found_label, label) == 0)
			break;
		++found_label;
	}
	return (sw_info *)found_label;
}
