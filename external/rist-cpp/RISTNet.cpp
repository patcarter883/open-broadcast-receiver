//
// Created by Anders Cedronius (Edgeware AB) on 2020-03-14.
//

#include "RISTNet.h"
#include "RISTNetInternal.h"

//---------------------------------------------------------------------------------------------------------------------
//
//
// RIST Network tools
//
//
//---------------------------------------------------------------------------------------------------------------------

bool RISTNetTools::isIPv4(const std::string &rStr) {
    sockaddr_in lsa{};
    return inet_pton(AF_INET, rStr.c_str(), &(lsa.sin_addr)) != 0;
}

bool RISTNetTools::isIPv6(const std::string &rStr) {
    sockaddr_in6 lsa{};
    return inet_pton(AF_INET6, rStr.c_str(), &(lsa.sin6_addr)) != 0;
}

bool RISTNetTools::buildRISTURL(const std::string &lIP, const std::string &lPort, std::string &rURL, bool lListen) {
    int lIPType;
    if (isIPv4(lIP)) {
        lIPType = AF_INET;
    } else if (isIPv6(lIP)) {
        lIPType = AF_INET6;
    } else {
        LOGGER(true, LOGG_ERROR, " " << "Provided IP-Address not valid.")
        return false;
    }
    int32_t lPortNum = 0;
    std::stringstream lPortNumStr(lPort);
    lPortNumStr >> lPortNum;
    if (lPortNum < 1 || lPortNum > UINT16_MAX) {
        LOGGER(true, LOGG_ERROR, " " << "Provided Port number not valid.")
        return false;
    }
    std::string lRistURL{};
    if (lIPType == AF_INET) {
        lRistURL += "rist://";
    } else {
        lRistURL += "rist6://";
    }
    if (lListen) {
        lRistURL += "@";
    }
    if (lIPType == AF_INET) {
        lRistURL += lIP + ":" + lPort;
    } else {
        lRistURL += "[" + lIP + "]:" + lPort;
    }
    rURL = lRistURL;
    return true;
}

//---------------------------------------------------------------------------------------------------------------------
//
//
// RISTNetReceiver  --  RECEIVER
//
//
//---------------------------------------------------------------------------------------------------------------------

RISTNetReceiver::RISTNetReceiver() {
    // Set the callback stubs
    validateConnectionCallback = std::bind(&RISTNetReceiver::validateConnectionStub, this, std::placeholders::_1,
                                           std::placeholders::_2);
    networkDataCallback = std::bind(&RISTNetReceiver::dataFromClientStub, this, std::placeholders::_1,
                                    std::placeholders::_2, std::placeholders::_3);
    LOGGER(false, LOGG_NOTIFY, "RISTNetReceiver constructed")
}

RISTNetReceiver::~RISTNetReceiver() {
    if (mRistContext) {
        int lStatus = rist_destroy(mRistContext);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_receiver_destroy failure")
        }
    }
    LOGGER(false, LOGG_NOTIFY, "RISTNetReceiver destruct")
}

//---------------------------------------------------------------------------------------------------------------------
// RISTNetReceiver  --  Callbacks --- Start
//---------------------------------------------------------------------------------------------------------------------


std::shared_ptr<RISTNetReceiver::NetworkConnection> RISTNetReceiver::validateConnectionStub(std::string lIPAddress,
                                                                                            uint16_t lPort) {
    LOGGER(true, LOGG_ERROR,
           "validateConnectionCallback not implemented. Will not accept connection from: " << lIPAddress << ":"
                                                                                           << unsigned(lPort))
    return nullptr;
}

int RISTNetReceiver::dataFromClientStub(const uint8_t *pBuf, size_t lSize,
                                         std::shared_ptr<NetworkConnection> &rConnection) {
    LOGGER(true, LOGG_ERROR, "networkDataCallback not implemented. Data is lost")
    return -1;
}

