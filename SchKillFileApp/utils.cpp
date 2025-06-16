#include "StdAfx.h"
#include "utils.h"
#include <winsvc.h>

BOOL ExtRes(CString strFileName, WORD wResID, CString strFileType)
{
	DWORD dwWrite;
	HANDLE hFile = CreateFile(strFileName, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		return FALSE;
	}

	HRSRC hrsc = FindResource(NULL, MAKEINTRESOURCE(wResID), strFileType);
	if (hrsc == NULL) return FALSE;
	HGLOBAL hg = LoadResource(NULL, hrsc);
	if (hg == NULL) return FALSE;
	DWORD dwSize = SizeofResource(NULL, hrsc);

	WriteFile(hFile, hg, dwSize, &dwWrite, NULL);
	CloseHandle(hFile);

	return TRUE;
}

BOOL LoadNTDriver(CHAR* lpszDriverName, CHAR* lpszDriverPath)
{
	CHAR szDriverImagePath[256];
	BOOL bRet = TRUE;

	SC_HANDLE hServiceMgr = NULL;
	SC_HANDLE hServiceDDK = NULL;

	GetFullPathName(lpszDriverPath, 256, szDriverImagePath, NULL);

	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hServiceMgr == NULL)
	{
		bRet = FALSE;
		goto BeforeLeave;
	}

	hServiceDDK = CreateService(hServiceMgr,
		lpszDriverName,
		lpszDriverName,
		SERVICE_ALL_ACCESS,
		SERVICE_KERNEL_DRIVER,
		SERVICE_DEMAND_START,
		SERVICE_ERROR_IGNORE,
		szDriverImagePath,
		NULL, NULL, NULL, NULL, NULL
	);

	DWORD dwRtn;

	if (hServiceDDK == NULL)
	{
		dwRtn = GetLastError();
		if (dwRtn != ERROR_IO_PENDING && dwRtn != ERROR_SERVICE_EXISTS)
		{
			bRet = FALSE;
			goto BeforeLeave;
		}

		hServiceDDK = OpenService(hServiceMgr, lpszDriverName, SERVICE_ALL_ACCESS);
		if (hServiceDDK == NULL)
		{
			bRet = FALSE;
			goto BeforeLeave;
		}
	}

	bRet = TRUE;

BeforeLeave:
	if (hServiceDDK)
	{
		CloseServiceHandle(hServiceDDK);
	}
	if (hServiceMgr)
	{
		CloseServiceHandle(hServiceMgr);
	}

	return bRet;
}

BOOL StartServ(CHAR* lpszDriverName, CHAR* lpszDriverPath)
{
	CHAR szDriverImagePath[256];
	BOOL bRet = TRUE;

	SC_HANDLE hServiceMgr = NULL;
	SC_HANDLE hServiceDDK = NULL;

	GetFullPathName(lpszDriverPath, 256, szDriverImagePath, NULL);

	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hServiceMgr == NULL)
	{
		bRet = FALSE;
		goto BeforeLeave;
	}

	hServiceDDK = OpenService(hServiceMgr, lpszDriverName, SERVICE_ALL_ACCESS);
	if (hServiceDDK == NULL)
	{
		bRet = FALSE;
		goto BeforeLeave;
	}

	bRet = StartService(hServiceDDK, NULL, NULL);
	if (!bRet)
	{
		DWORD dwRtn = GetLastError();
		if (dwRtn != ERROR_IO_PENDING && dwRtn != ERROR_SERVICE_ALREADY_RUNNING)
		{
			bRet = FALSE;
			goto BeforeLeave;
		}
		else
		{
			if (dwRtn == ERROR_IO_PENDING)
			{
				bRet = FALSE;
				goto BeforeLeave;
			}
			else
			{
				bRet = TRUE;
				goto BeforeLeave;
			}
		}
	}

BeforeLeave:
	if (hServiceDDK)
	{
		CloseServiceHandle(hServiceDDK);
	}
	if (hServiceMgr)
	{
		CloseServiceHandle(hServiceMgr);
	}

	return bRet;
}

BOOL UnloadNTDriver(CHAR* szSvrName)
{
	bool bRet = TRUE;

	SC_HANDLE hServiceMgr = NULL;
	SC_HANDLE hServiceDDK = NULL;
	SERVICE_STATUS SvrSta;

	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hServiceMgr == NULL)
	{
		bRet = FALSE;
		goto BeforeLeave;
	}

	hServiceDDK = OpenService(hServiceMgr, szSvrName, SERVICE_ALL_ACCESS);
	if (hServiceDDK == NULL)
	{
		bRet = FALSE;
		goto BeforeLeave;
	}

	if (!ControlService(hServiceDDK, SERVICE_CONTROL_STOP, &SvrSta))
	{
		bRet = FALSE;
		goto BeforeLeave;
	}

	if (!DeleteService(hServiceDDK))
	{
		bRet = FALSE;
		goto BeforeLeave;
	}

BeforeLeave:
	if (hServiceDDK)
	{
		CloseServiceHandle(hServiceDDK);
	}
	if (hServiceMgr)
	{
		CloseServiceHandle(hServiceMgr);
	}

	return bRet;
}
