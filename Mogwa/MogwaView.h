// MogwaView.h: CMogwaView 헤더 파일
//

#include <L2Webview/L2WebviewListener.h>
#include <string_view>

#pragma once

namespace scheme {
	inline constexpr std::wstring_view document_load = L"m2_document_load";
}

namespace webview_message {
    inline constexpr std::wstring_view show_native_message = L"show_native_message\n";
    inline constexpr std::wstring_view retry_kis_stream = L"retry_kis_stream";
    inline constexpr std::wstring_view clear_kis_credentials = L"clear_kis_credentials";
    inline constexpr std::wstring_view clear_openai_api_key = L"clear_openai_api_key";
    inline constexpr std::wstring_view save_openai_api_key = L"save_openai_api_key\n";
    inline constexpr std::wstring_view sync_manual_holdings = L"sync_manual_holdings\n";
    inline constexpr std::wstring_view sync_realtime_watchlist = L"sync_realtime_watchlist\n";
    inline constexpr std::wstring_view load_portfolio_history = L"load_portfolio_history\n";
    inline constexpr std::wstring_view load_performance_history = L"load_performance_history\n";
    inline constexpr std::wstring_view request_realtime_quote = L"request_realtime_quote\n";
    inline constexpr std::wstring_view save_kis_credentials = L"save_kis_credentials\n";
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
