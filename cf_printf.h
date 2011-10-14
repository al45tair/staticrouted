/*
 *  cf_printf.h
 *  staticrouted
 *
 *  Created by Alastair Houghton on 13/02/2010.
 *  Copyright 2010 Coriolis Systems Limited. All rights reserved.
 *
 */

#ifndef CF_PRINTF_H_
#define CF_PRINTF_H_

#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdarg.h>

CFIndex cf_vfprintf (FILE *fp, CFStringRef format, va_list val);
CFIndex cf_vprintf (CFStringRef format, va_list val);
CFIndex cf_fprintf (FILE *fp, CFStringRef format, ...);
CFIndex cf_printf (CFStringRef format, ...);

#endif /* CF_PRINTF_H_ */
