// KISDomain.h: KISDomain 헤더 파일.

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace kis_domain {
    struct information_token {
        bool ready = false;
        std::string access_token;
        std::string access_token_expired;
        std::string token_type;
        int64_t expires_in = 0;
    };

    struct balance_item1 {
        std::string cano;
        std::string acnt_prdt_cd;
        std::string prdt_type_cd;
        std::string ovrs_pdno;
        std::string ovrs_item_name;
        double frcr_evlu_pfls_amt = 0;
        double evlu_pfls_rt = 0;
        double pchs_avg_pric = 0;
        double ovrs_cblc_qty = 0;
        double ord_psbl_qty = 0;
        double frcr_pchs_amt1 = 0;
        double ovrs_stck_evlu_amt = 0;
        double now_pric2 = 0;
        std::string tr_crcy_cd;
        std::string ovrs_excg_cd;
        std::string loan_type_cd;
        std::string loan_dt;
        std::string expd_dt;
    };

    struct balance_item2 {
        double frcr_pchs_amt1 = 0;
        double ovrs_rlzt_pfls_amt = 0;
        double ovrs_tot_pfls = 0;
        double rlzt_erng_rt = 0;
        double tot_evlu_pfls_amt = 0;
        double tot_pftrt = 0;
        double frcr_buy_amt_smtl1 = 0;
        double ovrs_rlzt_pfls_amt2 = 0;
        double frcr_buy_amt_smtl2 = 0;
    };

    struct minute_bar {
        std::string local_timestamp;  // 현지시간 YYYYMMDDHHMMSS
        std::string kr_timestamp;     // 한국시간 YYYYMMDDHHMMSS
        double open = 0;
        double high = 0;
        double low = 0;
        double close = 0;
    };

    struct daily_bar {
        std::string date;             // YYYYMMDD
        double open = 0;
        double high = 0;
        double low = 0;
        double close = 0;
        double volume = 0;
    };

    struct overseas_quote {
        std::string ticker;
        std::string exchange;
        double previous_close = 0;
        double price = 0;
        double change = 0;
        double change_rate = 0;
        double volume = 0;
        double amount = 0;
        std::string change_sign;
        std::string orderable;
    };

    struct information_balance {
        std::vector<balance_item1> stockList;
        balance_item2 summary;
    };
}
