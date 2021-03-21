//******************************************************************************************************
//  DataSubscriber.cpp - Gbtc
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
//  03/26/2012 - Stephen C. Wills
//       Generated original version of source code.
//  03/22/2018 - J. Ritchie Carroll
//         Updated DataSubscriber callback function signatures to always include instance reference.
//
//******************************************************************************************************

// ReSharper disable CppClangTidyPerformanceNoAutomaticMove
// ReSharper disable CppClangTidyClangDiagnosticShadowUncapturedLocal
// ReSharper disable CppClangTidyClangDiagnosticMisleadingIndentation
#include "DataSubscriber.h"
#include "Constants.h"
#include "CompactMeasurement.h"
#include "../Convert.h"
#include "../EndianConverter.h"
#include "../Version.h"
#include <functional>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

using namespace std;
using namespace boost;
using namespace boost::asio;
using namespace boost::asio::ip;
using namespace sttp;
using namespace sttp::transport;

// --- SubscriptionInfo ---

SubscriptionInfo::SubscriptionInfo() :
    Throttled(false),
    PublishInterval(0.0),
    UdpDataChannel(false),
    DataChannelLocalPort(9500),
    IncludeTime(true),
    LagTime(10.0),
    LeadTime(5.0),
    UseLocalClockAsRealTime(false),
    UseMillisecondResolution(false),
    RequestNaNValueFilter(false),
    ProcessingInterval(-1)
{
}

// --- SubscriberConnector ---

SubscriberConnector::SubscriberConnector() :
    m_port(0),
    m_timer(nullptr),
    m_maxRetries(-1),
    m_retryInterval(2000),
    m_maxRetryInterval(120000),
    m_connectAttempt(0),
    m_connectionRefused(false),
    m_autoReconnect(true),
    m_cancel(false)
{
}

SubscriberConnector::~SubscriberConnector() noexcept
{
    try
    {
        Cancel();
    }
    catch (...)
    {
        // ReSharper disable once CppRedundantControlFlowJump
        return;
    }
}

// Auto-reconnect handler.
void SubscriberConnector::AutoReconnect(DataSubscriber* subscriber)
{
    SubscriberConnector& connector = subscriber->GetSubscriberConnector();

    if (connector.m_cancel || subscriber->m_disposing)
        return;

    // Make sure to wait on any running reconnect to complete...
    connector.m_autoReconnectThread.join();
    
    connector.m_autoReconnectThread = Thread([&connector,subscriber]
    {
        // Reset connection attempt counter if last attempt was not refused
        if (!connector.m_connectionRefused)
            connector.ResetConnection();
        
        if (connector.m_maxRetries != -1 && connector.m_connectAttempt >= connector.m_maxRetries)
        {
            if (connector.m_errorMessageCallback != nullptr)
                connector.m_errorMessageCallback(subscriber, "Maximum connection retries attempted. Auto-reconnect canceled.");
            return;
        }

        // Apply exponential back-off algorithm for retry attempt delays
        const int32_t exponent = (connector.m_connectAttempt > 13 ? 13 : connector.m_connectAttempt) - 1;
        int32_t retryInterval = connector.m_connectAttempt > 0 ? connector.m_retryInterval * static_cast<int32_t>(pow(2, exponent)) : 0;

        if (retryInterval > connector.m_maxRetryInterval)
            retryInterval = connector.m_maxRetryInterval;

        // Notify the user that we are attempting to reconnect.
        if (connector.m_errorMessageCallback != nullptr)
        {
            stringstream errorMessageStream;

            errorMessageStream << "Connection";

            if (connector.m_connectAttempt > 0)
                errorMessageStream << " attempt " << connector.m_connectAttempt + 1;

            errorMessageStream << " to \"" << connector.m_hostname << ":" << connector.m_port << "\" was terminated. ";
           
            if (retryInterval > 0)
                errorMessageStream << "Attempting to reconnect in " << retryInterval / 1000.0 << " seconds...";
            else
                errorMessageStream << "Attempting to reconnect...";

            connector.m_errorMessageCallback(subscriber, errorMessageStream.str());
        }

        connector.m_timer = Timer::WaitTimer(retryInterval);
        connector.m_timer->Wait();

        if (connector.m_cancel || subscriber->m_disposing)
            return;

        if (connector.Connect(*subscriber, true) == ConnectCanceled)
            return;

        // Notify the user that reconnect attempt was completed.
        if (!connector.m_cancel && connector.m_reconnectCallback != nullptr)
            connector.m_reconnectCallback(subscriber);
    });    
}

// Registers a callback to provide error messages each time
// the subscriber fails to connect during a connection sequence.
void SubscriberConnector::RegisterErrorMessageCallback(const ErrorMessageCallback& errorMessageCallback)
{
    m_errorMessageCallback = errorMessageCallback;
}

// Registers a callback to notify after an automatic reconnection attempt has been made.
void SubscriberConnector::RegisterReconnectCallback(const ReconnectCallback& reconnectCallback)
{
    m_reconnectCallback = reconnectCallback;
}

int SubscriberConnector::Connect(DataSubscriber& subscriber, const SubscriptionInfo& info)
{
    if (m_cancel)
        return ConnectCanceled;

    subscriber.SetSubscriptionInfo(info);
    return Connect(subscriber, false, "");
}

int SubscriberConnector::Connect(DataSubscriber& subscriber, const SubscriptionInfo& info, const string& cert_file) {
    if (m_cancel)
        return ConnectCanceled;
    subscriber.SetSubscriptionInfo(info);
    return Connect(subscriber, false, cert_file);
}

// Begin connection sequence.
int SubscriberConnector::Connect(DataSubscriber& subscriber, bool autoReconnecting, const string& cert_file)
{
    if (m_autoReconnect)
        subscriber.RegisterAutoReconnectCallback(&AutoReconnect);

    m_cancel = false;

    while (!subscriber.m_disposing)
    {
        if (m_maxRetries != -1 && m_connectAttempt >= m_maxRetries)
        {
            if (m_errorMessageCallback != nullptr)
                m_errorMessageCallback(&subscriber, "Maximum connection retries attempted. Auto-reconnect canceled.");
            break;
        }

        string errorMessage;
        bool connected = false;

        try
        {
            m_connectAttempt++;

            if (subscriber.m_disposing)
                return ConnectCanceled;

            subscriber.Connect(m_hostname, m_port, autoReconnecting, cert_file);
            
            connected = true;
            break;
        }
        catch (SubscriberException& ex)
        {
            errorMessage = ex.what();
        }
        catch (SystemError& ex)
        {
            errorMessage = ex.what();
        }
        catch (...)
        {
            errorMessage = current_exception_diagnostic_information(true);
        }

        if (!connected && !subscriber.m_disposing)
        {
            // Apply exponential back-off algorithm for retry attempt delays
            const int32_t exponent = (m_connectAttempt > 13 ? 13 : m_connectAttempt) - 1;
            int32_t retryInterval = m_connectAttempt > 0 ? m_retryInterval * static_cast<int32_t>(pow(2, exponent)) : 0;
            autoReconnecting = true;

            if (retryInterval > m_maxRetryInterval)
                retryInterval = m_maxRetryInterval;

            if (m_errorMessageCallback != nullptr)
            {
                stringstream errorMessageStream;
                errorMessageStream << "Connection";

                if (m_connectAttempt > 0)
                    errorMessageStream << " attempt " << m_connectAttempt + 1;

                errorMessageStream << " to \"" << m_hostname << ":" << m_port << "\" failed: " << errorMessage << ". ";
       
                if (retryInterval > 0)
                    errorMessageStream << "Attempting to reconnect in " << retryInterval / 1000.0 << " seconds...";
                else
                    errorMessageStream << "Attempting to reconnect...";

                m_errorMessageCallback(&subscriber, errorMessageStream.str());
            }

            if (retryInterval > 0)
            {
                m_timer = Timer::WaitTimer(retryInterval);
                m_timer->Wait();

                if (m_cancel)
                    return ConnectCanceled;
            }
        }
    }

    return subscriber.m_disposing ? ConnectCanceled : subscriber.IsConnected() ? ConnectSuccess : ConnectFailed;
}

