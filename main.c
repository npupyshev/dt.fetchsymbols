#include <stdio.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "MobileDevice.h"

AMDeviceNotificationRef notification;
AMDeviceRef device;
const char *shared_cache_path = NULL;
const char *shared_cache_arch = NULL;
const char *dyld_path         = NULL;
uint32_t    file_index        = 0;
const char *file_path         = NULL;
bool        list_files        = false;

CFStringRef AMDCopyErrorText(void);

const uint32_t kCommand_ListFilesPlist = 0x30303030;
const uint32_t kCommand_ListFiles      = 0;
const uint32_t kCommand_GetFile        = 0x01000000;

static inline unsigned short bswap_16(unsigned short x) {
    return (x>>8) | (x<<8);
}

static inline unsigned int bswap_32(unsigned int x) {
    return (bswap_16(x&0xffff)<<16) | (bswap_16(x>>16));
}

static inline unsigned long long bswap_64(unsigned long long x) {
    return (((unsigned long long)bswap_32(x&0xffffffffull))<<32) |
    (bswap_32(x>>32));
}

CFDictionaryRef listFilesPlistCommand(void);
void getFileCommand(int index, const char *path);
void help(void);

#define DTPathToFileAtIndex(files, idx) CFStringGetCStringPtr(CFArrayGetValueAtIndex(files, (CFIndex)idx), CFStringGetSystemEncoding())

int getDyldIndex() {
    int index = 0;
    CFDictionaryRef response = listFilesPlistCommand();
    
    if (response != NULL) {
        CFArrayRef filesList;
        filesList = CFDictionaryGetValue(response, CFSTR("files"));
        if (filesList != NULL) {
            for (CFIndex i = 0; i < CFArrayGetCount(filesList); i++) {
                if (CFEqual(CFArrayGetValueAtIndex(filesList, i), CFSTR("/usr/lib/dyld"))) {
                    index = (int)i;
					break;
                }
            }
            CFRelease(filesList);
        }
    }
    
    return index;
}

int getDyldSharedCacheIndex(CFStringRef architecture) {
    int32_t index = -1;
    CFDictionaryRef response = listFilesPlistCommand();
	CFStringRef sharedCachePath = CFSTR("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_");
    
    if (response != NULL) {
        CFArrayRef filesList;
        filesList = CFDictionaryGetValue(response, CFSTR("files"));
		
		if (architecture) {
			CFStringRef strings[] = {sharedCachePath, architecture};
			CFArrayRef stringsArray = CFArrayCreate(kCFAllocatorDefault, (const void **)strings, 2, &kCFTypeArrayCallBacks);
			if (stringsArray) {
				sharedCachePath = CFStringCreateByCombiningStrings(kCFAllocatorDefault, stringsArray, CFSTR(""));
			} else {
				puts("[!] Unexpected CoreFoundation error.");
				architecture = NULL;
			}
			CFRelease(stringsArray);
		}
		
		if (filesList != NULL) {
			bool match;
            for (CFIndex i = 0; i < CFArrayGetCount(filesList); i++) {
				if (architecture) match = CFEqual(CFArrayGetValueAtIndex(filesList, i), sharedCachePath);
				else match = CFStringFind(CFArrayGetValueAtIndex(filesList, i), sharedCachePath, 0).length != 0;
                if (match) {
                    index = (int)i;
					break;
                }
            }
			if (!match) {
				if (architecture) {
					CFStringRef message = CFStringCreateWithFormat(kCFAllocatorDefault, 0, CFSTR("[-] Can't find dyld shared cache for architecture %@."), architecture);
					if (message) {
						CFShow(message);
						CFRelease(message);
					} else
						puts("[-] Can't find dyld shared cache for the given architecture.");
				}
				else
					puts("[-] Can't find dyld shared cache.");
			}
            CFRelease(filesList);
        }
		
		if (architecture) {
			CFRelease(sharedCachePath);
		}
    }
    
    return index;
}

