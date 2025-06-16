// SchKillFileAppDlg.cpp : implementation file
//
#include "stdafx.h"
#include "SchKillFileApp.h"
#include "SchKillFileAppDlg.h"
#include "utils.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

#define SERVICE_NAME  "SchKillFileDrv"
#define DRIVER_NAME	  "SchKillFileDrv.sys"

/////////////////////////////////////////////////////////////////////////////
// CSchKillFileAppDlg dialog

CSchKillFileAppDlg::CSchKillFileAppDlg(CWnd* pParent /*=NULL*/)
	: CDialog(CSchKillFileAppDlg::IDD, pParent)
{
	//{{AFX_DATA_INIT(CSchKillFileAppDlg)
	//}}AFX_DATA_INIT
	// Note that LoadIcon does not require a subsequent DestroyIcon in Win32
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CSchKillFileAppDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(CSchKillFileAppDlg)
	DDX_Control(pDX, IDC_STATICMYGITPROFILE, m_myGitProfile);
	DDX_Control(pDX, IDC_LISTFILE, m_list);
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CSchKillFileAppDlg, CDialog)
	//{{AFX_MSG_MAP(CSchKillFileAppDlg)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_SELALL, OnBnClickedSelAll)
	ON_BN_CLICKED(IDC_SELNONE, OnBnClickedSelNone)
	ON_BN_CLICKED(IDC_ADDFILE, OnBnClickedAddFile)
	ON_BN_CLICKED(IDC_ADDFOLDER, OnBnClickedAddFolder)
	ON_BN_CLICKED(IDC_REMOVESEL, OnBnClickedRemove)
	ON_BN_CLICKED(IDC_FCDEL, OnBnClickedForceDelete)
	ON_WM_DROPFILES()
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONDOWN()
	ON_WM_CLOSE()
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CSchKillFileAppDlg message handlers

#ifndef PROCESSOR_ARCHITECTURE_ARM64
#define PROCESSOR_ARCHITECTURE_ARM64            12
#endif

BOOL CSchKillFileAppDlg::OnInitDialog()
{
	SYSTEM_INFO systemInfo;
	BOOL loaded = FALSE;
	HMODULE hKernel32 = NULL;
	typedef void (WINAPI* LPFN_GetNativeSystemInfo)(LPSYSTEM_INFO);
	LPFN_GetNativeSystemInfo pGetNativeSystemInfo = NULL;

	CDialog::OnInitDialog();

	// Set the icon for this dialog.  The framework does this automatically
	// when the application's main window is not a dialog
	SetIcon(m_hIcon, TRUE); // Set big icon
	SetIcon(m_hIcon, FALSE); // Set small icon

	// TODO: Add extra initialization here
	hKernel32 = LoadLibrary("kernel32.dll");
	if (hKernel32)
	{
		pGetNativeSystemInfo = (LPFN_GetNativeSystemInfo)GetProcAddress(hKernel32, "GetNativeSystemInfo");
	}

	if (pGetNativeSystemInfo)
	{
		// Successfully found and loaded GetNativeSystemInfo
		pGetNativeSystemInfo(&systemInfo);
		switch (systemInfo.wProcessorArchitecture) {
		case PROCESSOR_ARCHITECTURE_AMD64:
			loaded = ExtRes(DRIVER_NAME, (WORD)IDR_SYSX64, "SYS");
			break;
		case PROCESSOR_ARCHITECTURE_INTEL:
			loaded = ExtRes(DRIVER_NAME, (WORD)IDR_SYSX86, "SYS");
			break;
		case PROCESSOR_ARCHITECTURE_ARM64:
			loaded = ExtRes(DRIVER_NAME, (WORD)IDR_SYSA64, "SYS");
			break;
		case PROCESSOR_ARCHITECTURE_ARM:
			loaded = ExtRes(DRIVER_NAME, (WORD)IDR_SYSA32, "SYS");
			break;
		case PROCESSOR_ARCHITECTURE_UNKNOWN:
		default:
			break;
		}
	}
	else
	{
		// If GetNativeSystemInfo is not available, then it's Windows 2000 case
		loaded = ExtRes(DRIVER_NAME, (WORD)IDR_SYSX86, "SYS");
	}

	if (!loaded)
	{
		MessageBoxA("Failed to release driver!");
		ExitProcess(0);
		return FALSE;
	}

	BOOL bRet = LoadNTDriver(SERVICE_NAME, DRIVER_NAME);
	if (!bRet)
	{
		UnloadNTDriver(SERVICE_NAME);
		bRet = LoadNTDriver(SERVICE_NAME, DRIVER_NAME);
		if (!bRet)
		{
			MessageBoxA("Failed to load driver!");
			ExitProcess(0);
			return FALSE;
		}
	}

	bRet = StartServ(SERVICE_NAME, DRIVER_NAME);
	if (!bRet)
	{
		MessageBoxA("Failed to start service, program exiting!");
		ExitProcess(0);
		return FALSE;
	}

	DeleteFile(DRIVER_NAME);

	m_list.SetExtendedStyle(LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	m_list.InsertColumn(0, "File/Folder Name");
	m_list.InsertColumn(1, "File/Folder Path");
	m_list.SetColumnWidth(0, 130);
	m_list.SetColumnWidth(1, 380);

	return TRUE; // return TRUE unless you set the focus to a control
}

void CSchKillFileAppDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	CDialog::OnSysCommand(nID, lParam);
}

