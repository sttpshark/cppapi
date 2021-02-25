//******************************************************************************************************
//  SecureDataPublisher.cpp - Gbtc
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

// ReSharper disable CppClangTidyPerformanceNoAutomaticMove
#include "../filterexpressions/FilterExpressionParser.h"
#include "SecureDataPublisher.h"
#include "MetadataSchema.h"
#include "ActiveMeasurementsSchema.h"

using namespace std;
using namespace boost::asio::ip;
using namespace sttp;
using namespace sttp::data;
using namespace sttp::filterexpressions;
using namespace sttp::transport;

struct UserCommandData
{
    SecureSubscriberConnection* connection {};
    uint32_t command {};
    vector<uint8_t> data;
};

SecureDataPublisher::SecureDataPublisher() :
    m_nodeID(NewGuid()),
    m_securityMode(SecurityMode::None),
    m_maximumAllowedConnections(-1),
    m_isMetadataRefreshAllowed(true),
    m_isNaNValueFilterAllowed(true),
    m_isNaNValueFilterForced(false),
    m_supportsTemporalSubscriptions(false),
    m_useBaseTimeOffsets(true),
    m_cipherKeyRotationPeriod(60000),
    m_started(false),
    m_shuttingDown(false),
    m_stopped(false),
    m_userData(nullptr),
    m_clientAcceptor(m_commandChannelService),
    m_context(boost::asio::ssl::context::sslv23),
    m_ca(std::getenv("CA")),
    m_pk(std::getenv("PK")),
    m_dh(std::getenv("DH"))
{
}

SecureDataPublisher::SecureDataPublisher(const TcpEndPoint& endpoint) :
    SecureDataPublisher()
{
    Start(endpoint);
}

SecureDataPublisher::SecureDataPublisher(uint16_t port, bool ipV6) :
    SecureDataPublisher()
{
    Start(port, ipV6);
}

SecureDataPublisher::SecureDataPublisher(const string& networkInterfaceIP, uint16_t port) :
    SecureDataPublisher()
{
    Start(networkInterfaceIP, port);
}

SecureDataPublisher::~SecureDataPublisher() noexcept
{
    try
    {
        m_threadPool.ShutDown();
        ShutDown(true);
    }
    catch (...)
    {
        // ReSharper disable once CppRedundantControlFlowJump
        return;
    }
}

SecureDataPublisher::CallbackDispatcher::CallbackDispatcher() :
    Source(nullptr),
    Data(nullptr),
    Function(nullptr)
{
}

void SecureDataPublisher::StartAccept()
{
    const SecureSubscriberConnectionPtr connection = NewSharedPtr<SubscriberConnection, SecureDataPublisherPtr, IOContext&, SslContext>(shared_from_this(), m_commandChannelService, m_context);
    m
    m_clientAcceptor.async_accept(connection->CommandChannelSocket(), [this, connection]<typename T0>(T0&& error)
    {
        AcceptConnection(connection, error);
    });
}

void SecureDataPublisher::AcceptConnection(const SecureSubscriberConnectionPtr& connection, const ErrorCode& error)
{
    if (IsShuttingDown())
        return;

    if (!error)
    {
        WriterLock writeLock(m_secureSubscriberConnectionsLock);
        const bool connectionAccepted = m_maximumAllowedConnections == -1 || static_cast<int32_t>(m_secureSubscriberConnections.size()) < m_maximumAllowedConnections;
        m_secureSubscriberConnections.insert(connection);
        // Initiate connection, if connection is not accepted, at least resolve connection info
        connection->Start(connectionAccepted);
        // TODO: For secured connections, validate certificate and IP information here to assign subscriberID
        connection->CommandChannelSocket().async_handshake(boost::asio::ssl::stream_base::server,
            [this, self](const boost::system::error_code& error)
            {
                if (!error && connectionAccepted)
                    DispatchClientConnected(AddDispatchReference(connection));
                else {
                    stringstream errorMessageStream;

                    errorMessageStream << "Subscriber connection from \"";
                    errorMessageStream << connection->GetConnectionID();
                    errorMessageStream << "\" refused: connection would exceed " ;
                    errorMessageStream << m_maximumAllowedConnections;
                    errorMessageStream << " maximum allowed connections.";
                    
                    DispatchErrorMessage(errorMessageStream.str());

                    connection->SendResponse(ServerResponse::Failed, ServerCommand::Connect, "Connection refused: too many active connections.");

                    // Allow a moment for response to be received before stopping connection
                    m_threadPool.Queue(1000, AddDispatchReference(connection), [&,this](void* state)
                    {
                        SecureSubscriberConnection* connectionPtr = static_cast<SubscriberConnection*>(state);
                        SecureSubscriberConnectionPtr connectionRef = ReleaseDispatchReference(connectionPtr);
                        connectionRef->Stop();
                    });
                }
            }
        );
    }

    StartAccept();
}

void SecureDataPublisher::ConnectionTerminated(const SecureSubscriberConnectionPtr& connection)
{
    DispatchClientDisconnected(AddDispatchReference(connection));
}

void SecureDataPublisher::RemoveConnection(const SecureSubscriberConnectionPtr& connection)
{
    m_routingTables.RemoveRoutes(connection);
    connection->Stop();

    WriterLock writeLock(m_secureSubscriberConnectionsLock);
    m_secureSubscriberConnections.erase(connection);
}

SubscriberConnection* SecureDataPublisher::AddDispatchReference(SubscriberConnectionPtr connectionRef) // NOLINT
{
    SecureSubscriberConnection* connectionPtr = connectionRef.get();
    ScopeLock lock(m_secureSubscriberConnectionDispatchRefsLock);

    // Increment reference count for subscriber connection shared pointer until all instances are delivered
    const auto iterator = m_secureSubscriberConnectionDispatchRefs.find(connectionRef);

    if (iterator != m_secureSubscriberConnectionDispatchRefs.end())
        iterator->second++;
    else
        m_secureSubscriberConnectionDispatchRefs.emplace(connectionRef, static_cast<uint32_t>(1));

    return connectionPtr;
}

