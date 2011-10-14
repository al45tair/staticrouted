/*
 *  staticroute.c
 *  staticroute
 *
 *  Created by Alastair Houghton on 13/02/2010.
 *  Copyright 2010 Coriolis Systems Limited. All rights reserved.
 *
 */

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "cf_printf.h"

CFStringRef kRoutesKey = CFSTR("com.coriolis-systems.StaticRoutes");
SCPreferencesRef systemConfPrefs;
SCDynamicStoreRef dynamicStore;

struct destination {
  int af;
  int prefix_len;
  
  union {
    struct in_addr v4;
    struct in6_addr v6;
  } ip;
};

int list_services (void);
int list_all_routes (void);
int list_routes (const char *service_name);
int add_route (struct destination dest, const char *service_name);
int delete_route (struct destination dest, const char *service_name);

CFPropertyListRef
sc_get_value_at_path (SCPreferencesRef scprefs,
                      CFStringRef path)
{
  CFArrayRef splitPath 
    = CFStringCreateArrayBySeparatingStrings(kCFAllocatorDefault,
                                             path,
                                             CFSTR("/"));

  if (!splitPath)
    return NULL;
  
  CFIndex count = CFArrayGetCount (splitPath);
  CFPropertyListRef obj;
  
  if (count < 2) {
    CFRelease (splitPath);
    return NULL;
  }
  
  obj = SCPreferencesGetValue(scprefs, CFArrayGetValueAtIndex (splitPath, 1));
  
  for (CFIndex n = 2; obj && n < count; ++n)
    obj = CFDictionaryGetValue (obj, CFArrayGetValueAtIndex (splitPath, n));
  
  CFRelease (splitPath);
  
  return obj;
}

const char *usage_text =
"usage: staticroute list-services\n"
"\n"
"       Lists all network services for the current location.\n"
"\n"
"usage: staticroute list [network-service]\n"
"\n"
"       Lists all static routes defined for the specified service in the\n"
"       current location.  If no service is specified, list all static\n"
"       routes currently defined.\n"
"\n"
"usage: staticroute add <address> <network-service>\n"
"\n"
"       Adds a static route to the specified address for the specified\n"
"       service in the current location.  The address may be specified\n"
"       in either of the following forms:\n"
"\n"
"           192.168.0.1         - a route for a single host\n"
"           192.168.5.0/24      - a route to the network 192.168.5\n"
"\n"
"usage: staticroute delete <address> <network-service>\n"
"\n"
"       Removes a static route from the specified service in the current\n"
"       location.\n"
"\n";

static void
usage (void)
{
  fputs (usage_text, stderr);
}

static bool
parse_dest (const char *str, struct destination *pdest)
{
  const char *ptr = strchr (str, '/');
  bool max_prefix = false;
  
  if (!ptr || sscanf (ptr + 1, "%d", &pdest->prefix_len) != 1)
    max_prefix = true;
  
  if (ptr) {
    size_t len = ptr - str;
    char *pcopy = (char *)malloc (len + 1);
    memcpy (pcopy, str, len);
    pcopy[len] = '\0';
    str = pcopy;
  }
  
  if (inet_pton (AF_INET, str, &pdest->ip.v4)) {
    uint32_t mask;
    pdest->af = AF_INET;
    
    if (max_prefix)
      pdest->prefix_len = 32;
    
    if (pdest->prefix_len <= 0) {
      mask = 0;
      pdest->prefix_len = 0;
    } else {
      if (pdest->prefix_len > 32)
        pdest->prefix_len = 32;
      
      mask = 0xffffffffu << (32 - pdest->prefix_len);
    }
    
    pdest->ip.v4.s_addr &= OSSwapHostToBigInt32 (mask);
    
    if (ptr)
      free ((void *)str);
    return true;
  } else if (inet_pton (AF_INET6, str, &pdest->ip.v6)) {
    pdest->af = AF_INET6;
    
    if (max_prefix)
      pdest->prefix_len = 128;
    
    if (pdest->prefix_len < 0)
      pdest->prefix_len = 0;
    else if (pdest->prefix_len > 128)
      pdest->prefix_len = 128;
    
    uint32_t *words = (uint32_t *)&pdest->ip.v6.s6_addr;
    
    if (pdest->prefix_len == 0) {
      words[0] = 0;
      words[1] = 0;
      words[2] = 0;
      words[3] = 0;
    } else if (pdest->prefix_len <= 32) {
      uint32_t mask = 0xffffffffu << (32 - pdest->prefix_len);
      words[0] &= OSSwapHostToBigInt32 (mask);
      words[1] = 0;
      words[2] = 0;
      words[3] = 0;
    } else if (pdest->prefix_len <= 64) {
      uint32_t mask = 0xffffffffu << (64 - pdest->prefix_len);
      words[1] &= OSSwapHostToBigInt32 (mask);
      words[2] = 0;
      words[3] = 0;
    } else if (pdest->prefix_len <= 96) {
      uint32_t mask = 0xffffffffu << (96 - pdest->prefix_len);
      words[2] &= OSSwapHostToBigInt32 (mask);
      words[3] = 0;
    } else {
      uint32_t mask = 0xffffffffu << (128 - pdest->prefix_len);
      words[3] &= OSSwapHostToBigInt32 (mask);
    }
    
    if (ptr)
      free ((void *)str);
    return true;
  } else {
    if (ptr)
      free ((void *)str);
    return false;
  }
}

