#include <condition_variable>
#include <thread>

#include <gtest/gtest.h>

#include "RISTNet.h"

const std::string kValidPsk = "Th1$_is_4n_0pt10N4L_P$k";
const std::string kInvalidPsk = "Th1$_is_4_F4k3_P$k";

const std::chrono::seconds kConnectTimeout = std::chrono::seconds(2);
const std::chrono::seconds kDisconnectTimeout = std::chrono::seconds(10);
const std::chrono::seconds kReceiveTimeout = std::chrono::seconds(2);

class TestFixtureReceiver : public ::testing::Test {
protected:
    bool checkSenderConnecting() {
        std::unique_lock<std::mutex> lock(mConnectedMutex);
        bool successfulWait = mConnectedCondition.wait_for(lock, kConnectTimeout, [&]() { return mConnected; });
        mConnected = false;
        return successfulWait;
    }

    void SetUp() override {
        mConnected = false;
        mReceiver = std::make_unique<RISTNetReceiver>();
        mReceiverSettings.mPSK = kValidPsk;
        mReceiverCtx = std::make_shared<RISTNetReceiver::NetworkConnection>();

        mReceiver->validateConnectionCallback = [&](const std::string& ipAddress, uint16_t port) {
            EXPECT_EQ(ipAddress, "127.0.0.1");
            {
                std::lock_guard<std::mutex> lock(mConnectedMutex);
                mConnected = true;
            }
            mConnectedCondition.notify_one();
            return mReceiverCtx;
        };

        mReceiverInterfaces.emplace_back("rist://@0.0.0.0:8000");
        ASSERT_TRUE(mReceiver->initReceiver(mReceiverInterfaces, mReceiverSettings));
    }

    void TearDown() override {
        mReceiverCtx.reset();
        mReceiver.reset();
        mReceiverInterfaces.clear();
    }

    std::unique_ptr<RISTNetReceiver> mReceiver;
    std::vector<std::string> mReceiverInterfaces;
    RISTNetReceiver::RISTNetReceiverSettings mReceiverSettings;
    std::shared_ptr<RISTNetReceiver::NetworkConnection> mReceiverCtx;

private:
    std::condition_variable mConnectedCondition;
    std::mutex mConnectedMutex;
    bool mConnected = false;
};

class TestFixture : public TestFixtureReceiver {
protected:
    bool checkSenderDisconnecting() {
        std::unique_lock<std::mutex> lock(mDisconnectMutex);
        // TODO Does not work with 6 sec or shorter wait, see STAR-202
        bool successfulWait = mDisconnectCondition.wait_for(lock, kDisconnectTimeout, [&]() { return mDisconnected; });
        mDisconnected = false;
        return successfulWait;
    }

    void SetUp() override {
        TestFixtureReceiver::SetUp();
        mDisconnected = false;
        mSenderSettings.mPSK = kValidPsk;

        mSenderInterfaces.emplace_back("rist://127.0.0.1:8000", 0);
        mSender = std::make_unique<RISTNetSender>();
        ASSERT_TRUE(mSender->initSender(mSenderInterfaces, mSenderSettings));

        ASSERT_TRUE(checkSenderConnecting()) << "Timeout waiting for sender to connect";

        // notice when sender disconnects
        mReceiver->clientDisconnectedCallback =
            [&](const std::shared_ptr<RISTNetReceiver::NetworkConnection>& connection, const rist_peer& peer) {
                {
                    std::lock_guard<std::mutex> lock(mDisconnectMutex);
                    mDisconnected = true;
                }
                mDisconnectCondition.notify_one();
            };
    }

    void TearDown() override {
        mSender.reset();
        TestFixtureReceiver::TearDown();
        mSenderInterfaces.clear();
    }

    std::unique_ptr<RISTNetSender> mSender;
    std::vector<std::tuple<std::string, int>> mSenderInterfaces;
    RISTNetSender::RISTNetSenderSettings mSenderSettings;

private:
    std::condition_variable mDisconnectCondition;
    std::mutex mDisconnectMutex;
    bool mDisconnected = false;
};

TEST(TestRist, BuildRistUrl) {
    std::string url;
    EXPECT_TRUE(RISTNetTools::buildRISTURL("0.0.0.0", "8000", url, true));
    EXPECT_EQ(url, "rist://@0.0.0.0:8000");
    EXPECT_TRUE(RISTNetTools::buildRISTURL("127.0.0.1", "65535", url, false));
    EXPECT_EQ(url, "rist://127.0.0.1:65535");
    EXPECT_TRUE(RISTNetTools::buildRISTURL("::", "9000", url, true));
    EXPECT_EQ(url, "rist6://@[::]:9000");

    EXPECT_FALSE(RISTNetTools::buildRISTURL("example.com", "9000", url, true));
    EXPECT_FALSE(RISTNetTools::buildRISTURL("0.0.0.0", "65536", url, true));
}

