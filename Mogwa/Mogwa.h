// Mogwa.h: Mogwa 헤더 파일.
//
#pragma once

#ifndef __AFXWIN_H__
	#error "PCH에 대해 이 파일을 포함하기 전에 'pch.h'를 포함합니다."
#endif

#include "resource.h"       // 주 기호입니다.


// CMogwaApp:
// 이 클래스의 구현에 대해서는 Mogwa.cpp을(를) 참조하세요.
//

class CMogwaApp : public CWinAppEx
{
public:
	CMogwaApp() noexcept;
public:
	virtual BOOL InitInstance();

	afx_msg void OnAppAbout();
	DECLARE_MESSAGE_MAP()
};

extern CMogwaApp theApp;