int
main (int argc, char **argv)
{
  CFErrorRef err;
  int ret = 0;
  
  if (argc < 2) {
    usage ();
    return 0;
  }
  
  systemConfPrefs = SCPreferencesCreate (kCFAllocatorDefault,
                                         CFSTR("staticroute"),
                                         NULL);
  
  if (!systemConfPrefs) {
    CFStringRef desc;

    err = SCCopyLastError();
    desc = CFErrorCopyDescription (err);
    cf_fprintf (stderr, 
                CFSTR("staticroute: unable to attach to system configuration"
                      " - %ld: %@\n"),
                CFErrorGetCode (err), desc);
    CFRelease (err);
    CFRelease (desc);
    return 1;
  }

  dynamicStore = SCDynamicStoreCreate (kCFAllocatorDefault,
                                       CFSTR("staticroute"),
                                       NULL, NULL);
  
  if (!dynamicStore) {
    CFStringRef desc;
    
    err = SCCopyLastError();
    desc = CFErrorCopyDescription (err);
    cf_fprintf (stderr, 
                CFSTR("staticroute: unable to attach to system configuration"
                      " - %ld: %@\n"),
                CFErrorGetCode (err), desc);
    CFRelease (err);
    CFRelease (desc);
    CFRelease (systemConfPrefs);
    return 1;
  }    
  
  if (argc == 2 && strcasecmp (argv[1], "list-services") == 0)
    ret = list_services ();
  else if (argc == 2 && strcasecmp (argv[1], "list") == 0)
    ret = list_all_routes ();
  else if (argc == 3 && strcasecmp (argv[1], "list") == 0)
    ret = list_routes (argv[2]);
  else if (argc == 4 && strcasecmp (argv[1], "add") == 0) {
    struct destination dest;
    
    if (!parse_dest (argv[2], &dest)) {
      cf_fprintf (stderr, CFSTR("staticroute: bad address format \"%s\".\n"),
                  argv[2]);
      ret = 1;
    } else {
      ret = add_route (dest, argv[3]);
    }
  } else if (argc == 4 && strcasecmp (argv[1], "delete") == 0) {
    struct destination dest;
    
    if (!parse_dest (argv[2], &dest)) {
      cf_fprintf (stderr, CFSTR("staticroute: bad address format \"%s\".\n"),
                  argv[2]);
      ret = 1;
    } else {
      ret = delete_route (dest, argv[3]);
    }
  } else
    usage ();

  CFRelease (dynamicStore);
  CFRelease (systemConfPrefs);

  return ret;
}

