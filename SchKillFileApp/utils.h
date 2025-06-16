#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#include <winioctl.h>
#include "../common.h"

BOOL ExtRes(CString strFileName, WORD wResID, CString strFileType);
BOOL LoadNTDriver(CHAR* lpszDriverName, CHAR* lpszDriverPath);
BOOL StartServ(CHAR* lpszDriverName, CHAR* lpszDriverPath);
BOOL UnloadNTDriver(CHAR* szSvrName);