CFDictionaryRef listFilesPlistCommand() {
    mach_error_t ret = 0;
    AMDServiceConnectionRef serviceConnection = NULL;
    CFDictionaryRef response = NULL;
    
    AMDeviceStartSession(device);
    
    ret = AMDeviceSecureStartService(device, AMSVC_DT_FETCH_SYMBOLS, NULL, &serviceConnection);
    if (ret == MDERR_OK) {
        uint32_t commandConfirmationBuffer = 0;
        AMDServiceConnectionSend(serviceConnection, &kCommand_ListFilesPlist, sizeof(uint32_t));
        AMDServiceConnectionReceive(serviceConnection, &commandConfirmationBuffer, sizeof(uint32_t));
        if (commandConfirmationBuffer == kCommand_ListFilesPlist) {
            CFPropertyListFormat format;
            AMDServiceConnectionReceiveMessage(serviceConnection, &response, &format);
        }
        AMDServiceConnectionInvalidate(serviceConnection);
    }
    
    AMDeviceStopSession(device);
    return response;
}

/*
 * index - the index of file in an array returned by ListFiles or ListFilesPlist command.
 *  path - where to save the file on the host machine.
 */
void getFileCommand(int index, const char *path) {
	if (index < 0) return;
	
    mach_error_t ret = MDERR_OK;
    AMDServiceConnectionRef serviceConnection = NULL;
    
    AMDeviceStartSession(device);
    ret = AMDeviceSecureStartService(device, AMSVC_DT_FETCH_SYMBOLS, NULL, &serviceConnection);
    if (ret != MDERR_OK) {
        puts("[-] Can not connect to com.apple.dt.fetchsymbols service.");
    } else {
        CFDictionaryRef imageList = (CFDictionaryRef)listFilesPlistCommand();
        if (imageList && (CFGetTypeID(imageList) == CFDictionaryGetTypeID())) {
            CFArrayRef files = CFDictionaryGetValue(imageList, CFSTR("files"));
            if (files && (CFGetTypeID(files) == CFArrayGetTypeID()) && (CFArrayGetCount(files) > index)) {
                uint64_t rsize = 0;
                rsize = AMDServiceConnectionSend(serviceConnection, &kCommand_GetFile, sizeof(uint32_t));
                if (rsize != sizeof(uint32_t)) {
                    puts("[-] Can not send message to com.apple.dt.fetchsymbols service. Size mismatch.");
                } else {
                    uint32_t commandConfirmation = 0;
                    AMDServiceConnectionReceive(serviceConnection, &commandConfirmation, sizeof(uint32_t));
                    /*
                     * Command confirmation. Sent for all commands.
                     */
                    if (commandConfirmation != kCommand_GetFile) {
                        puts("[!] com.apple.dt.fetchsymbols service internal error.");
                    } else {
                        uint32_t bsindex = bswap_32(index);
                        rsize = AMDServiceConnectionSend(serviceConnection, &bsindex, sizeof(uint32_t));
                        if (rsize != sizeof(uint32_t)) {
                            puts("[-] Failed to request file size.");
                        } else {
                            uint64_t size = 0;
                            AMDServiceConnectionReceive(serviceConnection, &size, sizeof(uint64_t));
                            size = bswap_64(size);
                            if (size != 0) {
                                int file = 0;
                                if (access(path, F_OK) == -1) {
                                    file = open(path, O_RDWR | O_CREAT);
                                    chmod(path, S_IROTH | S_IRGRP | S_IWUSR | S_IRUSR);
                                }
                                else
                                    file = open(path, O_RDWR);
                                
                                if (file >= 0) {
                                    /*
                                     * Set file size.
                                     */
                                    lseek(file, size-1, SEEK_SET);
                                    write(file, "", 1);
                                    
                                    void *map = mmap(0, size, PROT_WRITE | PROT_READ, MAP_SHARED, file, 0);
                                    if (map != MAP_FAILED) {
                                        printf("[*] Receiving %s...\n", DTPathToFileAtIndex(files, index));
                                        rsize = AMDServiceConnectionReceive(serviceConnection, map, size);
                                        while (rsize < size) {
                                            rsize += AMDServiceConnectionReceive(serviceConnection, (void *)(map + rsize), size-rsize);
                                            if (rsize <= 0) {
                                                puts("Error");
                                            }
                                            printf("[*] Received %3.2f MB of %3.2f MB (%llu%%).\n\e[1A", (double)rsize/(1024*1024), (double)size/(1024*1024),(uint64_t)((double)rsize/(double)size*100));
                                        }
                                        if (rsize == size) printf("\n[+] Done receiving %s.\n", DTPathToFileAtIndex(files, index));
                                        munmap(map, size);
                                    } else puts("[-] Error. Please restart the program.");
                                    close(file);
                                } else printf("[-] File \"%s\" can not be opened.\n", path);
                            } else puts("[-] Error. File size is zero.");
                        }
                    }
                }
            }
        } else puts("[-] Index does not exist.");
        AMDServiceConnectionInvalidate(serviceConnection);
        if (imageList) CFRelease(imageList);
    }
    AMDeviceStopSession(device);
}