CFDictionaryRef
service_by_name (CFStringRef serviceName, CFStringRef *pServiceID)
{
  CFStringRef currentSetPath = SCPreferencesGetValue (systemConfPrefs,
                                                      CFSTR("CurrentSet"));    
  CFDictionaryRef currentSet = sc_get_value_at_path (systemConfPrefs, currentSetPath);

  CFDictionaryRef network = CFDictionaryGetValue (currentSet,
                                                  CFSTR("Network"));
  CFDictionaryRef global = CFDictionaryGetValue (network,
                                                 CFSTR("Global"));
  CFDictionaryRef services = CFDictionaryGetValue(network,
                                                  CFSTR("Service"));
  CFDictionaryRef ipv4 = CFDictionaryGetValue (global,
                                               CFSTR("IPv4"));
  CFArrayRef serviceOrder = CFDictionaryGetValue (ipv4,
                                                  CFSTR("ServiceOrder"));
  CFIndex serviceCount = CFArrayGetCount (serviceOrder);
  
  for (CFIndex n = 0; n < serviceCount; ++n) {
    CFStringRef serviceID = CFArrayGetValueAtIndex (serviceOrder, n);
    CFDictionaryRef serviceInfo = CFDictionaryGetValue (services, serviceID);
    CFStringRef servicePath = CFDictionaryGetValue (serviceInfo,
                                                    CFSTR("__LINK__"));
    CFDictionaryRef service = sc_get_value_at_path (systemConfPrefs,
                                                    servicePath);
    CFStringRef name = CFDictionaryGetValue (service,
                                             CFSTR("UserDefinedName"));
    
    if (CFStringCompare (serviceName, name, kCFCompareCaseInsensitive)
        == kCFCompareEqualTo) {
      if (pServiceID)
        *pServiceID = serviceID;
      return service;
    }
  }
  
  return NULL;
}

int
list_services (void)
{
  SCPreferencesLock (systemConfPrefs, true);
  {
    CFStringRef currentSetPath = SCPreferencesGetValue (systemConfPrefs,
                                                        CFSTR("CurrentSet"));    
    CFDictionaryRef currentSet = sc_get_value_at_path (systemConfPrefs, currentSetPath);
    CFDictionaryRef network = CFDictionaryGetValue (currentSet,
                                                    CFSTR("Network"));
    CFDictionaryRef global = CFDictionaryGetValue (network,
                                                   CFSTR("Global"));
    CFDictionaryRef services = CFDictionaryGetValue(network,
                                                    CFSTR("Service"));
    CFDictionaryRef ipv4 = CFDictionaryGetValue (global,
                                                 CFSTR("IPv4"));
    CFArrayRef serviceOrder = CFDictionaryGetValue (ipv4,
                                                    CFSTR("ServiceOrder"));
    CFIndex serviceCount = CFArrayGetCount (serviceOrder);
    
    for (CFIndex n = 0; n < serviceCount; ++n) {
      CFStringRef serviceID = CFArrayGetValueAtIndex (serviceOrder, n);
      CFDictionaryRef serviceInfo = CFDictionaryGetValue (services, serviceID);
      CFStringRef servicePath = CFDictionaryGetValue (serviceInfo,
                                                      CFSTR("__LINK__"));
      CFDictionaryRef service = sc_get_value_at_path (systemConfPrefs,
                                                      servicePath);
      CFStringRef name = CFDictionaryGetValue (service,
                                               CFSTR("UserDefinedName"));
      
      cf_printf (CFSTR("%@\n"), name);
    }
  }
  SCPreferencesUnlock (systemConfPrefs);
  
  return 0;
}

