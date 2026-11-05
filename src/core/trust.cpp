// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/trust.hpp"

#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>

#include <memory>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace ring0 {
namespace {

struct FileCloser {
    void operator()(HANDLE h) const noexcept { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
using UniqueFile = std::unique_ptr<void, FileCloser>;

struct CatAdminReleaser {
    void operator()(HCATADMIN h) const noexcept { if (h) CryptCATAdminReleaseContext(h, 0); }
};
using UniqueCatAdmin = std::unique_ptr<void, CatAdminReleaser>;

class CatInfo {
public:
    CatInfo(HCATADMIN admin, HCATINFO info) noexcept : admin_(admin), info_(info) {}
    ~CatInfo() { if (info_) CryptCATAdminReleaseCatalogContext(admin_, info_, 0); }
    CatInfo(const CatInfo&) = delete;
    CatInfo& operator=(const CatInfo&) = delete;
    HCATINFO get() const noexcept { return info_; }
private:
    HCATADMIN admin_;
    HCATINFO info_;
};

// Pairs every VERIFY with the CLOSE that frees the provider state, and reads the
// signer while that state is still alive.
struct VerifyResult {
    LONG status;
    std::wstring signer;
};

std::wstring signer_of(HANDLE state)
{
    CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(state);
    if (!pd) return {};
    CRYPT_PROVIDER_SGNR* sg = WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0);
    if (!sg || sg->csCertChain == 0 || !sg->pasCertChain || !sg->pasCertChain[0].pCert) return {};
    PCCERT_CONTEXT cert = sg->pasCertChain[0].pCert;
    const DWORD n = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
    if (n <= 1) return {};
    std::wstring name(n, L'\0');
    CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), n);
    name.resize(n - 1);
    return name;
}

VerifyResult run_trust(WINTRUST_DATA& td)
{
    td.cbStruct            = sizeof td;
    td.dwUIChoice          = WTD_UI_NONE;
    td.fdwRevocationChecks = WTD_REVOKE_NONE;
    td.dwStateAction       = WTD_STATEACTION_VERIFY;
    // Never touch the network: a task manager must not stall on CRL retrieval.
    td.dwProvFlags         = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    VerifyResult r{WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &td), {}};
    if (r.status == ERROR_SUCCESS) r.signer = signer_of(td.hWVTStateData);

    td.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &td);
    return r;
}

VerifyResult verify_embedded(const std::wstring& path)
{
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct      = sizeof fi;
    fi.pcwszFilePath = path.c_str();

    WINTRUST_DATA td{};
    td.dwUnionChoice = WTD_CHOICE_FILE;
    td.pFile         = &fi;
    return run_trust(td);
}

// Catalog hashes are SHA256 on current builds, but catalogs installed by older
// packages are SHA1 only, so both are tried.
VerifyResult verify_catalog(const std::wstring& path)
{
    VerifyResult none{static_cast<LONG>(TRUST_E_NOSIGNATURE), {}};

    UniqueFile file(CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return none;

    for (const wchar_t* alg : {BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA1_ALGORITHM}) {
        HCATADMIN raw_admin = nullptr;
        if (!CryptCATAdminAcquireContext2(&raw_admin, nullptr, alg, nullptr, 0)) continue;
        UniqueCatAdmin admin(raw_admin);

        DWORD size = 0;
        CryptCATAdminCalcHashFromFileHandle2(raw_admin, file.get(), &size, nullptr, 0);
        if (size == 0) continue;
        std::vector<BYTE> hash(size);
        if (!CryptCATAdminCalcHashFromFileHandle2(raw_admin, file.get(), &size, hash.data(), 0)) continue;

        CatInfo cat(raw_admin, CryptCATAdminEnumCatalogFromHash(raw_admin, hash.data(), size, 0, nullptr));
        if (!cat.get()) continue;

        CATALOG_INFO ci{};
        ci.cbStruct = sizeof ci;
        if (!CryptCATCatalogInfoFromContext(cat.get(), &ci, 0)) continue;

        std::wstring tag;
        tag.reserve(size * 2);
        static constexpr wchar_t hex[] = L"0123456789ABCDEF";
        for (BYTE b : hash) {
            tag.push_back(hex[b >> 4]);
            tag.push_back(hex[b & 0xF]);
        }

        WINTRUST_CATALOG_INFO wci{};
        wci.cbStruct             = sizeof wci;
        wci.pcwszCatalogFilePath = ci.wszCatalogFile;
        wci.pcwszMemberTag       = tag.c_str();
        wci.pcwszMemberFilePath  = path.c_str();
        wci.hMemberFile          = file.get();
        wci.pbCalculatedFileHash = hash.data();
        wci.cbCalculatedFileHash = size;
        wci.hCatAdmin            = raw_admin;

        WINTRUST_DATA td{};
        td.dwUnionChoice = WTD_CHOICE_CATALOG;
        td.pCatalog      = &wci;
        return run_trust(td);
    }
    return none;
}

} // namespace

TrustInfo verify(std::wstring_view path)
{
    if (path.empty()) return {};
    const std::wstring p(path);

    VerifyResult r = verify_embedded(p);
    if (r.status == static_cast<LONG>(TRUST_E_NOSIGNATURE)) r = verify_catalog(p);

    if (r.status == ERROR_SUCCESS) return {TrustState::Signed, std::move(r.signer)};
    if (r.status == static_cast<LONG>(TRUST_E_NOSIGNATURE)) return {TrustState::Unsigned, {}};
    return {TrustState::Invalid, {}};
}

TrustCache::TrustCache()
    : worker_([this](std::stop_token st) { run(st); })
{
}

TrustCache::~TrustCache()
{
    worker_.request_stop();
    wake_.notify_all();
}

void TrustCache::request(std::wstring_view path)
{
    if (path.empty()) return;
    {
        std::lock_guard lock(mutex_);
        std::wstring key(path);
        if (done_.contains(key) || queued_.contains(key)) return;
        queued_.insert(key);
        pending_.push_back(std::move(key));
    }
    wake_.notify_one();
}

TrustInfo TrustCache::lookup(std::wstring_view path) const
{
    std::lock_guard lock(mutex_);
    if (auto it = done_.find(std::wstring(path)); it != done_.end()) return it->second;
    return {};
}

void TrustCache::run(std::stop_token stop)
{
    for (;;) {
        std::wstring path;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return !pending_.empty(); })) return;
            path = std::move(pending_.front());
            pending_.pop_front();
        }
        // Verification runs outside the lock so lookup() stays non-blocking.
        TrustInfo info = verify(path);
        std::lock_guard lock(mutex_);
        queued_.erase(path);
        done_.insert_or_assign(std::move(path), std::move(info));
    }
}

} // namespace ring0