void device_notification_callback(struct am_device_notification_callback_info *info, int cookie) {
    switch (info->msg) {
        case ADNCI_MSG_CONNECTED:
            if (!device) {
                if (AMDeviceConnect(info->dev) == MDERR_OK) {
                    device = info->dev;
                    CFShow(CFStringCreateWithFormat(CFAllocatorGetDefault(), NULL, CFSTR("\e[1A[+] Device connected: %@, iOS %@."), AMDeviceCopyValue(device, NULL, CFSTR("ProductType")), AMDeviceCopyValue(device, NULL, CFSTR("ProductVersion"))));
                    
					if (list_files) {
                        CFDictionaryRef response = listFilesPlistCommand();
                        if (response) {
                        CFArrayRef files = CFDictionaryGetValue(response, CFSTR("files"));
                            if (files) {
                                for (CFIndex i = 0; i < CFArrayGetCount(files); i++) {
                                    CFShow(CFStringCreateWithFormat(CFAllocatorGetDefault(), 0, CFSTR("  %li: %@"), i, CFArrayGetValueAtIndex(files, i)));
                                }
                            } else puts("[-] Can not get list of files.");
                        }
                    }
                    
                    if (file_path) getFileCommand(file_index, file_path);
                    
					if (shared_cache_path) {
						CFStringRef architecture = NULL;
						if (shared_cache_arch) {
							architecture = CFStringCreateWithCStringNoCopy(kCFAllocatorDefault, shared_cache_arch, CFStringGetSystemEncoding(), kCFAllocatorNull);
						}
						getFileCommand(getDyldSharedCacheIndex(architecture), shared_cache_path);
						if (architecture) {
							CFRelease(architecture);
						}
					}
                    
                    if (dyld_path) getFileCommand(getDyldIndex(), dyld_path);
                    
                    CFRunLoopStop(CFRunLoopGetMain());
                } else
                    puts("[!] Connection error. Please reconnect your device.");
            }
            break;
            
        case ADNCI_MSG_DISCONNECTED:
            if (info->dev == device) {
                puts("[*] Device disconnected.");
            }
            break;
            
        case ADNCI_MSG_UNSUBSCRIBED:
            puts("[-] Unsubscribed from device connection notifications.\n    Please restart the program.");
            break;
            
        default:
            break;
    }
}

int main(int argc, const char * argv[]) {
    if (argc == 1) help();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) list_files = true;
        else if (!strcmp(argv[i], "-c")) {
            if ((i + 1) < argc)
                shared_cache_path = argv[++i];
            else
                help();
		}
		else if (!strcmp(argv[i], "-C")) {
			if ((i + 2) < argc) {
				shared_cache_arch = argv[++i];
				shared_cache_path = argv[++i];
			} else
				help();
		}
        else if (!strcmp(argv[i], "-d")) {
            if ((i + 1) < argc)
                dyld_path = argv[++i];
            else
                help();
        }
        else if (!strcmp(argv[i], "-f")) {
            if ((i + 2) < argc) {
                file_index = atoi(argv[++i]);
                file_path = argv[++i];
            }
            else
                help();
        }
        else
            help();
    }
    
    mach_error_t ret = MDERR_OK;
    ret = AMDeviceNotificationSubscribe(&device_notification_callback, 0, 0, 0, &notification);
    if (ret == MDERR_OK) {
        puts("[*] Waiting for device.");
        CFRunLoopRun();
    } else
        puts("[-] Failed to subscribe for device connection notifications.");
    return 0;
}

void help() {
    puts("\n[*] DTFetchSymbols client v1.1");
    puts(" Your iOS device needs a mounted developer image to");
    puts(" use this tool.\n");
    puts(" Options:");
    puts("  -l           -  List available files.");
    puts("  -f n path    -  Download file with index n to path 'path'.");
	puts("  -c path      -  Download dyld shared cache to path 'path'.");
	puts("  -C arch path -  Download dyld shared cache for architecture 'arch' to path 'path'.");
    puts("  -d path      -  Download /usr/lib/dyld to path 'path'.");
    puts("  -h           -  Display this message.");
    exit(0);
}
