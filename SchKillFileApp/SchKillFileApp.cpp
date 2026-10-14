// SchKillFileApp.cpp : Defines the class behaviors for the application.
//

#include "stdafx.h"
#include "SchKillFileApp.h"
#include "SchKillFileAppDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

const TCHAR szUniqueString[] = _T("SchKillFileUniqueApp");
UINT UWM_ARE_YOU_ACTIVE = ::RegisterWindowMessage(szUniqueString);

/////////////////////////////////////////////////////////////////////////////
// CSchKillFileAppApp

BEGIN_MESSAGE_MAP(CSchKillFileAppApp, CWinApp)
	//{{AFX_MSG_MAP(CSchKillFileAppApp)
		// NOTE - the ClassWizard will add and remove mapping macros here.
		//    DO NOT EDIT what you see in these blocks of generated code!
	//}}AFX_MSG
	ON_COMMAND(ID_HELP, CWinApp::OnHelp)
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CSchKillFileAppApp construction

CSchKillFileAppApp::CSchKillFileAppApp()
{
	// TODO: add construction code here,
	// Place all significant initialization in InitInstance
}

/////////////////////////////////////////////////////////////////////////////
// The one and only CSchKillFileAppApp object

CSchKillFileAppApp theApp;

/////////////////////////////////////////////////////////////////////////////
// CSchKillFileAppApp initialization

BOOL CSchKillFileAppApp::InitInstance()
{
	AfxEnableControlContainer();

	// Standard initialization
	// If you are not using these features and wish to reduce the size
	//  of your final executable, you should remove from the following
	//  the specific initialization routines you do not need.

#if _MSC_VER < 1200
#ifdef _AFXDLL
	Enable3dControls();			// Call this when using MFC in a shared DLL
#else
	Enable3dControlsStatic();	// Call this when linking to MFC statically
#endif
#endif

	HANDLE hMutex = ::CreateMutex(NULL, FALSE, szUniqueString);
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		// If duplicated instance was found
		CloseHandle(hMutex);

		// Found the existing form precisely
		HWND hWndPrev = ::GetDesktopWindow();
		HWND hWndChild = ::GetWindow(hWndPrev, GW_CHILD);

		while (hWndChild)
		{
			DWORD_PTR dwResult;
			// Use message sending with timeout to prevent the target process from freezing and causing the current process to suspend.
			if (::SendMessageTimeout(hWndChild, UWM_ARE_YOU_ACTIVE, 0, 0,
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &dwResult))
			{
				if (dwResult == 1) // Once the target form confirmed the identity
				{
					// Wake up and pin to top
					if (::IsIconic(hWndChild))
						::ShowWindow(hWndChild, SW_RESTORE);

					::SetForegroundWindow(hWndChild);
					::SetFocus(hWndChild);
					break;
				}
			}
			hWndChild = ::GetWindow(hWndChild, GW_HWNDNEXT);
		}

		return FALSE; // Exit the current instance
	}

	CSchKillFileAppDlg dlg;
	m_pMainWnd = &dlg;
	INT_PTR nResponse = dlg.DoModal();
	if (nResponse == IDOK)
	{
		// TODO: Place code here to handle when the dialog is
		//  dismissed with OK
	}
	else if (nResponse == IDCANCEL)
	{
		// TODO: Place code here to handle when the dialog is
		//  dismissed with Cancel
	}

	// Release at the end
	if(hMutex) CloseHandle(hMutex);

	// Since the dialog has been closed, return FALSE so that we exit the
	//  application, rather than start the application's message pump.
	return FALSE;
}
