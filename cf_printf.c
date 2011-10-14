/*
 *  cf_printf.c
 *  staticrouted
 *
 *  Created by Alastair Houghton on 13/02/2010.
 *  Copyright 2010 Coriolis Systems Limited. All rights reserved.
 *
 */

#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <langinfo.h>
#include <string.h>
#include <stdbool.h>

#include "cf_printf.h"

static bool knowsStringEncoding = false;
static CFStringEncoding encoding = kCFStringEncodingASCII;

static void
detect_encoding (void)
{
  const char *encoding_name;
  
  setlocale (LC_ALL, "");
  
  encoding_name = nl_langinfo (CODESET);
  
  if (!encoding_name)
    encoding = CFStringGetSystemEncoding ();
  else {
    CFStringRef encodingName = CFStringCreateWithCString (kCFAllocatorDefault, encoding_name, kCFStringEncodingASCII);
    encoding = CFStringConvertIANACharSetNameToEncoding (encodingName);
    CFRelease (encodingName);
  }
  
  knowsStringEncoding = true;
}

CFIndex
cf_vfprintf (FILE *fp, CFStringRef format, va_list val)
{
  CFStringRef formatted
  = CFStringCreateWithFormatAndArguments (kCFAllocatorDefault,
                                          NULL,
                                          format,
                                          val);
  const char *ptr;
  CFIndex ret = 0;
  
  if (!knowsStringEncoding)
    detect_encoding ();
  
  ptr = CFStringGetCStringPtr (formatted, encoding);
  if (ptr) {
    ret = fputs (ptr, fp);
  } else {
    UInt8 buffer[256];
    CFIndex formattedLen = CFStringGetLength (formatted);
    CFRange range = CFRangeMake (0, formattedLen);
    
    while (!feof (fp) && !ferror (fp)
           && range.location < formattedLen) {
      CFIndex usedBuf = 0;
      CFIndex converted = CFStringGetBytes (formatted,
                                            range,
                                            encoding,
                                            '?',
                                            false,
                                            buffer,
                                            sizeof (buffer),
                                            &usedBuf);
      
      if (!converted)
        break;
      
      if (fwrite (buffer, 1, usedBuf, fp) != usedBuf)
        break;
      
      range.length -= converted;
      range.location += converted;
      
      if (usedBuf && ret + usedBuf <= ret)
        ret = INT_MAX;
      else
        ret += usedBuf;
    }
  }
  
  CFRelease (formatted);
  return ret;
}

CFIndex
cf_fprintf (FILE *fp, CFStringRef format, ...)
{
  CFIndex ret;
  va_list val;
  
  va_start (val, format);
  ret = cf_vfprintf (fp, format, val);
  va_end (val);
  
  return ret;
}

CFIndex
cf_vprintf (CFStringRef format, va_list val)
{
  return cf_vfprintf (stdout, format, val);
}

CFIndex
cf_printf (CFStringRef format, ...)
{
  int ret;
  va_list val;
  
  va_start (val, format);
  ret = cf_vfprintf (stdout, format, val);
  va_end (val);
  
  return ret;
}