TEST(TestRist, Init) {
    RISTNetReceiver receiver;
    std::vector<std::string> receiverInterfaces;
    RISTNetReceiver::RISTNetReceiverSettings receiverSettings;
    receiverSettings.mPSK = kValidPsk;

    receiver.validateConnectionCallback = [&](const std::string& ipAddress, uint16_t port) {
        return std::make_shared<RISTNetReceiver::NetworkConnection>();
    };

    EXPECT_FALSE(receiver.initReceiver(receiverInterfaces, receiverSettings));
    receiverInterfaces.emplace_back("");
    EXPECT_FALSE(receiver.initReceiver(receiverInterfaces, receiverSettings));
    receiverInterfaces.clear();
    receiverInterfaces.emplace_back("rist://@0.0.0.0:8000");
    ASSERT_TRUE(receiver.initReceiver(receiverInterfaces, receiverSettings));

    RISTNetSender sender;
    std::vector<std::tuple<std::string, int>> senderInterfaces;
    RISTNetSender::RISTNetSenderSettings senderSettings;
    senderSettings.mPSK = kValidPsk;

    EXPECT_FALSE(sender.initSender(senderInterfaces, senderSettings));
    senderInterfaces.emplace_back("", 1);
    EXPECT_FALSE(sender.initSender(senderInterfaces, senderSettings));
    senderInterfaces.clear();
    senderInterfaces.emplace_back("rist://127.0.0.1:8000", 1);
    ASSERT_TRUE(sender.initSender(senderInterfaces, senderSettings));
}

TEST_F(TestFixture, StartStop) {
    mReceiver->getActiveClients(
        [&](std::map<rist_peer*, std::shared_ptr<RISTNetReceiver::NetworkConnection>>& activeClients) {
            EXPECT_EQ(activeClients.size(), 1);
        });

    mSender->destroySender();
    EXPECT_TRUE(checkSenderDisconnecting()) << "Timeout waiting for sender disconnect";
}

// TODO Enable test when STAR-255 is fixed
TEST_F(TestFixture, DISABLED_ReceiverCloseConnections) {
    rist_peer* client = nullptr;
    mReceiver->getActiveClients(
        [&](std::map<rist_peer*, std::shared_ptr<RISTNetReceiver::NetworkConnection>>& activeClients) {
            ASSERT_EQ(activeClients.size(), 1);
            client = activeClients.begin()->first;
        });

    RISTNetSender sender2;
    ASSERT_TRUE(sender2.initSender(mSenderInterfaces, mSenderSettings));

    ASSERT_TRUE(checkSenderConnecting()) << "Timeout waiting for sender2 to connect";
    mReceiver->getActiveClients(
        [&](std::map<rist_peer*, std::shared_ptr<RISTNetReceiver::NetworkConnection>>& activeClients) {
            EXPECT_EQ(activeClients.size(), 2);
        });

    EXPECT_TRUE(mReceiver->closeClientConnection(client));
    EXPECT_TRUE(checkSenderDisconnecting()) << "Timeout waiting for sender disconnect";
    mReceiver->getActiveClients(
        [&](std::map<rist_peer*, std::shared_ptr<RISTNetReceiver::NetworkConnection>>& activeClients) {
            EXPECT_EQ(activeClients.size(), 1);
        });

    mReceiver->closeAllClientConnections();
    EXPECT_TRUE(checkSenderDisconnecting()) << "Timeout waiting for sender2 disconnect";
    mReceiver->getActiveClients(
        [&](std::map<rist_peer*, std::shared_ptr<RISTNetReceiver::NetworkConnection>>& activeClients) {
            EXPECT_EQ(activeClients.size(), 0);
        });
}