// Cancel all current and future connection sequences.
void SubscriberConnector::Cancel()
{
    m_cancel = true;

    // Cancel any waiting timer operations by setting immediate timer expiration
    if (m_timer != nullptr)
        m_timer->Stop();

    m_autoReconnectThread.join();
}

// Sets the hostname of the publisher to connect to.
void SubscriberConnector::SetHostname(const string& hostname)
{
    m_hostname = hostname;
}

// Sets the port that the publisher is listening on.
void SubscriberConnector::SetPort(const uint16_t port)
{
    m_port = port;
}

// Sets the maximum number of retries during a connection sequence.
void SubscriberConnector::SetMaxRetries(const int32_t maxRetries)
{
    m_maxRetries = maxRetries;
}

// Sets the interval of idle time (in milliseconds) between connection attempts.
void SubscriberConnector::SetRetryInterval(const int32_t retryInterval)
{
    m_retryInterval = retryInterval;
}

// Sets maximum retry interval - connection retry attempts use exponential
// back-off algorithm up to this defined maximum.
void SubscriberConnector::SetMaxRetryInterval(const int32_t maxRetryInterval)
{
    m_maxRetryInterval = maxRetryInterval;
}

// Sets the flag that determines whether the subscriber should
// automatically attempt to reconnect when the connection is terminated.
void SubscriberConnector::SetAutoReconnect(const bool autoReconnect)
{
    m_autoReconnect = autoReconnect;
}

// Sets flag indicating connection was refused.
void SubscriberConnector::SetConnectionRefused(const bool connectionRefused)
{
    m_connectionRefused = connectionRefused;
}

// Resets connector for a new connection
void SubscriberConnector::ResetConnection()
{
    m_connectAttempt = 0;
    m_cancel = false;
}

// Gets the hostname of the publisher to connect to.
string SubscriberConnector::GetHostname() const
{
    return m_hostname;
}

// Gets the port that the publisher is listening on.
uint16_t SubscriberConnector::GetPort() const
{
    return m_port;
}

// Gets the maximum number of retries during a connection sequence.
int32_t SubscriberConnector::GetMaxRetries() const
{
    return m_maxRetries;
}

// Gets the interval of idle time between connection attempts.
int32_t SubscriberConnector::GetRetryInterval() const
{
    return m_retryInterval;
}

// Gets maximum retry interval - connection retry attempts use exponential
// back-off algorithm up to this defined maximum.
int32_t SubscriberConnector::GetMaxRetryInterval() const
{
    return m_maxRetryInterval;
}

// Gets the flag that determines whether the subscriber should
// automatically attempt to reconnect when the connection is terminated.
bool SubscriberConnector::GetAutoReconnect() const
{
    return m_autoReconnect;
}

// Gets flag indicating connection was refused.
bool SubscriberConnector::GetConnectionRefused() const
{
    return m_connectionRefused;
}

// --- DataSubscriber ---

DataSubscriber::DataSubscriber() :
    m_subscriberID(Empty::Guid),
    m_compressPayloadData(true),
    m_compressMetadata(true),
    m_compressSignalIndexCache(true),
    m_connected(false),
    m_subscribed(false),
    m_disconnecting(false),
    m_disconnected(false),
    m_disposing(false),
    m_userData(nullptr),
    m_totalCommandChannelBytesReceived(0UL),
    m_totalDataChannelBytesReceived(0UL),
    m_totalMeasurementsReceived(0UL),
    m_assemblySource(STTP_TITLE),
    m_assemblyVersion(STTP_VERSION),
    m_assemblyUpdatedOn(STTP_UPDATEDON),
    m_signalIndexCache(nullptr),
    m_timeIndex(0),
    m_baseTimeOffsets { 0, 0 },
    m_tsscResetRequested(false),
    m_tsscSequenceNumber(0),
    m_commandChannelSocket(m_commandChannelService),
    m_commandContext(boost::asio::ssl::context::sslv23),
    m_commandSecureChannelSocket(m_commandChannelService, m_commandContext),
    m_readBuffer(Common::MaxPacketSize),
    m_writeBuffer(Common::MaxPacketSize),
    m_dataChannelSocket(m_dataChannelService)
{
}

// Destructor calls disconnect to clean up after itself.
DataSubscriber::~DataSubscriber() noexcept
{
    try
    {
        m_disposing = true;
        m_connector.Cancel();
        Disconnect(true, false);
    }
    catch (...)
    {
        // ReSharper disable once CppRedundantControlFlowJump
        return;
    }
}

DataSubscriber::CallbackDispatcher::CallbackDispatcher() :
    Source(nullptr),
    Data(nullptr),
    Function(nullptr)
{
}

// All callbacks are run from the callback thread from here.
void DataSubscriber::RunCallbackThread()
{
    while (true)
    {
        m_callbackQueue.WaitForData();

        if (IsDisconnecting())
            break;

        const CallbackDispatcher dispatcher = m_callbackQueue.Dequeue();

        if (dispatcher.Function != nullptr)
            dispatcher.Function(dispatcher.Source, *dispatcher.Data);
    }
}

// All responses received from the server are handled by this thread with the
// exception of data packets which may or may not be handled by this thread.
void DataSubscriber::RunCommandChannelResponseThread()
{
    async_read(m_commandChannelSocket, buffer(m_readBuffer, Common::PayloadHeaderSize), [this]<typename T0, typename T1>(T0&& error, T1&& bytesTransferred)
    {
        ReadPayloadHeader(error, bytesTransferred);
    });

    m_commandChannelService.run();
}