int RISTNetReceiver::receiveData(void *pArg, rist_data_block *pDataBlock) {
    RISTNetReceiver *lWeakSelf = (RISTNetReceiver *) pArg;
    std::lock_guard<std::recursive_mutex> lLock(lWeakSelf->mClientListMtx);
    int retVal = 0;

    if (lWeakSelf->mClientListReceiver.empty()) {
        auto lEmptyContext = std::make_shared<NetworkConnection>();
        lEmptyContext->mObject.reset();
        if (lWeakSelf->receivePktCallback == nullptr && lWeakSelf->receiveCallback == nullptr) {
            retVal = lWeakSelf->networkDataCallback((const uint8_t *) pDataBlock->payload, pDataBlock->payload_len, lEmptyContext, pDataBlock->peer, pDataBlock->flow_id);
        } else if (lWeakSelf->receiveCallback == nullptr){
            retVal = lWeakSelf->receivePktCallback(*pDataBlock, lEmptyContext);
        } else {
            retVal = lWeakSelf->receiveCallback(pDataBlock, lEmptyContext);
        }
        rist_receiver_data_block_free2(&pDataBlock);
        return retVal;
    }
    auto netObj = lWeakSelf->mClientListReceiver.find(pDataBlock->peer);
    if (netObj != lWeakSelf->mClientListReceiver.end()) {
        auto netCon = netObj->second;
        if (lWeakSelf->receivePktCallback == nullptr && lWeakSelf->receiveCallback == nullptr) {
            retVal = lWeakSelf->networkDataCallback((const uint8_t *) pDataBlock->payload, pDataBlock->payload_len, netCon, pDataBlock->peer, pDataBlock->flow_id);
        } else if (lWeakSelf->receiveCallback == nullptr){
            retVal = lWeakSelf->receivePktCallback(*pDataBlock, netCon);
        } else {
            retVal = lWeakSelf->receiveCallback(pDataBlock, netCon);
        }
        rist_receiver_data_block_free2(&pDataBlock);
        return retVal;
    } else {
        LOGGER(true, LOGG_ERROR, "receivesendDataData mClientListReceiver <-> peer mismatch.")
    }
    return -1;
}

int RISTNetReceiver::receiveOOBData(void *pArg, const rist_oob_block *pOOBBlock) {
    RISTNetReceiver *lWeakSelf = (RISTNetReceiver *) pArg;
    if (lWeakSelf->networkOOBDataCallback) {  //This is a optional callback
        if (lWeakSelf->mClientListReceiver.empty()) {
            auto lEmptyContext = std::make_shared<NetworkConnection>(); //In this case we got no connections the NetworkConnection will contain a std::any == nullptr
            lEmptyContext->mObject.reset();
            lWeakSelf->networkOOBDataCallback((const uint8_t *) pOOBBlock->payload, pOOBBlock->payload_len, lEmptyContext, pOOBBlock->peer);
            return 0;
        }
        std::lock_guard<std::recursive_mutex> lLock(lWeakSelf->mClientListMtx);
        auto netObj = lWeakSelf->mClientListReceiver.find(pOOBBlock->peer);
        if (netObj != lWeakSelf->mClientListReceiver.end()) {
            auto netCon = netObj->second;
            lWeakSelf->networkOOBDataCallback((const uint8_t *) pOOBBlock->payload, pOOBBlock->payload_len, netCon, pOOBBlock->peer);
            return 0;
        }
    }
    return 0;
}


int RISTNetReceiver::clientConnect(void *pArg, const char* pConnectingIP, uint16_t lConnectingPort, const char* pIP, uint16_t lPort, rist_peer *pPeer) {
    RISTNetReceiver *lWeakSelf = (RISTNetReceiver *) pArg;
    auto lNetObj = lWeakSelf->validateConnectionCallback(std::string(pConnectingIP), lConnectingPort);
    if (lNetObj) {
        std::lock_guard<std::recursive_mutex> lLock(lWeakSelf->mClientListMtx);

        lWeakSelf->mClientListReceiver[pPeer] = lNetObj;
        return 0; // Accept the connection
    }
    return -1; // Reject the connection
}

int RISTNetReceiver::clientDisconnect(void *pArg, rist_peer *pPeer) {
    RISTNetReceiver *lWeakSelf = (RISTNetReceiver *) pArg;
    std::lock_guard<std::recursive_mutex> lLock(lWeakSelf->mClientListMtx);
    if (lWeakSelf->mClientListReceiver.empty()) {
        return 0;
    }

    auto netObj = lWeakSelf->mClientListReceiver.find(pPeer);
    if (netObj == lWeakSelf->mClientListReceiver.end()) {
        LOGGER(true, LOGG_ERROR, "RISTNetReceiver::clientDisconnect unknown peer")
        return 0;
    }

    if (lWeakSelf->clientDisconnectedCallback) {
        lWeakSelf->clientDisconnectedCallback(netObj->second, *pPeer);
    }

    lWeakSelf->mClientListReceiver.erase(pPeer);
    return 0;
}

