//******************************************************************************************************
//  SecureSubscriberConnection.h - Gbtc
//
//  Copyright � 2019, Grid Protection Alliance.  All Rights Reserved.
//
//  Licensed to the Grid Protection Alliance (GPA) under one or more contributor license agreements. See
//  the NOTICE file distributed with this work for additional information regarding copyright ownership.
//  The GPA licenses this file to you under the MIT License (MIT), the "License"; you may not use this
//  file except in compliance with the License. You may obtain a copy of the License at:
//
//      http://opensource.org/licenses/MIT
//
//  Unless agreed to in writing, the subject software distributed under the License is distributed on an
//  "AS-IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. Refer to the
//  License for the specific language governing permissions and limitations.
//
//  Code Modification History:
//  ----------------------------------------------------------------------------------------------------
//  02/07/2019 - J. Ritchie Carroll
//       Generated original version of source code.
//
//******************************************************************************************************

#pragma once

#include "../CommonTypes.h"
#include "../Timer.h"
#include "../data/DataSet.h"
#include "SignalIndexCache.h"
#include "TransportTypes.h"
#include "tssc/TSSCEncoder.h"
#include <deque>

namespace sttp {
namespace transport
{
    class DataPublisher;
    typedef sttp::SharedPtr<DataPublisher> DataPublisherPtr;

    class SecureSubscriberConnection;
    typedef sttp::SharedPtr<SecureSubscriberConnection> SecureSubscriberConnectionPtr;

    // Represents a subscriber connection to a data publisher
    class SecureSubscriberConnection : public sttp::EnableSharedThisPtr<SecureSubscriberConnection> // NOLINT
    {
    private:
        static constexpr const uint32_t TSSCBufferSize = 32768U;

        const DataPublisherPtr m_parent;
        sttp::IOContext& m_commandChannelService;
        sttp::Strand m_tcpWriteStrand;
        sttp::Timer m_pingTimer;
        sttp::Guid m_subscriberID;
        const sttp::Guid m_instanceID;
        std::string m_connectionID;
        std::string m_subscriptionInfo;
        uint32_t m_operationalModes;
        uint32_t m_encoding;
        sttp::datetime_t m_startTimeConstraint;
        sttp::datetime_t m_stopTimeConstraint;
        int32_t m_processingInterval;
        bool m_temporalSubscriptionCanceled;
        bool m_usingPayloadCompression;
        bool m_includeTime;
        bool m_useLocalClockAsRealTime;
        float64_t m_lagTime;
        float64_t m_leadTime;
        float64_t m_publishInterval;
        bool m_useMillisecondResolution;
        bool m_trackLatestMeasurements;
        bool m_isNaNFiltered;
        std::atomic_bool m_connectionAccepted;
        std::atomic_bool m_isSubscribed;
        std::atomic_bool m_startTimeSent;
        std::atomic_bool m_dataChannelActive;
        std::atomic_bool m_stopped;

        // Command channel
        sttp::TcpSocket m_commandChannelSocket;
        sttp::SslTcpSocket m_commandSslSocket;
        sttp::SslContext m_commandSslContext;
        std::vector<uint8_t> m_readBuffer;
        std::deque<SharedPtr<std::vector<uint8_t>>> m_tcpWriteBuffers;
        sttp::IPAddress m_ipAddress;
        std::string m_hostName;

        // Data channel
        uint16_t m_udpPort;
        sttp::Mutex m_dataChannelMutex;
        sttp::WaitHandle m_dataChannelWaitHandle;
        sttp::IOContext m_dataChannelService;
        sttp::UdpSocket m_dataChannelSocket;
        sttp::SslUdpSocket m_dataSslSocket;
        sttp::SslContext m_dataSslContext;
        std::string m_ca;
        sttp::Strand m_udpWriteStrand;
        std::deque<SharedPtr<std::vector<uint8_t>>> m_udpWriteBuffers;
        std::vector<uint8_t> m_keys[2];
        std::vector<uint8_t> m_ivs[2];

        // Statistics counters
        uint64_t m_totalCommandChannelBytesSent;
        uint64_t m_totalDataChannelBytesSent;
        uint64_t m_totalMeasurementsSent;

