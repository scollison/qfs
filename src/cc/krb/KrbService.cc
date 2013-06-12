//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2013/06/08
// Author: Mike Ovsiannikov
//
// Copyright 2013 Quantcast Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// Kerberos 5 service side authentication implementation.
//
//----------------------------------------------------------------------------

#include "KrbService.h"

#include <krb5/krb5.h>

#include <errno.h>

#include <string>
#include <algorithm>

namespace KFS
{

using std::string;
using std::max;

class KrbService::Impl
{
public:
    Impl()
        : mCtx(),
          mAuthCtx(),
          mServerPtr(0),
          mKeyTabPtr(0),
          mErrCode(0),
          mOutBuf(),
          mKeyBlockPtr(0),
          mUserPrincipalStrPtr(0),
          mUserPrincipalStrLen(0),
          mInitedFlag(false),
          mAuthInitedFlag(false),
          mDetectReplayFlag(false),
          mServiceName(),
          mServiceHostName(),
          mErrorMsg()
    {
        mOutBuf.data  = 0;
        mOutBuf.length = 0;
    }
    ~Impl()
        { Impl::CleanupSelf(); }
    const char* Init(
        const char* inServiceHostNamePtr,
        const char* inServeiceNamePtr,
        const char* inKeyTabNamePtr,
        bool        inDetectReplayFlag)
    {
        CleanupSelf();
        mDetectReplayFlag = inDetectReplayFlag;
        mErrCode = 0;
        mErrorMsg.clear();
        mServiceName.clear();
        mKeyTabFileName.clear();
        mServiceHostName.clear();
        if (inServeiceNamePtr) {
            mServiceName = inServeiceNamePtr;
        }
        if (inKeyTabNamePtr) {
            mKeyTabFileName = inKeyTabNamePtr;
        }
        if (inServiceHostNamePtr) {
            mServiceHostName = inServiceHostNamePtr;
        }
        InitSelf();
        if (mErrCode) {
            mErrorMsg = ErrToStr(mErrCode);
            CleanupSelf();
        }
        return (mErrCode ? mErrorMsg.c_str() : 0);
    }
    const char* Cleanup()
    {
        mErrCode = CleanupSelf();
        if (mErrCode) {
            mErrorMsg = ErrToStr(mErrCode);
        }
        return (mErrCode ? mErrorMsg.c_str() : 0);
    }
    const char* Request(
        const char* inDataPtr,
        int         inDataLen)
    {
        if (! mInitedFlag) {
            mErrCode  = EINVAL;
            mErrorMsg = "not initialized yet, invoke KrbService::Init";
            return mErrorMsg.c_str();
        }
        krb5_free_data_contents(mCtx, &mOutBuf);
        CleanupAuth();
        InitAuth();
        if (mErrCode) {
            mErrorMsg = ErrToStr(mErrCode);
        } else {
            krb5_data theData = { 0 };
            theData.length = max(0, inDataLen);
            theData.data   = const_cast<char*>(inDataPtr);
            krb5_flags   theReqOptions = { 0 };
            // krb5_ticket* theTicket     = 0;
            mErrCode = krb5_rd_req(
                mCtx,
                &mAuthCtx,
                &theData,
                mServerPtr,
                mKeyTabPtr,
                &theReqOptions,
                0 // &theTicket
            );
            // krb5_free_ticket(mCtx, theTicket);
            if (! mErrCode) {
                return 0;
            }
            mErrorMsg = ErrToStr(mErrCode);
            CleanupAuth();
        }
        return mErrorMsg.c_str();
    }
    const char* Reply(
        int          inPrincipalUnparseFlags,
        const char*& outReplyPtr,
        int&         outReplyLen,
        const char*& outSessionKeyPtr,
        int&         outSessionKeyLen,
        const char*& outUserPrincipalStrPtr)
    {
        outReplyPtr            = 0;
        outReplyLen            = 0;
        outSessionKeyPtr       = 0;
        outSessionKeyLen       = 0;
        outUserPrincipalStrPtr = 0;
        if (! mInitedFlag) {
            mErrCode  = EINVAL;
            mErrorMsg = "not initialized yet, invoke KrbService::Init";
            return mErrorMsg.c_str();
        }
        if (! mAuthInitedFlag) {
            mErrCode  = EINVAL;
            mErrorMsg =
                "not ready to process reply, invoke KrbService::Request";
            return mErrorMsg.c_str();
        }
        if (mOutBuf.data || mKeyBlockPtr) {
            mErrCode  = EINVAL;
            mErrorMsg = "possible extraneous invocation of KrbClient::Reply";
            return mErrorMsg.c_str();
        }
        krb5_free_data_contents(mCtx, &mOutBuf);
        mErrCode = krb5_mk_rep(mCtx, mAuthCtx, &mOutBuf);
        if (! mErrCode) {
            if (mKeyBlockPtr) {
                krb5_free_keyblock(mCtx, mKeyBlockPtr);
                mKeyBlockPtr = 0;
            }
            mErrCode = krb5_auth_con_getkey(mCtx, mAuthCtx, &mKeyBlockPtr);
            krb5_authenticator* theAuthenticatorPtr = 0;
            if (! mErrCode) {
                mErrCode = krb5_auth_con_getauthenticator(
                    mCtx, mAuthCtx, &theAuthenticatorPtr);
            }
            if (! mErrCode && (! theAuthenticatorPtr ||
                    ! theAuthenticatorPtr->client)) {
                mErrCode = EINVAL;
            }
            if (! mErrCode) {
#if ! defined(KRB5_PRINCIPAL_UNPARSE_SHORT) && \
        ! defined(KRB5_PRINCIPAL_UNPARSE_NO_REALM) && \
        ! defined(KRB5_PRINCIPAL_UNPARSE_DISPLAY)
                // FIXME: make flags work with older versions.
                mErrCode = krb5_unparse_name_ext(
                    mCtx,
                    theAuthenticatorPtr->client,
                    &mUserPrincipalStrPtr,
                    &mUserPrincipalStrLen
                );
#else
                mErrCode = krb5_unparse_name_flags_ext(
                    mCtx,
                    theAuthenticatorPtr->client,
                    ((inPrincipalUnparseFlags & kPrincipalUnparseShort) != 0 ?
                        KRB5_PRINCIPAL_UNPARSE_SHORT : 0) |
                    ((inPrincipalUnparseFlags & kPrincipalUnparseNoRealm) != 0 ?
                        KRB5_PRINCIPAL_UNPARSE_NO_REALM : 0) |
                    ((inPrincipalUnparseFlags & kPrincipalUnparseDisplay) != 0 ?
                        KRB5_PRINCIPAL_UNPARSE_DISPLAY : 0),
                    &mUserPrincipalStrPtr,
                    &mUserPrincipalStrLen
                );
#endif
                if (! mErrCode && ! mUserPrincipalStrPtr) {
                    mErrCode = EINVAL;
                }
            }
            if (theAuthenticatorPtr) {
                krb5_free_authenticator(mCtx, theAuthenticatorPtr);
            }
            if (! mErrCode) {
                outReplyPtr      = reinterpret_cast<const char*>(mOutBuf.data);
                outReplyLen      = (int)mOutBuf.length;
                outSessionKeyPtr =
                    reinterpret_cast<const char*>(mKeyBlockPtr->contents);
                outSessionKeyLen = (int)mKeyBlockPtr->length;
                outUserPrincipalStrPtr = mUserPrincipalStrPtr;
                return 0;
            }
        }
        mErrorMsg = ErrToStr(mErrCode);
        return mErrorMsg.c_str();
    }
    int GetErrorCode() const
        { return mErrCode; }

private:
    string            mKeyTabFileName;
    krb5_context      mCtx;
    krb5_auth_context mAuthCtx;
    krb5_principal    mServerPtr;
    krb5_keytab       mKeyTabPtr;
    krb5_error_code   mErrCode;
    krb5_data         mOutBuf;
    krb5_keyblock*    mKeyBlockPtr;
    char*             mUserPrincipalStrPtr;
    unsigned int      mUserPrincipalStrLen;
    bool              mInitedFlag;
    bool              mAuthInitedFlag;
    bool              mDetectReplayFlag;
    string            mServiceName;
    string            mServiceHostName;
    string            mErrorMsg;