int RISTNetReceiver::gotStatistics(void *pArg, const rist_stats *stats) {
    RISTNetReceiver *lWeakSelf = static_cast<RISTNetReceiver*>(pArg);
    if (lWeakSelf->statisticsCallback) {
        lWeakSelf->statisticsCallback(*stats);
    }
    return rist_stats_free(stats);
}

void RISTNetReceiver::connectionStatus(void *arg, struct rist_peer *peer, enum rist_connection_status peer_connection_status) {
    RISTNetReceiver *lWeakSelf = static_cast<RISTNetReceiver*>(arg);
    lWeakSelf->mConnectionStatus = peer_connection_status;
    int sock, sockExtra;
    if(rist_peer_get_socket(peer, &sock, &sockExtra) == 0) {
        struct sockaddr_in sockAddr{};
        socklen_t sockAddrLen = sizeof(sockAddr);
        if(getsockname(sock, (struct sockaddr *)&sockAddr, &sockAddrLen) == 0) {
            lWeakSelf->mSocketPort_be = sockAddr.sin_port;
            lWeakSelf->mSocketIp_be = sockAddr.sin_addr.s_addr;
        }
    }
}

//---------------------------------------------------------------------------------------------------------------------
// RISTNetReceiver  --  Callbacks --- End
//---------------------------------------------------------------------------------------------------------------------

rist_connection_status RISTNetReceiver::getConnectionStatus() {
    return mConnectionStatus;
}

uint16_t RISTNetReceiver::getSockPort_be() {
    return mSocketPort_be;
}

uint32_t RISTNetReceiver::getSockIp_be() {
    return mSocketIp_be;
}

void RISTNetReceiver::getActiveClients(
        std::function<void(std::map<rist_peer *, std::shared_ptr<NetworkConnection>> &)> lFunction) {
    std::lock_guard<std::recursive_mutex> lLock(mClientListMtx);

    if (lFunction) {
        lFunction(mClientListReceiver);
    }
}

bool RISTNetReceiver::closeClientConnection(rist_peer *lPeer) {
    std::lock_guard<std::recursive_mutex> lLock(mClientListMtx);
    auto netObj = mClientListReceiver.find(lPeer);
    if (netObj == mClientListReceiver.end()) {
        LOGGER(true, LOGG_ERROR, "Could not find peer")
        return false;
    }
    int lStatus = rist_peer_destroy(mRistContext, lPeer);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_peer_destroy failed: ")
        return false;
    }
    return true;
}

void RISTNetReceiver::closeAllClientConnections() {
     std::lock_guard<std::recursive_mutex> lLock(mClientListMtx);
     // Collect peers to a vector first to avoid iterator invalidation
     // when rist_peer_destroy triggers clientDisconnect callback
     std::vector<rist_peer*> peersToDestroy;
     for (const auto& pair : mClientListReceiver) {
         peersToDestroy.push_back(pair.first);
     }
     // Now destroy each peer (safe from callback interference)
     for (rist_peer* lPeer : peersToDestroy) {
         int lStatus = rist_peer_destroy(mRistContext, lPeer);
         if (lStatus) {
             LOGGER(true, LOGG_ERROR, "rist_receiver_peer_destroy failed: ")
         }
     }
 }

bool RISTNetReceiver::destroyReceiver() {
    if (mRistContext) {
        int lStatus = rist_destroy(mRistContext);
        mRistContext = nullptr;
        std::lock_guard<std::recursive_mutex> lLock(mClientListMtx);
        mClientListReceiver.clear();
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_receiver_destroy fail.")
            return false;
        }
    } else {
        LOGGER(true, LOGG_WARN, "RIST receiver not initialised.")
        return false;
    }
    return true;
}

