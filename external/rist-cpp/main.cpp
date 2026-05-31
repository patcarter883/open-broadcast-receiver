//
// Created by Anders Cedronius (Edgeware AB) on 2020-03-15.
//

#include <iostream>
#include <algorithm>
#include <thread>
#include "RISTNet.h"

#ifdef WIN32
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#include <Winsock2.h>
#define _WINSOCKAPI_
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

//Create the receiver
RISTNetReceiver myRISTNetReceiver;

int packetCounter;

//This is my class managed by the network connection.
class MyClass {
public:
    MyClass() {
        someVariable = 10;
        std::cout << "My class is now created and some variable is containing the value: " << unsigned(someVariable)
                  << std::endl;
    };

    virtual ~MyClass() {
        std::cout << "My class is now destroyed and some variable is containing the value: " << unsigned(someVariable)
                  << std::endl;
    };
    int someVariable = 0;
};

//Return a connection object. (Return nullptr if you don't want to connect to that client)
std::shared_ptr<RISTNetReceiver::NetworkConnection> validateConnection(const std::string &ipAddress, uint16_t port) {
    std::cout << "Connecting IP: " << ipAddress << ":" << unsigned(port) << std::endl;

    //Do we want to allow this connection?
    //Do we have enough resources to accept this connection...

    // if not then -> return nullptr;
    // else return a ptr to a NetworkConnection.
    // this NetworkConnection may contain a pointer to any C++ object you provide.
    // That object ptr will be passed to you when the client communicates with you.
    // If the network connection is dropped the destructor in your class is called as long
    // as you do not also hold a reference to that pointer since it's shared.

    auto netConn = std::make_shared<RISTNetReceiver::NetworkConnection>(); // Create the network connection
    netConn->mObject = std::make_shared<MyClass>(); // Attach your object.
    return netConn;
}

int
dataFromSender(const uint8_t *buf, size_t len, std::shared_ptr<RISTNetReceiver::NetworkConnection> &connection,
               rist_peer *pPeer, uint16_t connectionID) {
    //Get back your class like this ->
    if (connection) {
        auto v = std::any_cast<std::shared_ptr<MyClass> &>(connection->mObject);
        v->someVariable++;
    }

    //Check the vector integrity
    bool testFail = false;
    for (int x = 0; x < len; x++) {
        if (buf[x] != (x & 0xff)) {
            testFail = true;
        }
    }

    if (testFail) {
        std::cout << "Did not receive the correct data" << std::endl;
        packetCounter++;
    } else {
        std::cout << "Got " << unsigned(len) << " expected data from connection id: " << unsigned(connectionID)
                  << std::endl;
    }

    return 0; //Keep connection
}

void oobDataFromReceiver(const uint8_t *buf, size_t len, std::shared_ptr<RISTNetSender::NetworkConnection> &connection,
                         rist_peer *pPeer) {

    if (connection->mObject.has_value()) {
        //You are the server so the connection has a Context
        std::cout << "Got " << unsigned(len) << " bytes of oob data from the receiver" << std::endl;
    } else {
        //You are not the server so the connection does not have a context (meaning you connected to the server and the server sends OOB to you).
        std::cout << "Got " << unsigned(len) << " bytes of oob data from the receiver" << std::endl;
    }

}

void clientDisconnect(const std::shared_ptr<RISTNetReceiver::NetworkConnection>& connection, const rist_peer& peer) {
    std::cout << "Client disconnected from receiver";
    if (connection) {
        auto v = std::any_cast<std::shared_ptr<MyClass> &>(connection->mObject);
        if (v) {
            std::cout << ", some variable is containing the value: " << v->someVariable << std::endl;
        } else {
            std::cout << ", ERROR: no object found!" << std::endl;
            std::flush(std::cout);
        }
    } else {
        std::cout << ", ERROR: no connection object." << std::endl;
    }
}