// Callback for async read of the payload header.
void DataSubscriber::ReadPayloadHeader(const ErrorCode& error, const size_t bytesTransferred)
{
    if (IsDisconnecting())
        return;

    if (error == error::connection_aborted || error == error::connection_reset || error == error::eof)
    {
        // Connection closed by peer; terminate connection
        m_connectionTerminationThread = Thread([this] { ConnectionTerminatedDispatcher(); });
        return;
    }

    if (error)
    {
        stringstream errorMessageStream;

        errorMessageStream << "Error reading data from command channel: ";
        errorMessageStream << SystemError(error).what();

        DispatchErrorMessage(errorMessageStream.str());
        return;
    }

    // Gather statistics
    m_totalCommandChannelBytesReceived += Common::PayloadHeaderSize;

    const uint32_t packetSize = EndianConverter::ToBigEndian<uint32_t>(m_readBuffer.data(), 0);

    if (packetSize > ConvertUInt32(m_readBuffer.size()))
        m_readBuffer.resize(packetSize);

    // Read packet (payload body)
    // This read method is guaranteed not to return until the
    // requested size has been read or an error has occurred.
    async_read(m_commandChannelSocket, buffer(m_readBuffer, packetSize), [this]<typename T0, typename T1>(T0&& error, T1&& bytesTransferred) // NOLINT
    {
        ReadPacket(error, bytesTransferred);
    });
}

// Callback for async read of packets.
void DataSubscriber::ReadPacket(const ErrorCode& error, const size_t bytesTransferred)
{
    if (IsDisconnecting())
        return;

    if (error == error::connection_aborted || error == error::connection_reset || error == error::eof)
    {
        // Connection closed by peer; terminate connection
        m_connectionTerminationThread = Thread([this] { ConnectionTerminatedDispatcher(); });
        return;
    }

    if (error)
    {
        stringstream errorMessageStream;

        errorMessageStream << "Error reading data from command channel: ";
        errorMessageStream << SystemError(error).what();

        DispatchErrorMessage(errorMessageStream.str());
        return;
    }

    // Gather statistics
    m_totalCommandChannelBytesReceived += bytesTransferred;

    // Process response
    ProcessServerResponse(&m_readBuffer[0], 0, ConvertUInt32(bytesTransferred));

    // Read next payload header
    async_read(m_commandChannelSocket, buffer(m_readBuffer, Common::PayloadHeaderSize), [this]<typename T0, typename T1>(T0&& error, T1&& bytesTransferred) // NOLINT
    {
        ReadPayloadHeader(error, bytesTransferred);
    });
}

// If the user defines a separate UDP channel for their
// subscription, data packets get handled from this thread.
void DataSubscriber::RunDataChannelResponseThread()
{
    vector<uint8_t> buffer(Common::MaxPacketSize);

    udp::endpoint endpoint(m_hostAddress, 0);
    ErrorCode error;
    stringstream errorMessageStream;

    while (true)
    {
        const uint32_t length = ConvertUInt32(m_dataChannelSocket.receive_from(asio::buffer(buffer), endpoint, 0, error));

        if (IsDisconnecting())
            break;

        if (error)
        {
            errorMessageStream << "Error reading data from command channel: ";
            errorMessageStream << SystemError(error).what();
            DispatchErrorMessage(errorMessageStream.str());
            break;
        }

        // Gather statistics
        m_totalDataChannelBytesReceived += length;

        ProcessServerResponse(&buffer[0], 0, length);
    }
}