bool RISTNetReceiver::initReceiver(std::vector<std::string> &rURLList,
                                   RISTNetReceiver::RISTNetReceiverSettings &rSettings) {
    if (rURLList.empty()) {
        LOGGER(true, LOGG_ERROR, "URL list is empty.")
        return false;
    }
    // I need the settings struct to be a member of RISTNet class, because when being destructed
    // there can be a segfault if the struct is deallocated before destroying all RIST environment.
    // That can happen if the settings struct used belongs to an upper class or is created as a heap
    // or stack variable.
    mRistReceiverSettings.mLogLevel = rSettings.mLogLevel;
    mRistReceiverSettings.mPeerConfig = rSettings.mPeerConfig;
    mRistReceiverSettings.mProfile = rSettings.mProfile;
    mRistReceiverSettings.mPSK = rSettings.mPSK;
    mRistReceiverSettings.mCNAME = rSettings.mCNAME;
    mRistReceiverSettings.mSessionTimeout = rSettings.mSessionTimeout;
    mRistReceiverSettings.mKeepAliveInterval = rSettings.mKeepAliveInterval;
    mRistReceiverSettings.mMaxJitter = rSettings.mMaxJitter;

    int lStatus;

    // Default log settings
    rist_logging_settings* lSettingsPtr = mRistReceiverSettings.mLogSetting.get();
    lStatus = rist_logging_set(&lSettingsPtr, mRistReceiverSettings.mLogLevel, nullptr, nullptr, nullptr, stderr);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_logging_set failed.")
        return false;
    }


    lStatus = rist_receiver_create(&mRistContext, mRistReceiverSettings.mProfile, mRistReceiverSettings.mLogSetting.get());
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_create fail.")
        return false;
    }
    for (auto &rURL: rURLList) {
        int keysize = 0;
        if (!mRistReceiverSettings.mPSK.empty()) {
            keysize = 128;
        }
        mRistPeerConfig.version = RIST_PEER_CONFIG_VERSION;
        mRistPeerConfig.virt_dst_port = RIST_DEFAULT_VIRT_DST_PORT;
        mRistPeerConfig.recovery_mode = mRistReceiverSettings.mPeerConfig.recovery_mode;
        mRistPeerConfig.recovery_maxbitrate = mRistReceiverSettings.mPeerConfig.recovery_maxbitrate;
        mRistPeerConfig.recovery_maxbitrate_return = mRistReceiverSettings.mPeerConfig.recovery_maxbitrate_return;
        mRistPeerConfig.recovery_length_min = mRistReceiverSettings.mPeerConfig.recovery_length_min;
        mRistPeerConfig.recovery_length_max = mRistReceiverSettings.mPeerConfig.recovery_length_max;
        mRistPeerConfig.recovery_rtt_min = mRistReceiverSettings.mPeerConfig.recovery_rtt_min;
        mRistPeerConfig.recovery_rtt_max = mRistReceiverSettings.mPeerConfig.recovery_rtt_max;
        mRistPeerConfig.weight = 5;
        mRistPeerConfig.congestion_control_mode = mRistReceiverSettings.mPeerConfig.congestion_control_mode;
        mRistPeerConfig.min_retries = mRistReceiverSettings.mPeerConfig.min_retries;
        mRistPeerConfig.max_retries = mRistReceiverSettings.mPeerConfig.max_retries;
        mRistPeerConfig.session_timeout = mRistReceiverSettings.mSessionTimeout;
        mRistPeerConfig.keepalive_interval =  mRistReceiverSettings.mKeepAliveInterval;
        mRistPeerConfig.key_size = keysize;

        if (keysize) {
            strncpy((char *) &mRistPeerConfig.secret[0], mRistReceiverSettings.mPSK.c_str(), 128);
        }

        if (!mRistReceiverSettings.mCNAME.empty()) {
            strncpy((char *) &mRistPeerConfig.cname[0], mRistReceiverSettings.mCNAME.c_str(), 128);
        }

        rist_peer_config* lTmp = &mRistPeerConfig;
        lStatus = rist_parse_address2(rURL.c_str(), &lTmp);
        if (lStatus)
        {
            LOGGER(true, LOGG_ERROR, "rist_parse_address fail: " << rURL)
            destroyReceiver();
            return false;
        }

        rist_peer *peer;
        lStatus =  rist_peer_create(mRistContext, &peer, &mRistPeerConfig);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_receiver_peer_create fail: " << rURL)
            destroyReceiver();
            return false;
        }
    }

    if (mRistReceiverSettings.mMaxJitter) {
        lStatus = rist_jitter_max_set(mRistContext, mRistReceiverSettings.mMaxJitter);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_receiver_jitter_max_set fail.")
            destroyReceiver();
            return false;
        }
    }

    lStatus = rist_oob_callback_set(mRistContext, receiveOOBData, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_oob_set fail.")
        destroyReceiver();
        return false;
    }

    lStatus = rist_receiver_data_callback_set2(mRistContext, receiveData, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_data_callback_set fail.")
        destroyReceiver();
        return false;
    }

    lStatus = rist_auth_handler_set(mRistContext, clientConnect, clientDisconnect, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_auth_handler_set fail.")
        destroyReceiver();
        return false;
    }

    lStatus = rist_stats_callback_set(mRistContext, 1000, gotStatistics, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_stats_callback_set fail.")
        destroyReceiver();
        return false;
    }

    lStatus = rist_connection_status_callback_set(mRistContext, connectionStatus, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_conn_status_callback_set fail.")
        destroyReceiver();
        return false;
    }

    lStatus = rist_start(mRistContext);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_start fail.")
        destroyReceiver();
        return false;
    }
    return true;
}

