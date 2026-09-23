// MogwaView.h: CMogwaView 헤더 파일
//

#include <L2Webview/L2WebviewListener.h>

#pragma once

namespace scheme {
	inline constexpr std::wstring_view document_load = L"m2_document_load";
}

class L2WebviewController;
class TradeManager;

class CMogwaView : public CView
{
protected:
	CMogwaView() noexcept;
	DECLARE_DYNCREATE(CMogwaView)
public:
	virtual void OnDraw(CDC* pDC);
	virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
	virtual BOOL PreTranslateMessage(MSG* pMsg);
public:
	virtual ~CMogwaView();
protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	DECLARE_MESSAGE_MAP()

	afx_msg LRESULT OnReceivePriceData(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnReceiveStreamStatus(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnReceivePortfolioHistory(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnReceivePerformanceHistory(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnReceiveQuoteSnapshot(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnExecuteWebviewScript(WPARAM wParam, LPARAM lParam);
private:
	class webview_control : public L2WebviewListener {
	public:
		webview_control(CMogwaView* parent);
		virtual ~webview_control();
	private:
		CMogwaView* _parent;
		std::shared_ptr<TradeManager> _manager;
		std::shared_ptr<std::mutex> _operation_mutex;

	public:
		void OnWebviewMessageReceive(const std::wstring& message) override;
	};
public:
	std::shared_ptr<L2WebviewController> _view;
	std::shared_ptr<webview_control> _listener;
};