// Processes a response sent by the server. Response codes are defined in the header file "Constants.h".
void DataSubscriber::ProcessServerResponse(uint8_t* buffer, const uint32_t offset, const uint32_t length)
{
    uint8_t* packetBodyStart = buffer + Common::ResponseHeaderSize;
    const uint32_t packetBodyLength = length - Common::ResponseHeaderSize;

    const uint8_t responseCode = buffer[0];
    const uint8_t commandCode = buffer[1];

    switch (responseCode)
    {
        case ServerResponse::Succeeded:
            HandleSucceeded(commandCode, packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::Failed:
            HandleFailed(commandCode, packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::DataPacket:
            HandleDataPacket(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::DataStartTime:
            HandleDataStartTime(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::ProcessingComplete:
            HandleProcessingComplete(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::UpdateSignalIndexCache:
            HandleUpdateSignalIndexCache(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::UpdateBaseTimes:
            HandleUpdateBaseTimes(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::ConfigurationChanged:
            HandleConfigurationChanged(packetBodyStart, 0, packetBodyLength);
            break;

        case ServerResponse::NoOP:
            break;

        default:
            stringstream errorMessageStream;
            errorMessageStream << "Encountered unexpected server response code: ";
            errorMessageStream << ToHex(responseCode);
            DispatchErrorMessage(errorMessageStream.str());
            break;
    }
}

// Handles success messages received from the server.
void DataSubscriber::HandleSucceeded(const uint8_t commandCode, uint8_t* data, const uint32_t offset, const uint32_t length)
{
    const uint32_t messageLength = ConvertUInt32(length / sizeof(char));
    stringstream messageStream;

    switch (commandCode)
    {
        case ServerCommand::MetadataRefresh:
            // Metadata refresh message is not sent with a
            // message, but rather the metadata itself.
            HandleMetadataRefresh(data, offset, length);
            break;
        case ServerCommand::Subscribe:
        case ServerCommand::Unsubscribe:
            // Do not break on these messages because there is
            // still an associated message to be processed.
            m_subscribed = (commandCode == ServerCommand::Subscribe);
            [[fallthrough]];
        case ServerCommand::UpdateProcessingInterval:
        case ServerCommand::RotateCipherKeys:
            // Each of these responses come with a message that will
            // be delivered to the user via the status message callback.
            if (data != nullptr)
            {
                char* messageStart = reinterpret_cast<char*>(data + offset);
                char* messageEnd = messageStart + messageLength;
                messageStream << "Received success code in response to server command " << ToHex(commandCode) << ": ";

                for (char* messageIter = messageStart; messageIter < messageEnd; ++messageIter)
                    messageStream << *messageIter;

                DispatchStatusMessage(messageStream.str());
            }
            break;
        default:
            // If we don't know what the message is, we can't interpret
            // the data sent with the packet. Deliver an error message
            // to the user via the error message callback.
            messageStream << "Received success code in response to unknown server command " << ToHex(commandCode);
            DispatchErrorMessage(messageStream.str());
            break;
    }
}

// Handles failure messages from the server.
void DataSubscriber::HandleFailed(const uint8_t commandCode, uint8_t* data, const uint32_t offset, const uint32_t length)
{
    if (data == nullptr)
        return;

    const uint32_t messageLength = ConvertUInt32(length / sizeof(char));
    stringstream messageStream;

    char* messageStart = reinterpret_cast<char*>(data + offset);
    char* messageEnd = messageStart + messageLength;

    if (commandCode == ServerCommand::Connect)
        m_connector.SetConnectionRefused(true);
    else
        messageStream << "Received failure code from server command " << ToHex(commandCode) << ": ";

    for (char* messageIter = messageStart; messageIter < messageEnd; ++messageIter)
        messageStream << *messageIter;

    DispatchErrorMessage(messageStream.str());
}

// Handles metadata refresh messages from the server.
void DataSubscriber::HandleMetadataRefresh(uint8_t* data, const uint32_t offset, const uint32_t length)
{
    Dispatch(&MetadataDispatcher, data, offset, length);
}

// Handles data start time reported by the server at the beginning of a subscription.
void DataSubscriber::HandleDataStartTime(uint8_t* data, const uint32_t offset, const uint32_t length)
{
    Dispatch(&DataStartTimeDispatcher, data, offset, length);
}

// Handles processing complete message sent by the server at the end of a temporal session.
void DataSubscriber::HandleProcessingComplete(uint8_t* data, const uint32_t offset, const uint32_t length)
{
    Dispatch(&ProcessingCompleteDispatcher, data, offset, length);
}

// Cache signal IDs sent by the server into the signal index cache.
void DataSubscriber::HandleUpdateSignalIndexCache(uint8_t* data, const uint32_t offset, const uint32_t length)
{
    if (data == nullptr)
        return;

    vector<uint8_t> uncompressedBuffer;

    if (m_compressSignalIndexCache)
    {
        const MemoryStream memoryStream(data, offset, length);

        // Perform zlib decompression on buffer
        StreamBuffer streamBuffer;

        streamBuffer.push(GZipDecompressor());
        streamBuffer.push(memoryStream);

        CopyStream(&streamBuffer, uncompressedBuffer);
    }
    else
    {
        WriteBytes(uncompressedBuffer, data, offset, length);
    }

    SignalIndexCachePtr signalIndexCache = NewSharedPtr<SignalIndexCache>();
    signalIndexCache->Parse(uncompressedBuffer, m_subscriberID);
    m_signalIndexCache.swap(signalIndexCache);

    DispatchSubscriptionUpdated(AddDispatchReference(m_signalIndexCache));
}

// Updates base time offsets.
void DataSubscriber::HandleUpdateBaseTimes(uint8_t* data, const uint32_t offset, const uint32_t length)
{
    if (data == nullptr)
        return;

    int32_t* timeIndexPtr = reinterpret_cast<int32_t*>(data + offset);
    int64_t* timeOffsetsPtr = reinterpret_cast<int64_t*>(timeIndexPtr + 1); //-V1032

    m_timeIndex = EndianConverter::Default.ConvertBigEndian(*timeIndexPtr);
    m_baseTimeOffsets[0] = EndianConverter::Default.ConvertBigEndian(timeOffsetsPtr[0]);
    m_baseTimeOffsets[1] = EndianConverter::Default.ConvertBigEndian(timeOffsetsPtr[1]);

    DispatchStatusMessage("Received new base time offset from publisher: " + ToString(FromTicks(m_baseTimeOffsets[m_timeIndex ^ 1])));
}

// Handles configuration changed message sent by the server at the end of a temporal session.
void DataSubscriber::HandleConfigurationChanged(uint8_t* data, const uint32_t offset, const uint32_t length)
{
    Dispatch(&ConfigurationChangedDispatcher);
}

// Handles data packets from the server. Decodes the measurements and provides them to the user via the new measurements callback.
void DataSubscriber::HandleDataPacket(uint8_t* data, uint32_t offset, const uint32_t length)
{
    const NewMeasurementsCallback newMeasurementsCallback = m_newMeasurementsCallback;

    if (newMeasurementsCallback != nullptr)
    {
        SubscriptionInfo& info = m_subscriptionInfo;
        uint8_t dataPacketFlags;
        int64_t frameLevelTimestamp = -1;

        bool includeTime = info.IncludeTime;

        // Read data packet flags
        dataPacketFlags = data[offset];
        offset++;

        // Read frame-level timestamp, if available
        if (dataPacketFlags & DataPacketFlags::Synchronized)
        {
            frameLevelTimestamp = EndianConverter::ToBigEndian<int64_t>(data, offset);
            offset += 8;
            includeTime = false;
        }

        // Read measurement count and gather statistics
        const uint32_t count = EndianConverter::ToBigEndian<uint32_t>(data, offset);
        m_totalMeasurementsReceived += count;
        offset += 4; //-V112

        vector<MeasurementPtr> measurements;

        if (dataPacketFlags & DataPacketFlags::Compressed)
            ParseTSSCMeasurements(data, offset, length, measurements);
        else
            ParseCompactMeasurements(data, offset, length, includeTime, info.UseMillisecondResolution, frameLevelTimestamp, measurements);

        newMeasurementsCallback(this, measurements);
    }
}

void DataSubscriber::ParseTSSCMeasurements(uint8_t* data, uint32_t offset, const uint32_t length, vector<MeasurementPtr>& measurements)
{
    MeasurementPtr measurement;
    string errorMessage;

    if (data[offset] != 85)
    {
        stringstream errorMessageStream;

        errorMessageStream << "TSSC version not recognized: ";
        errorMessageStream << ToHex(data[offset]);

        throw SubscriberException(errorMessageStream.str());
    }
    offset++;

    const uint16_t sequenceNumber = EndianConverter::ToBigEndian<uint16_t>(data, offset);
    offset += 2;

    if (sequenceNumber == 0 && m_tsscSequenceNumber > 0)
    {
        if (!m_tsscResetRequested)
        {
            stringstream statusMessageStream;
            statusMessageStream << "TSSC algorithm reset before sequence number: ";
            statusMessageStream << m_tsscSequenceNumber;
            DispatchStatusMessage(statusMessageStream.str());
        }

        m_tsscDecoder.Reset();
        m_tsscSequenceNumber = 0;
        m_tsscResetRequested = false;
    }

    if (m_tsscSequenceNumber != sequenceNumber)
    {
        if (!m_tsscResetRequested)
        {
            stringstream errorMessageStream;
            errorMessageStream << "TSSC is out of sequence. Expecting: ";
            errorMessageStream << m_tsscSequenceNumber;
            errorMessageStream << ", Received: ";
            errorMessageStream << sequenceNumber;
            DispatchErrorMessage(errorMessageStream.str());
        }

        // Ignore packets until the reset has occurred.
        return;
    }

    try
    {
        m_tsscDecoder.SetBuffer(data, offset, length);

        Guid signalID;
        string measurementSource;
        uint64_t measurementID;
        int32_t id;
        int64_t time;
        uint32_t quality;
        float32_t value;

        while (m_tsscDecoder.TryGetMeasurement(id, time, quality, value))
        {
            if (m_signalIndexCache->GetMeasurementKey(id, signalID, measurementSource, measurementID))
            {
                measurement = NewSharedPtr<Measurement>();

                measurement->SignalID = signalID;
                measurement->Source = measurementSource;
                measurement->ID = measurementID;
                measurement->Timestamp = time;
                measurement->Flags = static_cast<MeasurementStateFlags>(quality);
                measurement->Value = static_cast<float64_t>(value);

                measurements.push_back(measurement);
            }
        }
    }
    catch (SubscriberException& ex)
    {
        errorMessage = ex.what();
    }
    catch (...)
    {
        errorMessage = current_exception_diagnostic_information(true);
    }

    if (errorMessage.length() > 0)
    {
        stringstream errorMessageStream;
        errorMessageStream << "Decompression failure: ";
        errorMessageStream << errorMessage;
        DispatchErrorMessage(errorMessageStream.str());
    }

    m_tsscSequenceNumber++;

    // Do not increment to 0 on roll-over
    if (m_tsscSequenceNumber == 0)
        m_tsscSequenceNumber = 1;
}

void DataSubscriber::ParseCompactMeasurements(uint8_t* data, uint32_t offset, const uint32_t length, const bool includeTime, const bool useMillisecondResolution, const int64_t frameLevelTimestamp, vector<MeasurementPtr>& measurements)
{
    const MessageCallback errorMessageCallback = m_errorMessageCallback;

    if (m_signalIndexCache == nullptr)
        return;

    // Create measurement parser
    const CompactMeasurement parser(m_signalIndexCache, m_baseTimeOffsets, includeTime, useMillisecondResolution);

    while (length != offset)
    {
        MeasurementPtr measurement;

        if (!parser.TryParseMeasurement(data, offset, length, measurement))
        {
            if (errorMessageCallback != nullptr)
                errorMessageCallback(this, "Error parsing measurement");

            break;
        }

        if (frameLevelTimestamp > -1)
            measurement->Timestamp = frameLevelTimestamp;

        measurements.push_back(measurement);
    }
}

bool VerifyCertificate(bool preverified, boost::asio::ssl::verify_context& ctx)
{
    // The verify callback can be used to check whether the certificate that is
    // being presented is valid for the peer. For example, RFC 2818 describes
    // the steps involved in doing this for HTTPS. Consult the OpenSSL
    // documentation for more details. Note that the callback is called once
    // for each certificate in the certificate chain, starting from the root
    // certificate authority.

    // In this example we will simply print the certificate's subject name.
    char subject_name[256];
    X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
    X509_NAME_oneline(X509_get_subject_name(cert), subject_name, 256);

    return preverified;
}

SignalIndexCache* DataSubscriber::AddDispatchReference(SignalIndexCachePtr signalIndexCacheRef) // NOLINT
{
    SignalIndexCache* signalIndexCachePtr = signalIndexCacheRef.get();

    // Note that no lock is needed for m_signalIndexCacheDispatchRefs set since all calls from
    // HandleUpdateSignalIndexCache are from a single connection and are processed sequentially

    // Hold onto signal index cache shared pointer until it's delivered
    m_signalIndexCacheDispatchRefs.emplace(signalIndexCacheRef);

    return signalIndexCachePtr;
}

SignalIndexCachePtr DataSubscriber::ReleaseDispatchReference(SignalIndexCache* signalIndexCachePtr)
{
    const SignalIndexCachePtr signalIndexCacheRef = signalIndexCachePtr->GetReference();
    
    // Remove used reference to signal index cache pointer
    m_signalIndexCacheDispatchRefs.erase(signalIndexCacheRef);

    return signalIndexCacheRef;
}

// Dispatches the given function to the callback thread.
void DataSubscriber::Dispatch(const DispatcherFunction& function)
{
    Dispatch(function, nullptr, 0, 0);
}

// Dispatches the given function to the callback thread and provides the given data to that function when it is called.
void DataSubscriber::Dispatch(const DispatcherFunction& function, const uint8_t* data, const uint32_t offset, const uint32_t length)
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

void DataSubscriber::DispatchSubscriptionUpdated(SignalIndexCache* signalIndexCache)
{
    Dispatch(&SubscriptionUpdatedDispatcher, reinterpret_cast<uint8_t*>(&signalIndexCache), 0, sizeof(SignalIndexCache**));
}

// Invokes the status message callback on the callback thread and provides the given message to it.
void DataSubscriber::DispatchStatusMessage(const string& message)
{
    const uint32_t messageSize = ConvertUInt32((message.size() + 1) * sizeof(char));
    Dispatch(&StatusMessageDispatcher, reinterpret_cast<const uint8_t*>(message.c_str()), 0, messageSize);
}

// Invokes the error message callback on the callback thread and provides the given message to it.
void DataSubscriber::DispatchErrorMessage(const string& message)
{
    const uint32_t messageSize = ConvertUInt32((message.size() + 1) * sizeof(char));
    Dispatch(&ErrorMessageDispatcher, reinterpret_cast<const uint8_t*>(message.c_str()), 0, messageSize);
}

// Dispatcher function for status messages. Decodes the message and provides it to the user via the status message callback.
void DataSubscriber::StatusMessageDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;
    
    const MessageCallback statusMessageCallback = source->m_statusMessageCallback;
    
    if (statusMessageCallback != nullptr)
        statusMessageCallback(source, reinterpret_cast<const char*>(&buffer[0]));
}

// Dispatcher function for error messages. Decodes the message and provides it to the user via the error message callback.
void DataSubscriber::ErrorMessageDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    const MessageCallback errorMessageCallback = source->m_errorMessageCallback;

    if (errorMessageCallback != nullptr)
        errorMessageCallback(source, reinterpret_cast<const char*>(&buffer[0]));
}

// Dispatcher function for data start time. Decodes the start time and provides it to the user via the data start time callback.
void DataSubscriber::DataStartTimeDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    const DataStartTimeCallback dataStartTimeCallback = source->m_dataStartTimeCallback;

    if (dataStartTimeCallback != nullptr)
    {
        const int64_t dataStartTime = EndianConverter::ToBigEndian<int64_t>(buffer.data(), 0);
        dataStartTimeCallback(source, dataStartTime);
    }
}

// Dispatcher function for metadata. Provides encoded metadata to the user via the metadata callback.
void DataSubscriber::MetadataDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr) // Empty buffer OK
        return;

    const MetadataCallback metadataCallback = source->m_metadataCallback;

    if (metadataCallback != nullptr)
        metadataCallback(source, buffer);
}

void DataSubscriber::SubscriptionUpdatedDispatcher(DataSubscriber* source, const std::vector<uint8_t>& buffer)
{
    if (source == nullptr || buffer.empty())
        return;

    SignalIndexCache* signalIndexCachePtr = *reinterpret_cast<SignalIndexCache**>(const_cast<uint8_t*>(&buffer[0]));

    if (signalIndexCachePtr != nullptr)
    {
        const SubscriptionUpdatedCallback subscriptionUpdated = source->m_subscriptionUpdatedCallback;
        const SignalIndexCachePtr signalIndexCacheRef = source->ReleaseDispatchReference(signalIndexCachePtr); //-V821

        if (subscriptionUpdated != nullptr)
            subscriptionUpdated(source, signalIndexCacheRef);
    }
}

// Dispatcher for processing complete message that is sent by the server at the end of a temporal session.
void DataSubscriber::ProcessingCompleteDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr) // Empty buffer OK
        return;

    const MessageCallback processingCompleteCallback = source->m_processingCompleteCallback;

    if (processingCompleteCallback != nullptr)
    {
        stringstream messageStream;

        for (uint32_t i = 0; i < ConvertUInt32(buffer.size()); ++i)
            messageStream << buffer[i];

        processingCompleteCallback(source, messageStream.str());
    }
}

