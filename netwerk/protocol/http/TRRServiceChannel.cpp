/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TRRServiceChannel.h"

#include "HttpLog.h"
#include "mozilla/StaticPrefs_network.h"
#include "nsDNSPrefetch.h"
#include "nsEscape.h"
#include "nsHttpTransaction.h"
#include "nsICancelable.h"
#include "nsIHttpPushListener.h"
#include "nsIProtocolProxyService2.h"
#include "nsIOService.h"
#include "nsIURIFixup.h"
#include "nsISeekableStream.h"
#include "nsURLHelper.h"
#include "ReferrerInfo.h"

namespace mozilla {
namespace net {

NS_IMPL_ADDREF(TRRServiceChannel)
NS_IMPL_RELEASE(TRRServiceChannel)

NS_INTERFACE_MAP_BEGIN(TRRServiceChannel)
  NS_INTERFACE_MAP_ENTRY(nsIRequest)
  NS_INTERFACE_MAP_ENTRY(nsIChannel)
  NS_INTERFACE_MAP_ENTRY(nsIHttpChannel)
  NS_INTERFACE_MAP_ENTRY(nsIHttpChannelInternal)
  NS_INTERFACE_MAP_ENTRY(nsISupportsPriority)
  NS_INTERFACE_MAP_ENTRY(nsIClassOfService)
  NS_INTERFACE_MAP_ENTRY(nsIProxiedChannel)
  NS_INTERFACE_MAP_ENTRY(nsIProtocolProxyCallback)
  NS_INTERFACE_MAP_ENTRY(nsIStreamListener)
  NS_INTERFACE_MAP_ENTRY(nsIRequestObserver)
  NS_INTERFACE_MAP_ENTRY(nsITransportEventSink)
  NS_INTERFACE_MAP_ENTRY(nsIDNSListener)
  NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
  NS_INTERFACE_MAP_ENTRY_CONCRETE(TRRServiceChannel)
NS_INTERFACE_MAP_END_INHERITING(HttpBaseChannel)

TRRServiceChannel::TRRServiceChannel()
    : HttpAsyncAborter<TRRServiceChannel>(this),
      mTopWindowOriginComputed(false),
      mPushedStreamId(0),
      mCurrentEventTarget(GetCurrentThreadEventTarget()) {
  LOG(("TRRServiceChannel ctor [this=%p]\n", this));
}

TRRServiceChannel::~TRRServiceChannel() {
  LOG(("TRRServiceChannel dtor [this=%p]\n", this));
}

NS_IMETHODIMP
TRRServiceChannel::Cancel(nsresult status) {
  LOG(("TRRServiceChannel::Cancel [this=%p status=%" PRIx32 "]\n", this,
       static_cast<uint32_t>(status)));
  if (mCanceled) {
    LOG(("  ignoring; already canceled\n"));
    return NS_OK;
  }

  mCanceled = true;
  mStatus = status;
  if (mProxyRequest) {
    nsCOMPtr<nsICancelable> proxyRequet;
    proxyRequet.swap(mProxyRequest);
    NS_DispatchToMainThread(
        NS_NewRunnableFunction(
            "CancelProxyRequest",
            [proxyRequet, status]() { proxyRequet->Cancel(status); }),
        NS_DISPATCH_NORMAL);
  }
  CancelNetworkRequest(status);
  return NS_OK;
}

void TRRServiceChannel::CancelNetworkRequest(nsresult aStatus) {
  if (mTransaction) {
    nsresult rv = gHttpHandler->CancelTransaction(mTransaction, aStatus);
    if (NS_FAILED(rv)) {
      LOG(("failed to cancel the transaction\n"));
    }
  }
  if (mTransactionPump) mTransactionPump->Cancel(aStatus);
}

NS_IMETHODIMP
TRRServiceChannel::Suspend() {
  LOG(("TRRServiceChannel::SuspendInternal [this=%p]\n", this));

  if (mTransactionPump) {
    return mTransactionPump->Suspend();
  }

  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::Resume() {
  LOG(("TRRServiceChannel::Resume [this=%p]\n", this));

  if (mTransactionPump) {
    return mTransactionPump->Resume();
  }

  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetSecurityInfo(nsISupports** securityInfo) {
  NS_ENSURE_ARG_POINTER(securityInfo);
  *securityInfo = mSecurityInfo;
  NS_IF_ADDREF(*securityInfo);
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::AsyncOpen(nsIStreamListener* aListener) {
  NS_ENSURE_ARG_POINTER(aListener);
  NS_ENSURE_TRUE(!mIsPending, NS_ERROR_IN_PROGRESS);
  NS_ENSURE_TRUE(!mWasOpened, NS_ERROR_ALREADY_OPENED);

  if (mCanceled) {
    ReleaseListeners();
    return mStatus;
  }

  // HttpBaseChannel::MaybeWaitForUploadStreamLength can only be used on main
  // thread, so we can only return an error here.
#ifdef NIGHTLY_BUILD
  MOZ_ASSERT(!mPendingInputStreamLengthOperation);
#endif
  if (mPendingInputStreamLengthOperation) {
    return NS_ERROR_FAILURE;
  }

  if (!gHttpHandler->Active()) {
    LOG(("  after HTTP shutdown..."));
    ReleaseListeners();
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsresult rv = NS_CheckPortSafety(mURI);
  if (NS_FAILED(rv)) {
    ReleaseListeners();
    return rv;
  }

  mIsPending = true;
  mWasOpened = true;

  mListener = aListener;

  mAsyncOpenTime = TimeStamp::Now();

  rv = MaybeResolveProxyAndBeginConnect();
  if (NS_FAILED(rv)) {
    Unused << AsyncAbort(rv);
  }

  return NS_OK;
}

nsresult TRRServiceChannel::MaybeResolveProxyAndBeginConnect() {
  nsresult rv;

  // The common case for HTTP channels is to begin proxy resolution and return
  // at this point. The only time we know mProxyInfo already is if we're
  // proxying a non-http protocol like ftp. We don't need to discover proxy
  // settings if we are never going to make a network connection.
  if (!mProxyInfo &&
      !(mLoadFlags & (nsICachingChannel::LOAD_ONLY_FROM_CACHE |
                      nsICachingChannel::LOAD_NO_NETWORK_IO)) &&
      NS_SUCCEEDED(ResolveProxy())) {
    return NS_OK;
  }

  rv = BeginConnect();
  if (NS_FAILED(rv)) {
    Unused << AsyncAbort(rv);
  }

  return NS_OK;
}

nsresult TRRServiceChannel::ResolveProxy() {
  LOG(("TRRServiceChannel::ResolveProxy [this=%p]\n", this));
  if (!NS_IsMainThread()) {
    return NS_DispatchToMainThread(
        NewRunnableMethod("TRRServiceChannel::ResolveProxy", this,
                          &TRRServiceChannel::ResolveProxy),
        NS_DISPATCH_NORMAL);
  }

  MOZ_ASSERT(NS_IsMainThread());

  nsresult rv;
  nsCOMPtr<nsIChannel> channel;
  rv = NS_NewChannel(getter_AddRefs(channel), mURI,
                     nsContentUtils::GetSystemPrincipal(),
                     nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
                     nsIContentPolicy::TYPE_OTHER);
  if (NS_SUCCEEDED(rv)) {
    nsCOMPtr<nsIProtocolProxyService> pps =
        do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
      // using the nsIProtocolProxyService2 allows a minor performance
      // optimization, but if an add-on has only provided the original interface
      // then it is ok to use that version.
      nsCOMPtr<nsIProtocolProxyService2> pps2 = do_QueryInterface(pps);
      if (pps2) {
        rv = pps2->AsyncResolve2(channel, mProxyResolveFlags, this, nullptr,
                                 getter_AddRefs(mProxyRequest));
      } else {
        rv = pps->AsyncResolve(channel, mProxyResolveFlags, this, nullptr,
                               getter_AddRefs(mProxyRequest));
      }
    }
  }

  if (NS_FAILED(rv)) {
    if (!mCurrentEventTarget->IsOnCurrentThread()) {
      mCurrentEventTarget->Dispatch(
          NewRunnableMethod<nsresult>("TRRServiceChannel::AsyncAbort", this,
                                      &TRRServiceChannel::AsyncAbort, rv),
          NS_DISPATCH_NORMAL);
    }
  }

  return rv;
}

NS_IMETHODIMP
TRRServiceChannel::OnProxyAvailable(nsICancelable* request, nsIChannel* channel,
                                    nsIProxyInfo* pi, nsresult status) {
  LOG(("TRRServiceChannel::OnProxyAvailable [this=%p pi=%p status=%" PRIx32
       " mStatus=%" PRIx32 "]\n",
       this, pi, static_cast<uint32_t>(status),
       static_cast<uint32_t>(static_cast<nsresult>(mStatus))));

  if (!mCurrentEventTarget->IsOnCurrentThread()) {
    RefPtr<TRRServiceChannel> self = this;
    nsCOMPtr<nsIProxyInfo> info = pi;
    return mCurrentEventTarget->Dispatch(
        NS_NewRunnableFunction("TRRServiceChannel::OnProxyAvailable",
                               [self, info, status]() {
                                 self->OnProxyAvailable(nullptr, nullptr, info,
                                                        status);
                               }),
        NS_DISPATCH_NORMAL);
  }

  MOZ_ASSERT(mCurrentEventTarget->IsOnCurrentThread());

  mProxyRequest = nullptr;

  nsresult rv;

  // If status is a failure code, then it means that we failed to resolve
  // proxy info.  That is a non-fatal error assuming it wasn't because the
  // request was canceled.  We just failover to DIRECT when proxy resolution
  // fails (failure can mean that the PAC URL could not be loaded).

  if (NS_SUCCEEDED(status)) mProxyInfo = pi;

  if (!gHttpHandler->Active()) {
    LOG(
        ("nsHttpChannel::OnProxyAvailable [this=%p] "
         "Handler no longer active.\n",
         this));
    rv = NS_ERROR_NOT_AVAILABLE;
  } else {
    rv = BeginConnect();
  }

  if (NS_FAILED(rv)) {
    Unused << AsyncAbort(rv);
  }
  return rv;
}

const nsCString& TRRServiceChannel::GetTopWindowOrigin() {
  if (mTopWindowOriginComputed) {
    return mTopWindowOrigin;
  }

  nsresult rv = nsContentUtils::GetASCIIOrigin(mURI, mTopWindowOrigin);
  NS_ENSURE_SUCCESS(rv, mTopWindowOrigin);

  mTopWindowOriginComputed = true;
  return mTopWindowOrigin;
}

nsresult TRRServiceChannel::BeginConnect() {
  LOG(("TRRServiceChannel::BeginConnect [this=%p]\n", this));
  nsresult rv;

  // Construct connection info object
  nsAutoCString host;
  nsAutoCString scheme;
  int32_t port = -1;
  bool isHttps = mURI->SchemeIs("https");

  rv = mURI->GetScheme(scheme);
  if (NS_SUCCEEDED(rv)) rv = mURI->GetAsciiHost(host);
  if (NS_SUCCEEDED(rv)) rv = mURI->GetPort(&port);
  if (NS_SUCCEEDED(rv)) mURI->GetUsername(mUsername);
  if (NS_SUCCEEDED(rv)) rv = mURI->GetAsciiSpec(mSpec);
  if (NS_FAILED(rv)) {
    return rv;
  }

  // Reject the URL if it doesn't specify a host
  if (host.IsEmpty()) {
    rv = NS_ERROR_MALFORMED_URI;
    return rv;
  }
  LOG(("host=%s port=%d\n", host.get(), port));
  LOG(("uri=%s\n", mSpec.get()));

  nsCOMPtr<nsProxyInfo> proxyInfo;
  if (mProxyInfo) proxyInfo = do_QueryInterface(mProxyInfo);

  mRequestHead.SetHTTPS(isHttps);
  mRequestHead.SetOrigin(scheme, host, port);

  RefPtr<nsHttpConnectionInfo> connInfo = new nsHttpConnectionInfo(
      host, port, EmptyCString(), mUsername, GetTopWindowOrigin(), proxyInfo,
      OriginAttributes(), isHttps);
  mAllowAltSvc = (mAllowAltSvc && !gHttpHandler->IsSpdyBlacklisted(connInfo));

  RefPtr<AltSvcMapping> mapping;
  if (!mConnectionInfo && mAllowAltSvc &&  // per channel
      !(mLoadFlags & LOAD_FRESH_CONNECTION) &&
      AltSvcMapping::AcceptableProxy(proxyInfo) &&
      (scheme.EqualsLiteral("http") || scheme.EqualsLiteral("https")) &&
      (mapping = gHttpHandler->GetAltServiceMapping(
           scheme, host, port, mPrivateBrowsing, IsIsolated(),
           GetTopWindowOrigin(), OriginAttributes()))) {
    LOG(("TRRServiceChannel %p Alt Service Mapping Found %s://%s:%d [%s]\n",
         this, scheme.get(), mapping->AlternateHost().get(),
         mapping->AlternatePort(), mapping->HashKey().get()));

    if (!(mLoadFlags & LOAD_ANONYMOUS) && !mPrivateBrowsing) {
      nsAutoCString altUsedLine(mapping->AlternateHost());
      bool defaultPort =
          mapping->AlternatePort() ==
          (isHttps ? NS_HTTPS_DEFAULT_PORT : NS_HTTP_DEFAULT_PORT);
      if (!defaultPort) {
        altUsedLine.AppendLiteral(":");
        altUsedLine.AppendInt(mapping->AlternatePort());
      }
      rv = mRequestHead.SetHeader(nsHttp::Alternate_Service_Used, altUsedLine);
      MOZ_ASSERT(NS_SUCCEEDED(rv));
    }

    LOG(("TRRServiceChannel %p Using connection info from altsvc mapping",
         this));
    mapping->GetConnectionInfo(getter_AddRefs(mConnectionInfo), proxyInfo,
                               OriginAttributes());
    Telemetry::Accumulate(Telemetry::HTTP_TRANSACTION_USE_ALTSVC, true);
    Telemetry::Accumulate(Telemetry::HTTP_TRANSACTION_USE_ALTSVC_OE, !isHttps);
  } else if (mConnectionInfo) {
    LOG(("TRRServiceChannel %p Using channel supplied connection info", this));
    Telemetry::Accumulate(Telemetry::HTTP_TRANSACTION_USE_ALTSVC, false);
  } else {
    LOG(("TRRServiceChannel %p Using default connection info", this));

    mConnectionInfo = connInfo;
    Telemetry::Accumulate(Telemetry::HTTP_TRANSACTION_USE_ALTSVC, false);
  }

  // Need to re-ask the handler, since mConnectionInfo may not be the connInfo
  // we used earlier
  if (gHttpHandler->IsSpdyBlacklisted(mConnectionInfo)) {
    mAllowSpdy = 0;
    mCaps |= NS_HTTP_DISALLOW_SPDY;
    mConnectionInfo->SetNoSpdy(true);
  }

  // If mTimingEnabled flag is not set after OnModifyRequest() then
  // clear the already recorded AsyncOpen value for consistency.
  if (!mTimingEnabled) mAsyncOpenTime = TimeStamp();

  // if this somehow fails we can go on without it
  Unused << gHttpHandler->AddConnectionHeader(&mRequestHead, mCaps);

  // Adjust mCaps according to our request headers:
  //  - If "Connection: close" is set as a request header, then do not bother
  //    trying to establish a keep-alive connection.
  if (mRequestHead.HasHeaderValue(nsHttp::Connection, "close"))
    mCaps &= ~(NS_HTTP_ALLOW_KEEPALIVE);

  if (gHttpHandler->CriticalRequestPrioritization()) {
    if (mClassOfService & nsIClassOfService::Leader) {
      mCaps |= NS_HTTP_LOAD_AS_BLOCKING;
    }
    if (mClassOfService & nsIClassOfService::Unblocked) {
      mCaps |= NS_HTTP_LOAD_UNBLOCKED;
    }
    if (mClassOfService & nsIClassOfService::UrgentStart &&
        gHttpHandler->IsUrgentStartEnabled()) {
      mCaps |= NS_HTTP_URGENT_START;
      SetPriority(nsISupportsPriority::PRIORITY_HIGHEST);
    }
  }

  // Force-Reload should reset the persistent connection pool for this host
  if (mLoadFlags & LOAD_FRESH_CONNECTION) {
    // just the initial document resets the whole pool
    if (mLoadFlags & LOAD_INITIAL_DOCUMENT_URI) {
      gHttpHandler->AltServiceCache()->ClearAltServiceMappings();
      rv = gHttpHandler->ConnMgr()->DoShiftReloadConnectionCleanup(
          mConnectionInfo);
      if (NS_FAILED(rv)) {
        LOG(
            ("TRRServiceChannel::BeginConnect "
             "DoShiftReloadConnectionCleanup failed: %08x [this=%p]",
             static_cast<uint32_t>(rv), this));
      }
    }
  }

  if (mCanceled) {
    return mStatus;
  }

  MaybeStartDNSPrefetch();

  rv = ContinueOnBeforeConnect();
  if (NS_FAILED(rv)) {
    return rv;
  }

  return NS_OK;
}

nsresult TRRServiceChannel::ContinueOnBeforeConnect() {
  LOG(("TRRServiceChannel::ContinueOnBeforeConnect [this=%p]\n", this));

  // ensure that we are using a valid hostname
  if (!net_IsValidHostName(nsDependentCString(mConnectionInfo->Origin())))
    return NS_ERROR_UNKNOWN_HOST;

  if (mIsTRRServiceChannel) {
    mCaps |= NS_HTTP_LARGE_KEEPALIVE;
  }

  mCaps |= NS_HTTP_TRR_FLAGS_FROM_MODE(nsIRequest::GetTRRMode());

  // Finalize ConnectionInfo flags before SpeculativeConnect
  mConnectionInfo->SetAnonymous((mLoadFlags & LOAD_ANONYMOUS) != 0);
  mConnectionInfo->SetPrivate(mPrivateBrowsing);
  mConnectionInfo->SetIsolated(IsIsolated());
  mConnectionInfo->SetNoSpdy(mCaps & NS_HTTP_DISALLOW_SPDY);
  mConnectionInfo->SetBeConservative((mCaps & NS_HTTP_BE_CONSERVATIVE) ||
                                     mBeConservative);
  mConnectionInfo->SetTlsFlags(mTlsFlags);
  mConnectionInfo->SetIsTrrServiceChannel(mIsTRRServiceChannel);
  mConnectionInfo->SetTRRMode(nsIRequest::GetTRRMode());
  mConnectionInfo->SetIPv4Disabled(mCaps & NS_HTTP_DISABLE_IPV4);
  mConnectionInfo->SetIPv6Disabled(mCaps & NS_HTTP_DISABLE_IPV6);

  return Connect();
}

nsresult TRRServiceChannel::Connect() {
  LOG(("TRRServiceChannel::Connect [this=%p]\n", this));

  nsresult rv = SetupTransaction();
  if (NS_FAILED(rv)) {
    return rv;
  }

  rv = gHttpHandler->InitiateTransaction(mTransaction, mPriority);
  if (NS_FAILED(rv)) {
    return rv;
  }

  return mTransaction->AsyncRead(this, getter_AddRefs(mTransactionPump));
}

nsresult TRRServiceChannel::SetupTransaction() {
  LOG(("TRRServiceChannel::SetupTransaction [this=%p, cos=%u, prio=%d]\n", this,
       mClassOfService, mPriority));

  NS_ENSURE_TRUE(!mTransaction, NS_ERROR_ALREADY_INITIALIZED);

  nsresult rv;

  if (!mAllowSpdy) {
    mCaps |= NS_HTTP_DISALLOW_SPDY;
  }
  if (mBeConservative) {
    mCaps |= NS_HTTP_BE_CONSERVATIVE;
  }

  // Use the URI path if not proxying (transparent proxying such as proxy
  // CONNECT does not count here). Also figure out what HTTP version to use.
  nsAutoCString buf, path;
  nsCString* requestURI;

  // This is the normal e2e H1 path syntax "/index.html"
  rv = mURI->GetPathQueryRef(path);
  if (NS_FAILED(rv)) {
    return rv;
  }

  // path may contain UTF-8 characters, so ensure that they're escaped.
  if (NS_EscapeURL(path.get(), path.Length(), esc_OnlyNonASCII | esc_Spaces,
                   buf)) {
    requestURI = &buf;
  } else {
    requestURI = &path;
  }

  // trim off the #ref portion if any...
  int32_t ref1 = requestURI->FindChar('#');
  if (ref1 != kNotFound) {
    requestURI->SetLength(ref1);
  }

  if (mConnectionInfo->UsingConnect() || !mConnectionInfo->UsingHttpProxy()) {
    mRequestHead.SetVersion(gHttpHandler->HttpVersion());
  } else {
    mRequestHead.SetPath(*requestURI);

    // RequestURI should be the absolute uri H1 proxy syntax
    // "http://foo/index.html" so we will overwrite the relative version in
    // requestURI
    rv = mURI->GetUserPass(buf);
    if (NS_FAILED(rv)) return rv;
    if (!buf.IsEmpty() && ((strncmp(mSpec.get(), "http:", 5) == 0) ||
                           strncmp(mSpec.get(), "https:", 6) == 0)) {
      nsCOMPtr<nsIURIFixup> urifixup = services::GetURIFixup();
      if (NS_WARN_IF(!urifixup)) {
        return NS_ERROR_FAILURE;
      }

      nsCOMPtr<nsIURI> tempURI;
      nsresult rv = urifixup->CreateExposableURI(mURI, getter_AddRefs(tempURI));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      rv = tempURI->GetAsciiSpec(path);
      if (NS_FAILED(rv)) return rv;
      requestURI = &path;
    } else {
      requestURI = &mSpec;
    }

    // trim off the #ref portion if any...
    int32_t ref2 = requestURI->FindChar('#');
    if (ref2 != kNotFound) {
      requestURI->SetLength(ref2);
    }

    mRequestHead.SetVersion(gHttpHandler->ProxyHttpVersion());
  }

  mRequestHead.SetRequestURI(*requestURI);

  // Force setting no-cache header for TRRServiceChannel.
  // We need to send 'Pragma:no-cache' to inhibit proxy caching even if
  // no proxy is configured since we might be talking with a transparent
  // proxy, i.e. one that operates at the network level.  See bug #14772.
  rv = mRequestHead.SetHeaderOnce(nsHttp::Pragma, "no-cache", true);
  MOZ_ASSERT(NS_SUCCEEDED(rv));
  // If we're configured to speak HTTP/1.1 then also send 'Cache-control:
  // no-cache'
  if (mRequestHead.Version() >= HttpVersion::v1_1) {
    rv = mRequestHead.SetHeaderOnce(nsHttp::Cache_Control, "no-cache", true);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }

  // create wrapper for this channel's notification callbacks
  nsCOMPtr<nsIInterfaceRequestor> callbacks;
  NS_NewNotificationCallbacksAggregation(mCallbacks, mLoadGroup,
                                         getter_AddRefs(callbacks));

  // create the transaction object
  mTransaction = new nsHttpTransaction();
  LOG1(("TRRServiceChannel %p created nsHttpTransaction %p\n", this,
        mTransaction.get()));

  // See bug #466080. Transfer LOAD_ANONYMOUS flag to socket-layer.
  if (mLoadFlags & LOAD_ANONYMOUS) mCaps |= NS_HTTP_LOAD_ANONYMOUS;

  if (mLoadFlags & LOAD_CALL_CONTENT_SNIFFERS) {
    mCaps |= NS_HTTP_CALL_CONTENT_SNIFFER;
  }

  if (mTimingEnabled) mCaps |= NS_HTTP_TIMING_ENABLED;

  nsCOMPtr<nsIHttpPushListener> pushListener;
  NS_QueryNotificationCallbacks(mCallbacks, mLoadGroup,
                                NS_GET_IID(nsIHttpPushListener),
                                getter_AddRefs(pushListener));
  HttpTransactionShell::OnPushCallback pushCallback = nullptr;
  if (pushListener) {
    mCaps |= NS_HTTP_ONPUSH_LISTENER;
    nsWeakPtr weakPtrThis(
        do_GetWeakReference(static_cast<nsIHttpChannel*>(this)));
    pushCallback = [weakPtrThis](uint32_t aPushedStreamId,
                                 const nsACString& aUrl,
                                 const nsACString& aRequestString,
                                 HttpTransactionShell* aTransaction) {
      if (nsCOMPtr<nsIHttpChannel> channel = do_QueryReferent(weakPtrThis)) {
        return static_cast<TRRServiceChannel*>(channel.get())
            ->OnPush(aPushedStreamId, aUrl, aRequestString, aTransaction);
      }
      return NS_ERROR_NOT_AVAILABLE;
    };
  }

  EnsureRequestContext();

  rv = mTransaction->Init(
      mCaps, mConnectionInfo, &mRequestHead, mUploadStream, mReqContentLength,
      mUploadStreamHasHeaders, mCurrentEventTarget, callbacks, this,
      mTopLevelOuterContentWindowId, HttpTrafficCategory::eInvalid,
      mRequestContext, mClassOfService, mInitialRwin, mResponseTimeoutEnabled,
      mChannelId, nullptr, std::move(pushCallback), mTransWithPushedStream,
      mPushedStreamId);

  mTransWithPushedStream = nullptr;

  if (NS_FAILED(rv)) {
    mTransaction = nullptr;
    return rv;
  }

  return rv;
}

void TRRServiceChannel::SetPushedStreamTransactionAndId(
    HttpTransactionShell* aTransWithPushedStream, uint32_t aPushedStreamId) {
  MOZ_ASSERT(!mTransWithPushedStream);
  LOG(("TRRServiceChannel::SetPushedStreamTransaction [this=%p] trans=%p", this,
       aTransWithPushedStream));

  mTransWithPushedStream = aTransWithPushedStream;
  mPushedStreamId = aPushedStreamId;
}

nsresult TRRServiceChannel::OnPush(uint32_t aPushedStreamId,
                                   const nsACString& aUrl,
                                   const nsACString& aRequestString,
                                   HttpTransactionShell* aTransaction) {
  MOZ_ASSERT(aTransaction);
  LOG(("TRRServiceChannel::OnPush [this=%p, trans=%p]\n", this, aTransaction));

  MOZ_ASSERT(mCaps & NS_HTTP_ONPUSH_LISTENER);
  nsCOMPtr<nsIHttpPushListener> pushListener;
  NS_QueryNotificationCallbacks(mCallbacks, mLoadGroup,
                                NS_GET_IID(nsIHttpPushListener),
                                getter_AddRefs(pushListener));

  if (!pushListener) {
    LOG(
        ("TRRServiceChannel::OnPush [this=%p] notification callbacks do not "
         "implement nsIHttpPushListener\n",
         this));
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsCOMPtr<nsIURI> pushResource;
  nsresult rv;

  // Create a Channel for the Push Resource
  rv = NS_NewURI(getter_AddRefs(pushResource), aUrl);
  if (NS_FAILED(rv)) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIChannel> pushHttpChannel;
  rv = gHttpHandler->CreateTRRServiceChannel(pushResource, nullptr, 0, nullptr,
                                             nullptr,
                                             getter_AddRefs(pushHttpChannel));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = pushHttpChannel->SetLoadFlags(mLoadFlags);
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<TRRServiceChannel> channel;
  CallQueryInterface(pushHttpChannel, channel.StartAssignment());
  MOZ_ASSERT(channel);
  if (!channel) {
    return NS_ERROR_UNEXPECTED;
  }

  // new channel needs mrqeuesthead and headers from pushedStream
  channel->mRequestHead.ParseHeaderSet(aRequestString.BeginReading());
  channel->mLoadGroup = mLoadGroup;
  channel->mCallbacks = mCallbacks;

  // Link the pushed stream with the new channel and call listener
  channel->SetPushedStreamTransactionAndId(aTransaction, aPushedStreamId);
  rv = pushListener->OnPush(this, channel);
  return rv;
}

void TRRServiceChannel::MaybeStartDNSPrefetch() {
  if (mConnectionInfo->UsingHttpProxy() ||
      (mLoadFlags & (nsICachingChannel::LOAD_NO_NETWORK_IO |
                     nsICachingChannel::LOAD_ONLY_FROM_CACHE))) {
    return;
  }

  LOG(
      ("TRRServiceChannel::MaybeStartDNSPrefetch [this=%p] "
       "prefetching%s\n",
       this, mCaps & NS_HTTP_REFRESH_DNS ? ", refresh requested" : ""));

  OriginAttributes originAttributes;
  mDNSPrefetch = new nsDNSPrefetch(
      mURI, originAttributes, nsIRequest::GetTRRMode(), this, mTimingEnabled);
  mDNSPrefetch->PrefetchHigh(mCaps & NS_HTTP_REFRESH_DNS);
}

NS_IMETHODIMP
TRRServiceChannel::OnTransportStatus(nsITransport* trans, nsresult status,
                                     int64_t progress, int64_t progressMax) {
  return NS_OK;
}

nsresult TRRServiceChannel::CallOnStartRequest() {
  LOG(("TRRServiceChannel::CallOnStartRequest [this=%p]", this));

  if (mOnStartRequestCalled) {
    LOG(("CallOnStartRequest already invoked before"));
    return mStatus;
  }

  nsresult rv = NS_OK;
  mTracingEnabled = false;

  // Ensure mListener->OnStartRequest will be invoked before exiting
  // this function.
  auto onStartGuard = MakeScopeExit([&] {
    LOG(
        ("  calling mListener->OnStartRequest by ScopeExit [this=%p, "
         "listener=%p]\n",
         this, mListener.get()));
    MOZ_ASSERT(!mOnStartRequestCalled);

    if (mListener) {
      nsCOMPtr<nsIStreamListener> deleteProtector(mListener);
      mOnStartRequestCalled = true;
      deleteProtector->OnStartRequest(this);
    }
    mOnStartRequestCalled = true;
  });

  if (mResponseHead && !mResponseHead->HasContentCharset())
    mResponseHead->SetContentCharset(mContentCharsetHint);

  LOG(("  calling mListener->OnStartRequest [this=%p, listener=%p]\n", this,
       mListener.get()));

  // About to call OnStartRequest, dismiss the guard object.
  onStartGuard.release();

  if (mListener) {
    MOZ_ASSERT(!mOnStartRequestCalled,
               "We should not call OsStartRequest twice");
    nsCOMPtr<nsIStreamListener> deleteProtector(mListener);
    mOnStartRequestCalled = true;
    rv = deleteProtector->OnStartRequest(this);
    if (NS_FAILED(rv)) return rv;
  } else {
    NS_WARNING("OnStartRequest skipped because of null listener");
    mOnStartRequestCalled = true;
  }

  return NS_OK;
}

void TRRServiceChannel::ProcessAltService() {
  // e.g. Alt-Svc: h2=":443"; ma=60
  // e.g. Alt-Svc: h2="otherhost:443"
  // Alt-Svc       = 1#( alternative *( OWS ";" OWS parameter ) )
  // alternative   = protocol-id "=" alt-authority
  // protocol-id   = token ; percent-encoded ALPN protocol identifier
  // alt-authority = quoted-string ;  containing [ uri-host ] ":" port

  if (!mAllowAltSvc) {  // per channel opt out
    return;
  }

  if (!gHttpHandler->AllowAltSvc() || (mCaps & NS_HTTP_DISALLOW_SPDY)) {
    return;
  }

  nsAutoCString scheme;
  mURI->GetScheme(scheme);
  bool isHttp = scheme.EqualsLiteral("http");
  if (!isHttp && !scheme.EqualsLiteral("https")) {
    return;
  }

  nsAutoCString altSvc;
  Unused << mResponseHead->GetHeader(nsHttp::Alternate_Service, altSvc);
  if (altSvc.IsEmpty()) {
    return;
  }

  if (!nsHttp::IsReasonableHeaderValue(altSvc)) {
    LOG(("Alt-Svc Response Header seems unreasonable - skipping\n"));
    return;
  }

  nsAutoCString originHost;
  int32_t originPort = 80;
  mURI->GetPort(&originPort);
  if (NS_FAILED(mURI->GetAsciiHost(originHost))) {
    return;
  }

  nsCOMPtr<nsIInterfaceRequestor> callbacks;
  nsCOMPtr<nsProxyInfo> proxyInfo;
  NS_NewNotificationCallbacksAggregation(mCallbacks, mLoadGroup,
                                         getter_AddRefs(callbacks));
  if (mProxyInfo) {
    proxyInfo = do_QueryInterface(mProxyInfo);
  }

  AltSvcMapping::ProcessHeader(
      altSvc, scheme, originHost, originPort, mUsername, GetTopWindowOrigin(),
      mPrivateBrowsing, IsIsolated(), callbacks, proxyInfo,
      mCaps & NS_HTTP_DISALLOW_SPDY, OriginAttributes());
}

NS_IMETHODIMP
TRRServiceChannel::OnStartRequest(nsIRequest* request) {
  LOG(("TRRServiceChannel::OnStartRequest [this=%p request=%p status=%" PRIx32
       "]\n",
       this, request, static_cast<uint32_t>(static_cast<nsresult>(mStatus))));

  if (!(mCanceled || NS_FAILED(mStatus))) {
    // capture the request's status, so our consumers will know ASAP of any
    // connection failures, etc - bug 93581
    nsresult status;
    request->GetStatus(&status);
    mStatus = status;
  }

  MOZ_ASSERT(request == mTransactionPump, "Unexpected request");

  mAfterOnStartRequestBegun = true;
  if (mTransaction) {
    if (!mSecurityInfo) {
      // grab the security info from the connection object; the transaction
      // is guaranteed to own a reference to the connection.
      mSecurityInfo = mTransaction->SecurityInfo();
    }
  }

  if (NS_SUCCEEDED(mStatus) && mTransaction) {
    // mTransactionPump doesn't hit OnInputStreamReady and call this until
    // all of the response headers have been acquired, so we can take
    // ownership of them from the transaction.
    mResponseHead = mTransaction->TakeResponseHead();
    if (mResponseHead) {
      uint32_t httpStatus = mResponseHead->Status();
      if ((httpStatus < 500) && (httpStatus != 421) && (httpStatus != 407)) {
        ProcessAltService();
      }
    } else {
      NS_WARNING("No response head in OnStartRequest");
    }
  }

  // avoid crashing if mListener happens to be null...
  if (!mListener) {
    MOZ_ASSERT_UNREACHABLE("mListener is null");
    return NS_OK;
  }

  return CallOnStartRequest();
}

NS_IMETHODIMP
TRRServiceChannel::OnDataAvailable(nsIRequest* request, nsIInputStream* input,
                                   uint64_t offset, uint32_t count) {
  LOG(("TRRServiceChannel::OnDataAvailable [this=%p request=%p offset=%" PRIu64
       " count=%" PRIu32 "]\n",
       this, request, offset, count));

  // don't send out OnDataAvailable notifications if we've been canceled.
  if (mCanceled) return mStatus;

  MOZ_ASSERT(mResponseHead, "No response head in ODA!!");

  if (mListener) {
    return mListener->OnDataAvailable(this, input, offset, count);
  }

  return NS_ERROR_ABORT;
}

NS_IMETHODIMP
TRRServiceChannel::OnStopRequest(nsIRequest* request, nsresult status) {
  LOG(("TRRServiceChannel::OnStopRequest [this=%p request=%p status=%" PRIx32
       "]\n",
       this, request, static_cast<uint32_t>(status)));

  if (mCanceled || NS_FAILED(mStatus)) status = mStatus;

  mTransactionTimings = mTransaction->Timings();
  mTransaction = nullptr;
  mTransactionPump = nullptr;

  if (mListener) {
    LOG(("TRRServiceChannel %p calling OnStopRequest\n", this));
    MOZ_ASSERT(mOnStartRequestCalled,
               "OnStartRequest should be called before OnStopRequest");
    MOZ_ASSERT(!mOnStopRequestCalled, "We should not call OnStopRequest twice");
    mOnStopRequestCalled = true;
    mListener->OnStopRequest(this, status);
  }
  mOnStopRequestCalled = true;

  mDNSPrefetch = nullptr;

  if (mLoadGroup) {
    mLoadGroup->RemoveRequest(this, nullptr, status);
  }

  ReleaseListeners();
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::OnLookupComplete(nsICancelable* request, nsIDNSRecord* rec,
                                    nsresult status) {
  LOG(
      ("TRRServiceChannel::OnLookupComplete [this=%p] prefetch complete%s: "
       "%s status[0x%" PRIx32 "]\n",
       this, mCaps & NS_HTTP_REFRESH_DNS ? ", refresh requested" : "",
       NS_SUCCEEDED(status) ? "success" : "failure",
       static_cast<uint32_t>(status)));

  // We no longer need the dns prefetch object. Note: mDNSPrefetch could be
  // validly null if OnStopRequest has already been called.
  // We only need the domainLookup timestamps when not loading from cache
  if (mDNSPrefetch && mDNSPrefetch->TimingsValid() && mTransaction) {
    TimeStamp connectStart = mTransaction->GetConnectStart();
    TimeStamp requestStart = mTransaction->GetRequestStart();
    // We only set the domainLookup timestamps if we're not using a
    // persistent connection.
    if (requestStart.IsNull() && connectStart.IsNull()) {
      mTransaction->SetDomainLookupStart(mDNSPrefetch->StartTimestamp());
      mTransaction->SetDomainLookupEnd(mDNSPrefetch->EndTimestamp());
    }
  }
  mDNSPrefetch = nullptr;

  // Unset DNS cache refresh if it was requested,
  if (mCaps & NS_HTTP_REFRESH_DNS) {
    mCaps &= ~NS_HTTP_REFRESH_DNS;
    if (mTransaction) {
      mTransaction->SetDNSWasRefreshed();
    }
  }

  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::OnLookupByTypeComplete(nsICancelable* aRequest,
                                          nsIDNSByTypeRecord* aRes,
                                          nsresult aStatus) {
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::LogBlockedCORSRequest(const nsAString& aMessage,
                                         const nsACString& aCategory) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRServiceChannel::LogMimeTypeMismatch(const nsACString& aMessageName,
                                       bool aWarning, const nsAString& aURL,
                                       const nsAString& aContentType) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRServiceChannel::SetupFallbackChannel(const char* aFallbackKey) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRServiceChannel::SetNotificationCallbacks(nsIInterfaceRequestor* aCallbacks) {
  mCallbacks = aCallbacks;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::SetPriority(int32_t value) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

void TRRServiceChannel::OnClassOfServiceUpdated() {
  LOG(("TRRServiceChannel::OnClassOfServiceUpdated this=%p, cos=%u", this,
       mClassOfService));

  if (mTransaction) {
    gHttpHandler->UpdateClassOfServiceOnTransaction(mTransaction,
                                                    mClassOfService);
  }
}

NS_IMETHODIMP
TRRServiceChannel::SetClassFlags(uint32_t inFlags) {
  uint32_t previous = mClassOfService;
  mClassOfService = inFlags;
  if (previous != mClassOfService) {
    OnClassOfServiceUpdated();
  }
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::AddClassFlags(uint32_t inFlags) {
  uint32_t previous = mClassOfService;
  mClassOfService |= inFlags;
  if (previous != mClassOfService) {
    OnClassOfServiceUpdated();
  }
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::ClearClassFlags(uint32_t inFlags) {
  uint32_t previous = mClassOfService;
  mClassOfService &= ~inFlags;
  if (previous != mClassOfService) {
    OnClassOfServiceUpdated();
  }
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::ResumeAt(uint64_t aStartPos, const nsACString& aEntityID) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

void TRRServiceChannel::DoAsyncAbort(nsresult aStatus) {
  Unused << AsyncAbort(aStatus);
}

NS_IMETHODIMP
TRRServiceChannel::GetProxyInfo(nsIProxyInfo** result) {
  if (!mConnectionInfo)
    *result = mProxyInfo;
  else
    *result = mConnectionInfo->ProxyInfo();
  NS_IF_ADDREF(*result);
  return NS_OK;
}

NS_IMETHODIMP TRRServiceChannel::GetHttpProxyConnectResponseCode(
    int32_t* aResponseCode) {
  NS_ENSURE_ARG_POINTER(aResponseCode);

  *aResponseCode = -1;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetLoadFlags(nsLoadFlags* aLoadFlags) {
  return HttpBaseChannel::GetLoadFlags(aLoadFlags);
}

NS_IMETHODIMP
TRRServiceChannel::SetLoadFlags(nsLoadFlags aLoadFlags) {
  if (aLoadFlags & (nsICachingChannel::LOAD_ONLY_FROM_CACHE | LOAD_FROM_CACHE |
                    nsICachingChannel::LOAD_NO_NETWORK_IO |
                    nsICachingChannel::LOAD_CHECK_OFFLINE_CACHE)) {
    MOZ_ASSERT(false, "Wrong load flags!");
    return NS_ERROR_FAILURE;
  }

  return HttpBaseChannel::SetLoadFlags(aLoadFlags);
}

NS_IMETHODIMP
TRRServiceChannel::GetURI(nsIURI** aURI) {
  return HttpBaseChannel::GetURI(aURI);
}

NS_IMETHODIMP
TRRServiceChannel::GetNotificationCallbacks(
    nsIInterfaceRequestor** aCallbacks) {
  return HttpBaseChannel::GetNotificationCallbacks(aCallbacks);
}

NS_IMETHODIMP
TRRServiceChannel::GetLoadGroup(nsILoadGroup** aLoadGroup) {
  return HttpBaseChannel::GetLoadGroup(aLoadGroup);
}

NS_IMETHODIMP
TRRServiceChannel::GetRequestMethod(nsACString& aMethod) {
  return HttpBaseChannel::GetRequestMethod(aMethod);
}

void TRRServiceChannel::DoNotifyListener() {
  LOG(("TRRServiceChannel::DoNotifyListener this=%p", this));

  // In case nsHttpChannel::OnStartRequest wasn't called (e.g. due to flag
  // LOAD_ONLY_IF_MODIFIED) we want to set mAfterOnStartRequestBegun to true
  // before notifying listener.
  if (!mAfterOnStartRequestBegun) {
    mAfterOnStartRequestBegun = true;
  }

  if (mListener && !mOnStartRequestCalled) {
    nsCOMPtr<nsIStreamListener> listener = mListener;
    mOnStartRequestCalled = true;
    listener->OnStartRequest(this);
  }
  mOnStartRequestCalled = true;

  // Make sure mIsPending is set to false. At this moment we are done from
  // the point of view of our consumer and we have to report our self
  // as not-pending.
  mIsPending = false;

  if (mListener && !mOnStopRequestCalled) {
    nsCOMPtr<nsIStreamListener> listener = mListener;
    mOnStopRequestCalled = true;
    listener->OnStopRequest(this, mStatus);
  }
  mOnStopRequestCalled = true;

  // We have to make sure to drop the references to listeners and callbacks
  // no longer needed.
  ReleaseListeners();

  DoNotifyListenerCleanup();
}

void TRRServiceChannel::DoNotifyListenerCleanup() {}

NS_IMETHODIMP
TRRServiceChannel::GetDomainLookupStart(TimeStamp* _retval) {
  if (mTransaction)
    *_retval = mTransaction->GetDomainLookupStart();
  else
    *_retval = mTransactionTimings.domainLookupStart;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetDomainLookupEnd(TimeStamp* _retval) {
  if (mTransaction)
    *_retval = mTransaction->GetDomainLookupEnd();
  else
    *_retval = mTransactionTimings.domainLookupEnd;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetConnectStart(TimeStamp* _retval) {
  if (mTransaction)
    *_retval = mTransaction->GetConnectStart();
  else
    *_retval = mTransactionTimings.connectStart;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetTcpConnectEnd(TimeStamp* _retval) {
  if (mTransaction)
    *_retval = mTransaction->GetTcpConnectEnd();
  else
    *_retval = mTransactionTimings.tcpConnectEnd;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetSecureConnectionStart(TimeStamp* _retval) {
  if (mTransaction)
    *_retval = mTransaction->GetSecureConnectionStart();
  else
    *_retval = mTransactionTimings.secureConnectionStart;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetConnectEnd(TimeStamp* _retval) {
  if (mTransaction)
    *_retval = mTransaction->GetConnectEnd();
  else
    *_retval = mTransactionTimings.connectEnd;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetRequestStart(TimeStamp* _retval) {
  if (mTransaction)
    *_retval = mTransaction->GetRequestStart();
  else
    *_retval = mTransactionTimings.requestStart;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetResponseStart(TimeStamp* _retval) {
  if (mTransaction)
    *_retval = mTransaction->GetResponseStart();
  else
    *_retval = mTransactionTimings.responseStart;
  return NS_OK;
}

NS_IMETHODIMP
TRRServiceChannel::GetResponseEnd(TimeStamp* _retval) {
  if (mTransaction)
    *_retval = mTransaction->GetResponseEnd();
  else
    *_retval = mTransactionTimings.responseEnd;
  return NS_OK;
}

}  // namespace net
}  // namespace mozilla