SubscriberConnectionPtr SecureDataPublisher::ReleaseDispatchReference(SubscriberConnection* connectionPtr)
{
    const SecureSubscriberConnectionPtr connectionRef = connectionPtr->GetReference();
    ScopeLock lock(m_secureSubscriberConnectionDispatchRefsLock);
    
    // Decrement reference count to subscriber connection pointer
    const auto iterator = m_secureSubscriberConnectionDispatchRefs.find(connectionRef);

    if (iterator != m_secureSubscriberConnectionDispatchRefs.end())
    {
        iterator->second--;

        // Remove references when count hits zero
        if (iterator->second < 1)
            m_secureSubscriberConnectionDispatchRefs.erase(connectionRef);
    }
        
    return connectionRef;
}

void SecureDataPublisher::Dispatch(const DispatcherFunction& function)
{
    Dispatch(function, nullptr, 0, 0);
}

void SecureDataPublisher::Dispatch(const DispatcherFunction& function, const uint8_t* data, const uint32_t offset, const uint32_t length)
{
    CallbackDispatcher dispatcher;
    SharedPtr<vector<uint8_t>> dataVector = NewSharedPtr<vector<uint8_t>>();

    dataVector->resize(length);

    if (data != nullptr)
    {
        for (uint32_t i = 0; i < length; ++i)
            dataVector->at(i) = data[offset + i];
    }

    dispatcher.Source = this;
    dispatcher.Data = dataVector;
    dispatcher.Function = function;

    m_callbackQueue.Enqueue(dispatcher);
}

void SecureDataPublisher::DispatchStatusMessage(const string& message)
{
    const uint32_t messageSize = ConvertUInt32((message.size() + 1) * sizeof(char));
    Dispatch(&StatusMessageDispatcher, reinterpret_cast<const uint8_t*>(message.c_str()), 0, messageSize);
}

void SecureDataPublisher::DispatchErrorMessage(const string& message)
{
    const uint32_t messageSize = ConvertUInt32((message.size() + 1) * sizeof(char));
    Dispatch(&ErrorMessageDispatcher, reinterpret_cast<const uint8_t*>(message.c_str()), 0, messageSize);
}

void SecureDataPublisher::DispatchClientConnected(SubscriberConnection* connection)
{
    Dispatch(&ClientConnectedDispatcher, reinterpret_cast<uint8_t*>(&connection), 0, sizeof(SubscriberConnection**));
}

void SecureDataPublisher::DispatchClientDisconnected(SubscriberConnection* connection)
{
    Dispatch(&ClientDisconnectedDispatcher, reinterpret_cast<uint8_t*>(&connection), 0, sizeof(SubscriberConnection**));
}

void SecureDataPublisher::DispatchProcessingIntervalChangeRequested(SubscriberConnection* connection)
{
    Dispatch(&ProcessingIntervalChangeRequestedDispatcher, reinterpret_cast<uint8_t*>(&connection), 0, sizeof(SubscriberConnection**));
}

void SecureDataPublisher::DispatchTemporalSubscriptionRequested(SubscriberConnection* connection)
{
    Dispatch(&TemporalSubscriptionRequestedDispatcher, reinterpret_cast<uint8_t*>(&connection), 0, sizeof(SubscriberConnection**));
}

void SecureDataPublisher::DispatchTemporalSubscriptionCanceled(SubscriberConnection* connection)
{
    Dispatch(&TemporalSubscriptionCanceledDispatcher, reinterpret_cast<uint8_t*>(&connection), 0, sizeof(SubscriberConnection**));
}

void SecureDataPublisher::DispatchUserCommand(SubscriberConnection* connection, const uint32_t command, const uint8_t* data, const uint32_t length)
{
    // ReSharper disable once CppNonReclaimedResourceAcquisition
    UserCommandData* userCommandData = new UserCommandData();
    userCommandData->connection = connection;
    userCommandData->command = command;

    for (uint32_t i = 0; i < length; i++)
        userCommandData->data.push_back(data[i]);

    Dispatch(&UserCommandDispatcher, reinterpret_cast<uint8_t*>(&userCommandData), 0, sizeof(UserCommandData**));
}

// Dispatcher function for status messages. Decodes the message and provides it to the user via the status message callback.
void SecureDataPublisher::StatusMessageDispatcher(SecureDataPublisher* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    const MessageCallback statusMessageCallback = source->m_statusMessageCallback;

    if (statusMessageCallback != nullptr)
        statusMessageCallback(source, reinterpret_cast<const char*>(&buffer[0]));
}

// Dispatcher function for error messages. Decodes the message and provides it to the user via the error message callback.
void SecureDataPublisher::ErrorMessageDispatcher(SecureDataPublisher* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    const MessageCallback errorMessageCallback = source->m_errorMessageCallback;

    if (errorMessageCallback != nullptr)
        errorMessageCallback(source, reinterpret_cast<const char*>(&buffer[0]));
}

void SecureDataPublisher::ClientConnectedDispatcher(SecureDataPublisher* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    SecureSubscriberConnection* connectionPtr = *reinterpret_cast<SubscriberConnection**>(const_cast<uint8_t*>(&buffer[0]));

    if (connectionPtr != nullptr)
    {
        const SecureSubscriberConnectionCallback clientConnectedCallback = source->m_clientConnectedCallback;
        const SecureSubscriberConnectionPtr connectionRef = source->ReleaseDispatchReference(connectionPtr); //-V821

        if (clientConnectedCallback != nullptr)
            clientConnectedCallback(source, connectionRef);
    }
}

void SecureDataPublisher::ClientDisconnectedDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    SecureSubscriberConnection* connectionPtr = *reinterpret_cast<SubscriberConnection**>(const_cast<uint8_t*>(&buffer[0]));

    if (connectionPtr != nullptr)
    {
        const SecureSubscriberConnectionCallback clientDisconnectedCallback = source->m_clientDisconnectedCallback;
        const SecureSubscriberConnectionPtr connectionRef = source->ReleaseDispatchReference(connectionPtr);

        if (clientDisconnectedCallback != nullptr)
            clientDisconnectedCallback(source, connectionRef);

        source->RemoveConnection(connectionRef);
    }
}