// Dispatcher for processing configuration changed message that is sent by the server when measurement availability has changed
void DataSubscriber::ConfigurationChangedDispatcher(DataSubscriber* source, const vector<uint8_t>& buffer)
{
    if (source == nullptr) // Empty buffer OK
        return;

    const ConfigurationChangedCallback configurationChangedCallback = source->m_configurationChangedCallback;

    if (configurationChangedCallback != nullptr)
        configurationChangedCallback(source);
}

// Dispatcher for connection terminated. This is called from its own separate thread
// in order to cleanly shut down the subscriber in case the connection was terminated
// by the peer. Additionally, this allows the user to automatically reconnect in their
// callback function without having to spawn their own separate thread.
void DataSubscriber::ConnectionTerminatedDispatcher()
{
    Disconnect(false, true);
}

// Registers the status message callback.
void DataSubscriber::RegisterStatusMessageCallback(const MessageCallback& statusMessageCallback)
{
    m_statusMessageCallback = statusMessageCallback;
}

// Registers the error message callback.
void DataSubscriber::RegisterErrorMessageCallback(const MessageCallback& errorMessageCallback)
{
    m_errorMessageCallback = errorMessageCallback;
}

// Registers the data start time callback.
void DataSubscriber::RegisterDataStartTimeCallback(const DataStartTimeCallback& dataStartTimeCallback)
{
    m_dataStartTimeCallback = dataStartTimeCallback;
}