bool RISTNetReceiver::sendOOBData(rist_peer *pPeer, const uint8_t *pData, size_t lSize) {
    if (!mRistContext) {
        LOGGER(true, LOGG_ERROR, "RISTNetReceiver not initialised.")
        return false;
    }

    rist_oob_block myOOBBlock = {nullptr};
    myOOBBlock.peer = pPeer;
    myOOBBlock.payload = pData;
    myOOBBlock.payload_len = lSize;

    int lStatus = rist_oob_write(mRistContext, &myOOBBlock);
    if (lStatus) {
        // open-broadcast-receiver patch: a transient best-effort OOB telemetry
        // write must NOT tear down the receiver (and with it the live media
        // path). Upstream called destroyReceiver() here; we log and return
        // false so the caller can simply skip this telemetry tick.
        // See open-broadcast-receiver/DECISIONS.md §6.
        LOGGER(true, LOGG_ERROR, "rist_receiver_oob_write failed (telemetry tick dropped).")
        return false;
    }
    return true;
}

void RISTNetReceiver::getVersion(uint32_t &rCppWrapper, uint32_t &rRistMajor, uint32_t &rRistMinor) {
    rCppWrapper = CPP_WRAPPER_VERSION;
    rRistMajor = LIBRIST_API_VERSION_MAJOR;
    rRistMinor = LIBRIST_API_VERSION_MINOR;
}

rist_peer_config* RISTNetReceiver::getPeerConfig() {
    return &mRistPeerConfig;
}

//---------------------------------------------------------------------------------------------------------------------
//
//
// RISTNetSender  --  SENDER
//
//
//---------------------------------------------------------------------------------------------------------------------

RISTNetSender::RISTNetSender() {
    validateConnectionCallback = std::bind(&RISTNetSender::validateConnectionStub, this, std::placeholders::_1,
                                           std::placeholders::_2);
    // LOGGER(false, LOGG_NOTIFY, "RISTNetSender constructed")
}

RISTNetSender::~RISTNetSender() {
    if (mRistContext) {
        int lStatus = rist_destroy(mRistContext);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_sender_destroy fail.")
        }
    }
    LOGGER(false, LOGG_NOTIFY, "RISTNetClient destruct.")
}

//---------------------------------------------------------------------------------------------------------------------
// RISTNetSender  --  Callbacks --- Start
//---------------------------------------------------------------------------------------------------------------------

std::shared_ptr<RISTNetSender::NetworkConnection> RISTNetSender::validateConnectionStub(const std::string &ipAddress,
                                                                                        uint16_t port) {
    LOGGER(true, LOGG_ERROR,
           "validateConnectionCallback not implemented. Will not accept connection from: " << ipAddress << ":"
                                                                                           << unsigned(port))
    return 0;
}

int RISTNetSender::receiveOOBData(void *pArg, const rist_oob_block *pOOBBlock) {
    RISTNetSender *lWeakSelf = (RISTNetSender *) pArg;
    if (lWeakSelf->networkOOBDataCallback) {  //This is a optional callback
        if (lWeakSelf->mClientListSender.empty()) {
            auto lEmptyContext = std::make_shared<NetworkConnection>(); //In this case we got no connections the NetworkConnection will contain a std::any == nullptr
            lEmptyContext->mObject.reset();
            lWeakSelf->networkOOBDataCallback((const uint8_t *) pOOBBlock->payload, pOOBBlock->payload_len, lEmptyContext, pOOBBlock->peer);
            return 0;
        }
        std::lock_guard<std::recursive_mutex> lLock(lWeakSelf->mClientListMtx);
        auto netObj = lWeakSelf->mClientListSender.find(pOOBBlock->peer);
        if (netObj != lWeakSelf->mClientListSender.end()) {
            auto netCon = netObj->second;
            lWeakSelf->networkOOBDataCallback((const uint8_t *) pOOBBlock->payload, pOOBBlock->payload_len, netCon, pOOBBlock->peer);
            return 0;
        }
    }
    return 0;
}