int main() {

    uint32_t cppWrapperVersion;
    uint32_t ristMajor;
    uint32_t ristMinor;
    RISTNetReceiver::getVersion(cppWrapperVersion, ristMajor, ristMinor);
    std::cout << "cppRISTWrapper version: " << unsigned(cppWrapperVersion) << " librist version: "
              << unsigned(ristMajor) << "." << unsigned(ristMinor) << std::endl;

    packetCounter = 0;

    //validate the connecting client
    myRISTNetReceiver.validateConnectionCallback =
            std::bind(&validateConnection, std::placeholders::_1, std::placeholders::_2);
    //receive data from the client
    myRISTNetReceiver.networkDataCallback =
            std::bind(&dataFromSender, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
                      std::placeholders::_4, std::placeholders::_5);
    // client has disconnected
    myRISTNetReceiver.clientDisconnectedCallback =
        std::bind(&clientDisconnect, std::placeholders::_1, std::placeholders::_2);

    //---------------------
    //
    // set-up the receiver
    //
    //---------------------

    //Generate a vector of RIST URL's,  ip(name), ports, RIST URL output, listen(true) or send mode (false)
    std::string lURL;
    std::vector<std::string> interfaceListReceiver;
    if (RISTNetTools::buildRISTURL("0.0.0.0", "8000", lURL, true)) {
        interfaceListReceiver.push_back(lURL);
    }
    if (RISTNetTools::buildRISTURL("0.0.0.0", "9000", lURL, true)) {
        interfaceListReceiver.push_back(lURL);
    }

    //Populate the settings
    RISTNetReceiver::RISTNetReceiverSettings myReceiveConfiguration;
    myReceiveConfiguration.mLogLevel = RIST_LOG_WARN;
    //myReceiveConfiguration.mPSK = "fdijfdoijfsopsmcfjiosdmcjfiompcsjofi33849384983943"; //Enable encryption by providing a PSK

    //Initialize the receiver
    if (!myRISTNetReceiver.initReceiver(interfaceListReceiver, myReceiveConfiguration)) {
        std::cout << "Failed starting the server" << std::endl;
        return EXIT_FAILURE;
    }

    //---------------------
    //
    // set-up the sender
    //
    //---------------------

    //Create a sender.
    RISTNetSender myRISTNetSender;

    myRISTNetSender.networkOOBDataCallback = std::bind(&oobDataFromReceiver, std::placeholders::_1,
                                                       std::placeholders::_2, std::placeholders::_3,
                                                       std::placeholders::_4);

    //Generate a vector of RIST URL's,  ip(name), ports, RIST URL output, listen(true) or send mode (false)
    std::vector<std::tuple<std::string, int>> interfaceListSender;
    if (RISTNetTools::buildRISTURL("127.0.0.1", "8000", lURL, false)) {
        interfaceListSender.push_back(std::tuple<std::string, int>(lURL,5));
    }

    RISTNetSender::RISTNetSenderSettings mySendConfiguration;
    mySendConfiguration.mLogLevel = RIST_LOG_WARN;
    //mySendConfiguration.mPSK = "fdijfdoijfsopsmcfjiosdmcjfiompcsjofi33849384983943"; //Enable encryption by providing a PSK
    auto retVal = myRISTNetSender.initSender(interfaceListSender, mySendConfiguration);
    if (!retVal) {
        std::cout << "initSender fail" << std::endl;
    }

    std::vector<uint8_t> mydata(1000);
    std::generate(mydata.begin(), mydata.end(), [n = 0]() mutable { return n++; });
    while (packetCounter++ < 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::cout << "Send packet" << std::endl;
        myRISTNetSender.sendData((const uint8_t *) mydata.data(), mydata.size());
    }

    myRISTNetReceiver.getActiveClients(
            [&](std::map<rist_peer *, std::shared_ptr<RISTNetReceiver::NetworkConnection>> &rClientList) {
                for (auto &rPeer: rClientList) {
                    std::cout << "Send OOB message" << std::endl;
                    myRISTNetReceiver.sendOOBData(rPeer.first, mydata.data(), mydata.size());
                }
            }
    );

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "RIST test end" << std::endl;

    return EXIT_SUCCESS;

}
