#include "pch.h"
#include "AppConfig.h"

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <utility>
#include <Windows.h>
#include <wincred.h>

#pragma comment(lib, "Advapi32.lib")

namespace {
    constexpr wchar_t kis_credential_target[] = L"Mogwa.KIS.OpenAPI";
    constexpr wchar_t openai_credential_target[] = L"Mogwa.OpenAI.API";

    std::string read_environment(const char* name)
    {
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};

        std::string result(value);
        std::free(value);
        return result;
    }

    std::string trim_ascii(std::string value)
    {
        const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(),
            [&](unsigned char ch) { return !is_space(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
            [&](unsigned char ch) { return !is_space(ch); }).base(), value.end());
        return value;
    }

    std::string masked_key(const std::string& value)
    {
        if (value.empty()) return "<empty>";
        if (value.size() <= 8) return std::string(value.size(), '*') + " len=" + std::to_string(value.size());
        return value.substr(0, 4) + "..." + value.substr(value.size() - 4)
            + " len=" + std::to_string(value.size());
    }

    bool read_kis_credentials(app_config::settings& output)
    {
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(kis_credential_target, CRED_TYPE_GENERIC, 0, &credential)) return false;

        const std::string value(
            reinterpret_cast<const char*>(credential->CredentialBlob),
            credential->CredentialBlobSize);
        CredFree(credential);

        const size_t first = value.find('\n');
        const size_t second = first == std::string::npos ? first : value.find('\n', first + 1);
        const size_t third = second == std::string::npos ? second : value.find('\n', second + 1);
        if (first == std::string::npos || second == std::string::npos || third == std::string::npos) {
            return false;
        }

        output.kis_app_key = value.substr(0, first);
        output.kis_app_secret = value.substr(first + 1, second - first - 1);
        output.kis_account_number = value.substr(second + 1, third - second - 1);
        output.kis_account_product_code = value.substr(third + 1);
        return output.has_kis_credentials();
    }

    bool read_openai_api_key(std::string& output)
    {
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(openai_credential_target, CRED_TYPE_GENERIC, 0, &credential)) return false;

        output.assign(
            reinterpret_cast<const char*>(credential->CredentialBlob),
            credential->CredentialBlobSize);
        CredFree(credential);
        return !output.empty();
    }

    bool delete_credential(const wchar_t* target)
    {
        if (CredDeleteW(target, CRED_TYPE_GENERIC, 0)) return true;
        return GetLastError() == ERROR_NOT_FOUND;
    }

    bool write_credential(const wchar_t* target, const std::string& value)
    {
        if (value.empty() || value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;

        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<LPWSTR>(target);
        credential.CredentialBlobSize = static_cast<DWORD>(value.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        credential.UserName = const_cast<LPWSTR>(L"Mogwa");
        return CredWriteW(&credential, 0) == TRUE;
    }
}

bool app_config::settings::has_kis_credentials() const noexcept
{
    return !kis_app_key.empty() && !kis_app_secret.empty() && !kis_account_number.empty();
}

bool app_config::settings::has_openai_api_key() const noexcept
{
    return !openai_api_key.empty();
}

bool app_config::store_kis_credentials(const settings& value)
{
    if (!value.has_kis_credentials()) return false;
    const std::string payload = value.kis_app_key + '\n'
        + value.kis_app_secret + '\n'
        + value.kis_account_number + '\n'
        + (value.kis_account_product_code.empty() ? "01" : value.kis_account_product_code);
    return write_credential(kis_credential_target, payload);
}

bool app_config::clear_kis_credentials()
{
    return delete_credential(kis_credential_target);
}

bool app_config::store_openai_api_key(const std::string& api_key)
{
    return write_credential(openai_credential_target, api_key);
}

bool app_config::clear_openai_api_key()
{
    return delete_credential(openai_credential_target);
}

app_config::settings app_config::load()
{
    settings result;
    const char* kis_source = "none";

    // UI-saved credentials are authoritative. Environment variables are kept
    // only as a fallback for developer/CI use so stale environment values cannot
    // silently override what the user saved in the application.
    settings stored;
    if (read_kis_credentials(stored)) {
        result.kis_app_key = std::move(stored.kis_app_key);
        result.kis_app_secret = std::move(stored.kis_app_secret);
        result.kis_account_number = std::move(stored.kis_account_number);
        result.kis_account_product_code = std::move(stored.kis_account_product_code);
        kis_source = "WindowsCredential";
    }
    else {
        result.kis_app_key = read_environment("MOGWA_KIS_APP_KEY");
        result.kis_app_secret = read_environment("MOGWA_KIS_APP_SECRET");
        result.kis_account_number = read_environment("MOGWA_KIS_ACCOUNT_NUMBER");
        const std::string product_code = read_environment("MOGWA_KIS_ACCOUNT_PRODUCT_CODE");
        if (!product_code.empty()) result.kis_account_product_code = product_code;
        if (result.has_kis_credentials()) kis_source = "Environment";
    }

    result.kis_app_key = trim_ascii(std::move(result.kis_app_key));
    result.kis_app_secret = trim_ascii(std::move(result.kis_app_secret));
    result.kis_account_number = trim_ascii(std::move(result.kis_account_number));
    result.kis_account_product_code = trim_ascii(std::move(result.kis_account_product_code));
    if (result.kis_account_product_code.empty()) result.kis_account_product_code = "01";

    if (!read_openai_api_key(result.openai_api_key)) {
        result.openai_api_key = read_environment("OPENAI_API_KEY");
    }

    TRACE(L"[AppConfig] KIS credential source=%hs appkey=%hs account_len=%zu product=%hs\n",
        kis_source, masked_key(result.kis_app_key).c_str(),
        result.kis_account_number.size(), result.kis_account_product_code.c_str());
    return result;
}