int RISTNetSender::clientConnect(void *pArg, const char* pConnectingIP, uint16_t lConnectingPort, const char* pIP, uint16_t lPort, rist_peer *pPeer) {
    RISTNetSender *lWeakSelf = (RISTNetSender *) pArg;
    auto lNetObj = lWeakSelf->validateConnectionCallback(std::string(pConnectingIP), lConnectingPort);
    if (lNetObj) {
        std::lock_guard<std::recursive_mutex> lLock(lWeakSelf->mClientListMtx);
        lWeakSelf->mClientListSender[pPeer] = lNetObj;
        return 0; // Accept the connection
    }
    return -1; // Reject the connection
}

int RISTNetSender::clientDisconnect(void *pArg, rist_peer *pPeer) {
    RISTNetSender *lWeakSelf = (RISTNetSender *) pArg;
    std::lock_guard<std::recursive_mutex> lLock(lWeakSelf->mClientListMtx);
    if (lWeakSelf->mClientListSender.empty()) {
        return 0;
    }

    auto netObj = lWeakSelf->mClientListSender.find(pPeer);
    if (netObj == lWeakSelf->mClientListSender.end()) {
        LOGGER(true, LOGG_ERROR, "RISTNetSender::clientDisconnect unknown peer")
        return 0;
    }

    if (lWeakSelf->clientDisconnectedCallback) {
        lWeakSelf->clientDisconnectedCallback(netObj->second, *pPeer);
    }

    lWeakSelf->mClientListSender.erase(pPeer);
    return 0;
}

int RISTNetSender::gotStatistics(void *pArg, const rist_stats *stats) {
    RISTNetSender *lWeakSelf = static_cast<RISTNetSender*>(pArg);
    if (lWeakSelf->statisticsCallback) {
        lWeakSelf->statisticsCallback(*stats);
    }
    return rist_stats_free(stats);
}

void RISTNetSender::connectionStatus(void *arg, struct rist_peer *peer, enum rist_connection_status peer_connection_status) {
    RISTNetSender *lWeakSelf = static_cast<RISTNetSender*>(arg);
    lWeakSelf->mConnectionStatus = peer_connection_status;
    int sock, sockExtra;
    if(rist_peer_get_socket(peer, &sock, &sockExtra) == 0) {
        struct sockaddr_in sockAddr{};
        socklen_t sockAddrLen = sizeof(sockAddr);
        if(getsockname(sock, (struct sockaddr *)&sockAddr, &sockAddrLen) == 0) {
            lWeakSelf->mSocketPort_be = sockAddr.sin_port;
            lWeakSelf->mSocketIp_be = sockAddr.sin_addr.s_addr;
        }
    }
}

//---------------------------------------------------------------------------------------------------------------------
// RISTNetSender  --  Callbacks --- End
//---------------------------------------------------------------------------------------------------------------------

rist_connection_status RISTNetSender::getConnectionStatus() {
    return mConnectionStatus;
}

uint16_t RISTNetSender::getSockPort_be() {
    return mSocketPort_be;
}

uint32_t RISTNetSender::getSockIp_be() {
    return mSocketIp_be;
}

void RISTNetSender::getActiveClients(
        const std::function<void(std::map<rist_peer *, std::shared_ptr<NetworkConnection>> &)> lFunction) {
    std::lock_guard<std::recursive_mutex> lLock(mClientListMtx);
    if (lFunction) {
        lFunction(mClientListSender);
    }
}

bool RISTNetSender::closeClientConnection(rist_peer *lPeer) {
    std::lock_guard<std::recursive_mutex> lLock(mClientListMtx);
    auto netObj = mClientListSender.find(lPeer);
    if (netObj == mClientListSender.end()) {
        LOGGER(true, LOGG_ERROR, "Could not find peer")
        return false;
    }
    int lStatus = rist_peer_destroy(mRistContext, lPeer);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_peer_destroy failed: ")
        return false;
    }
    return true;
}

void RISTNetSender::closeAllClientConnections() {
     std::lock_guard<std::recursive_mutex> lLock(mClientListMtx);
     // Collect peers to a vector first to avoid iterator invalidation
     // when rist_peer_destroy triggers clientDisconnect callback
     std::vector<rist_peer*> peersToDestroy;
     for (const auto& pair : mClientListSender) {
         peersToDestroy.push_back(pair.first);
     }
     // Now destroy each peer (safe from callback interference)
     for (rist_peer* pPeer : peersToDestroy) {
         int status = rist_peer_destroy(mRistContext, pPeer);
         if (status) {
             LOGGER(true, LOGG_ERROR, "rist_sender_peer_destroy failed: ")
         }
     }
     mClientListSender.clear();
 }

