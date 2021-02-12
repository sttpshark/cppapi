//******************************************************************************************************
//  SecureDataPublisher.h - Gbtc
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
//  10/25/2018 - J. Ritchie Carroll
//       Generated original version of source code.
//
//******************************************************************************************************

#pragma once

#include "../CommonTypes.h"
#include "../ThreadSafeQueue.h"
#include "../ThreadPool.h"
#include "../data/DataSet.h"
#include "SecureSubscriberConnection.h"
#include "RoutingTables.h"
#include "TransportTypes.h"
#include "Constants.h"

namespace sttp {
namespace filterexpressions
{
    class ExpressionTree;
    typedef SharedPtr<ExpressionTree> ExpressionTreePtr;
}}

namespace sttp {
namespace transport
{
    class SecureDataPublisher : public EnableSharedThisPtr<SecureDataPublisher> // NOLINT
    {
    public:
        // Function pointer types
        typedef std::function<void(SecureDataPublisher*, const std::vector<uint8_t>&)> DispatcherFunction;
        typedef std::function<void(SecureDataPublisher*, const std::string&)> MessageCallback;
        typedef std::function<void(SecureDataPublisher*, const SecureSubscriberConnectionPtr&)> SecureSubscriberConnectionCallback;
        typedef std::function<void(SecureDataPublisher*, const SecureSubscriberConnectionPtr&, uint32_t, const std::vector<uint8_t>&)> UserCommandCallback;

    private:
        // Structure used to dispatch
        // callbacks on the callback thread.
        struct CallbackDispatcher
        {
            SecureDataPublisher* Source;
            SharedPtr<std::vector<uint8_t>> Data;
            DispatcherFunction Function;

            CallbackDispatcher();
        };

        Guid m_nodeID;
        data::DataSetPtr m_metadata;
        data::DataSetPtr m_filteringMetadata;
        RoutingTables m_routingTables;
        std::unordered_set<SecureSubscriberConnectionPtr> m_secureSubscriberConnections;
        SharedMutex m_secureSubscriberConnectionsLock;
        SecurityMode m_securityMode;
        int32_t m_maximumAllowedConnections;
        bool m_isMetadataRefreshAllowed;
        bool m_isNaNValueFilterAllowed;
        bool m_isNaNValueFilterForced;
        bool m_supportsTemporalSubscriptions;
        bool m_useBaseTimeOffsets;
        uint32_t m_cipherKeyRotationPeriod;
        std::atomic_bool m_started;
        std::atomic_bool m_shuttingDown;
        std::atomic_bool m_stopped;
        Mutex m_connectActionMutex;
        Thread m_shutdownThread;
        ThreadPool m_threadPool;
        void* m_userData;

        // Dispatch reference - unordered map needed to manage reference
        // counts on subscriber connections since these are persisted
        std::unordered_map<SecureSubscriberConnectionPtr, uint32_t> m_secureSubscriberConnectionDispatchRefs;
        Mutex m_secureSubscriberConnectionDispatchRefsLock;

        // Callback queue
        Thread m_callbackThread;
        ThreadSafeQueue<CallbackDispatcher> m_callbackQueue;

        // Command channel
        Thread m_commandChannelAcceptThread;
        IOContext m_commandChannelService;
        TcpAcceptor m_clientAcceptor;

        // Command channel handlers
        void StartAccept();
        void AcceptConnection(const SecureSubscriberConnectionPtr& connection, const ErrorCode& error);
        void ConnectionTerminated(const SecureSubscriberConnectionPtr& connection);
        void RemoveConnection(const SecureSubscriberConnectionPtr& connection);

        // Callbacks
        MessageCallback m_statusMessageCallback;
        MessageCallback m_errorMessageCallback;
        SecureSubscriberConnectionCallback m_clientConnectedCallback;
        SecureSubscriberConnectionCallback m_clientDisconnectedCallback;
        SecureSubscriberConnectionCallback m_processingIntervalChangeRequestedCallback;
        SecureSubscriberConnectionCallback m_temporalSubscriptionRequestedCallback;
        SecureSubscriberConnectionCallback m_temporalSubscriptionCanceledCallback;
        UserCommandCallback m_userCommandCallback;

        SecureSubscriberConnection* AddDispatchReference(SecureSubscriberConnectionPtr connectionRef);
        SecureSubscriberConnectionPtr ReleaseDispatchReference(SecureSubscriberConnection* connectionPtr);