// Registers the metadata callback.
void DataSubscriber::RegisterMetadataCallback(const MetadataCallback& metadataCallback)
{
    m_metadataCallback = metadataCallback;
}

// Registers the subscription updated callback.
void DataSubscriber::RegisterSubscriptionUpdatedCallback(const SubscriptionUpdatedCallback& subscriptionUpdatedCallback)
{
    m_subscriptionUpdatedCallback = subscriptionUpdatedCallback;
}

// Registers the new measurements callback.
void DataSubscriber::RegisterNewMeasurementsCallback(const NewMeasurementsCallback& newMeasurementsCallback)
{
    m_newMeasurementsCallback = newMeasurementsCallback;
}

// Registers the processing complete callback.
void DataSubscriber::RegisterProcessingCompleteCallback(const MessageCallback& processingCompleteCallback)
{
    m_processingCompleteCallback = processingCompleteCallback;
}

// Registers the configuration changed callback.
void DataSubscriber::RegisterConfigurationChangedCallback(const ConfigurationChangedCallback& configurationChangedCallback)
{
    m_configurationChangedCallback = configurationChangedCallback;
}

// Registers the connection terminated callback.
void DataSubscriber::RegisterConnectionTerminatedCallback(const ConnectionTerminatedCallback& connectionTerminatedCallback)
{
    m_connectionTerminatedCallback = connectionTerminatedCallback;
}

// Registers the auto-reconnect callback.
void DataSubscriber::RegisterAutoReconnectCallback(const ConnectionTerminatedCallback& autoReconnectCallback)
{
    m_autoReconnectCallback = autoReconnectCallback;
}

const Guid& DataSubscriber::GetSubscriberID() const
{
    return m_subscriberID;
}

// Returns true if payload data is compressed (TSSC only).
bool DataSubscriber::IsPayloadDataCompressed() const
{
    return m_compressPayloadData;
}

// Set the value which determines whether payload data is compressed.
void DataSubscriber::SetPayloadDataCompressed(const bool compressed)
{
    // This operational mode can only be changed before connect - dynamic updates not supported
    m_compressPayloadData = compressed;
}

// Returns true if metadata exchange is compressed (GZip only).
bool DataSubscriber::IsMetadataCompressed() const
{
    return m_compressMetadata;
}

// Set the value which determines whether metadata exchange is compressed.
void DataSubscriber::SetMetadataCompressed(const bool compressed)
{
    m_compressMetadata = compressed;

    if (m_commandChannelSocket.is_open())
        SendOperationalModes();
}

// Returns true if signal index cache exchange is compressed (GZip only).
bool DataSubscriber::IsSignalIndexCacheCompressed() const
{
    return m_compressSignalIndexCache;
}

// Set the value which determines whether signal index cache exchange is compressed.
void DataSubscriber::SetSignalIndexCacheCompressed(const bool compressed)
{
    m_compressSignalIndexCache = compressed;

    if (m_commandChannelSocket.is_open())
        SendOperationalModes();
}

// Gets user defined data reference
void* DataSubscriber::GetUserData() const
{
    return m_userData;
}

// Sets user defined data reference
void DataSubscriber::SetUserData(void* userData)
{
    m_userData = userData;
}

SubscriberConnector& DataSubscriber::GetSubscriberConnector()
{
    return m_connector;
}

// Returns the subscription info object used to define the most recent subscription.
const SubscriptionInfo& DataSubscriber::GetSubscriptionInfo() const
{
    return m_subscriptionInfo;
}

void DataSubscriber::SetSubscriptionInfo(const SubscriptionInfo& info)
{
    m_subscriptionInfo = info;
}

// Synchronously connects to publisher.
// public:
void DataSubscriber::Connect(const string& hostname, const uint16_t port, const string& cert_file)
{
    // User requests to connect are not an auto-reconnect attempt
    Connect(hostname, port, false, cert_file);
}

// private:
void DataSubscriber::Connect(const string& hostname, const uint16_t port, const bool autoReconnecting, const string cert_file)
{
    if (m_connected)
        throw SubscriberException("Subscriber is already connected; disconnect first");

    // Let any pending connect or disconnect operation complete before new connect - prevents destruction disconnect before connection is completed
    ScopeLock lock(m_connectActionMutex);
    DnsResolver resolver(m_commandChannelService);
    const DnsResolver::query dnsQuery(hostname, to_string(port));
    ErrorCode error;

    m_disconnected = false;
    m_totalCommandChannelBytesReceived = 0UL;
    m_totalDataChannelBytesReceived = 0UL;
    m_totalMeasurementsReceived = 0UL;    
    
    if (!autoReconnecting)
        m_connector.ResetConnection();

    m_connector.SetConnectionRefused(false);

    // TLS Connection in development
    string f = getenv("CERT_FILE");
    if (f) {
        try {
            m_context.load_verify_file(f);
            m_commandChannelSocket.set_verify_mode(ssl::verify_peer);
            m_commandChannelSocket.set_verify_callback([&](bool b, context c){ return this->VerifyCertificate(b, c););
            async_connect(m_commandChannelSocket.lowest_layer(), resolver.resolve(dnsQuery), m_udpPort),
            [this](const system::error_code& error,const tcp::endpoint& /*endpoint*/)
            {
                if (!error)
                {
                    m_commandChannelSocket.async_handshake(boost::asio::ssl::stream_base::client,
                    [this](const boost::system::error_code& error)
                    {
                        if (!error)
                        {
                            if (!m_commandChannelSocket.is_open())
                                throw SubscriberException("Failed to connect to host");

                            m_hostAddress = hostEndpoint.address();

                            #if BOOST_LEGACY
                                m_commandChannelService.reset();
                            #else
                                m_commandChannelService.restart();
                            #endif
                            m_callbackThread = Thread([this]{ RunCallbackThread(); });
                            m_commandChannelResponseThread = Thread([this]{ RunCommandChannelResponseThread(); });
                            m_connected = true;

                            SendOperationalModes();
                        }
                        else
                        {
                            throw SubscriberException("Handshake failed: " + error.message());
                        }
                    });
                }
                else
                {
                    throw SubscriberException("Connect failed: " + error.message());
                }
            });
        } catch {
            throw SubscriberException(e.what());
        }
    } else {
        const TcpEndPoint hostEndpoint = connect(m_commandChannelSocket, resolver.resolve(dnsQuery), error);

        if (error)
            throw SystemError(error);

        if (!m_commandChannelSocket.is_open())
            throw SubscriberException("Failed to connect to host");

        m_hostAddress = hostEndpoint.address();

        #if BOOST_LEGACY
            m_commandChannelService.reset();
        #else
            m_commandChannelService.restart();
        #endif

        m_callbackThread = Thread([this]{ RunCallbackThread(); });
        m_commandChannelResponseThread = Thread([this]{ RunCommandChannelResponseThread(); });
        m_connected = true;

        SendOperationalModes();
    }
}