        // Measurement parsing
        SignalIndexCachePtr m_signalIndexCache;
        sttp::SharedMutex m_signalIndexCacheLock;
        TimerPtr m_baseTimeRotationTimer;
        int32_t m_timeIndex;
        int64_t m_baseTimeOffsets[2];
        int64_t m_latestTimestamp;
        datetime_t m_lastPublishTime;
        std::unordered_map<sttp::Guid, MeasurementPtr> m_latestMeasurements;
        sttp::Mutex m_latestMeasurementsLock;
        TimerPtr m_throttledPublicationTimer;
        tssc::TSSCEncoder m_tsscEncoder;
        uint8_t m_tsscWorkingBuffer[TSSCBufferSize];
        bool m_tsscResetRequested;
        uint16_t m_tsscSequenceNumber;

        // Server request handlers
        void HandleSubscribe(uint8_t* data, uint32_t length);
        bool SecureSubscriberConnection::verify_certificate(bool preverified, boost::asio::ssl::verify_context& ctx);
        void HandleSubscribeFailure(const std::string& message);
        void HandleUnsubscribe();
        void HandleMetadataRefresh(uint8_t* data, uint32_t length);
        void HandleRotateCipherKeys();
        void HandleUpdateProcessingInterval(const uint8_t* data, uint32_t length);
        void HandleDefineOperationalModes(uint8_t* data, uint32_t length);
        void HandleUserCommand(uint32_t command, uint8_t* data, uint32_t length);

        SignalIndexCachePtr ParseSubscriptionRequest(const std::string& filterExpression, bool& success);
        void PublishCompactMeasurements(const std::vector<MeasurementPtr>& measurements);
        void PublishCompactDataPacket(const std::vector<uint8_t>& packet, int32_t count);
        void PublishTSSCMeasurements(const std::vector<MeasurementPtr>& measurements);
        void PublishTSSCDataPacket(int32_t count);
        bool SendDataStartTime(uint64_t timestamp);
        void ReadCommandChannel();
        void ReadPayloadHeader(const ErrorCode& error, size_t bytesTransferred);
        void ParseCommand(const ErrorCode& error, size_t bytesTransferred);
        std::vector<uint8_t> SerializeSignalIndexCache(SignalIndexCache& signalIndexCache) const;
        std::vector<uint8_t> SerializeMetadata(const sttp::data::DataSetPtr& metadata) const;
        sttp::data::DataSetPtr FilterClientMetadata(const StringMap<sttp::filterexpressions::ExpressionTreePtr>& filterExpressions) const;
        void CommandChannelSendAsync();
        void CommandChannelWriteHandler(const ErrorCode& error, size_t bytesTransferred);
        void DataChannelSendAsync();
        void DataChannelWriteHandler(const ErrorCode& error, size_t bytesTransferred);

        static void PingTimerElapsed(Timer*, void* userData);
    public:
        SecureSubscriberConnection(DataPublisherPtr parent, sttp::IOContext& commandChannelService);
        ~SecureSubscriberConnection();

        const DataPublisherPtr& GetParent() const;
        SecureSubscriberConnectionPtr GetReference();

        sttp::SslTcpSocket& CommandChannelSocket();
        sttp::SslContext& CommandSslContext();

        // Gets or sets subscriber UUID used when subscriber is known and pre-established
        const sttp::Guid& GetSubscriberID() const;
        void SetSubscriberID(const sttp::Guid& id);

        // Gets a UUID representing a unique run-time identifier for the current subscriber connection,
        // this can be used to disambiguate when the same subscriber makes multiple connections
        const sttp::Guid& GetInstanceID() const;

        // Gets subscriber connection identification, e.g., remote IP/port, for display and logging references
        const std::string& GetConnectionID() const;

        // Gets subscriber remote IP address
        const sttp::IPAddress& GetIPAddress() const;

        // Gets subscriber communications port
        const std::string& GetHostName() const;

        // Gets or sets established subscriber operational modes
        uint32_t GetOperationalModes() const;
        void SetOperationalModes(uint32_t value);

        // Gets established subscriber string encoding
        uint32_t GetEncoding() const;

        // Gets flags that determines if this subscription is temporal based
        bool GetIsTemporalSubscription() const;

        // Gets or sets the start time temporal processing constraint
        const sttp::datetime_t& GetStartTimeConstraint() const;
        void SetStartTimeConstraint(const sttp::datetime_t& value);

        // Gets or sets the stop time temporal processing constraint
        const sttp::datetime_t& GetStopTimeConstraint() const;
        void SetStopTimeConstraint(const sttp::datetime_t& value);