        // Dispatchers
        void Dispatch(const DispatcherFunction& function);
        void Dispatch(const DispatcherFunction& function, const uint8_t* data, uint32_t offset, uint32_t length);
        void DispatchStatusMessage(const std::string& message);
        void DispatchErrorMessage(const std::string& message);
        void DispatchClientConnected(SecureSubscriberConnection* connection);
        void DispatchClientDisconnected(SecureSubscriberConnection* connection);
        void DispatchProcessingIntervalChangeRequested(SecureSubscriberConnection* connection);
        void DispatchTemporalSubscriptionRequested(SecureSubscriberConnection* connection);
        void DispatchTemporalSubscriptionCanceled(SecureSubscriberConnection* connection);
        void DispatchUserCommand(SecureSubscriberConnection* connection, uint32_t command, const uint8_t* data, uint32_t length);

        static void StatusMessageDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer);
        static void ErrorMessageDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer);
        static void ClientConnectedDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer);
        static void ClientDisconnectedDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer);
        static void ProcessingIntervalChangeRequestedDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer);
        static void TemporalSubscriptionRequestedDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer);
        static void TemporalSubscriptionCanceledDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer);
        static void UserCommandDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer);
        static int32_t GetColumnIndex(const data::DataTablePtr& table, const std::string& columnName);

        void ShutDown(bool joinThread);
        bool IsShuttingDown() const { return m_shuttingDown || m_stopped; }
    
    public:
        // Creates a new instance of the data publisher.
        SecureDataPublisher();

        // The following constructors will auto-start SecureDataPublisher using specified connection info
        SecureDataPublisher(const TcpEndPoint& endpoint);
        SecureDataPublisher(uint16_t port, bool ipV6 = false);                        // Bind to default NIC
        SecureDataPublisher(const std::string& networkInterfaceIP, uint16_t port);    // Bind to specified NIC IP, format determines IP version

        // Releases all threads and sockets
        // tied up by the publisher.
        ~SecureDataPublisher() noexcept;

        // Iterator handler delegates
        typedef std::function<void(SecureSubscriberConnectionPtr, void* userData)> SecureSubscriberConnectionIteratorHandlerFunction;

        // Defines metadata from existing metadata records
        void DefineMetadata(const std::vector<DeviceMetadataPtr>& deviceMetadata, const std::vector<MeasurementMetadataPtr>& measurementMetadata, const std::vector<PhasorMetadataPtr>& phasorMetadata, int32_t versionNumber = 0);

        // Defines metadata from an existing dataset
        void DefineMetadata(const data::DataSetPtr& metadata);

        // Gets primary metadata. This dataset contains all the normalized metadata tables that define
        // the available detail about the data points that can be subscribed to by clients.
        const data::DataSetPtr& GetMetadata() const;

        // Gets filtering metadata. This dataset, derived from primary metadata, contains a flattened
        // table used to subscribe to a filtered set of points with an expression, e.g.:
        // FILTER ActiveMeasurements WHERE SignalType LIKE '%PHA'
        const data::DataSetPtr& GetFilteringMetadata() const;

        // Filters primary MeasurementDetail metadata returning values as measurement metadata records
        std::vector<MeasurementMetadataPtr> FilterMetadata(const std::string& filterExpression) const;

        // Starts or restarts SecureDataPublisher using specified connection info
        void Start(const TcpEndPoint& endpoint);
        void Start(uint16_t port, bool ipV6 = false);                       // Bind to default NIC
        void Start(const std::string& networkInterfaceIP, uint16_t port);   // Bind to specified NIC IP, format determines IP version
        
        // Shuts down SecureDataPublisher
        //
        // The method does not return until all connections have been closed
        // and all threads spawned by the publisher have shut down gracefully
        void Stop();

        // Determines if SecureDataPublisher has been started
        bool IsStarted() const;

        void PublishMeasurements(const std::vector<Measurement>& measurements);
        void PublishMeasurements(const std::vector<MeasurementPtr>& measurements);

        // Node ID defines a unique identification for the SecureDataPublisher
        // instance that gets included in published metadata so that clients
        // can easily distinguish the source of the measurements
        const Guid& GetNodeID() const;
        void SetNodeID(const Guid& value);

        SecurityMode GetSecurityMode() const;
        void SetSecurityMode(SecurityMode value);

        // Gets or sets value that defines the maximum number of allowed connections, -1 = no limit
        int32_t GetMaximumAllowedConnections() const;
        void SetMaximumAllowedConnections(int32_t value);

        // Gets or sets flag that determines if metadata refresh is allowed by subscribers
        bool GetIsMetadataRefreshAllowed() const;
        void SetIsMetadataRefreshAllowed(bool value);

        // Gets or sets flag that determines if NaN value filter is allowed by subscribers
        bool GetIsNaNValueFilterAllowed() const;
        void SetNaNValueFilterAllowed(bool value);

        // Gets or sets flag that determines if NaN value filter is forced by publisher, regardless of subscriber request
        bool GetIsNaNValueFilterForced() const;
        void SetIsNaNValueFilterForced(bool value);

        bool GetSupportsTemporalSubscriptions() const;
        void SetSupportsTemporalSubscriptions(bool value);

        uint32_t GetCipherKeyRotationPeriod() const;
        void SetCipherKeyRotationPeriod(uint32_t value);

        // Gets or sets flag that determines if base time offsets should be used in compact format
        bool GetUseBaseTimeOffsets() const;
        void SetUseBaseTimeOffsets(bool value);

        uint16_t GetPort() const;
        bool IsIPv6() const;

        // Gets or sets user defined data reference
        void* GetUserData() const;
        void SetUserData(void* userData);

        // Statistical functions
        uint64_t GetTotalCommandChannelBytesSent();
        uint64_t GetTotalDataChannelBytesSent();
        uint64_t GetTotalMeasurementsSent();

        // Callback registration
        //
        // Callback functions are defined with the following signatures:
        //   void HandleStatusMessage(SecureDataPublisher* source, const string& message)
        //   void HandleErrorMessage(SecureDataPublisher* source, const string& message)
        //   void HandleClientConnected(SecureDataPublisher* source, const SecureSubscriberConnectionPtr& connection);
        //   void HandleClientDisconnected(SecureDataPublisher* source, const SecureSubscriberConnectionPtr& connection);
        //   void HandleProcessingIntervalChangeRequested(SecureDataPublisher* source, const SecureSubscriberConnectionPtr& connection);
        //   void HandleTemporalSubscriptionRequested(SecureDataPublisher* source, const SecureSubscriberConnectionPtr& connection);
        //   void HandleTemporalSubscriptionCanceled(SecureDataPublisher* source, const SecureSubscriberConnectionPtr& connection);
        //   void HandleUserCommand(SecureDataPublisher* source, const SecureSubscriberConnectionPtr& connection, uint32_t command, const std::vector<uint8_t>& buffer);
        void RegisterStatusMessageCallback(const MessageCallback& statusMessageCallback);
        void RegisterErrorMessageCallback(const MessageCallback& errorMessageCallback);
        void RegisterClientConnectedCallback(const SecureSubscriberConnectionCallback& clientConnectedCallback);
        void RegisterClientDisconnectedCallback(const SecureSubscriberConnectionCallback& clientDisconnectedCallback);
        void RegisterProcessingIntervalChangeRequestedCallback(const SecureSubscriberConnectionCallback& processingIntervalChangeRequestedCallback);
        void RegisterTemporalSubscriptionRequestedCallback(const SecureSubscriberConnectionCallback& temporalSubscriptionRequestedCallback);
        void RegisterTemporalSubscriptionCanceledCallback(const SecureSubscriberConnectionCallback& temporalSubscriptionCanceledCallback);
        void RegisterUserCommandCallback(const UserCommandCallback& userCommandCallback);

        // SecureSubscriberConnection iteration function - note that full lock will be maintained on source collection
        // for the entire call, so keep work time minimized or clone collection before work
        void IterateSecureSubscriberConnections(const SecureSubscriberConnectionIteratorHandlerFunction& iteratorHandler, void* userData);

        void DisconnectSubscriber(const SecureSubscriberConnectionPtr& connection);
        void DisconnectSubscriber(const Guid& instanceID);

        friend class SecureSubscriberConnection;
        friend class PublisherInstance;
    };

    typedef SharedPtr<SecureSecureDataPublisher> SecureSecureDataPublisherPtr;
}}