// Disconnects from the publisher.
// public:
void DataSubscriber::Disconnect()
{
    if (IsDisconnecting())
        return;

    // Disconnect method executes shutdown on a separate thread without stopping to prevent
    // issues where user may call disconnect method from a dispatched event thread. Also,
    // user requests to disconnect are not an auto-reconnect attempt
    Disconnect(false, false);
}

// private:
void DataSubscriber::Disconnect(const bool joinThread, const bool autoReconnecting)
{
    // Check if disconnect thread is running or subscriber has already disconnected
    if (IsDisconnecting())
    {
        if (!autoReconnecting && m_disconnecting && !m_disconnected)
            m_connector.Cancel();
        
        if (joinThread && !m_disconnected)
            m_disconnectThread.join();

        return;
    }
    
    // Notify running threads that the subscriber is disconnecting, i.e., disconnect thread is active
    m_disconnecting = true;
    m_connected = false;
    m_subscribed = false;

    m_disconnectThread = Thread([this, autoReconnecting]
    {
        try
        {
            // Let any pending connect operation complete before disconnect - prevents destruction disconnect before connection is completed
            if (!autoReconnecting)
            {
                m_connector.Cancel();
                m_connectionTerminationThread.join();
                m_connectActionMutex.lock();
            }

            ErrorCode error;

            // Release queues and close sockets so that threads can shut down gracefully
            m_callbackQueue.Release();
            m_commandChannelSocket.close(error);
            m_dataChannelSocket.shutdown(UdpSocket::shutdown_receive, error);
            m_dataChannelSocket.close(error);

            // Join with all threads to guarantee their completion before returning control to the caller
            m_callbackThread.join();
            m_commandChannelResponseThread.join();
            m_dataChannelResponseThread.join();

            // Empty queues and reset them so they can be used again later if the user decides to reconnect
            m_callbackQueue.Clear();
            m_callbackQueue.Reset();

            // Notify consumers of disconnect
            if (m_connectionTerminatedCallback != nullptr)
                m_connectionTerminatedCallback(this);

            if (!autoReconnecting)
                m_commandChannelService.stop();
        }
        catch (SystemError& ex)
        {
            cerr << "Exception while disconnecting data subscriber: " << ex.what();
        }
        catch (...)
        {
            cerr << "Exception while disconnecting data subscriber: " << boost::current_exception_diagnostic_information(true);
        }

        // Disconnect complete
        m_disconnected = true;
        m_disconnecting = false;

        if (autoReconnecting)
        {
            // Handling auto-connect callback separately from connection terminated callback
            // since they serve two different use cases and current implementation does not
            // support multiple callback registrations
            if (m_autoReconnectCallback != nullptr && !m_disposing)
                m_autoReconnectCallback(this);
        }
        else
        {
            m_connectActionMutex.unlock();
        }
    });

    if (joinThread)
        m_disconnectThread.join();
}

void DataSubscriber::Subscribe(const SubscriptionInfo& info)
{
    SetSubscriptionInfo(info);
    Subscribe();
}

// Subscribe to publisher in order to start receiving data.
void DataSubscriber::Subscribe()
{
    stringstream connectionStream;
    vector<uint8_t> buffer;
    uint32_t bigEndianConnectionStringSize;

    // Make sure to unsubscribe before attempting another
    // subscription so we don't leave connections open
    if (m_subscribed)
        Unsubscribe();

    m_totalMeasurementsReceived = 0UL;

    connectionStream << "throttled=" << m_subscriptionInfo.Throttled << ";";
    connectionStream << "publishInterval" << m_subscriptionInfo.PublishInterval << ";";
    connectionStream << "includeTime=" << m_subscriptionInfo.IncludeTime << ";";
    connectionStream << "lagTime=" << m_subscriptionInfo.LagTime << ";";
    connectionStream << "leadTime=" << m_subscriptionInfo.LeadTime << ";";
    connectionStream << "useLocalClockAsRealTime=" << m_subscriptionInfo.UseLocalClockAsRealTime << ";";
    connectionStream << "processingInterval=" << m_subscriptionInfo.ProcessingInterval << ";";
    connectionStream << "useMillisecondResolution=" << m_subscriptionInfo.UseMillisecondResolution << ";";
    connectionStream << "requestNaNValueFilter" << m_subscriptionInfo.RequestNaNValueFilter << ";";
    connectionStream << "assemblyInfo={source=" << m_assemblySource << "; version="<< m_assemblyVersion <<"; updatedOn=" << m_assemblyUpdatedOn << "};";

    if (!m_subscriptionInfo.FilterExpression.empty())
        connectionStream << "filterExpression={" << m_subscriptionInfo.FilterExpression << "};";

    if (m_subscriptionInfo.UdpDataChannel)
    {
        udp ipVersion = udp::v4();

        if (m_hostAddress.is_v6())
            ipVersion = udp::v6();

        // Attempt to bind to local UDP port
        m_dataChannelSocket.open(ipVersion);
        m_dataChannelSocket.bind(udp::endpoint(ipVersion, m_subscriptionInfo.DataChannelLocalPort));
        m_dataChannelResponseThread = Thread([this]{ RunDataChannelResponseThread(); });

        if (!m_dataChannelSocket.is_open())
            throw SubscriberException("Failed to bind to local port");

        connectionStream << "dataChannel={localport=" << m_subscriptionInfo.DataChannelLocalPort << "};";
    }

    if (!m_subscriptionInfo.StartTime.empty())
        connectionStream << "startTimeConstraint=" << m_subscriptionInfo.StartTime << ";";

    if (!m_subscriptionInfo.StopTime.empty())
        connectionStream << "stopTimeConstraint=" << m_subscriptionInfo.StopTime << ";";

    if (!m_subscriptionInfo.ConstraintParameters.empty())
        connectionStream << "timeConstraintParameters=" << m_subscriptionInfo.ConstraintParameters << ";";

    if (!m_subscriptionInfo.ExtraConnectionStringParameters.empty())
        connectionStream << m_subscriptionInfo.ExtraConnectionStringParameters << ";";

    string connectionString = connectionStream.str();
    uint8_t* connectionStringPtr = reinterpret_cast<uint8_t*>(&connectionString[0]);
    const uint32_t connectionStringSize = ConvertUInt32(connectionString.size() * sizeof(char));
    bigEndianConnectionStringSize = EndianConverter::Default.ConvertBigEndian(connectionStringSize);
    uint8_t* bigEndianConnectionStringSizePtr = reinterpret_cast<uint8_t*>(&bigEndianConnectionStringSize);

    const uint32_t bufferSize = 5 + connectionStringSize;
    buffer.resize(bufferSize, 0);

    buffer[0] = DataPacketFlags::Compact;
    buffer[1] = bigEndianConnectionStringSizePtr[0];
    buffer[2] = bigEndianConnectionStringSizePtr[1];
    buffer[3] = bigEndianConnectionStringSizePtr[2];
    buffer[4] = bigEndianConnectionStringSizePtr[3];

    for (size_t i = 0; i < connectionStringSize; ++i)
        buffer[5 + i] = connectionStringPtr[i];

    SendServerCommand(ServerCommand::Subscribe, &buffer[0], 0, bufferSize);

    // Reset TSSC decompresser on successful (re)subscription
    m_tsscResetRequested = true;
}

