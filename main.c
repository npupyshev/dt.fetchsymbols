#include <stdio.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "MobileDevice.h"

AMDeviceNotificationRef notification;
AMDeviceRef device;
const char *shared_cache_path = NULL;
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
void getFileCommand(uint32_t index, const char *path);
void help(void);

#define DTPathToFileAtIndex(idx) CFStringGetCStringPtr(CFArrayGetValueAtIndex(CFDictionaryGetValue(listFilesPlistCommand(), CFSTR("files")), (CFIndex)idx), CFStringGetSystemEncoding())

uint32_t getDyldIndex() {
    uint32_t index = 0;
    CFDictionaryRef response = listFilesPlistCommand();
    
    if (response != NULL) {
        CFArrayRef filesList;
        filesList = CFDictionaryGetValue(response, CFSTR("files"));
        if (filesList != NULL) {
            for (CFIndex i = 0; i < CFArrayGetCount(filesList); i++) {
                if (CFEqual(CFArrayGetValueAtIndex(filesList, i), CFSTR("/usr/lib/dyld"))) {
                    index = (uint32_t)i;
                }
            }
            CFRelease(filesList);
        }
    }
    
    return index;
}

uint32_t getDyldSharedCacheIndex() {
    uint32_t index = 1;
    CFDictionaryRef response = listFilesPlistCommand();
    
    if (response != NULL) {
        CFArrayRef filesList;
        filesList = CFDictionaryGetValue(response, CFSTR("files"));
        if (filesList != NULL) {
            for (CFIndex i = 0; i < CFArrayGetCount(filesList); i++) {
                if (CFStringFind(CFArrayGetValueAtIndex(filesList, i), CFSTR("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_"), 0).length == 56) {
                    index = (uint32_t)i;
                }
            }
            CFRelease(filesList);
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
void getFileCommand(uint32_t index, const char *path) {
    mach_error_t ret = MDERR_OK;
    AMDServiceConnectionRef serviceConnection = NULL;
    
    AMDeviceStartSession(device);
    ret = AMDeviceSecureStartService(device, AMSVC_DT_FETCH_SYMBOLS, NULL, &serviceConnection);
    if (ret != MDERR_OK) {
        puts("[-] Can not connect to com.apple.dt.fetchsymbols service.");
    } else {
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
                        if (!access(path, F_OK)) {
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
                                printf("[*] Receiving %s...\n", DTPathToFileAtIndex(index));
                                rsize = AMDServiceConnectionReceive(serviceConnection, map, size);
                                while (rsize < size) {
                                    rsize += AMDServiceConnectionReceive(serviceConnection, (void *)(map + rsize), size-rsize);
                                    if (rsize <= 0) {
                                        puts("Error");
                                    }
                                    printf("[*] Received %3.2f MB of %3.2f MB (%llu%%).\n\e[1A", (double)rsize/(1024*1024), (double)size/(1024*1024),(uint64_t)((double)rsize/(double)size*100));
                                }
                                if (rsize == size) printf("\n[+] Done receiving %s.\n", DTPathToFileAtIndex(index));
                                munmap(map, size);
                            } else puts("[-] Error. Please restart the program.");
                            close(file);
                        } else printf("[-] File %s can not be opened.", path);
                    } else puts("[-] Error. File size is zero.");
                }
            }
        }
        AMDServiceConnectionInvalidate(serviceConnection);
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
                    
                    if (shared_cache_path) getFileCommand(getDyldSharedCacheIndex(), shared_cache_path);
                    
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
    puts("\n[*] DTFetchSymbols client v1.0");
    puts(" Your iOS device needs a mounted developer image to");
    puts(" use this tool.\n");
    puts(" Options:");
    puts("  -l        -  List available files.");
    puts("  -f n path -  Download file with index n to path 'path'.");
    puts("  -c path   -  Download dyld shared cache to path 'path'.");
    puts("  -d path   -  Download /usr/lib/dyld to path 'path'.\n");
    puts("  -h        -  Display this message.");
    puts("[*] Author: https://theiphonewiki.com/wiki/User:npupyshev\n");
    exit(0);
}
