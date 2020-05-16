/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2008-2013 by Andreas Schneider <asn@cryptomilk.org>
 * Copyright (c) 2012-2013 by Klaas Freitag <freitag@owncloud.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config_csync.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef __HAIKU__
#define _BSD_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

#include "common/c_jhash.h"
#include "csync_util.h"
#include "vio/csync_vio.h"

Q_LOGGING_CATEGORY(lcCSyncUtils, "sync.csync.utils", QtInfoMsg)


typedef struct {
  const char *instr_str;
  enum csync_instructions_e instr_code;
} _instr_code_struct;

static const _instr_code_struct _instr[] =
{
  { "INSTRUCTION_NONE", CSYNC_INSTRUCTION_NONE },
  { "INSTRUCTION_EVAL", CSYNC_INSTRUCTION_EVAL },
  { "INSTRUCTION_REMOVE", CSYNC_INSTRUCTION_REMOVE },
  { "INSTRUCTION_RENAME", CSYNC_INSTRUCTION_RENAME },
  { "INSTRUCTION_EVAL_RENAME", CSYNC_INSTRUCTION_EVAL_RENAME },
  { "INSTRUCTION_NEW", CSYNC_INSTRUCTION_NEW },
  { "INSTRUCTION_CONFLICT", CSYNC_INSTRUCTION_CONFLICT },
  { "INSTRUCTION_IGNORE", CSYNC_INSTRUCTION_IGNORE },
  { "INSTRUCTION_SYNC", CSYNC_INSTRUCTION_SYNC },
  { "INSTRUCTION_STAT_ERR", CSYNC_INSTRUCTION_STAT_ERROR },
  { "INSTRUCTION_ERROR", CSYNC_INSTRUCTION_ERROR },
  { "INSTRUCTION_TYPE_CHANGE", CSYNC_INSTRUCTION_TYPE_CHANGE },
  { "INSTRUCTION_UPDATE_METADATA", CSYNC_INSTRUCTION_UPDATE_METADATA },
  { NULL, CSYNC_INSTRUCTION_ERROR }
};

struct csync_memstat_s {
  int size;
  int resident;
  int shared;
  int trs;
  int drs;
  int lrs;
  int dt;
};

const char *csync_instruction_str(enum csync_instructions_e instr)
{
  int idx = 0;

  while (_instr[idx].instr_str != NULL) {
    if (_instr[idx].instr_code == instr) {
      return _instr[idx].instr_str;
    }
    idx++;
  }

  return "ERROR!";
}


void csync_memstat_check(void) {
  int s = 0;
  struct csync_memstat_s m;
  FILE* fp;

  /* get process memory stats */
  fp = fopen("/proc/self/statm","r");
  if (fp == NULL) {
    return;
  }
  s = fscanf(fp, "%d%d%d%d%d%d%d", &m.size, &m.resident, &m.shared, &m.trs,
      &m.drs, &m.lrs, &m.dt);
  fclose(fp);
  if (s == EOF) {
    return;
  }

  qCInfo(lcCSyncUtils, "Memory: %dK total size, %dK resident, %dK shared",
      m.size * 4, m.resident * 4, m.shared * 4);
}

#ifndef HAVE_TIMEGM
#ifdef _WIN32
static int is_leap(unsigned y) {
    y += 1900;
    return (y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0);
}

static time_t timegm(struct tm *tm) {
    static const unsigned ndays[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31} };

    time_t res = 0;
    int i;

    for (i = 70; i < tm->tm_year; ++i)
        res += is_leap(i) ? 366 : 365;

    for (i = 0; i < tm->tm_mon; ++i)
        res += ndays[is_leap(tm->tm_year)][i];
     res += tm->tm_mday - 1;
     res *= 24;
     res += tm->tm_hour;
     res *= 60;
     res += tm->tm_min;
     res *= 60;
     res += tm->tm_sec;
     return res;
}
#else
/* A hopefully portable version of timegm */
static time_t timegm(struct tm *tm ) {
     time_t ret;
     char *tz;

     tz = getenv("TZ");
     setenv("TZ", "", 1);
     tzset();
     ret = mktime(tm);
     if (tz)
         setenv("TZ", tz, 1);
     else
         unsetenv("TZ");
     tzset();
     return ret;
}
#endif /* Platform switch */
#endif /* HAVE_TIMEGM */

#define RFC1123_FORMAT "%3s, %02d %3s %4d %02d:%02d:%02d GMT"
static const char short_months[12][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
/*
 * This function is borrowed from libneon's ne_httpdate_parse.
 * Unfortunately that one converts to local time but here UTC is
 * needed.
 * This one uses timegm instead, which returns UTC.
 */
time_t oc_httpdate_parse( const char *date ) {
    struct tm gmt;
    char wkday[4], mon[4];
    int n;
    time_t result = 0;

    memset(&gmt, 0, sizeof(struct tm));

    /*  it goes: Sun, 06 Nov 1994 08:49:37 GMT */
    n = sscanf(date, RFC1123_FORMAT,
               wkday, &gmt.tm_mday, mon, &gmt.tm_year, &gmt.tm_hour,
               &gmt.tm_min, &gmt.tm_sec);
    /* Is it portable to check n==7 here? */
    gmt.tm_year -= 1900;
    for (n=0; n<12; n++)
        if (strcmp(mon, short_months[n]) == 0)
            break;
    /* tm_mon comes out as 12 if the month is corrupt, which is desired,
     * since the mktime will then fail */
    gmt.tm_mon = n;
    gmt.tm_isdst = -1;
    result = timegm(&gmt);
    return result;
}


bool csync_is_collision_safe_hash(const QByteArray &checksum_header)
{
    return checksum_header.startsWith("SHA1:")
        || checksum_header.startsWith("MD5:");
}
