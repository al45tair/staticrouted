/*
 *  staticrouted.c
 *  staticrouted
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
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>

#include "cf_printf.h"

CFStringRef kRoutesKey = CFSTR("com.coriolis-systems.StaticRoutes");
SCPreferencesRef systemConfPrefs;
SCDynamicStoreRef dynamicStore;

void dynamic_store_changed (SCDynamicStoreRef store,
                            CFArrayRef changedKeys,
                            void *info);
void setup_routes_for_service (CFStringRef serviceID);
bool remove_route (CFStringRef address,
                   CFNumberRef prefixLen,
                   CFStringRef router);
bool add_route (CFStringRef address,
                CFNumberRef prefixLen,
                CFStringRef router);
bool do_route (const char *cmd,
               CFStringRef address,
               CFNumberRef prefixLen,
               CFStringRef router);

int
main (void)
{
  CFErrorRef err;
  SCDynamicStoreContext context;
  
  systemConfPrefs = SCPreferencesCreate (kCFAllocatorDefault,
                                         CFSTR("staticroute"),
                                         NULL);
  
  if (!systemConfPrefs) {
    CFStringRef desc;
    
    err = SCCopyLastError();
    desc = CFErrorCopyDescription (err);
    cf_fprintf (stderr, 
                CFSTR("staticrouted: unable to attach to system configuration"
                      " - %ld: %@\n"),
                CFErrorGetCode (err), desc);
    CFRelease (err);
    CFRelease (desc);
    return 1;
  }
  
  memset (&context, 0, sizeof (context));
  
  dynamicStore = SCDynamicStoreCreate (kCFAllocatorDefault,
                                       CFSTR("staticroute"),
                                       dynamic_store_changed,
                                       &context);
  
  if (!dynamicStore) {
    CFStringRef desc;
    
    err = SCCopyLastError();
    desc = CFErrorCopyDescription (err);
    cf_fprintf (stderr, 
                CFSTR("staticrouted: unable to attach to system configuration"
                      " - %ld: %@\n"),
                CFErrorGetCode (err), desc);
    CFRelease (err);
    CFRelease (desc);
    CFRelease (systemConfPrefs);
    return 1;
  }    

  // Bind the store to the run loop
  CFRunLoopRef runLoop = CFRunLoopGetCurrent();
  CFRunLoopSourceRef storeSource
    = SCDynamicStoreCreateRunLoopSource(kCFAllocatorDefault, dynamicStore, 0);
  
  CFRunLoopAddSource(runLoop, storeSource, kCFRunLoopCommonModes);
  
  /* Tell the dynamic store to monitor our key and to look for network service
     state changes */
  CFStringRef regexpArray[] = {
    CFSTR("^Setup:/Network/Service/.*"),
    CFSTR("^State:/Network/Service/.*"),
  };
  CFArrayRef regexps = CFArrayCreate (kCFAllocatorDefault,
                                      (const void **)regexpArray, 2,
                                      &kCFTypeArrayCallBacks);
  SCDynamicStoreSetNotificationKeys (dynamicStore, NULL, regexps);
  CFRelease (regexps);
  
  // Trigger immediately
  CFArrayRef keys = SCDynamicStoreCopyKeyList(dynamicStore, regexpArray[1]);
  dynamic_store_changed (dynamicStore, keys, NULL);
  CFRelease (keys);
  
  // Run
  CFRunLoopRun ();
  
  CFRelease (dynamicStore);
  CFRelease (systemConfPrefs);
  CFRelease (storeSource);
  
  return 0;
}

void
install_routes (const void *value, void *context)
{
  CFStringRef serviceID = (CFStringRef)value;
  
  // Install our routes
  setup_routes_for_service (serviceID);
}

void
dynamic_store_changed (SCDynamicStoreRef store,
                       CFArrayRef changedKeys,
                       void *info)
{
  CFIndex n, numKeys = CFArrayGetCount (changedKeys);
  CFMutableSetRef services = CFSetCreateMutable(kCFAllocatorDefault,
                                                0,
                                                &kCFTypeSetCallBacks);
  
  for (n = 0; n < numKeys; ++n) {
    CFStringRef key = CFArrayGetValueAtIndex (changedKeys, n);
    CFArrayRef components = 
      CFStringCreateArrayBySeparatingStrings (kCFAllocatorDefault,
                                              key,
                                              CFSTR("/"));
    
    if (components && CFArrayGetCount (components) >= 4) {
      CFStringRef serviceID = CFArrayGetValueAtIndex (components, 3);

      CFSetAddValue (services, serviceID);
    }
    
    CFRelease (components);
  }
  
  CFSetApplyFunction (services, install_routes, NULL);
  CFRelease (services);
}

