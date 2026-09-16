module;

#include "Platform.h"
#include "Resource.h"
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <cstring>

export module lemonsquash.signature;

namespace Lemonsquash {
    namespace {
        HRESULT WINAPI CheckSigningIdentity(PCRYPT_PROVIDER_DATA, DWORD stepError, DWORD, DWORD signerCount, PWTD_GENERIC_CHAIN_POLICY_SIGNER_INFO* signers, void* expected) {
            if (stepError)
                return static_cast<HRESULT>(stepError);

            if (signerCount != 1 || !signers || !signers[0])
                return TRUST_E_NOSIGNATURE;

            auto signer = signers[0];
            auto chain = signer->pChainContext;
            auto certificate = static_cast<PCCERT_CONTEXT>(expected);

            if (signer->dwError)
                return static_cast<HRESULT>(signer->dwError);

            if (!chain || !chain->cChain || !chain->rgpChain[0]->cElement || !signer->pMsgSignerInfo)
                return TRUST_E_SUBJECT_NOT_TRUSTED;

            auto actual = chain->rgpChain[0]->rgpElement[0]->pCertContext;

            if (actual->cbCertEncoded != certificate->cbCertEncoded || std::memcmp(actual->pbCertEncoded, certificate->pbCertEncoded, certificate->cbCertEncoded))
                return TRUST_E_SUBJECT_NOT_TRUSTED;

            auto algorithm = signer->pMsgSignerInfo->HashAlgorithm.pszObjId;

            if (!algorithm || std::strcmp(algorithm, szOID_NIST_sha256))
                return TRUST_E_SUBJECT_NOT_TRUSTED;

            if (CertVerifyTimeValidity(nullptr, certificate->pCertInfo))
                return CERT_E_EXPIRED;

            AUTHENTICODE_EXTRA_CERT_CHAIN_POLICY_PARA extra{sizeof(extra)};
            extra.pSignerInfo = signer->pMsgSignerInfo;

            CERT_CHAIN_POLICY_PARA policy{sizeof(policy)};
            policy.pvExtraPolicyPara = &extra;

            CERT_CHAIN_POLICY_STATUS status{sizeof(status)};

            if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_AUTHENTICODE, chain, &policy, &status))
                return TRUST_E_SUBJECT_NOT_TRUSTED;

            return static_cast<HRESULT>(status.dwError);
        }
    }
}

export namespace Lemonsquash {
    void VerifyExecutableSignature(const std::filesystem::path& executable) {
        auto module = GetModuleHandleW(nullptr);
        auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_UPDATE_CERTIFICATE), RT_RCDATA);
        auto loaded = resource ? LoadResource(module, resource) : nullptr;
        auto bytes = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
        auto size = resource ? SizeofResource(module, resource) : 0;

        if (!bytes || !size)
            throw std::runtime_error("The update signing certificate is missing");

        std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)> certificate(
            CertCreateCertificateContext(X509_ASN_ENCODING, bytes, size), CertFreeCertificateContext);

        if (!certificate)
            winrt::throw_last_error();

        auto closeStore = [](HCERTSTORE store) { CertCloseStore(store, 0); };
        std::unique_ptr<void, decltype(closeStore)> store(
            CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr), closeStore);

        if (!store || !CertAddCertificateContextToStore(store.get(), certificate.get(), CERT_STORE_ADD_NEW, nullptr))
            winrt::throw_last_error();

        CERT_CHAIN_ENGINE_CONFIG config{sizeof(config)};
        config.hExclusiveRoot = store.get();
        config.dwFlags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AIA;

        HCERTCHAINENGINE engine = nullptr;

        winrt::check_bool(CertCreateCertificateChainEngine(&config, &engine));

        std::unique_ptr<void, decltype(&CertFreeCertificateChainEngine)> chainEngine(engine, CertFreeCertificateChainEngine);
        LPSTR usage = const_cast<LPSTR>(szOID_PKIX_KP_CODE_SIGNING);
        CERT_CHAIN_PARA parameters{sizeof(parameters)};
        parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        parameters.RequestedUsage.Usage = {1, &usage};

        WTD_GENERIC_CHAIN_POLICY_CREATE_INFO signerChain{sizeof(signerChain)};
        signerChain.hChainEngine = engine;
        signerChain.pChainPara = &parameters;
        signerChain.dwFlags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AIA;

        WTD_GENERIC_CHAIN_POLICY_DATA policy{sizeof(policy)};
        policy.pSignerChainInfo = &signerChain;
        policy.pfnPolicyCallback = CheckSigningIdentity;
        policy.pvPolicyArg = const_cast<PCERT_CONTEXT>(certificate.get());

        auto handle = CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (handle == INVALID_HANDLE_VALUE)
            winrt::throw_last_error();

        std::unique_ptr<void, decltype(&CloseHandle)> file(handle, CloseHandle);        
        WINTRUST_FILE_INFO info{sizeof(info)};
        info.pcwszFilePath = executable.c_str();
        info.hFile = file.get();
        
        WINTRUST_DATA trust{sizeof(trust)};
        trust.pPolicyCallbackData = &policy;
        trust.dwUIChoice = WTD_UI_NONE;
        trust.fdwRevocationChecks = WTD_REVOKE_NONE;
        trust.dwUnionChoice = WTD_CHOICE_FILE;
        trust.pFile = &info;
        trust.dwStateAction = WTD_STATEACTION_VERIFY;
        trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE | WTD_DISABLE_MD2_MD4;

        GUID action = WINTRUST_ACTION_GENERIC_CHAIN_VERIFY;
        auto result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);

        trust.dwStateAction = WTD_STATEACTION_CLOSE;
        
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);

        if (result != ERROR_SUCCESS)
            throw std::runtime_error("The certificate is invalid.");
    }
}
