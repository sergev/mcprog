#ifndef __SW_INFO_H__
#define __SW_INFO_H__

#define AREA_SIZE 8192
#define INFO_LABEL "^SINFO^"

#include <time.h>

typedef struct sw_info_
{
    char        label[8];
    char        sw_version[16];
    unsigned    len;
    unsigned    crc;
    char        board_sn[128];
    char        filename[128];
    time_t      time;
} sw_info;

sw_info *find_info(char *area, int area_size);

#endif
