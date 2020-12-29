//******************************************************************************************************
//  ThreadPool.h - Gbtc
//
//  Copyright � 2020, Grid Protection Alliance.  All Rights Reserved.
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
//  12/27/2020 - J. Ritchie Carroll
//       Generated original version of source code.
//
//******************************************************************************************************

#pragma once

#include <unordered_set>
#include "ThreadSafeQueue.h"
#include "Timer.h"

namespace sttp
{
	class ThreadPool // NOLINT
	{
	private:
		std::unordered_set<TimerPtr> m_waitTimers;
		Mutex m_waitTimersLock;
        Thread m_removeCompletedTimersThread;
        ThreadSafeQueue<TimerPtr> m_completedTimers;
		std::atomic_bool m_disposing;

	public:
		ThreadPool();
		~ThreadPool() noexcept;

		void ShutDown();

		void Queue(const std::function<void()>& action);
		void Queue(void* state, const std::function<void(void*)>& action);
		void Queue(uint32_t delay, const std::function<void()>& action);
		void Queue(uint32_t delay, void* state, const std::function<void(void*)>& action);
	};
}