struct remove_ctx {
  CFStringRef serviceID;
  CFMutableDictionaryRef activeStaticRoutes;
};

void
remove_routes (const void *key, const void *value, void *context)
{
  struct remove_ctx *ctx = (struct remove_ctx *)context;
  CFDictionaryRef route = (CFDictionaryRef)value;
  CFStringRef address = CFDictionaryGetValue (route, CFSTR("address"));
  CFNumberRef prefixLen = CFDictionaryGetValue (route,
                                                CFSTR("prefixLength"));
  CFStringRef router = CFDictionaryGetValue (route, CFSTR("router"));
  
  if (address && prefixLen && router) {
    cf_fprintf (stderr,
                CFSTR("staticrouted: removing route %@/%@ -> %@ for service %@.\n"),
                address, prefixLen, router,
                ctx->serviceID);
    
    if (remove_route (address, prefixLen, router))
      CFDictionaryRemoveValue (ctx->activeStaticRoutes, key);
  } else {
    CFDictionaryRemoveValue (ctx->activeStaticRoutes, key);
  }
}

void
setup_routes_for_service (CFStringRef serviceID)
{
  SCPreferencesSynchronize (systemConfPrefs);
  SCPreferencesLock (systemConfPrefs, true);
  CFDictionaryRef staticRoutes = SCPreferencesGetValue (systemConfPrefs,
                                                        kRoutesKey);
  CFArrayRef routes;
  CFIndex routeCount;
  
  if (!staticRoutes) {
    SCPreferencesUnlock (systemConfPrefs);
    return;
  }
  
  routes = CFDictionaryGetValue (staticRoutes, serviceID);
  
  if (!routes) {
    SCPreferencesUnlock (systemConfPrefs);
    return;
  }
  
  routeCount = CFArrayGetCount (routes);
  
  CFStringRef dynamicKey
    = CFStringCreateWithFormat (kCFAllocatorDefault,
                                NULL,
                                CFSTR("State:/com.coriolis-systems.StaticRoutes/Service/%@"),
                                serviceID);
  CFMutableDictionaryRef activeStaticRoutes = NULL;
  {
    CFDictionaryRef activeStaticRoutesOrig = SCDynamicStoreCopyValue(dynamicStore,
                                                                     dynamicKey);
    
    if (activeStaticRoutesOrig) {
      activeStaticRoutes = CFDictionaryCreateMutableCopy(kCFAllocatorDefault,
                                                         0,
                                                         activeStaticRoutesOrig);
      CFRelease (activeStaticRoutesOrig);
    } else {
      activeStaticRoutes = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                     0,
                                                     &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
    }
  }
  CFMutableDictionaryRef inactiveStaticRoutes
    = CFDictionaryCreateMutableCopy (kCFAllocatorDefault,
                                     0,
                                     activeStaticRoutes);
  
  CFStringRef ipv4Key 
    = CFStringCreateWithFormat (kCFAllocatorDefault,
                                NULL,
                                CFSTR("State:/Network/Service/%@/IPv4"),
                                serviceID);
  CFStringRef ipv6Key
    = CFStringCreateWithFormat (kCFAllocatorDefault,
                                NULL,
                                CFSTR("State:/Network/Service/%@/IPv6"),
                                serviceID);
  CFDictionaryRef serviceStateIPv4 = SCDynamicStoreCopyValue (dynamicStore,
                                                              ipv4Key);
  CFDictionaryRef serviceStateIPv6 = SCDynamicStoreCopyValue (dynamicStore,
                                                              ipv6Key);
  CFRelease (ipv4Key);
  CFRelease (ipv6Key);
    
  CFStringRef ipv4Router = NULL;
  CFStringRef ipv6Router = NULL;
  
  if (serviceStateIPv4) {
    ipv4Router = CFDictionaryGetValue (serviceStateIPv4,
                                       CFSTR("Router"));

    if (!ipv4Router) {
      CFStringRef networkSig = CFDictionaryGetValue (serviceStateIPv4,
                                                     CFSTR("NetworkSignature"));
      CFArrayRef components = CFStringCreateArrayBySeparatingStrings (kCFAllocatorDefault,
                                                                      networkSig,
                                                                      CFSTR(";"));
      CFIndex count = CFArrayGetCount (components);
      CFIndex n;
      
      for (n = 0; n < count; ++n) {
        CFStringRef component = CFArrayGetValueAtIndex (components, n);
        if (CFStringHasPrefix (component, CFSTR("IPv4.Router="))) {
          CFIndex len = CFStringGetLength (component);
          ipv4Router = CFStringCreateWithSubstring (kCFAllocatorDefault,
                                                    component,
                                                    CFRangeMake (12,
                                                                 len - 12));
          break;
        }
      }

      CFRelease (components);
    } else {
      CFRetain (ipv4Router);
    }
  }

  if (serviceStateIPv6) {
    ipv6Router = CFDictionaryGetValue (serviceStateIPv6,
                                       CFSTR("Router"));
    if (!ipv6Router) {
      CFStringRef networkSig = CFDictionaryGetValue (serviceStateIPv6,
                                                     CFSTR("NetworkSignature"));
      CFArrayRef components = CFStringCreateArrayBySeparatingStrings (kCFAllocatorDefault,
                                                                      networkSig,
                                                                      CFSTR(";"));
      CFIndex count = CFArrayGetCount (components);
      CFIndex n;
      
      for (n = 0; n < count; ++n) {
        CFStringRef component = CFArrayGetValueAtIndex (components, n);
        if (CFStringHasPrefix (component, CFSTR("IPv6.Router="))) {
          CFIndex len = CFStringGetLength (component);
          ipv6Router = CFStringCreateWithSubstring (kCFAllocatorDefault,
                                                    component,
                                                    CFRangeMake (12,
                                                                 len - 12));
          break;
        }
      }

      CFRelease (components);
    } else {
      CFRetain (ipv6Router);
    }
  }
  
  for (CFIndex n = 0; n < routeCount; ++n) {
    CFDictionaryRef route = CFArrayGetValueAtIndex (routes, n);
    CFStringRef addressFamily = CFDictionaryGetValue (route,
                                                      CFSTR("addressFamily"));
    CFStringRef address = CFDictionaryGetValue (route, CFSTR("address"));
    CFNumberRef prefixLen = CFDictionaryGetValue (route,
                                                  CFSTR("prefixLength"));
    CFStringRef router = NULL;
    
    if (CFStringCompare (addressFamily, CFSTR("IPv4"), 0)
        == kCFCompareEqualTo)
      router = ipv4Router;
    else if (CFStringCompare (addressFamily, CFSTR("IPv6"), 0)
             == kCFCompareEqualTo)
      router = ipv6Router;
    
    if (!router)
      continue;
    
    CFStringRef key = CFStringCreateWithFormat (kCFAllocatorDefault,
                                                NULL,
                                                CFSTR("%@/%@/%@"),
                                                addressFamily,
                                                address,
                                                prefixLen);
    CFDictionaryRef oldRouteInfo = CFDictionaryGetValue (activeStaticRoutes, key);
    CFStringRef oldRouter = (oldRouteInfo
                             ? CFDictionaryGetValue (oldRouteInfo, CFSTR("router"))
                             : NULL);
    if (oldRouter && CFStringCompare (router, oldRouter, 0) == kCFCompareEqualTo) {
      CFDictionaryRemoveValue (inactiveStaticRoutes, key);
      CFRelease (key);
      continue;
    }
    
    if (oldRouter) {
      cf_fprintf (stderr,
                  CFSTR("staticrouted: removing old route %@/%@ -> %@ for "
                        "service %@.\n"),
                  address, prefixLen, oldRouter,
                  serviceID);
      if (remove_route (address, prefixLen, oldRouter))
        CFDictionaryRemoveValue (activeStaticRoutes, key);
      CFDictionaryRemoveValue (inactiveStaticRoutes, key);
    }
    
    cf_fprintf (stderr,
                CFSTR("staticrouted: adding route %@/%@ -> %@ for service %@.\n"),
                address, prefixLen, router,
                serviceID);    
    if (add_route (address, prefixLen, router)) {
      CFTypeRef keys[4] = { 
        CFSTR("addressFamily"),
        CFSTR("address"),
        CFSTR("prefixLength"),
        CFSTR("router")
      };
      CFTypeRef values[4] = { addressFamily, address, prefixLen, router };
      CFDictionaryRef routeInfo = CFDictionaryCreate(kCFAllocatorDefault,
                                                     keys, values, 4,
                                                     &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
      CFDictionaryAddValue (activeStaticRoutes, key, routeInfo);
      CFDictionaryRemoveValue (inactiveStaticRoutes, key);
      CFRelease (routeInfo);
    }
    
    CFRelease (key);
  }
  
  struct remove_ctx ctx = { serviceID, activeStaticRoutes };
  CFDictionaryApplyFunction(inactiveStaticRoutes, remove_routes, &ctx);
  
  if (serviceStateIPv4)
    CFRelease (serviceStateIPv4);
  if (serviceStateIPv6)
    CFRelease (serviceStateIPv6);  
  if (ipv4Router)
    CFRelease (ipv4Router);
  if (ipv6Router)
    CFRelease (ipv6Router);
  
  SCDynamicStoreSetValue(dynamicStore, dynamicKey, activeStaticRoutes);
  
  CFRelease (dynamicKey);
  CFRelease (activeStaticRoutes);
  CFRelease (inactiveStaticRoutes);
  SCPreferencesUnlock (systemConfPrefs);
}

bool
remove_route (CFStringRef address,
              CFNumberRef prefixLen,
              CFStringRef router)
{
  return do_route ("delete", address, prefixLen, router);
}

bool
add_route (CFStringRef address,
           CFNumberRef prefixLen,
           CFStringRef router)
{
  return do_route ("add", address, prefixLen, router);
}

bool
do_route (const char *cmd,
          CFStringRef address,
          CFNumberRef prefixLen,
          CFStringRef router)
{
  UInt8 routerBuf[256];
  UInt8 destBuf[256];
  CFIndex usedBuf = 0;
  int prefix = 0;
  
  // Grab the prefix length as an int
  CFNumberGetValue (prefixLen, kCFNumberIntType, &prefix);
  
  // Grab the router address as a UTF-8 string
  usedBuf = 0;
  CFStringGetBytes (router, CFRangeMake (0, CFStringGetLength (router)),
                    kCFStringEncodingUTF8, '?', false, routerBuf,
                    sizeof (routerBuf), &usedBuf);
  routerBuf[usedBuf] = '\0';
  
  // Grab the address as a UTF-8 string and tack /prefix-len to the end
  usedBuf = 0;
  CFStringGetBytes (address, CFRangeMake (0, CFStringGetLength (address)),
                    kCFStringEncodingUTF8, '?', false, destBuf,
                    sizeof (destBuf), &usedBuf);
  sprintf ((char *)destBuf + usedBuf, "/%d", prefix);
  
  // Build our route command
  char * const argv[] = {
    "/sbin/route",
    (char *)cmd,
    (char *)destBuf,
    (char *)routerBuf
  };
  
  // Spawn it
  pid_t childPid;
  posix_spawn_file_actions_t actions;
  
  posix_spawn_file_actions_init (&actions);
  posix_spawn_file_actions_addopen (&actions, STDOUT_FILENO,
                                    "/dev/null", O_RDWR, 0644);
  
  if (posix_spawn (&childPid, "/sbin/route",
                   &actions, NULL,
                   argv, NULL) < 0) {
    posix_spawn_file_actions_destroy (&actions);
    cf_fprintf (stderr,
                CFSTR("staticrouted: unable to spawn /sbin/route "
                      "- errno %d: %s.\n"),
                errno,
                strerror (errno));
    return false;
  }
  
  posix_spawn_file_actions_destroy (&actions);

  int status = 0;
  
  waitpid (childPid, &status, 0);
  
  if (WIFSIGNALED (status)) {
    cf_fprintf (stderr,
                CFSTR ("staticrouted: /sbin/route appears to have been "
                       "killed - signal %d.\n"),
                WTERMSIG (status));
    return false;
  }
  
  if (WEXITSTATUS (status) != 0) {
    cf_fprintf (stderr,
                CFSTR ("staticrouted: /sbin/route failed with code %d.\n"),
                WEXITSTATUS (status));
    return false;
  }
  
  return true;
}