bool RISTNetSender::destroySender() {
    if (mRistContext) {
        int lStatus = rist_destroy(mRistContext);
        mRistContext = nullptr;
        std::lock_guard<std::recursive_mutex> lLock(mClientListMtx);
        mClientListSender.clear();
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_sender_destroy fail.")
            return false;
        }
    } else {
        LOGGER(true, LOGG_WARN, "RIST Sender not running.")
    }
    return true;
}

bool RISTNetSender::initSender(std::vector<std::tuple<std::string,int>> &rPeerList,
                               RISTNetSenderSettings &rSettings) {

    if (rPeerList.empty()) {
        LOGGER(true, LOGG_ERROR, "URL list is empty.")
        return false;
    }
    // I need the settings struct to be a member of RISTNet class, because when being destructed
    // there can be a segfault if the struct is deallocated before destroying all RIST environment.
    // That can happen if the settings struct used belongs to an upper class or is created as a heap
    // or stack variable.
    mRistSenderSettings.mLogSetting.get()->log_cb = rSettings.mLogSetting.get()->log_cb;
    mRistSenderSettings.mLogLevel = rSettings.mLogLevel;
    mRistSenderSettings.mPeerConfig = rSettings.mPeerConfig;
    mRistSenderSettings.mProfile = rSettings.mProfile;
    mRistSenderSettings.mPSK = rSettings.mPSK;
    mRistSenderSettings.mCNAME = rSettings.mCNAME;
    mRistSenderSettings.mSessionTimeout = rSettings.mSessionTimeout;
    mRistSenderSettings.mKeepAliveInterval = rSettings.mKeepAliveInterval;
    mRistSenderSettings.mMaxJitter = rSettings.mMaxJitter;

    int lStatus;
    // Default log settings
    rist_logging_settings* lSettingsPtr = mRistSenderSettings.mLogSetting.get();
    lStatus = rist_logging_set(&lSettingsPtr, mRistSenderSettings.mLogLevel, mRistSenderSettings.mLogSetting.get()->log_cb, nullptr, nullptr, stderr);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_logging_set failed.")
        return false;
    }

    lStatus = rist_sender_create(&mRistContext, mRistSenderSettings.mProfile, 0, mRistSenderSettings.mLogSetting.get());
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_create fail.")
        return false;
    }
    for (auto &rPeerInfo: rPeerList) {

        auto peerURL = std::get<0>(rPeerInfo);

        int keysize = 0;
        if (!mRistSenderSettings.mPSK.empty()) {
            keysize = 128;
        }
        mRistPeerConfig.version = RIST_PEER_CONFIG_VERSION;
        mRistPeerConfig.virt_dst_port = RIST_DEFAULT_VIRT_DST_PORT;
        mRistPeerConfig.recovery_mode = mRistSenderSettings.mPeerConfig.recovery_mode;
        mRistPeerConfig.recovery_maxbitrate = mRistSenderSettings.mPeerConfig.recovery_maxbitrate;
        mRistPeerConfig.recovery_maxbitrate_return = mRistSenderSettings.mPeerConfig.recovery_maxbitrate_return;
        mRistPeerConfig.recovery_length_min = mRistSenderSettings.mPeerConfig.recovery_length_min;
        mRistPeerConfig.recovery_length_max = mRistSenderSettings.mPeerConfig.recovery_length_max;
        mRistPeerConfig.recovery_rtt_min = mRistSenderSettings.mPeerConfig.recovery_rtt_min;
        mRistPeerConfig.recovery_rtt_max = mRistSenderSettings.mPeerConfig.recovery_rtt_max;
        mRistPeerConfig.weight = std::get<1>(rPeerInfo);
        mRistPeerConfig.congestion_control_mode = mRistSenderSettings.mPeerConfig.congestion_control_mode;
        mRistPeerConfig.min_retries = mRistSenderSettings.mPeerConfig.min_retries;
        mRistPeerConfig.max_retries = mRistSenderSettings.mPeerConfig.max_retries;
        mRistPeerConfig.session_timeout = mRistSenderSettings.mSessionTimeout;
        mRistPeerConfig.keepalive_interval =  mRistSenderSettings.mKeepAliveInterval;
        mRistPeerConfig.key_size = keysize;

        if (keysize) {
            strncpy((char *) &mRistPeerConfig.secret[0], mRistSenderSettings.mPSK.c_str(), 128);
        }

        if (!mRistSenderSettings.mCNAME.empty()) {
            strncpy((char *) &mRistPeerConfig.cname[0], mRistSenderSettings.mCNAME.c_str(), 128);
        }

        rist_peer_config* lTmp = &mRistPeerConfig;
        lStatus = rist_parse_address2(peerURL.c_str(), &lTmp);
        if (lStatus)
        {
            LOGGER(true, LOGG_ERROR, "rist_parse_address fail: " << peerURL)
            destroySender();
            return false;
        }

        rist_peer *peer;
        lStatus =  rist_peer_create(mRistContext, &peer, &mRistPeerConfig);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_sender_peer_create fail: " << peerURL)
            destroySender();
            return false;
        }
    }

    if (mRistSenderSettings.mMaxJitter) {
        lStatus = rist_jitter_max_set(mRistContext, mRistSenderSettings.mMaxJitter);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_sender_jitter_max_set fail.")
            destroySender();
            return false;
        }
    }

    lStatus = rist_oob_callback_set(mRistContext, receiveOOBData, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_oob_set fail.")
        destroySender();
        return false;
    }

    lStatus = rist_auth_handler_set(mRistContext, clientConnect, clientDisconnect, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_auth_handler_set fail.")
        destroySender();
        return false;
    }

    lStatus = rist_stats_callback_set(mRistContext, 1000, gotStatistics, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_stats_callback_set fail.")
        destroySender();
        return false;
    }

    lStatus = rist_connection_status_callback_set(mRistContext, connectionStatus, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_conn_status_callback_set fail.")
        destroySender();
        return false;
    }

    lStatus = rist_start(mRistContext);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_start fail.")
        destroySender();
        return false;
    }

    return true;
}