void SecureDataPublisher::ProcessingIntervalChangeRequestedDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    SecureSubscriberConnection* connectionPtr = *reinterpret_cast<SubscriberConnection**>(const_cast<uint8_t*>(&buffer[0]));

    if (connectionPtr != nullptr)
    {
        const SecureSubscriberConnectionCallback temporalProcessingIntervalChangeRequestedCallback = source->m_processingIntervalChangeRequestedCallback;
        const SecureSubscriberConnectionPtr connectionRef = source->ReleaseDispatchReference(connectionPtr); //-V821

        if (temporalProcessingIntervalChangeRequestedCallback != nullptr)
            temporalProcessingIntervalChangeRequestedCallback(source, connectionRef);
    }
}

void SecureDataPublisher::TemporalSubscriptionRequestedDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    SecureSubscriberConnection* connectionPtr = *reinterpret_cast<SubscriberConnection**>(const_cast<uint8_t*>(&buffer[0]));

    if (connectionPtr != nullptr)
    {
        const SecureSubscriberConnectionCallback temporalSubscriptionRequestedCallback = source->m_temporalSubscriptionRequestedCallback;
        const SecureSubscriberConnectionPtr connectionRef = source->ReleaseDispatchReference(connectionPtr); //-V821

        if (temporalSubscriptionRequestedCallback != nullptr)
            temporalSubscriptionRequestedCallback(source, connectionRef);
    }
}

void SecureDataPublisher::TemporalSubscriptionCanceledDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    SecureSubscriberConnection* connectionPtr = *reinterpret_cast<SubscriberConnection**>(const_cast<uint8_t*>(&buffer[0]));

    if (connectionPtr != nullptr)
    {
        const SecureSubscriberConnectionCallback temporalSubscriptionCanceledCallback = source->m_temporalSubscriptionCanceledCallback;
        const SecureSubscriberConnectionPtr connectionRef = source->ReleaseDispatchReference(connectionPtr); //-V821

        if (temporalSubscriptionCanceledCallback != nullptr)
            temporalSubscriptionCanceledCallback(source, connectionRef);
    }
}

void SecureDataPublisher::UserCommandDispatcher(SecureDataPublisher* source, const std::vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    UserCommandData* userCommandData = *reinterpret_cast<UserCommandData**>(const_cast<uint8_t*>(&buffer[0]));

    if (userCommandData != nullptr && userCommandData->connection != nullptr)
    {
        const UserCommandCallback userCommandCallback = source->m_userCommandCallback;
        const SecureSubscriberConnectionPtr connectionRef = source->ReleaseDispatchReference(userCommandData->connection); //-V821

        if (userCommandCallback != nullptr)
            userCommandCallback(source, connectionRef, userCommandData->command, userCommandData->data);
    }

    delete userCommandData;
}

int32_t SecureDataPublisher::GetColumnIndex(const sttp::data::DataTablePtr& table, const std::string& columnName)
{
    const DataColumnPtr& column = table->Column(columnName);
    
    if (column == nullptr)
        throw PublisherException("Column name \"" + columnName + "\" was not found in table \"" + table->Name() + "\"");
    
    return column->Index();
}