int
list_all_routes (void)
{
  bool gotOne = false;
  
  SCPreferencesLock (systemConfPrefs, true);
  {
    CFDictionaryRef staticRoutes = SCPreferencesGetValue (systemConfPrefs,
                                                          kRoutesKey);

    if (!staticRoutes) {
      cf_printf (CFSTR("No static routes defined.\n"));
      SCPreferencesUnlock (systemConfPrefs);
      return 0;
    }
    
    CFStringRef currentSetPath = SCPreferencesGetValue (systemConfPrefs,
                                                        CFSTR("CurrentSet"));    
    CFDictionaryRef currentSet = sc_get_value_at_path (systemConfPrefs, currentSetPath);
    CFDictionaryRef network = CFDictionaryGetValue (currentSet,
                                                    CFSTR("Network"));
    CFDictionaryRef global = CFDictionaryGetValue (network,
                                                   CFSTR("Global"));
    CFDictionaryRef services = CFDictionaryGetValue(network,
                                                    CFSTR("Service"));
    CFDictionaryRef ipv4 = CFDictionaryGetValue (global,
                                                 CFSTR("IPv4"));
    CFArrayRef serviceOrder = CFDictionaryGetValue (ipv4,
                                                    CFSTR("ServiceOrder"));
    CFIndex serviceCount = CFArrayGetCount (serviceOrder);
    
    for (CFIndex n = 0; n < serviceCount; ++n) {
      CFStringRef serviceID = CFArrayGetValueAtIndex (serviceOrder, n);
      CFDictionaryRef serviceInfo = CFDictionaryGetValue (services, serviceID);
      CFStringRef servicePath = CFDictionaryGetValue (serviceInfo,
                                                      CFSTR("__LINK__"));
      CFDictionaryRef service = sc_get_value_at_path (systemConfPrefs,
                                                      servicePath);
      CFStringRef name = CFDictionaryGetValue (service,
                                               CFSTR("UserDefinedName"));
      CFArrayRef routes = CFDictionaryGetValue (staticRoutes, serviceID);
      
      if (routes) {
        CFIndex routeCount = CFArrayGetCount (routes);
        
        for (CFIndex m = 0; m < routeCount; ++m) {
          CFDictionaryRef route = CFArrayGetValueAtIndex (routes, m);
          CFStringRef address = CFDictionaryGetValue (route, CFSTR("address"));
          CFNumberRef prefixLen = CFDictionaryGetValue (route,
                                                        CFSTR("prefixLength"));
          CFStringRef formattedAddr = CFStringCreateWithFormat (kCFAllocatorDefault,
                                                                NULL,
                                                                CFSTR("%@/%@"),
                                                                address,
                                                                prefixLen);
          cf_printf (CFSTR("%@ %@\n"), formattedAddr, name);
          gotOne = true;
          
          CFRelease (formattedAddr);
        }
      }
    }
  }
  SCPreferencesUnlock (systemConfPrefs);
  
  if (!gotOne)
    cf_printf (CFSTR("No static routes defined.\n"));
  
  return 0;
}

int
list_routes (const char *service_name)
{
  CFStringRef serviceName = CFStringCreateWithCString(kCFAllocatorDefault,
                                                      service_name,
                                                      kCFStringEncodingUTF8);
  CFStringRef serviceID = NULL;
  CFDictionaryRef service = service_by_name (serviceName, &serviceID);
  
  if (!service) {
    cf_fprintf (stderr, CFSTR("staticroute: cannot find service %@\n"),
                serviceName);
    CFRelease (serviceName);
    return 1;
  }
  
  SCPreferencesLock (systemConfPrefs, true);
  {
    CFDictionaryRef staticRoutes = SCPreferencesGetValue (systemConfPrefs,
                                                          kRoutesKey);
    CFArrayRef routes = NULL;
    
    if (staticRoutes)
      routes = CFDictionaryGetValue (staticRoutes, serviceID);
    
    if (!routes || !CFArrayGetCount (routes))
      cf_printf (CFSTR("No static routes defined for service %@.\n"),
                 serviceName);
    else {
      CFIndex routeCount = CFArrayGetCount (routes);
      for (CFIndex n = 0; n < routeCount; ++n) {
        CFDictionaryRef route = CFArrayGetValueAtIndex (routes, n);
        CFStringRef address = CFDictionaryGetValue (route, CFSTR("address"));
        CFNumberRef prefixLen = CFDictionaryGetValue (route,
                                                      CFSTR("prefixLength"));
        
        cf_printf (CFSTR("%@/%@\n"), address, prefixLen);
      }
    }
  }
  SCPreferencesUnlock (systemConfPrefs);
  
  CFRelease (serviceName);
  
  return 0;
}

