//******************************************************************************************************
//  Constants.cpp - Gbtc
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
//  02/20/2019 - J. Ritchie Carroll
//       Generated original version of source code.
//
//******************************************************************************************************

#include "Constants.h"

using namespace sttp::transport;

MeasurementStateFlags sttp::transport::operator &(MeasurementStateFlags lhs, MeasurementStateFlags rhs)
{
    return static_cast<MeasurementStateFlags> (
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(lhs) &
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(rhs)
    );
}

MeasurementStateFlags sttp::transport::operator ^(MeasurementStateFlags lhs, MeasurementStateFlags rhs)
{
    return static_cast<MeasurementStateFlags> (
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(lhs) ^
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(rhs)
    );
}

MeasurementStateFlags sttp::transport::operator ~(MeasurementStateFlags rhs)
{
    return static_cast<MeasurementStateFlags> (
        ~static_cast<std::underlying_type<MeasurementStateFlags>::type>(rhs)
    );
}

MeasurementStateFlags& sttp::transport::operator |=(MeasurementStateFlags &lhs, MeasurementStateFlags rhs)
{
    lhs = static_cast<MeasurementStateFlags> (
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(lhs) |
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(rhs)
    );

    return lhs;
}

MeasurementStateFlags& sttp::transport::operator &=(MeasurementStateFlags &lhs, MeasurementStateFlags rhs)
{
    lhs = static_cast<MeasurementStateFlags> (
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(lhs) &
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(rhs)
    );

    return lhs;
}

MeasurementStateFlags& sttp::transport::operator ^=(MeasurementStateFlags &lhs, MeasurementStateFlags rhs)
{
    lhs = static_cast<MeasurementStateFlags> (
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(lhs) ^
        static_cast<std::underlying_type<MeasurementStateFlags>::type>(rhs)
    );

    return lhs;
}

// Define "instances" of all static constants so values can be passed by reference and found by linker
const size_t Common::MaxPacketSize;
const uint32_t Common::PayloadHeaderSize;
const uint32_t Common::ResponseHeaderSize;

const uint8_t DataPacketFlags::Synchronized;
const uint8_t DataPacketFlags::Compact;
const uint8_t DataPacketFlags::CipherIndex;
const uint8_t DataPacketFlags::Compressed;
const uint8_t DataPacketFlags::LittleEndianCompression;
const uint8_t DataPacketFlags::NoFlags;

const uint8_t ServerCommand::Connect;
const uint8_t ServerCommand::MetadataRefresh;
const uint8_t ServerCommand::Subscribe;
const uint8_t ServerCommand::Unsubscribe;
const uint8_t ServerCommand::RotateCipherKeys;
const uint8_t ServerCommand::UpdateProcessingInterval;
const uint8_t ServerCommand::DefineOperationalModes;
const uint8_t ServerCommand::ConfirmNotification;
const uint8_t ServerCommand::ConfirmBufferBlock;
const uint8_t ServerCommand::UserCommand00;
const uint8_t ServerCommand::UserCommand01;
const uint8_t ServerCommand::UserCommand02;
const uint8_t ServerCommand::UserCommand03;
const uint8_t ServerCommand::UserCommand04;
const uint8_t ServerCommand::UserCommand05;
const uint8_t ServerCommand::UserCommand06;
const uint8_t ServerCommand::UserCommand07;
const uint8_t ServerCommand::UserCommand08;
const uint8_t ServerCommand::UserCommand09;
const uint8_t ServerCommand::UserCommand10;
const uint8_t ServerCommand::UserCommand11;
const uint8_t ServerCommand::UserCommand12;
const uint8_t ServerCommand::UserCommand13;
const uint8_t ServerCommand::UserCommand14;
const uint8_t ServerCommand::UserCommand15;

const uint8_t ServerResponse::Succeeded;
const uint8_t ServerResponse::Failed;
const uint8_t ServerResponse::DataPacket;
const uint8_t ServerResponse::UpdateSignalIndexCache;
const uint8_t ServerResponse::UpdateBaseTimes;
const uint8_t ServerResponse::UpdateCipherKeys;
const uint8_t ServerResponse::DataStartTime;
const uint8_t ServerResponse::ProcessingComplete;
const uint8_t ServerResponse::BufferBlock;
const uint8_t ServerResponse::Notify;
const uint8_t ServerResponse::ConfigurationChanged;
const uint8_t ServerResponse::UserResponse00;
const uint8_t ServerResponse::UserResponse01;
const uint8_t ServerResponse::UserResponse02;
const uint8_t ServerResponse::UserResponse03;
const uint8_t ServerResponse::UserResponse04;
const uint8_t ServerResponse::UserResponse05;
const uint8_t ServerResponse::UserResponse06;
const uint8_t ServerResponse::UserResponse07;
const uint8_t ServerResponse::UserResponse08;
const uint8_t ServerResponse::UserResponse09;
const uint8_t ServerResponse::UserResponse10;
const uint8_t ServerResponse::UserResponse11;
const uint8_t ServerResponse::UserResponse12;
const uint8_t ServerResponse::UserResponse13;
const uint8_t ServerResponse::UserResponse14;
const uint8_t ServerResponse::UserResponse15;
const uint8_t ServerResponse::NoOP;

const uint32_t OperationalModes::VersionMask;
const uint32_t OperationalModes::CompressionModeMask;
const uint32_t OperationalModes::EncodingMask;
const uint32_t OperationalModes::ReceiveExternalMetadata;
const uint32_t OperationalModes::ReceiveInternalMetadata;
const uint32_t OperationalModes::CompressPayloadData;
const uint32_t OperationalModes::CompressSignalIndexCache;
const uint32_t OperationalModes::CompressMetadata;
const uint32_t OperationalModes::NoFlags;

const uint32_t OperationalEncoding::UTF16LE;
const uint32_t OperationalEncoding::UTF16BE;
const uint32_t OperationalEncoding::UTF8;

const uint32_t CompressionModes::GZip;
const uint32_t CompressionModes::TSSC;
const uint32_t CompressionModes::None;