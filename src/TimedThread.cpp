/// @file
/// @version 1.0
/// 
/// @section LICENSE
/// 
/// This program is free software; you can redistribute it and/or modify it under
/// the terms of the BSD license: http://opensource.org/licenses/BSD-3-Clause

#include "PlatformSocket.h"
#include "TimedThread.h"

namespace sakit
{
	TimedThread::TimedThread(PlatformSocket* socket, float* timeout, float* retryFrequency) : WorkerThread(socket)
	{
		this->name = "SAKit timed worker";
		this->timeout = timeout;
		this->retryFrequency = retryFrequency;
	}

	TimedThread::~TimedThread()
	{
	}

}