TEST_F(TestFixture, SendReceive) {
    const uint16_t kSentPackets = 5;
    const uint16_t kBufferSize = 1024;

    std::condition_variable receiverCondition;
    std::mutex receiverMutex;
    uint16_t nReceivedPackets = 0;
    mReceiver->networkDataCallback = [&](const uint8_t* buf, size_t size,
                                         std::shared_ptr<RISTNetReceiver::NetworkConnection>& connection,
                                         rist_peer* peer, uint16_t connectionId) {
        EXPECT_EQ(connection, mReceiverCtx);
        EXPECT_EQ(size, kBufferSize);
        {
            std::lock_guard<std::mutex> lock(receiverMutex);
            EXPECT_EQ(*buf, '0' + nReceivedPackets);
            ++nReceivedPackets;
        }
        receiverCondition.notify_one();
        return 0;
    };

    std::atomic<size_t> receiverStatisticCounter = 0;
    mReceiver->statisticsCallback = [&](const rist_stats& stats) {
        receiverStatisticCounter++;
    };
    std::atomic<size_t> senderStatisticCounter = 0;
    mSender->statisticsCallback = [&](const rist_stats& stats) {
        senderStatisticCounter++;
    };

    std::vector<uint8_t> sendBuffer(kBufferSize);
    for (auto i = 0; i < kSentPackets; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::fill(sendBuffer.begin(), sendBuffer.end(), '0' + i);
        EXPECT_TRUE(mSender->sendData(static_cast<const uint8_t*>(sendBuffer.data()), sendBuffer.size()));
    }

    std::this_thread::sleep_for(std::chrono::seconds(10));

    size_t numCallsReceiver = receiverStatisticCounter;
    EXPECT_GE(numCallsReceiver, 9) << "Expected the statistics to be delivered at least 9 times for receiver";
    size_t numCallsSender = senderStatisticCounter;
    EXPECT_GE(numCallsSender, 9) << "Expected the statistics to be delivered at least 9 times for sender";

    {
        std::unique_lock<std::mutex> lock(receiverMutex);
        bool successfulWait =
            receiverCondition.wait_for(lock, kReceiveTimeout, [&]() { return nReceivedPackets == kSentPackets; });
        EXPECT_TRUE(successfulWait) << "Timeout waiting for receiving data from sender, sent " << kSentPackets
                                    << " packet(s) and received " << nReceivedPackets << " packet(s).";
    }
}

// TODO Enable test when STAR-38 is fixed.
TEST(TestRist, DISABLED_TestPsk) {
    RISTNetReceiver receiver;
    std::vector<std::string> receiverInterfaces{"rist://@0.0.0.0:8000"};
    RISTNetReceiver::RISTNetReceiverSettings receiverSettings;
    receiverSettings.mPSK = kValidPsk;

    receiver.validateConnectionCallback = [&](const std::string& ipAddress, uint16_t port) {
        return std::make_shared<RISTNetReceiver::NetworkConnection>();
    };

    ASSERT_TRUE(receiver.initReceiver(receiverInterfaces, receiverSettings));

    std::vector<std::tuple<std::string, int>> senderInterfaces{
        std::tuple<std::string, int>("rist://127.0.0.1:8000", 0)};
    RISTNetSender::RISTNetSenderSettings senderSettings;
    senderSettings.mPSK = kInvalidPsk;
    RISTNetSender sender;
    EXPECT_FALSE(sender.initSender(senderInterfaces, senderSettings));

    senderSettings.mPSK = kValidPsk;
    EXPECT_TRUE(sender.initSender(senderInterfaces, senderSettings));
}

TEST_F(TestFixture, LargeMessage) {
    const uint16_t kBufferSize = 10'000 - 32; // Largest packet allowed: RIST_MAX_PACKET_SIZE - 32
    std::vector<uint8_t> sendBuffer(kBufferSize, 1);
    EXPECT_TRUE(mSender->sendData((const uint8_t*)sendBuffer.data(), sendBuffer.size()));

    std::vector<uint8_t> largeBuffer(kBufferSize + 1, 1);
    EXPECT_FALSE(mSender->sendData((const uint8_t*)largeBuffer.data(), largeBuffer.size()));
}

// TODO Enable test when STAR-260 is fixed
TEST_F(TestFixtureReceiver, DISABLED_RejectConnection) {
    mReceiverCtx = nullptr;

    std::vector<std::tuple<std::string, int>> senderInterfaces{
        std::tuple<std::string, int>("rist://127.0.0.1:8000", 0)};
    RISTNetSender::RISTNetSenderSettings senderSettings;
    senderSettings.mPSK = kValidPsk;
    RISTNetSender sender;
    EXPECT_FALSE(sender.initSender(senderInterfaces, senderSettings)) << "Expected sender connection rejected";
    EXPECT_TRUE(checkSenderConnecting()) << "Timeout waiting for sender to connect";

    mReceiver->getActiveClients(
        [&](std::map<rist_peer*, std::shared_ptr<RISTNetReceiver::NetworkConnection>>& activeClients) {
            EXPECT_EQ(activeClients.size(), 0);
        });
}