int
add_route (struct destination dest, const char *service_name)
{
  CFStringRef serviceName = CFStringCreateWithCString(kCFAllocatorDefault,
                                                      service_name,
                                                      kCFStringEncodingUTF8);
  CFStringRef serviceID = NULL;
  CFDictionaryRef service = service_by_name (serviceName, &serviceID);
  int ret = 0;
  
  if (!service) {
    cf_fprintf (stderr, CFSTR("staticroute: cannot find service %@\n"),
                serviceName);
    CFRelease (serviceName);
    return 1;
  }
  
  // Work out the addressFamily
  CFStringRef addressFamily;
  
  switch (dest.af) {
    case AF_INET:
      addressFamily = CFSTR("IPv4");
      break;
    case AF_INET6:
      addressFamily = CFSTR("IPv6");
      break;
    default:
      addressFamily = CFSTR("Unknown");
      break;
  }
  
  SCPreferencesLock (systemConfPrefs, true);
  {
    CFDictionaryRef oldStaticRoutes = SCPreferencesGetValue (systemConfPrefs,
                                                             kRoutesKey);
    CFMutableDictionaryRef staticRoutes;

    if (!oldStaticRoutes) {
      staticRoutes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    } else {
      staticRoutes = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0,
                                                   oldStaticRoutes);
    }
    
    // Find the routes for this service
    CFArrayRef oldRoutes = CFDictionaryGetValue(staticRoutes, serviceID);
    CFMutableArrayRef routes;
    
    if (oldRoutes)
      routes = CFArrayCreateMutableCopy (kCFAllocatorDefault, 0, oldRoutes);
    else {
      routes = CFArrayCreateMutable (kCFAllocatorDefault, 0,
                                     &kCFTypeArrayCallBacks);
    }
    
    // Use the new mutable array
    CFDictionarySetValue (staticRoutes, serviceID, routes);
    CFRelease (routes);
    
    // Build the address string
    char buffer[256];
    inet_ntop (dest.af, &dest.ip, buffer, sizeof (buffer));

    CFStringRef addressString = CFStringCreateWithCString(kCFAllocatorDefault,
                                                          buffer,
                                                          kCFStringEncodingUTF8);
    
    // Construct the route dictionary
    CFStringRef keys[] = { CFSTR("addressFamily"),
                           CFSTR("address"),
                           CFSTR("prefixLength") };
    CFNumberRef prefixLen = CFNumberCreate(kCFAllocatorDefault,
                                           kCFNumberIntType, &dest.prefix_len);
    CFPropertyListRef values[3] = { addressFamily, addressString, prefixLen };
    CFDictionaryRef routeDict = CFDictionaryCreate(kCFAllocatorDefault,
                                                   (const void **)keys,
                                                   (const void **)values, 3,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
    
    // Add the dictionary to the routes list
    CFArrayAppendValue (routes, routeDict);
    
    // Release references
    CFRelease (routeDict);
    CFRelease (prefixLen);
    CFRelease (addressString);
    
    // Set the value in the store
    if (!SCPreferencesSetValue(systemConfPrefs, kRoutesKey, staticRoutes)) {
      cf_fprintf (stderr, 
                  CFSTR("staticroute: cannot add route to system configuration "
                        "database.\n"));
      ret = 1;
    }
    
    // Commit the changes
    if (!ret && !SCPreferencesCommitChanges (systemConfPrefs)) {
      cf_fprintf (stderr,
                  CFSTR("staticroute: cannot commit changes to system "
                        "configuration database.\n"));
      ret = 1;
    }
    
    // Apply the changes
    if (!ret && !SCPreferencesApplyChanges (systemConfPrefs)) {
      cf_fprintf (stderr,
                  CFSTR("staticroute: cannot apply changes to system "
                        "configuration database.\n"));
      ret = 1;
    }
    
    CFRelease (staticRoutes);
  }
  SCPreferencesUnlock (systemConfPrefs);
  
  // Notify the dynamic store key for this service ID
  CFStringRef storeKey = CFStringCreateWithFormat (kCFAllocatorDefault,
                                                   NULL,
                                                   CFSTR("Setup:/Network/Service/%@/%@"),
                                                   serviceID,
                                                   addressFamily);
  SCDynamicStoreNotifyValue (dynamicStore, storeKey);
  CFRelease (storeKey);
  CFRelease (serviceName);

  return ret;
}