void SecureDataPublisher::DefineMetadata(const vector<DeviceMetadataPtr>& deviceMetadata, const vector<MeasurementMetadataPtr>& measurementMetadata, const vector<PhasorMetadataPtr>& phasorMetadata, const int32_t versionNumber)
{
    typedef unordered_map<uint16_t, char> PhasorTypeMap;
    typedef SharedPtr<PhasorTypeMap> PhasorTypeMapPtr;
    const PhasorTypeMapPtr nullPhasorTypeMap = nullptr;

    // Load meta-data schema
    const DataSetPtr metadata = DataSet::FromXml(MetadataSchema, MetadataSchemaLength);
    const DataTablePtr& deviceDetail = metadata->Table("DeviceDetail");
    const DataTablePtr& measurementDetail = metadata->Table("MeasurementDetail");
    const DataTablePtr& phasorDetail = metadata->Table("PhasorDetail");
    const DataTablePtr& schemaVersion = metadata->Table("SchemaVersion");

    StringMap<PhasorTypeMapPtr> phasorTypes;
    PhasorTypeMapPtr phasors;

    if (deviceDetail != nullptr)
    {
        const int32_t nodeID = GetColumnIndex(deviceDetail, "NodeID");
        const int32_t uniqueID = GetColumnIndex(deviceDetail, "UniqueID");
        const int32_t isConcentrator = GetColumnIndex(deviceDetail, "IsConcentrator");
        const int32_t acronym = GetColumnIndex(deviceDetail, "Acronym");
        const int32_t name = GetColumnIndex(deviceDetail, "Name");
        const int32_t accessID = GetColumnIndex(deviceDetail, "AccessID");
        const int32_t parentAcronym = GetColumnIndex(deviceDetail, "ParentAcronym");
        const int32_t protocolName = GetColumnIndex(deviceDetail, "ProtocolName");
        const int32_t framesPerSecond = GetColumnIndex(deviceDetail, "FramesPerSecond");
        const int32_t companyAcronym = GetColumnIndex(deviceDetail, "CompanyAcronym");
        const int32_t vendorAcronym = GetColumnIndex(deviceDetail, "VendorAcronym");
        const int32_t vendorDeviceName = GetColumnIndex(deviceDetail, "VendorDeviceName");
        const int32_t longitude = GetColumnIndex(deviceDetail, "Longitude");
        const int32_t latitude = GetColumnIndex(deviceDetail, "Latitude");
        const int32_t enabled = GetColumnIndex(deviceDetail, "Enabled");
        const int32_t updatedOn = GetColumnIndex(deviceDetail, "UpdatedOn");

        for (size_t i = 0; i < deviceMetadata.size(); i++)
        {
            const DeviceMetadataPtr device = deviceMetadata[i];

            if (device == nullptr)
                continue;

            DataRowPtr row = deviceDetail->CreateRow();

            row->SetGuidValue(nodeID, m_nodeID);
            row->SetGuidValue(uniqueID, device->UniqueID);
            row->SetBooleanValue(isConcentrator, false);
            row->SetStringValue(acronym, device->Acronym);
            row->SetStringValue(name, device->Name);
            row->SetInt32Value(accessID, device->AccessID);
            row->SetStringValue(parentAcronym, device->ParentAcronym);
            row->SetStringValue(protocolName, device->ProtocolName);
            row->SetInt32Value(framesPerSecond, device->FramesPerSecond);
            row->SetStringValue(companyAcronym, device->CompanyAcronym);
            row->SetStringValue(vendorAcronym, device->VendorAcronym);
            row->SetStringValue(vendorDeviceName, device->VendorDeviceName);
            row->SetDecimalValue(longitude, decimal_t(device->Longitude));
            row->SetDecimalValue(latitude, decimal_t(device->Latitude));
            row->SetBooleanValue(enabled, true);
            row->SetDateTimeValue(updatedOn, device->UpdatedOn);

            deviceDetail->AddRow(row);
        }
    }

    if (phasorDetail != nullptr)
    {
        const int32_t id = GetColumnIndex(phasorDetail, "ID");
        const int32_t deviceAcronym = GetColumnIndex(phasorDetail, "DeviceAcronym");
        const int32_t label = GetColumnIndex(phasorDetail, "Label");
        const int32_t type = GetColumnIndex(phasorDetail, "Type");
        const int32_t phase = GetColumnIndex(phasorDetail, "Phase");
        const int32_t sourceIndex = GetColumnIndex(phasorDetail, "SourceIndex");
        const int32_t updatedOn = GetColumnIndex(phasorDetail, "UpdatedOn");

        for (size_t i = 0; i < phasorMetadata.size(); i++)
        {
            const PhasorMetadataPtr phasor = phasorMetadata[i];

            if (phasor == nullptr)
                continue;

            DataRowPtr row = phasorDetail->CreateRow();

            row->SetInt32Value(id, ConvertInt32(i));
            row->SetStringValue(deviceAcronym, phasor->DeviceAcronym);
            row->SetStringValue(label, phasor->Label);
            row->SetStringValue(type, phasor->Type);
            row->SetStringValue(phase, phasor->Phase);
            row->SetInt32Value(sourceIndex, phasor->SourceIndex);
            row->SetDateTimeValue(updatedOn, phasor->UpdatedOn);

            phasorDetail->AddRow(row);

            // Track phasor information related to device for measurement signal type derivation later
            if (!TryGetValue(phasorTypes, phasor->DeviceAcronym, phasors, nullPhasorTypeMap))
            {
                phasors = NewSharedPtr<PhasorTypeMap>();
                phasorTypes[phasor->DeviceAcronym] = phasors;
            }

            phasors->insert_or_assign(phasor->SourceIndex, phasor->Type.empty() ? 'I' : phasor->Type[0]);
        }
    }

    if (measurementDetail != nullptr)
    {
        const int32_t deviceAcronym = GetColumnIndex(measurementDetail, "DeviceAcronym");
        const int32_t id = GetColumnIndex(measurementDetail, "ID");
        const int32_t signalID = GetColumnIndex(measurementDetail, "SignalID");
        const int32_t pointTag = GetColumnIndex(measurementDetail, "PointTag");
        const int32_t signalReference = GetColumnIndex(measurementDetail, "SignalReference");
        const int32_t signalAcronym = GetColumnIndex(measurementDetail, "SignalAcronym");
        const int32_t phasorSourceIndex = GetColumnIndex(measurementDetail, "PhasorSourceIndex");
        const int32_t description = GetColumnIndex(measurementDetail, "Description");
        const int32_t internal = GetColumnIndex(measurementDetail, "Internal");
        const int32_t enabled = GetColumnIndex(measurementDetail, "Enabled");
        const int32_t updatedOn = GetColumnIndex(measurementDetail, "UpdatedOn");
        char phasorType = 'I';

        for (size_t i = 0; i < measurementMetadata.size(); i++)
        {
            const MeasurementMetadataPtr measurement = measurementMetadata[i];
            DataRowPtr row = measurementDetail->CreateRow();

            row->SetStringValue(deviceAcronym, measurement->DeviceAcronym);
            row->SetStringValue(id, measurement->ID);
            row->SetGuidValue(signalID, measurement->SignalID);
            row->SetStringValue(pointTag, measurement->PointTag);
            row->SetStringValue(signalReference, ToString(measurement->Reference));

            if (TryGetValue(phasorTypes, measurement->DeviceAcronym, phasors, nullPhasorTypeMap))
                TryGetValue(*phasors, measurement->PhasorSourceIndex, phasorType, 'I');

            row->SetStringValue(signalAcronym, GetSignalTypeAcronym(measurement->Reference.Kind, phasorType));
            row->SetInt32Value(phasorSourceIndex, measurement->PhasorSourceIndex);
            row->SetStringValue(description, measurement->Description);
            row->SetBooleanValue(internal, true);
            row->SetBooleanValue(enabled, true);
            row->SetDateTimeValue(updatedOn, measurement->UpdatedOn);

            measurementDetail->AddRow(row);
        }
    }

    if (schemaVersion != nullptr)
    {
        DataRowPtr row = schemaVersion->CreateRow();
        row->SetInt32Value("VersionNumber", versionNumber);
        schemaVersion->AddRow(row);
    }

    DefineMetadata(metadata);
}

