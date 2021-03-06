/// @file
/// @version 1.0
/// 
/// @section LICENSE
/// 
/// This program is free software; you can redistribute it and/or modify it under
/// the terms of the BSD license: http://opensource.org/licenses/BSD-3-Clause

#include <hltypes/harray.h>
#include <hltypes/hlog.h>
#include <hltypes/hstring.h>

#include "PlatformSocket.h"
#include "sakit.h"
#include "sakitUtil.h"
#include "Server.h"
#include "ServerDelegate.h"
#include "Socket.h"
#include "TcpSocket.h"
#include "WorkerThread.h"

namespace sakit
{
	Server::Server(ServerDelegate* serverDelegate) : Base(), Binder(this->socket, dynamic_cast<BinderDelegate*>(serverDelegate))
	{
		this->serverDelegate = serverDelegate;
		this->serverThread = NULL;
		this->socket->setServerMode(true);
		Binder::_integrate(&this->state, &this->mutexState, &this->localHost, &this->localPort);
	}

	Server::~Server()
	{
		if (this->serverThread != NULL)
		{
			this->serverThread->join();
			delete this->serverThread;
		}
	}

	bool Server::isRunning()
	{
		hmutex::ScopeLock lock(&this->mutexState);
		return (this->state == RUNNING);
	}

	bool Server::startAsync()
	{
		hmutex::ScopeLock lock(&this->mutexState);
		if (!this->_canStart(this->state))
		{
			return false;
		}
		this->state = RUNNING;
		this->serverThread->result = RUNNING;
		this->serverThread->start();
		return true;
	}

	bool Server::stopAsync()
	{
		hmutex::ScopeLock lock(&this->mutexState);
		if (!this->_canStop(this->state))
		{
			return false;
		}
		this->serverThread->executing = false;
		return true;
	}

	void Server::update(float timeDelta)
	{
		Binder::_update(timeDelta);
		hmutex::ScopeLock lock(&this->mutexState);
		hmutex::ScopeLock lockThread(&this->serverThread->mutex);
		State result = this->serverThread->result;
		if (result == RUNNING || result == IDLE)
		{
			return;
		}
		this->serverThread->result = IDLE;
		this->state = BOUND;
		lockThread.release();
		lock.release();
		// delegate calls
		switch (result)
		{
		case FINISHED:	this->serverDelegate->onStopped(this);		break;
		case FAILED:	this->serverDelegate->onStartFailed(this);	break;
		default:													break;
		}
	}

	bool Server::_canStart(State state)
	{
		harray<State> allowed;
		allowed += BOUND;
		return _checkState(state, allowed, "start");
	}

	bool Server::_canStop(State state)
	{
		harray<State> allowed;
		allowed += RUNNING;
		return _checkState(state, allowed, "stop");
	}

}