        // Gets or sets the desired processing interval, in milliseconds, for temporal history playback.
        // With the exception of the values of -1 and 0, this value specifies the desired processing interval for data, i.e.,
        // basically a delay, or timer interval, over which to process data. A value of -1 means to use the default processing
        // interval while a value of 0 means to process data as fast as possible.
        int32_t GetProcessingInterval() const;
        void SetProcessingInterval(int32_t value);

        // Gets or sets flag that determines if payload compression should be enabled in data packets
        bool GetUsingPayloadCompression() const;

        // Gets or sets flag that determines if the compact measurement format should be used in data packets
        bool GetUsingCompactMeasurementFormat() const;

        // Gets or sets flag that determines if time should be included in data packets when the compact measurement format used
        bool GetIncludeTime() const;
        void SetIncludeTime(bool value);

        // Gets or sets flag that determines if local clock should be used for current time instead of latest reasonable timestamp
        // when using compact format with rotation of base time offsets
        bool GetUseLocalClockAsRealTime() const;
        void SetUseLocalClockAsRealTime(bool value);

        // Gets or sets the allowed past time deviation tolerance, in seconds (can be sub-second). This value is used to determine
        // reasonability of timestamps as compared to the local clock when using compact format and base time offsets.
        float64_t GetLagTime() const;
        void SetLagTime(float64_t value);

        // Gets or sets the allowed future time deviation tolerance, in seconds (can be sub-second). This value is used to determine
        // reasonability of timestamps as compared to the local clock when using compact format and base time offsets.
        float64_t GetLeadTime() const;
        void SetLeadTime(float64_t value);
        
        // Gets or sets value used to control throttling speed for real-time subscriptions when tracking latest measurements.
        float64_t GetPublishInterval() const;
        void SetPublishInterval(float64_t value);

        // Gets or sets flag that determines if time should be restricted to millisecond resolution in data packets when the
        // compact measurement format used; otherwise, full resolution time will be used
        bool GetUseMillisecondResolution() const;
        void SetUseMillisecondResolution(bool value);

        // Gets or sets flag that determines if latest measurements should tracked for subscription throttling. When property is true,
        // subscription data speed will be reduced by the lag-time property for real-time subscriptions and the processing interval
        // property for temporal subscriptions.
        bool GetTrackLatestMeasurements() const;
        void SetTrackLatestMeasurements(bool value);

        // Gets or sets flag that determines if NaN values should be excluded from data packets
        bool GetIsNaNFiltered() const;
        void SetIsNaNFiltered(bool value);

        // Gets or sets flag that determines if connection is currently subscribed
        bool GetIsSubscribed() const;
        void SetIsSubscribed(bool value);

        // Gets or sets subscription details about subscriber
        const std::string& GetSubscriptionInfo() const;
        void SetSubscriptionInfo(const std::string& value);

        // Gets or sets signal index cache for subscriber representing run-time mappings for subscribed points
        const SignalIndexCachePtr& GetSignalIndexCache();
        void SetSignalIndexCache(SignalIndexCachePtr signalIndexCache);

        // Statistical functions
        uint64_t GetTotalCommandChannelBytesSent() const;
        uint64_t GetTotalDataChannelBytesSent() const;
        uint64_t GetTotalMeasurementsSent() const;

        bool CipherKeysDefined() const;
        std::vector<uint8_t> Keys(int32_t cipherIndex);
        std::vector<uint8_t> IVs(int32_t cipherIndex);

        void Start(bool connectionAccepted = true);
        void Stop(bool shutdownSocket = true);

        void PublishMeasurements(const std::vector<MeasurementPtr>& measurements);
        void CancelTemporalSubscription();

        bool SendResponse(uint8_t responseCode, uint8_t commandCode);
        bool SendResponse(uint8_t responseCode, uint8_t commandCode, const std::string& message);
        bool SendResponse(uint8_t responseCode, uint8_t commandCode, const std::vector<uint8_t>& data);

        std::string DecodeString(const uint8_t* data, uint32_t offset, uint32_t length) const;
        std::vector<uint8_t> EncodeString(const std::string& value) const;
    };

    typedef sttp::SharedPtr<SecureSubscriberConnection> SecureSubscriberConnectionPtr;

}}

// Setup standard hash code for SecureSubscriberConnectionPtr
namespace std  // NOLINT
{
    template<>
    struct hash<sttp::transport::SecureSubscriberConnectionPtr>
    {
        size_t operator () (const sttp::transport::SecureSubscriberConnectionPtr& connection) const noexcept
        {
            return boost::hash<sttp::transport::SecureSubscriberConnectionPtr>()(connection);
        }
    };
}