void SecureDataPublisher::DefineMetadata(const DataSetPtr& metadata)
{
    m_metadata = metadata;

    // Create device data map used to build a flatter meta-data view used for easier client filtering
    struct DeviceData
    {
        int32_t DeviceID{};
        int32_t FramesPerSecond{};
        string Company;
        string Protocol;
        string ProtocolType;
        decimal_t Longitude;
        decimal_t Latitude;
    };

    typedef SharedPtr<DeviceData> DeviceDataPtr;
    const DeviceDataPtr nullDeviceData = nullptr;

    const DataTablePtr& deviceDetail = metadata->Table("DeviceDetail");
    StringMap<DeviceDataPtr> deviceData;

    if (deviceDetail != nullptr)
    {
        const int32_t acronym = GetColumnIndex(deviceDetail, "Acronym");
        const int32_t protocolName = GetColumnIndex(deviceDetail, "ProtocolName");
        const int32_t framesPerSecond = GetColumnIndex(deviceDetail, "FramesPerSecond");
        const int32_t companyAcronym = GetColumnIndex(deviceDetail, "CompanyAcronym");
        const int32_t longitude = GetColumnIndex(deviceDetail, "Longitude");
        const int32_t latitude = GetColumnIndex(deviceDetail, "Latitude");

        for (int32_t i = 0; i < deviceDetail->RowCount(); i++)
        {
            const DataRowPtr& row = deviceDetail->Row(i);
            const DeviceDataPtr device = NewSharedPtr<DeviceData>();

            device->DeviceID = i;
            device->FramesPerSecond = row->ValueAsInt32(framesPerSecond).GetValueOrDefault();
            device->Company = row->ValueAsString(companyAcronym).GetValueOrDefault();
            device->Protocol = row->ValueAsString(protocolName).GetValueOrDefault();
            device->ProtocolType = GetProtocolType(device->Protocol);
            device->Longitude = row->ValueAsDecimal(longitude).GetValueOrDefault();
            device->Latitude = row->ValueAsDecimal(latitude).GetValueOrDefault();

            string deviceAcronymRef = row->ValueAsString(acronym).GetValueOrDefault();

            if (!deviceAcronymRef.empty())
                deviceData[deviceAcronymRef] = device;
        }
    }

    // Create phasor data map used to build a flatter meta-data view used for easier client filtering
    struct PhasorData
    {
        int32_t PhasorID{};
        string PhasorType;
        string Phase;
    };

    typedef SharedPtr<PhasorData> PhasorDataPtr;
    const PhasorDataPtr nullPhasorData = nullptr;

    typedef unordered_map<int32_t, PhasorDataPtr> PhasorDataMap;
    typedef SharedPtr<PhasorDataMap> PhasorDataMapPtr;
    const PhasorDataMapPtr nullPhasorDataMap = nullptr;

    const DataTablePtr& phasorDetail = metadata->Table("PhasorDetail");
    StringMap<PhasorDataMapPtr> phasorData;

    if (phasorDetail != nullptr)
    {
        const int32_t id = GetColumnIndex(phasorDetail, "ID");
        const int32_t deviceAcronym = GetColumnIndex(phasorDetail, "DeviceAcronym");
        const int32_t type = GetColumnIndex(phasorDetail, "Type");
        const int32_t phase = GetColumnIndex(phasorDetail, "Phase");
        const int32_t sourceIndex = GetColumnIndex(phasorDetail, "SourceIndex");
        
        for (int32_t i = 0; i < phasorDetail->RowCount(); i++)
        {
            const DataRowPtr& row = phasorDetail->Row(i);
            
            string deviceAcronymRef = row->ValueAsString(deviceAcronym).GetValueOrDefault();

            if (deviceAcronymRef.empty())
                continue;

            PhasorDataMapPtr phasorMap;
            const PhasorDataPtr phasor = NewSharedPtr<PhasorData>();

            phasor->PhasorID = row->ValueAsInt32(id).GetValueOrDefault();
            phasor->PhasorType = row->ValueAsString(type).GetValueOrDefault();
            phasor->Phase = row->ValueAsString(phase).GetValueOrDefault();

            if (!TryGetValue(phasorData, deviceAcronymRef, phasorMap, nullPhasorDataMap))
            {
                phasorMap = NewSharedPtr<PhasorDataMap>();
                phasorData[deviceAcronymRef] = phasorMap;
            }

            phasorMap->insert_or_assign(row->ValueAsInt32(sourceIndex).GetValueOrDefault(), phasor);
        }
    }

    // Load active meta-data measurements schema
    DataSetPtr filteringMetadata = DataSet::FromXml(ActiveMeasurementsSchema, ActiveMeasurementsSchemaLength);

    // Build active meta-data measurements from all meta-data
    const DataTablePtr& measurementDetail = metadata->Table("MeasurementDetail");
    const DataTablePtr& activeMeasurements = filteringMetadata->Table("ActiveMeasurements");

    if (measurementDetail != nullptr && activeMeasurements != nullptr)
    {
        // Lookup column indices for measurement detail table
        const int32_t md_deviceAcronym = GetColumnIndex(measurementDetail, "DeviceAcronym");
        const int32_t md_id = GetColumnIndex(measurementDetail, "ID");
        const int32_t md_signalID = GetColumnIndex(measurementDetail, "SignalID");
        const int32_t md_pointTag = GetColumnIndex(measurementDetail, "PointTag");
        const int32_t md_signalReference = GetColumnIndex(measurementDetail, "SignalReference");
        const int32_t md_signalAcronym = GetColumnIndex(measurementDetail, "SignalAcronym");
        const int32_t md_phasorSourceIndex = GetColumnIndex(measurementDetail, "PhasorSourceIndex");
        const int32_t md_description = GetColumnIndex(measurementDetail, "Description");
        const int32_t md_internal = GetColumnIndex(measurementDetail, "Internal");
        const int32_t md_enabled = GetColumnIndex(measurementDetail, "Enabled");
        const int32_t md_updatedOn = GetColumnIndex(measurementDetail, "UpdatedOn");

        // Lookup column indices for active measurements table
        const int32_t am_sourceNodeID = GetColumnIndex(activeMeasurements, "SourceNodeID");
        const int32_t am_id = GetColumnIndex(activeMeasurements, "ID");
        const int32_t am_signalID = GetColumnIndex(activeMeasurements, "SignalID");
        const int32_t am_pointTag = GetColumnIndex(activeMeasurements, "PointTag");
        const int32_t am_signalReference = GetColumnIndex(activeMeasurements, "SignalReference");
        const int32_t am_internal = GetColumnIndex(activeMeasurements, "Internal");
        const int32_t am_subscribed = GetColumnIndex(activeMeasurements, "Subscribed");
        const int32_t am_device = GetColumnIndex(activeMeasurements, "Device");
        const int32_t am_deviceID = GetColumnIndex(activeMeasurements, "DeviceID");
        const int32_t am_framesPerSecond = GetColumnIndex(activeMeasurements, "FramesPerSecond");
        const int32_t am_protocol = GetColumnIndex(activeMeasurements, "Protocol");
        const int32_t am_protocolType = GetColumnIndex(activeMeasurements, "ProtocolType");
        const int32_t am_signalType = GetColumnIndex(activeMeasurements, "SignalType");
        const int32_t am_engineeringUnits = GetColumnIndex(activeMeasurements, "EngineeringUnits");
        const int32_t am_phasorID = GetColumnIndex(activeMeasurements, "PhasorID");
        const int32_t am_phasorType = GetColumnIndex(activeMeasurements, "PhasorType");
        const int32_t am_phase = GetColumnIndex(activeMeasurements, "Phase");
        const int32_t am_adder = GetColumnIndex(activeMeasurements, "Adder");
        const int32_t am_multiplier = GetColumnIndex(activeMeasurements, "Multiplier");
        const int32_t am_company = GetColumnIndex(activeMeasurements, "Company");
        const int32_t am_longitude = GetColumnIndex(activeMeasurements, "Longitude");
        const int32_t am_latitude = GetColumnIndex(activeMeasurements, "Latitude");
        const int32_t am_description = GetColumnIndex(activeMeasurements, "Description");
        const int32_t am_updatedOn = GetColumnIndex(activeMeasurements, "UpdatedOn");

        for (int32_t i = 0; i < measurementDetail->RowCount(); i++)
        {
            const DataRowPtr& md_row = measurementDetail->Row(i);

            if (!md_row->ValueAsBoolean(md_enabled).GetValueOrDefault())
                continue;
            
            DataRowPtr am_row = activeMeasurements->CreateRow();

            am_row->SetGuidValue(am_sourceNodeID, m_nodeID);
            am_row->SetStringValue(am_id, md_row->ValueAsString(md_id));
            am_row->SetGuidValue(am_signalID, md_row->ValueAsGuid(md_signalID));
            am_row->SetStringValue(am_pointTag, md_row->ValueAsString(md_pointTag));
            am_row->SetStringValue(am_signalReference, md_row->ValueAsString(md_signalReference));
            am_row->SetInt32Value(am_internal, md_row->ValueAsBoolean(md_internal).GetValueOrDefault() ? 1 : 0);
            am_row->SetInt32Value(am_subscribed, 0);
            am_row->SetStringValue(am_description, md_row->ValueAsString(md_description));
            am_row->SetDoubleValue(am_adder, 0.0);
            am_row->SetDoubleValue(am_multiplier, 1.0);
            am_row->SetDateTimeValue(am_updatedOn, md_row->ValueAsDateTime(md_updatedOn));

            string signalType = md_row->ValueAsString(md_signalAcronym).GetValueOrDefault();

            if (signalType.empty())
                signalType = "CALC";

            am_row->SetStringValue(am_signalType, signalType);
            am_row->SetStringValue(am_engineeringUnits, GetEngineeringUnits(signalType));

            string deviceAcronymRef = md_row->ValueAsString(md_deviceAcronym).GetValueOrDefault();

            if (deviceAcronymRef.empty())
            {
                // Set any default values when measurement is not associated with a device
                am_row->SetInt32Value(am_framesPerSecond, 30);
            }
            else
            {
                am_row->SetStringValue(am_device, deviceAcronymRef);

                DeviceDataPtr device;

                // Lookup associated device record
                if (TryGetValue(deviceData, deviceAcronymRef, device, nullDeviceData))
                {
                    am_row->SetInt32Value(am_deviceID, device->DeviceID);
                    am_row->SetInt32Value(am_framesPerSecond, device->FramesPerSecond);
                    am_row->SetStringValue(am_company, device->Company);
                    am_row->SetStringValue(am_protocol, device->Protocol);
                    am_row->SetStringValue(am_protocolType, device->ProtocolType);
                    am_row->SetDecimalValue(am_longitude, device->Longitude);
                    am_row->SetDecimalValue(am_latitude, device->Latitude);
                }

                PhasorDataMapPtr phasorMap;

                // Lookup associated phasor records
                if (TryGetValue(phasorData, deviceAcronymRef, phasorMap, nullPhasorDataMap))
                {
                    PhasorDataPtr phasor;
                    int32_t sourceIndex = md_row->ValueAsInt32(md_phasorSourceIndex).GetValueOrDefault();

                    if (TryGetValue(*phasorMap, sourceIndex, phasor, nullPhasorData))
                    {
                        am_row->SetInt32Value(am_phasorID, phasor->PhasorID);
                        am_row->SetStringValue(am_phasorType, phasor->PhasorType);
                        am_row->SetStringValue(am_phase, phasor->Phase);
                    }
                }
            }

            activeMeasurements->AddRow(am_row);
        }
    }

    m_filteringMetadata.swap(filteringMetadata);

    // Notify all subscribers that the configuration metadata has changed
    ReaderLock readLock(m_secureSubscriberConnectionsLock);

    for (const auto& connection : m_secureSubscriberConnections)
        connection->SendResponse(ServerResponse::ConfigurationChanged, ServerCommand::Subscribe);
}