// Test: Verify iterator bug fix in closeAllClientConnections
// This test ensures that calling closeAllClientConnections multiple times
// does not cause crashes or iterator invalidation issues.
TEST_F(TestFixture, CloseAllClientConnectionsIterationSafety) {
    // Connect one sender
    ASSERT_TRUE(checkSenderConnecting()) << "Timeout waiting for sender to connect";
    mReceiver->getActiveClients(
        [&](std::map<rist_peer*, std::shared_ptr<RISTNetReceiver::NetworkConnection>>& activeClients) {
            ASSERT_EQ(activeClients.size(), 1);
        });

    // First call should close all connections
    mReceiver->closeAllClientConnections();

    // Second call should be safe (no crash) even with empty client list
    // This verifies the iterator fix - previously this could crash due to callback
    // triggering during iteration
    mReceiver->closeAllClientConnections();

    // Third call to verify idempotency
    mReceiver->closeAllClientConnections();

    // Verify no clients remain
    mReceiver->getActiveClients(
        [&](std::map<rist_peer*, std::shared_ptr<RISTNetReceiver::NetworkConnection>>& activeClients) {
            EXPECT_EQ(activeClients.size(), 0);
        });
}

// Test: Verify RIST_PROFILE_SIMPLE can be configured
TEST(TestRist, ProfileSimpleConfiguration) {
    RISTNetReceiver receiver;
    std::vector<std::string> receiverInterfaces{"rist://@0.0.0.0:8001"};
    RISTNetReceiver::RISTNetReceiverSettings receiverSettings;
    receiverSettings.mPSK = kValidPsk;
    receiverSettings.mProfile = RIST_PROFILE_SIMPLE;

    receiver.validateConnectionCallback = [&](const std::string& ipAddress, uint16_t port) {
        return std::make_shared<RISTNetReceiver::NetworkConnection>();
    };

    ASSERT_TRUE(receiver.initReceiver(receiverInterfaces, receiverSettings));

    // Verify profile was set correctly
    rist_peer_config* peerConfig = receiver.getPeerConfig();
    EXPECT_NE(peerConfig, nullptr);
}

// Test: Verify RIST_PROFILE_MAIN can be configured
TEST(TestRist, ProfileMainConfiguration) {
    RISTNetReceiver receiver;
    std::vector<std::string> receiverInterfaces{"rist://@0.0.0.0:8002"};
    RISTNetReceiver::RISTNetReceiverSettings receiverSettings;
    receiverSettings.mPSK = kValidPsk;
    receiverSettings.mProfile = RIST_PROFILE_MAIN;

    receiver.validateConnectionCallback = [&](const std::string& ipAddress, uint16_t port) {
        return std::make_shared<RISTNetReceiver::NetworkConnection>();
    };

    ASSERT_TRUE(receiver.initReceiver(receiverInterfaces, receiverSettings));

    rist_peer_config* peerConfig = receiver.getPeerConfig();
    EXPECT_NE(peerConfig, nullptr);
}

// Test: Verify RIST_PROFILE_ADVANCED can be configured
TEST(TestRist, ProfileAdvancedConfiguration) {
    RISTNetReceiver receiver;
    std::vector<std::string> receiverInterfaces{"rist://@0.0.0.0:8003"};
    RISTNetReceiver::RISTNetReceiverSettings receiverSettings;
    receiverSettings.mPSK = kValidPsk;
    receiverSettings.mProfile = RIST_PROFILE_ADVANCED;

    receiver.validateConnectionCallback = [&](const std::string& ipAddress, uint16_t port) {
        return std::make_shared<RISTNetReceiver::NetworkConnection>();
    };

    ASSERT_TRUE(receiver.initReceiver(receiverInterfaces, receiverSettings));

    rist_peer_config* peerConfig = receiver.getPeerConfig();
    EXPECT_NE(peerConfig, nullptr);
}

// Test: Verify profile settings work with sender initialization
TEST(TestRist, SenderProfileSimpleConfiguration) {
    RISTNetSender sender;
    std::vector<std::tuple<std::string, int>> senderInterfaces{
        std::tuple<std::string, int>("rist://127.0.0.1:8001", 0)};
    RISTNetSender::RISTNetSenderSettings senderSettings;
    senderSettings.mProfile = RIST_PROFILE_SIMPLE;

    ASSERT_TRUE(sender.initSender(senderInterfaces, senderSettings));
}

TEST(TestRist, SenderProfileMainConfiguration) {
    RISTNetSender sender;
    std::vector<std::tuple<std::string, int>> senderInterfaces{
        std::tuple<std::string, int>("rist://127.0.0.1:8002", 0)};
    RISTNetSender::RISTNetSenderSettings senderSettings;
    senderSettings.mProfile = RIST_PROFILE_MAIN;

    ASSERT_TRUE(sender.initSender(senderInterfaces, senderSettings));
}

TEST(TestRist, SenderProfileAdvancedConfiguration) {
    RISTNetSender sender;
    std::vector<std::tuple<std::string, int>> senderInterfaces{
        std::tuple<std::string, int>("rist://127.0.0.1:8003", 0)};
    RISTNetSender::RISTNetSenderSettings senderSettings;
    senderSettings.mProfile = RIST_PROFILE_ADVANCED;

    ASSERT_TRUE(sender.initSender(senderInterfaces, senderSettings));
}
