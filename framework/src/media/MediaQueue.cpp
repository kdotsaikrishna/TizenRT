/* ****************************************************************
 *
 * Copyright 2018 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ******************************************************************/

#include "MediaQueue.h"
#include "media/media_log.h"

namespace media {
MediaQueue::MediaQueue()
{
	var = 0;
}
MediaQueue::~MediaQueue()
{
}

std::function<void()> MediaQueue::deQueue()
{
	char *str;
	if (var != 0) {
		if (var == 1) {
			str = "[RW]";
		}
		if (var == 2) {
			str = "[ROW]";
		}
		meddbg("%s before dequeue lock\n", str);
	}
	std::unique_lock<std::mutex> lock(mQueueMtx);
	if (var != 0) {
		meddbg("%s after dequeue lock\n", str);
	}
	if (mQueueData.empty()) {
		if (var != 0) {
			meddbg("%s queue empty ---- before wait\n", str);
		}
		mQueueCv.wait(lock);
		if (var != 0) {
			meddbg("%s after wait\n", str);
		}
	}

	auto data = std::move(mQueueData.front());
	mQueueData.pop();
	return data;
}

bool MediaQueue::isEmpty()
{
	std::unique_lock<std::mutex> lock(mQueueMtx);
	return mQueueData.empty();
}

void MediaQueue::clearQueue(void)
{
	std::unique_lock<std::mutex> lock(mQueueMtx);
   	std::queue<std::function<void()>> empty;
   	std::swap(mQueueData, empty);
}
} // namespace media
