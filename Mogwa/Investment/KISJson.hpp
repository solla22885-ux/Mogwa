//KISJson.hpp: KISJson 구현 파일.

#include "KISDomain.h"
#include <boost/json.hpp>

namespace kis_json {
    boost::json::object to_json(const kis_domain::balance_item1& balance) {
        boost::json::object obj;

        //string
        obj["cano"]           = balance.cano;
        obj["acnt_prdt_cd"]   = balance.acnt_prdt_cd;
        obj["prdt_type_cd"]   = balance.prdt_type_cd;
        obj["ovrs_pdno"]      = balance.ovrs_pdno;
        obj["ovrs_item_name"] = balance.ovrs_item_name;
        obj["tr_crcy_cd"]     = balance.tr_crcy_cd;
        obj["ovrs_excg_cd"]   = balance.ovrs_excg_cd;
        obj["loan_type_cd"]   = balance.loan_type_cd;
        obj["loan_dt"]        = balance.loan_dt;
        obj["expd_dt"]        = balance.expd_dt;

        //double
        obj["frcr_evlu_pfls_amt"] = balance.frcr_evlu_pfls_amt;
        obj["evlu_pfls_rt"]       = balance.evlu_pfls_rt;
        obj["pchs_avg_pric"]      = balance.pchs_avg_pric;
        obj["ovrs_cblc_qty"]      = balance.ovrs_cblc_qty;
        obj["ord_psbl_qty"]       = balance.ord_psbl_qty;
        obj["frcr_pchs_amt1"]     = balance.frcr_pchs_amt1;
        obj["ovrs_stck_evlu_amt"] = balance.ovrs_stck_evlu_amt;
        obj["now_pric2"]          = balance.now_pric2;
        return obj;
    }

    boost::json::object to_json(const kis_domain::balance_item2& balance) {
        boost::json::object obj;

        //double
        obj["frcr_pchs_amt1"]      = balance.frcr_pchs_amt1;
        obj["ovrs_rlzt_pfls_amt"]  = balance.ovrs_rlzt_pfls_amt;
        obj["ovrs_tot_pfls"]       = balance.ovrs_tot_pfls;
        obj["rlzt_erng_rt"]        = balance.rlzt_erng_rt;
        obj["tot_evlu_pfls_amt"]   = balance.tot_evlu_pfls_amt;
        obj["tot_pftrt"]           = balance.tot_pftrt;
        obj["frcr_buy_amt_smtl1"]  = balance.frcr_buy_amt_smtl1;
        obj["ovrs_rlzt_pfls_amt2"] = balance.ovrs_rlzt_pfls_amt2;
        obj["frcr_buy_amt_smtl2"]  = balance.frcr_buy_amt_smtl2;
        return obj;
    }

    boost::json::object to_json(const kis_domain::information_balance& x) {
        boost::json::array arr;
        arr.reserve(x.stockList.size());
        for (const kis_domain::balance_item1& it : x.stockList) {
            arr.push_back(to_json(it));
        }

        boost::json::object root;
        root["stock_list"] = std::move(arr);
        root["summary"] = to_json(x.summary);
        return root;
    }
}