const DataSetPtr& SecureDataPublisher::GetMetadata() const
{
    return m_metadata;
}

const DataSetPtr& SecureDataPublisher::GetFilteringMetadata() const
{
    return m_filteringMetadata;
}

vector<MeasurementMetadataPtr> SecureDataPublisher::FilterMetadata(const string& filterExpression) const
{
    if (m_metadata == nullptr)
        throw PublisherException("Cannot filter metadata, no metadata has been defined.");

    vector<DataRowPtr> rows = FilterExpressionParser::Select(m_metadata, filterExpression, "MeasurementDetail");
    vector<MeasurementMetadataPtr> measurementMetadata;
    const DataTablePtr& measurementDetail = m_metadata->Table("MeasurementDetail");
    
    const int32_t deviceAcronym = GetColumnIndex(measurementDetail, "DeviceAcronym");
    const int32_t id = GetColumnIndex(measurementDetail, "ID");
    const int32_t signalID = GetColumnIndex(measurementDetail, "SignalID");
    const int32_t pointTag = GetColumnIndex(measurementDetail, "PointTag");
    const int32_t signalReference = GetColumnIndex(measurementDetail, "SignalReference");
    const int32_t phasorSourceIndex = GetColumnIndex(measurementDetail, "PhasorSourceIndex");
    const int32_t description = GetColumnIndex(measurementDetail, "Description");
    const int32_t enabled = GetColumnIndex(measurementDetail, "Enabled");
    const int32_t updatedOn = GetColumnIndex(measurementDetail, "UpdatedOn");

    for (size_t i = 0; i < rows.size(); i++)
    {
        DataRowPtr row = rows[i];
        MeasurementMetadataPtr metadata = NewSharedPtr<MeasurementMetadata>();

        if (!row->ValueAsBoolean(enabled).GetValueOrDefault())
            continue;

        metadata->DeviceAcronym = row->ValueAsString(deviceAcronym).GetValueOrDefault();
        metadata->ID = row->ValueAsString(id).GetValueOrDefault();
        metadata->SignalID = row->ValueAsGuid(signalID).GetValueOrDefault();
        metadata->PointTag = row->ValueAsString(pointTag).GetValueOrDefault();
        metadata->Reference = SignalReference(row->ValueAsString(signalReference).GetValueOrDefault());
        metadata->PhasorSourceIndex = static_cast<uint16_t>(row->ValueAsInt32(phasorSourceIndex).GetValueOrDefault());
        metadata->Description = row->ValueAsString(description).GetValueOrDefault();
        metadata->UpdatedOn = row->ValueAsDateTime(updatedOn).GetValueOrDefault();

        measurementMetadata.push_back(metadata);
    }

    return measurementMetadata;
}