// If you add a minimize button to your dialog, you will need the code below
//  to draw the icon.  For MFC applications using the document/view model,
//  this is automatically done for you by the framework.

void CSchKillFileAppDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // device context for painting

		SendMessage(WM_ICONERASEBKGND, (WPARAM)dc.GetSafeHdc(), 0);

		// Center icon in client rectangle
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// Draw the icon
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialog::OnPaint();
	}
}

// The system calls this to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR CSchKillFileAppDlg::OnQueryDragIcon()
{
	return (HCURSOR)m_hIcon;
}

void CSchKillFileAppDlg::OnBnClickedForceDelete()
{
	// TODO: Add your control notification handler code here
	// Abra el mango del conductor una vez y reutilícelo en un bucle
	HANDLE hDev = CreateFile(WIN32_NAME, GENERIC_READ | GENERIC_WRITE, 0,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hDev == INVALID_HANDLE_VALUE)
	{
		MessageBoxA("Could not open driver handle!", "Error", MB_ICONERROR);
		return;
	}

	for (int j = 0; j < m_list.GetItemCount(); j++)
	{
		if (!m_list.GetCheck(j)) continue;

		CString str = m_list.GetItemText(j, 1);
		// Determinar si es un archivo o una carpeta
		bool isDir = (GetFileAttributes(str) & FILE_ATTRIBUTE_DIRECTORY) != 0;

		WCHAR szwPath[512] = { 0 };
		MultiByteToWideChar(CP_ACP, 0, CT2A(str), -1, szwPath, _countof(szwPath));

		DWORD dwRet = 0;
		DWORD ctrl = isDir ? IOCTL_DELETEFOLDER : IOCTL_DELETEFILE;
		DeviceIoControl(hDev, ctrl, szwPath, sizeof(szwPath), NULL, 0, &dwRet, NULL);

		m_list.DeleteItem(j);
		j--;
	}

	CloseHandle(hDev);
}

void CSchKillFileAppDlg::OnBnClickedAddFile()
{
	// TODO: Add your control notification handler code here
	CString strFilter = _T("All Files(*.*)|*.*||");
	CFileDialog fileDlg(
		TRUE,
		NULL,
		NULL,
		OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_ALLOWMULTISELECT | OFN_EXPLORER,
		strFilter
	);

	OPENFILENAME ofn = fileDlg.GetOFN();
	ofn.nMaxFile = 65536;
	ofn.lpstrFile = new TCHAR[ofn.nMaxFile];

	if (fileDlg.DoModal() == IDOK)
	{
		POSITION pos = fileDlg.GetStartPosition();
		while (pos)
		{
			CString strPath = fileDlg.GetNextPathName(pos);
			CString strName = strPath.Right(strPath.GetLength() - strPath.ReverseFind('\\') - 1);

			int nIndex = m_list.InsertItem(m_list.GetItemCount(), strName);
			m_list.SetItemText(nIndex, 1, strPath);
		}
	}

	delete[] ofn.lpstrFile;
}