int
delete_route (struct destination dest, const char *service_name)
{
  CFStringRef serviceName = CFStringCreateWithCString(kCFAllocatorDefault,
                                                      service_name,
                                                      kCFStringEncodingUTF8);
  CFStringRef serviceID = NULL;
  CFDictionaryRef service = service_by_name (serviceName, &serviceID);
  int ret = 0;
  
  if (!service) {
    cf_fprintf (stderr, CFSTR("staticroute: cannot find service %@\n"),
                serviceName);
    CFRelease (serviceName);
    return 1;
  }
  
  // Work out the addressFamily
  CFStringRef addressFamily;
  
  switch (dest.af) {
    case AF_INET:
      addressFamily = CFSTR("IPv4");
      break;
    case AF_INET6:
      addressFamily = CFSTR("IPv6");
      break;
    default:
      addressFamily = CFSTR("Unknown");
      break;
  }
  
  SCPreferencesLock (systemConfPrefs, true);
  {
    CFDictionaryRef oldStaticRoutes = SCPreferencesGetValue (systemConfPrefs,
                                                             kRoutesKey);
    CFMutableDictionaryRef staticRoutes;
    
    if (!oldStaticRoutes) {
      SCPreferencesUnlock (systemConfPrefs);
      cf_fprintf (stderr, CFSTR("staticroute: no routes for service %@\n"),
                  serviceName);
      CFRelease (serviceName);
      return 1;
    }
    
    // Make a mutable copy to work on
    staticRoutes = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0,
                                                 oldStaticRoutes);
    
    // Find the routes for this service
    CFArrayRef oldRoutes = CFDictionaryGetValue(staticRoutes, serviceID);
    CFMutableArrayRef routes;
    
    if (!oldRoutes) {
      CFRelease (staticRoutes);
      SCPreferencesUnlock (systemConfPrefs);
      cf_fprintf (stderr, CFSTR("staticroute: no routes for service %@\n"),
                  serviceName);
      CFRelease (serviceName);
      return 1;
    }
    
    routes = CFArrayCreateMutableCopy (kCFAllocatorDefault, 0, oldRoutes);
    
    // Use the new mutable array
    CFDictionarySetValue (staticRoutes, serviceID, routes);
    CFRelease (routes);
    
    // Build the address string
    char buffer[256];
    inet_ntop (dest.af, &dest.ip, buffer, sizeof (buffer));
    
    CFStringRef addressString = CFStringCreateWithCString(kCFAllocatorDefault,
                                                          buffer,
                                                          kCFStringEncodingUTF8);
    
    // Run through the routes list looking for it
    CFIndex routeCount = CFArrayGetCount (routes);
    CFIndex n;
    for (n = 0; n < routeCount; ++n) {
      CFDictionaryRef routeDict = CFArrayGetValueAtIndex (routes, n);
      CFStringRef address = CFDictionaryGetValue (routeDict, CFSTR("address"));
      CFNumberRef prefixLen = CFDictionaryGetValue(routeDict,
                                                   CFSTR("prefixLength"));
      int prefixLenValue;
      
      if (!CFNumberGetValue(prefixLen, kCFNumberIntType, &prefixLenValue))
        continue;
      
      if (CFStringCompare (address, addressString, kCFCompareCaseInsensitive)
          == kCFCompareEqualTo
          && prefixLenValue == dest.prefix_len)
        break;
    }
    
    CFRelease (addressString);

    if (n >= routeCount) {
      CFRelease (staticRoutes);
      SCPreferencesUnlock (systemConfPrefs);
      cf_fprintf (stderr, CFSTR("staticroute: no such route for service %@\n"),
                  serviceName);
      CFRelease (serviceName);
      return 1;
    }

    // Actually delete the route
    CFArrayRemoveValueAtIndex (routes, n);
    
    // Set the value in the store
    if (!SCPreferencesSetValue(systemConfPrefs, kRoutesKey, staticRoutes)) {
      cf_fprintf (stderr, 
                  CFSTR("staticroute: cannot add route to system configuration "
                        "database.\n"));
      ret = 1;
    }
    
    // Commit the changes
    if (!ret && !SCPreferencesCommitChanges (systemConfPrefs)) {
      cf_fprintf (stderr,
                  CFSTR("staticroute: cannot commit changes to system "
                        "configuration database.\n"));
      ret = 1;
    }
    
    // Apply the changes
    if (!ret && !SCPreferencesApplyChanges (systemConfPrefs)) {
      cf_fprintf (stderr,
                  CFSTR("staticroute: cannot apply changes to system "
                        "configuration database.\n"));
      ret = 1;
    }
        
    CFRelease (staticRoutes);
  }
  SCPreferencesUnlock (systemConfPrefs);
  
  // Notify the dynamic store key for this service ID
  CFStringRef storeKey = CFStringCreateWithFormat (kCFAllocatorDefault,
                                                   NULL,
                                                   CFSTR("Setup:/Network/Service/%@/%@"),
                                                   serviceID,
                                                   addressFamily);
  SCDynamicStoreNotifyValue (dynamicStore, storeKey);
  CFRelease (storeKey);
  CFRelease (serviceName);

  return ret;
}