void SecureDataPublisher::Start(const TcpEndPoint& endpoint)
{
    if (m_started)
        ShutDown(true);

    // Let any pending start operation complete before possible stop - prevents destruction stop before start is completed
    ScopeLock lock(m_connectActionMutex);

    m_stopped = false;

#if BOOST_LEGACY
    m_commandChannelService.reset();
#else
    m_commandChannelService.restart();
#endif
    m_context.set_options(
        boost::asio::ssl::context::default_workarounds
        | boost::asio::ssl::context::no_sslv2
        | boost::asio::ssl::context::single_dh_use);
    m_context.set_password_callback(std::bind(&server::get_password, this));
    m_context.use_certificate_chain_file(m_ca);
    m_context.use_private_key_file(m_pk, boost::asio::ssl::context::pem);
    m_context.use_tmp_dh_file(m_dh);
    m_clientAcceptor = TcpAcceptor(m_commandChannelService, endpoint, false); //-V601
    
    // Run call-back thread
    m_commandChannelAcceptThread = Thread([this]
    {
        while (true)
        {
            m_callbackQueue.WaitForData();

            if (IsShuttingDown())
                break;

            const CallbackDispatcher dispatcher = m_callbackQueue.Dequeue();

            if (dispatcher.Function != nullptr)
                dispatcher.Function(dispatcher.Source, *dispatcher.Data);
        }
    });

    // Run command channel accept thread
    m_callbackThread = Thread([this]
    {
        StartAccept();
        m_commandChannelService.run();
    });

    m_started = true;
}

void SecureDataPublisher::Start(const uint16_t port, const bool ipV6)
{
    Start(TcpEndPoint(ipV6 ? tcp::v6() : tcp::v4(), port));
}

void SecureDataPublisher::Start(const string& networkInterfaceIP, const uint16_t port)
{
    Start(TcpEndPoint(make_address(networkInterfaceIP), port));
}

void SecureDataPublisher::Stop()
{
    if (!m_started || IsShuttingDown())
        return;

    // Stop method executes shutdown on a separate thread without stopping to prevent
    // issues where user may call stop method from a dispatched event thread
    ShutDown(false);
}

void SecureDataPublisher::ShutDown(const bool joinThread)
{
    // Check if shutdown thread is running or publisher has already stopped
    if (IsShuttingDown())
    {
        if (joinThread && !m_stopped)
            m_shutdownThread.join();

        return;
    }

    // Notify running threads that the publisher is shutting down, i.e., shutdown thread is active
    m_shuttingDown = true;
    m_started = false;

    m_shutdownThread = Thread([this]
    {
        try
        {
            // Let any pending start operation complete before continuing stop - prevents destruction stop before start is completed
            ScopeLock lock(m_connectActionMutex);

            // Stop accepting new connections
            m_clientAcceptor.close();

            // Clear routing tables to cease any queued publication
            m_routingTables.Clear();

            // Shutdown client connections - execute on a separate thread to more safely handle callbacks
            Thread shutdownClientsThread([this]
            {
                WriterLock writeLock(m_secureSubscriberConnectionsLock);

                for (const auto& connection : m_secureSubscriberConnections)
                {
                    connection->Stop();

                    if (m_clientDisconnectedCallback != nullptr)
                        m_clientDisconnectedCallback(this, connection);
                }

                m_secureSubscriberConnections.clear();
            });

            // Release queues and close sockets so
            // that threads can shut down gracefully
            m_callbackQueue.Release();

            // Join with all threads to guarantee their completion
            // before returning control to the caller
            shutdownClientsThread.join();
            m_callbackThread.join();
            m_commandChannelAcceptThread.join();

            // Empty queues and reset them so they can be used
            // again later if the user decides to restart
            m_callbackQueue.Clear();
            m_callbackQueue.Reset();

            m_commandChannelService.stop();
        }
        catch (SystemError& ex)
        {
            cerr << "Exception while stopping data publisher: " << ex.what();
        }
        catch (...)
        {
            cerr << "Exception while stopping data publisher: " << boost::current_exception_diagnostic_information(true);
        }

        // Shutdown complete
        m_stopped = true;
        m_shuttingDown = false;
    });

    if (joinThread)
        m_shutdownThread.join();
}

bool SecureDataPublisher::IsStarted() const
{
    return m_started;
}

void SecureDataPublisher::PublishMeasurements(const vector<Measurement>& measurements)
{
    vector<MeasurementPtr> measurementPtrs;

    measurementPtrs.reserve(measurements.size());

    for (const auto& measurement : measurements)
        measurementPtrs.push_back(ToPtr(measurement));

    PublishMeasurements(measurementPtrs);
}

void SecureDataPublisher::PublishMeasurements(const vector<MeasurementPtr>& measurements)
{
    if (!m_started)
        return;

    m_routingTables.PublishMeasurements(measurements);
}

const sttp::Guid& SecureDataPublisher::GetNodeID() const
{
    return m_nodeID;
}

void SecureDataPublisher::SetNodeID(const sttp::Guid& value)
{
    m_nodeID = value;
}

SecurityMode SecureDataPublisher::GetSecurityMode() const
{
    return m_securityMode;
}

void SecureDataPublisher::SetSecurityMode(const SecurityMode value)
{
    m_securityMode = value;
}

int32_t SecureDataPublisher::GetMaximumAllowedConnections() const
{
    return m_maximumAllowedConnections;
}