void CSchKillFileAppDlg::OnBnClickedAddFolder()
{
	// TODO: Agregue aquí su código de controlador de notificación de control
	BROWSEINFO bi = { 0 };
	bi.hwndOwner = m_hWnd;
	bi.lpszTitle = _T("Select a folder to delete");
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
	LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
	if (pidl)
	{
		TCHAR szPath[MAX_PATH];
		if (SHGetPathFromIDList(pidl, szPath))
		{
			CString strPath = szPath;
			CString strName = strPath.Mid(strPath.ReverseFind(_T('\\')) + 1);
			int idx = m_list.InsertItem(m_list.GetItemCount(), strName);
			m_list.SetItemText(idx, 1, strPath);
		}
		CoTaskMemFree(pidl);
	}
}

void CSchKillFileAppDlg::OnBnClickedRemove()
{
	// TODO: Add your control notification handler code here
	for (int j = 0; j < m_list.GetItemCount(); j++)
	{
		if (m_list.GetCheck(j))
		{
			m_list.DeleteItem(j);
			j--;
		}
	}
}

void CSchKillFileAppDlg::OnDropFiles(HDROP hDropInfo)
{
	UINT nFileCount = DragQueryFile(hDropInfo, 0xFFFFFFFF, NULL, 0);
	for (UINT i = 0; i < nFileCount; ++i)
	{
		TCHAR filePath[MAX_PATH] = { 0 };
		DragQueryFile(hDropInfo, i, filePath, MAX_PATH);

		CString strPath = filePath;
		CString strName = strPath.Right(strPath.GetLength() - strPath.ReverseFind('\\') - 1);

		int nIndex = m_list.InsertItem(m_list.GetItemCount(), strName);
		m_list.SetItemText(nIndex, 1, strPath);
	}

	DragFinish(hDropInfo);
}

void CSchKillFileAppDlg::OnBnClickedSelAll()
{
	// TODO: Add your control notification handler code here
	for (int j = 0; j < m_list.GetItemCount(); j++)
	{
		m_list.SetCheck(j);
	}
}

void CSchKillFileAppDlg::OnBnClickedSelNone()
{
	// TODO: Add your control notification handler code here
	for (int j = 0; j < m_list.GetItemCount(); j++)
	{
		m_list.SetCheck(j, FALSE);
	}
}

void CSchKillFileAppDlg::OnMouseMove(UINT nFlags, CPoint point)
{
	// TODO: Add your message handler code here and/or call default
	CRect rcmyGitProfile;
	m_myGitProfile.GetClientRect(&rcmyGitProfile);
	m_myGitProfile.ClientToScreen(&rcmyGitProfile);
	ScreenToClient(&rcmyGitProfile);

	if (point.x > rcmyGitProfile.left && point.x < rcmyGitProfile.right && point.y > rcmyGitProfile.top && point.y < rcmyGitProfile.bottom)
	{
		HCURSOR hCursor;
		hCursor = ::LoadCursor(NULL, IDC_HAND);
		SetCursor(hCursor);
	}

	CDialog::OnMouseMove(nFlags, point);
}

void CSchKillFileAppDlg::OnLButtonDown(UINT nFlags, CPoint point)
{
	// TODO: Add your message handler code here and/or call default
	CRect rcmyGitProfile;

	m_myGitProfile.GetClientRect(&rcmyGitProfile);
	m_myGitProfile.ClientToScreen(&rcmyGitProfile);
	ScreenToClient(&rcmyGitProfile);

	if (point.x > rcmyGitProfile.left && point.x < rcmyGitProfile.right && point.y > rcmyGitProfile.top && point.y < rcmyGitProfile.bottom)
	{
		ShellExecute(NULL, NULL, _T("https://www.github.com/nuloperrito"), NULL, NULL, SW_NORMAL);
	}

	CDialog::OnLButtonDown(nFlags, point);
}


void CSchKillFileAppDlg::OnClose()
{
	// TODO: Add your message handler code here and/or call default
	UnloadNTDriver(SERVICE_NAME);
	CDialog::OnClose();
}