// Unsubscribe from publisher to stop receiving data.
void DataSubscriber::Unsubscribe()
{
    ErrorCode error;

    m_disconnecting = true;
    m_dataChannelSocket.shutdown(UdpSocket::shutdown_receive, error);
    m_dataChannelSocket.close(error);
    m_dataChannelResponseThread.join();
    m_disconnecting = false;

    SendServerCommand(ServerCommand::Unsubscribe);
}

// Sends a command to the server.
void DataSubscriber::SendServerCommand(const uint8_t commandCode)
{
    SendServerCommand(commandCode, nullptr, 0, 0);
}

// Sends a command along with the given message to the server.
void DataSubscriber::SendServerCommand(const uint8_t commandCode, string message)
{
    vector<uint8_t> buffer;
    uint32_t bigEndianMessageSize;
    uint8_t* messagePtr = reinterpret_cast<uint8_t*>(&message[0]);
    const uint32_t messageSize = ConvertUInt32(message.size() * sizeof(char));

    bigEndianMessageSize = EndianConverter::Default.ConvertBigEndian(messageSize);
    uint8_t* bigEndianMessageSizePtr = reinterpret_cast<uint8_t*>(&bigEndianMessageSize);

    const uint32_t bufferSize = 4 + messageSize;
    buffer.resize(bufferSize, 0);

    buffer[0] = bigEndianMessageSizePtr[0];
    buffer[1] = bigEndianMessageSizePtr[1];
    buffer[2] = bigEndianMessageSizePtr[2];
    buffer[3] = bigEndianMessageSizePtr[3];

    for (size_t i = 0; i < messageSize; ++i)
        buffer[4 + i] = messagePtr[i];

    SendServerCommand(commandCode, &buffer[0], 0, bufferSize);
}

// Sends a command along with the given data to the server.
void DataSubscriber::SendServerCommand(const uint8_t commandCode, const uint8_t* data, const uint32_t offset, const uint32_t length)
{
    if (!m_connected)
        return;

    const uint32_t packetSize = length + 1;
    uint32_t bigEndianPacketSize = EndianConverter::Default.ConvertBigEndian(packetSize);
    uint8_t* bigEndianPacketSizePtr = reinterpret_cast<uint8_t*>(&bigEndianPacketSize);
    const uint32_t commandBufferSize = packetSize + Common::PayloadHeaderSize;

    if (commandBufferSize > ConvertUInt32(m_writeBuffer.size()))
        m_writeBuffer.resize(commandBufferSize);

    // Insert packet size
    m_writeBuffer[0] = bigEndianPacketSizePtr[0];
    m_writeBuffer[1] = bigEndianPacketSizePtr[1];
    m_writeBuffer[2] = bigEndianPacketSizePtr[2];
    m_writeBuffer[3] = bigEndianPacketSizePtr[3];

    // Insert command code
    m_writeBuffer[4] = commandCode;

    if (data != nullptr)
    {
        for (size_t i = 0; i < length; ++i)
            m_writeBuffer[5 + i] = data[offset + i];
    }

    async_write(m_commandChannelSocket, buffer(m_writeBuffer, commandBufferSize), [this]<typename T0, typename T1>(T0&& error, T1&& bytesTransferred)
    {
        WriteHandler(error, bytesTransferred);
    });
}

void DataSubscriber::WriteHandler(const ErrorCode& error, const size_t bytesTransferred)
{
    if (IsDisconnecting())
        return;

    if (error == error::connection_aborted || error == error::connection_reset || error == error::eof)
    {
        // Connection closed by peer; terminate connection
        m_connectionTerminationThread = Thread([this]{ ConnectionTerminatedDispatcher(); });
        return;
    }

    if (error)
    {
        stringstream errorMessageStream;

        errorMessageStream << "Error reading data from command channel: ";
        errorMessageStream << SystemError(error).what();

        DispatchErrorMessage(errorMessageStream.str());
    }
}

// Convenience method to send the currently defined
// and/or supported operational modes to the server.
void DataSubscriber::SendOperationalModes()
{
    uint32_t operationalModes = CompressionModes::GZip;
    uint32_t bigEndianOperationalModes;

    operationalModes |= OperationalModes::VersionMask & 1U;
    operationalModes |= OperationalEncoding::UTF8;

    // TSSC compression only works with stateful connections
    if (m_compressPayloadData && !m_subscriptionInfo.UdpDataChannel)
        operationalModes |= OperationalModes::CompressPayloadData | CompressionModes::TSSC;

    if (m_compressMetadata)
        operationalModes |= OperationalModes::CompressMetadata;

    if (m_compressSignalIndexCache)
        operationalModes |= OperationalModes::CompressSignalIndexCache;

    bigEndianOperationalModes = EndianConverter::Default.ConvertBigEndian(operationalModes);
    SendServerCommand(ServerCommand::DefineOperationalModes, reinterpret_cast<uint8_t*>(&bigEndianOperationalModes), 0, 4);
}

// Gets the total number of bytes received via the command channel since last connection.
uint64_t DataSubscriber::GetTotalCommandChannelBytesReceived() const
{
    return m_totalCommandChannelBytesReceived;
}

// Gets the total number of bytes received via the data channel since last connection.
uint64_t DataSubscriber::GetTotalDataChannelBytesReceived() const
{
    if (m_subscriptionInfo.UdpDataChannel)
        return m_totalDataChannelBytesReceived;

    return m_totalCommandChannelBytesReceived;
}

// Gets the total number of measurements received since last subscription.
uint64_t DataSubscriber::GetTotalMeasurementsReceived() const
{
    return m_totalMeasurementsReceived;
}

// Indicates whether the subscriber is connected.
bool DataSubscriber::IsConnected() const
{
    return m_connected;
}

// Indicates whether the subscriber is subscribed.
bool DataSubscriber::IsSubscribed() const
{
    return m_subscribed;
}

void DataSubscriber::GetAssemblyInfo(string& source, string& version, string& updatedOn) const
{
    source = m_assemblySource;
    version = m_assemblyVersion;
    updatedOn = m_assemblyUpdatedOn;
}

void DataSubscriber::SetAssemblyInfo(const string& source, const string& version, const string& updatedOn)
{
    m_assemblySource = source;
    m_assemblyVersion = version;
    m_assemblyUpdatedOn = updatedOn;
}