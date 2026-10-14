#pragma once 

#define NT_DEVICE_NAME              L"\\Device\\SchKFDrv" 
#define DOS_DEVICE_NAME             L"\\DosDevices\\SchKFDrv" 
#define WIN32_NAME					"\\\\.\\SchKFDrv"

//
// Device IO Control Codes
//
#define IOCTL_BASE          0x800
#define MY_CTL_CODE(i)        \
    CTL_CODE                  \
    (                         \
        FILE_DEVICE_UNKNOWN,  \
        IOCTL_BASE + i,       \
        METHOD_BUFFERED,      \
        FILE_ANY_ACCESS       \
    )

#define IOCTL_DELETEFILE					 MY_CTL_CODE(0)
#define IOCTL_DELETEFOLDER                   MY_CTL_CODE(1)