    void InitSelf()
    {
        mErrCode = krb5_init_context(&mCtx);
        if (mErrCode) {
            return;
        }
        mInitedFlag = true;
        mServerPtr  = 0;
	mErrCode = krb5_sname_to_principal(
            mCtx,
            mServiceHostName.empty() ? 0 : mServiceHostName.c_str(),
            mServiceName.c_str(),
            KRB5_NT_UNKNOWN, // KRB5_NT_SRV_HST,
            &mServerPtr);
        if (mErrCode) {
            return;
        }
        mKeyTabPtr = 0;
        mErrCode = mKeyTabFileName.empty() ?
            krb5_kt_default(mCtx, &mKeyTabPtr) :
            krb5_kt_resolve(mCtx, mKeyTabFileName.c_str(), &mKeyTabPtr);
    }
    void InitAuth()
    {
        InitAuthSelf();
        if (mErrCode) {
            CleanupAuth();
        }
    }
    void InitAuthSelf()
    {
        if (! mInitedFlag) {
            mErrCode = EINVAL;
            return;
        }
        mErrCode = krb5_auth_con_init(mCtx, &mAuthCtx);
        if (mErrCode) {
            return;
        }
        mAuthInitedFlag = true;
        krb5_int32 theFlags = 0;
        mErrCode = krb5_auth_con_getflags(mCtx, mAuthCtx, &theFlags);
        if (mErrCode) {
            return;
        }
        // theFlags |= KRB5_AUTH_CONTEXT_DO_SEQUENCE;
        if (! mDetectReplayFlag) {
            theFlags &=
                ~(KRB5_AUTH_CONTEXT_DO_TIME | KRB5_AUTH_CONTEXT_RET_TIME);
        }
        mErrCode = krb5_auth_con_setflags(mCtx, mAuthCtx, theFlags);
        if (mDetectReplayFlag) {
            krb5_rcache theRCachePtr = 0;
            mErrCode = krb5_auth_con_getrcache(mCtx, mAuthCtx, &theRCachePtr);
            if (mErrCode) {
                return;
            }
	    if (! theRCachePtr)  {
                mErrCode = krb5_get_server_rcache(
                    mCtx,
                    krb5_princ_component(mCtx, mServerPtr, 0),
                    &theRCachePtr
                );
                if (mErrCode) {
                    return;
                }
            }
            mErrCode = krb5_auth_con_setrcache(mCtx, mAuthCtx, theRCachePtr);
        }
        if (mErrCode) {
            return;
        }
    }
    krb5_error_code CleanupSelf()
    {
        if (! mInitedFlag) {
            return 0;
        }
        krb5_error_code theErr = CleanupAuth();
        mInitedFlag = false;
        if (mKeyTabPtr) {
            krb5_error_code const theCloseErr = krb5_kt_close(mCtx, mKeyTabPtr);
            mKeyTabPtr = 0;
            if (! theErr && theCloseErr) {
                theErr = theCloseErr;
            }
        }
	if (mServerPtr) {
            krb5_free_principal(mCtx, mServerPtr);
            mServerPtr = 0;
        }
        krb5_free_data_contents(mCtx, &mOutBuf);
        if (mUserPrincipalStrPtr) {
            krb5_free_unparsed_name(mCtx, mUserPrincipalStrPtr);
            mUserPrincipalStrPtr = 0;
            mUserPrincipalStrLen = 0;
        }
        krb5_free_context(mCtx);
        return theErr;
    }
    krb5_error_code CleanupAuth()
    {
        if (! mInitedFlag || ! mAuthInitedFlag) {
            return 0;
        }
        if (mKeyBlockPtr) {
            krb5_free_keyblock(mCtx, mKeyBlockPtr);
            mKeyBlockPtr = 0;
        }
        mAuthInitedFlag = false;
        return krb5_auth_con_free(mCtx, mAuthCtx);
    }
    string ErrToStr(
        krb5_error_code inErrCode) const
    {
        if (! inErrCode) {
            return string();
        }
        if ( ! mCtx) {
            return string("no kerberos context");
        }
        const char* const theMsgPtr = krb5_get_error_message(mCtx, inErrCode);
        const string theMsg((theMsgPtr && *theMsgPtr) ?
            theMsgPtr : "unspecified kerberos error");
        if (theMsgPtr) {
            // cast away const to make it compatible with older krb5 releases.
            krb5_free_error_message(mCtx, const_cast<char*>(theMsgPtr));
        }
        return theMsg;
    }
};

KrbService::KrbService()
    : mImpl(*(new Impl()))
{
}

KrbService::~KrbService()
{
    delete &mImpl;
}

    const char*
KrbService::Init(
    const char* inServiceHostNamePtr,
    const char* inServeiceNamePtr,
    const char* inKeyTabNamePtr,
    bool        inDetectReplayFlag)
{
    return mImpl.Init(
        inServiceHostNamePtr,
        inServeiceNamePtr,
        inKeyTabNamePtr,
        inDetectReplayFlag
    );
}

    const char*
KrbService::Cleanup()
{
    return mImpl.Cleanup();
}

    const char*
KrbService::Request(
    const char* inDataPtr,
    int         inDataLen)
{
    return mImpl.Request(inDataPtr, inDataLen);
}

    const char*
KrbService::Reply(
    int          inPrincipalUnparseFlags,
    const char*& outReplyPtr,
    int&         outReplyLen,
    const char*& outSessionKeyPtr,
    int&         outSessionKeyLen,
    const char*& outUserPrincipalPtr)
{
    return mImpl.Reply(
        inPrincipalUnparseFlags,
        outReplyPtr,
        outReplyLen,
        outSessionKeyPtr,
        outSessionKeyLen,
        outUserPrincipalPtr
    );
}

    int
KrbService::GetErrorCode() const
{
    return mImpl.GetErrorCode();
}

}
