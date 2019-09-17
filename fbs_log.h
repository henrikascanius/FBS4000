#ifndef FBS_LOG_H

#define FBS_LOG_H

#include <stdint.h>
#include <syslog.h>


#define G_SEEK    1
#define G_DATA    2
#define G_MISC  256

static uint32_t logmask = G_MISC;

#define FBS_LOG(group, args...) \
do { \
    if (logmask & group) syslog(LOG_INFO, ##args); \
} while (0);

void fbs_openlog()
{
    openlog("FBS4000", LOG_NDELAY, LOG_USER);
}

#endif