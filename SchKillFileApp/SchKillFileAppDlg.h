// SchKillFileAppDlg.h : header file
//

#if !defined(__SchKillFileAppDLG_H_)
#define __SchKillFileAppDLG_H_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

/////////////////////////////////////////////////////////////////////////////
// CSchKillFileAppDlg dialog

class CSchKillFileAppDlg : public CDialog
{
// Construction
public:
	CSchKillFileAppDlg(CWnd* pParent = NULL);	// standard constructor

// Dialog Data
	//{{AFX_DATA(CSchKillFileAppDlg)
	enum { IDD = IDD_SchKillFileApp_DIALOG };
	CStatic	m_myGitProfile;
	CListCtrl	m_list;
	//}}AFX_DATA

	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CSchKillFileAppDlg)
	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:
	HICON m_hIcon;

	// Generated message map functions
	//{{AFX_MSG(CSchKillFileAppDlg)
	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnBnClickedForceDelete();
	afx_msg void OnBnClickedAddFile();
	afx_msg void OnBnClickedRemove();
	afx_msg void OnDropFiles(HDROP hDropInfo); 
	afx_msg void OnBnClickedSelAll();
	afx_msg void OnBnClickedSelNone();
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnClose();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
public:
	afx_msg void OnBnClickedAddFolder();
};

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(__SchKillFileAppDLG_H_)