void SecureDataPublisher::SetMaximumAllowedConnections(const int32_t value)
{
    m_maximumAllowedConnections = value;
}

bool SecureDataPublisher::GetIsMetadataRefreshAllowed() const
{
    return m_isMetadataRefreshAllowed;
}

void SecureDataPublisher::SetIsMetadataRefreshAllowed(const bool value)
{
    m_isMetadataRefreshAllowed = value;
}

bool SecureDataPublisher::GetIsNaNValueFilterAllowed() const
{
    return m_isNaNValueFilterAllowed;
}

void SecureDataPublisher::SetNaNValueFilterAllowed(const bool value)
{
    m_isNaNValueFilterAllowed = value;
}

bool SecureDataPublisher::GetIsNaNValueFilterForced() const
{
    return m_isNaNValueFilterForced;
}

void SecureDataPublisher::SetIsNaNValueFilterForced(const bool value)
{
    m_isNaNValueFilterForced = value;
}

bool SecureDataPublisher::GetSupportsTemporalSubscriptions() const
{
    return m_supportsTemporalSubscriptions;
}

void SecureDataPublisher::SetSupportsTemporalSubscriptions(const bool value)
{
    m_supportsTemporalSubscriptions = value;
}

uint32_t SecureDataPublisher::GetCipherKeyRotationPeriod() const
{
    return m_cipherKeyRotationPeriod;
}

auto SecureDataPublisher::SetCipherKeyRotationPeriod(const uint32_t value) -> void
{
    m_cipherKeyRotationPeriod = value;
}

bool SecureDataPublisher::GetUseBaseTimeOffsets() const
{
    return m_useBaseTimeOffsets;
}

void SecureDataPublisher::SetUseBaseTimeOffsets(const bool value)
{
    m_useBaseTimeOffsets = value;
}

uint16_t SecureDataPublisher::GetPort() const
{
    return m_clientAcceptor.local_endpoint().port();
}

bool SecureDataPublisher::IsIPv6() const
{
    return m_clientAcceptor.local_endpoint().protocol() == tcp::v6();
}

void* SecureDataPublisher::GetUserData() const
{
    return m_userData;
}

void SecureDataPublisher::SetUserData(void* userData)
{
    m_userData = userData;
}

uint64_t SecureDataPublisher::GetTotalCommandChannelBytesSent()
{
    uint64_t totalCommandChannelBytesSent = 0LL;

    ReaderLock readLock(m_secureSubscriberConnectionsLock);

    for (const auto& connection : m_secureSubscriberConnections)
        totalCommandChannelBytesSent += connection->GetTotalCommandChannelBytesSent();

    return totalCommandChannelBytesSent;
}

uint64_t SecureDataPublisher::GetTotalDataChannelBytesSent()
{
    uint64_t totalDataChannelBytesSent = 0LL;

    ReaderLock readLock(m_secureSubscriberConnectionsLock);

    for (const auto& connection : m_secureSubscriberConnections)
        totalDataChannelBytesSent += connection->GetTotalDataChannelBytesSent();

    return totalDataChannelBytesSent;
}

uint64_t SecureDataPublisher::GetTotalMeasurementsSent()
{
    uint64_t totalMeasurementsSent = 0LL;

    ReaderLock readLock(m_secureSubscriberConnectionsLock);

    for (const auto& connection : m_secureSubscriberConnections)
        totalMeasurementsSent += connection->GetTotalMeasurementsSent();

    return totalMeasurementsSent;
}

void SecureDataPublisher::RegisterStatusMessageCallback(const MessageCallback& statusMessageCallback)
{
    m_statusMessageCallback = statusMessageCallback;
}

void SecureDataPublisher::RegisterErrorMessageCallback(const MessageCallback& errorMessageCallback)
{
    m_errorMessageCallback = errorMessageCallback;
}

void SecureDataPublisher::RegisterClientConnectedCallback(const SecureSubscriberConnectionCallback& clientConnectedCallback)
{
    m_clientConnectedCallback = clientConnectedCallback;
}

void SecureDataPublisher::RegisterClientDisconnectedCallback(const SecureSubscriberConnectionCallback& clientDisconnectedCallback)
{
    m_clientDisconnectedCallback = clientDisconnectedCallback;
}

void SecureDataPublisher::RegisterProcessingIntervalChangeRequestedCallback(const SecureSubscriberConnectionCallback& processingIntervalChangeRequestedCallback)
{
    m_processingIntervalChangeRequestedCallback = processingIntervalChangeRequestedCallback;
}

void SecureDataPublisher::RegisterTemporalSubscriptionRequestedCallback(const SecureSubscriberConnectionCallback& temporalSubscriptionRequestedCallback)
{
    m_temporalSubscriptionRequestedCallback = temporalSubscriptionRequestedCallback;
}

void SecureDataPublisher::RegisterTemporalSubscriptionCanceledCallback(const SecureSubscriberConnectionCallback& temporalSubscriptionCanceledCallback)
{
    m_temporalSubscriptionCanceledCallback = temporalSubscriptionCanceledCallback;
}

void SecureDataPublisher::RegisterUserCommandCallback(const UserCommandCallback& userCommandCallback)
{
    m_userCommandCallback = userCommandCallback;
}

void SecureDataPublisher::IterateSubscriberConnections(const SecureSubscriberConnectionIteratorHandlerFunction& iteratorHandler, void* userData)
{
    if (iteratorHandler == nullptr)
        return;
    
    ReaderLock readLock(m_secureSubscriberConnectionsLock);

    for (const auto& connection : m_secureSubscriberConnections)
        iteratorHandler(connection, userData);
}

void SecureDataPublisher::DisconnectSubscriber(const SecureSubscriberConnectionPtr& connection)
{
    if (connection != nullptr)
        RemoveConnection(connection);
}

void SecureDataPublisher::DisconnectSubscriber(const sttp::Guid& instanceID)
{
    SecureSubscriberConnectionPtr targetConnection = nullptr;

    IterateSubscriberConnections([&targetConnection, instanceID](const SecureSubscriberConnectionPtr& connection, void* userData)
    {
        if (connection->GetInstanceID() == instanceID)
            targetConnection = connection;
    },
    nullptr);

    DisconnectSubscriber(targetConnection);
}