bool RISTNetSender::sendData(const uint8_t *pData, size_t lSize, uint16_t lConnectionID, uint16_t virt_dst_port, uint64_t seq, uint64_t ts_ntp) {
    if (!mRistContext) {
        LOGGER(true, LOGG_ERROR, "RISTNetSender not initialised.")
        return false;
    }

    rist_data_block myRISTDataBlock = {nullptr};
    myRISTDataBlock.payload = pData;
    myRISTDataBlock.payload_len = lSize;
    myRISTDataBlock.flow_id = lConnectionID;
    if (virt_dst_port != 0U)
    {
       myRISTDataBlock.virt_dst_port = virt_dst_port;
    }
    if (seq != 0U) {
        myRISTDataBlock.seq = seq;
    }
    if (ts_ntp != 0U) {
        myRISTDataBlock.ts_ntp = ts_ntp;
    }
    int lStatus = rist_sender_data_write(mRistContext, &myRISTDataBlock);
    if (lStatus < 0) {
        LOGGER(true, LOGG_ERROR, "rist_client_write failed.")
        destroySender();
        return false;
    }

    if (lStatus != lSize) {
        LOGGER(true, LOGG_ERROR, "Did send " << lStatus << " bytes, out of " << lSize << " bytes." )
        return false;
    }

    return true;
}

bool RISTNetSender::sendPkt(const rist_data_block pkt) {
    if (!mRistContext) {
        LOGGER(true, LOGG_ERROR, "RISTNetSender not initialised.")
        return false;
    }

    int lStatus = rist_sender_data_write(mRistContext, &pkt);
    if (lStatus < 0) {
        LOGGER(true, LOGG_ERROR, "rist_client_write failed.")
        destroySender();
        return false;
    }

    if (lStatus != pkt.payload_len) {
        LOGGER(true, LOGG_ERROR, "Did send " << lStatus << " bytes, out of " << pkt.payload_len << " bytes." )
        return false;
    }

    return true;
}

bool RISTNetSender::sendOOBData(rist_peer *pPeer, const uint8_t *pData, size_t lSize) {
    if (!mRistContext) {
        LOGGER(true, LOGG_ERROR, "RISTNetSender not initialised.")
        return false;
    }

    rist_oob_block myOOBBlock = {0};
    myOOBBlock.peer = pPeer;
    myOOBBlock.payload = pData;
    myOOBBlock.payload_len = lSize;

    int lStatus = rist_oob_write(mRistContext, &myOOBBlock);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_oob_write failed.")
        destroySender();
        return false;
    }
    return true;
}

void RISTNetSender::getVersion(uint32_t &rCppWrapper, uint32_t &rRistMajor, uint32_t &rRistMinor) {
    rCppWrapper = CPP_WRAPPER_VERSION;
    rRistMajor = LIBRIST_API_VERSION_MAJOR;
    rRistMinor = LIBRIST_API_VERSION_MINOR;
}

rist_peer_config* RISTNetSender::getPeerConfig() {
    return &mRistPeerConfig;
}
