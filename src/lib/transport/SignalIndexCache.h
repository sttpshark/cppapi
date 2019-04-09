//******************************************************************************************************
//  SignalIndexCache.h - Gbtc
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
//  03/08/2012 - Stephen C. Wills
//       Generated original version of source code.
//  02/07/2019 - J. Ritchie Carroll
//       Moved parse functionality into class, added generate functionality.
//
//******************************************************************************************************

#ifndef __SIGNAL_INDEX_CACHE_H
#define __SIGNAL_INDEX_CACHE_H

#include "../common/CommonTypes.h"
#include <unordered_set>

namespace sttp {
namespace transport
{
    class SubscriberConnection;
    typedef SharedPtr<SubscriberConnection> SubscriberConnectionPtr;

    // Maps 16-bit runtime IDs to 128-bit globally unique IDs.
    // Additionally provides reverse lookup and an extra mapping
    // to human-readable measurement keys.
    class SignalIndexCache
    {
    private:
        std::unordered_map<uint16_t, uint32_t> m_reference;
        std::vector<sttp::Guid> m_signalIDList;
        std::vector<std::string> m_sourceList;
        std::vector<uint32_t> m_idList;
        std::unordered_map<sttp::Guid, uint16_t> m_signalIDCache;
        uint32_t m_binaryLength;

    public:
        SignalIndexCache();

        // Adds a measurement key to the cache.
        void AddMeasurementKey(uint16_t signalIndex, const sttp::Guid& signalID, const std::string& source, uint32_t id, uint32_t charSizeEstimate = 1U);

        // Empties the cache.
        void Clear();

        // Determines whether an element with the given runtime ID exists in the signal index cache.
        bool Contains(uint16_t signalIndex) const;

        // Gets the globally unique signal ID associated with the given 16-bit runtime ID.
        sttp::Guid GetSignalID(uint16_t signalIndex) const;

        //Gets the full list of signal IDs as an unordered set
        std::unordered_set<sttp::Guid> GetSignalIDs() const;

        // Gets the first half of the human-readable measurement
        // key associated with the given 16-bit runtime ID.
        const std::string& GetSource(uint16_t signalIndex) const;

        // Gets the second half of the human-readable measurement
        // key associated with the given 16-bit runtime ID.
        uint32_t GetID(uint16_t signalIndex) const;

        // Gets the globally unique signal ID as well as the human-readable
        // measurement key associated with the given 16-bit runtime ID.
        bool GetMeasurementKey(uint16_t signalIndex, sttp::Guid& signalID, std::string& source, uint32_t& id) const;

        // Gets the 16-bit runtime ID associated with the given globally unique signal ID.
        uint16_t GetSignalIndex(const sttp::Guid& signalID) const;

        // Gets the mapped signal count
        uint32_t Count() const;

        // Gets an estimated binary size of a serialized signal index cache useful for pre-allocating
        // a vector size, for an exact size call RecalculateBinaryLength first
        uint32_t GetBinaryLength() const;

        void RecalculateBinaryLength(const SubscriberConnection& connection);

        void Parse(const std::vector<uint8_t>& buffer, Guid& subscriberID);

        void Serialize(const SubscriberConnection& connection, std::vector<uint8_t>& buffer);
    };

    typedef SharedPtr<SignalIndexCache> SignalIndexCachePtr;
}}

#endif