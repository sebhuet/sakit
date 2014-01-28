/// @file
/// @author  Boris Mikic
/// @version 1.0
/// 
/// @section LICENSE
/// 
/// This program is free software; you can redistribute it and/or modify it under
/// the terms of the BSD license: http://www.opensource.org/licenses/bsd-license.php

#include <hltypes/hlog.h>
#include <hltypes/hstream.h>
#include <hltypes/hstring.h>

#include "PlatformSocket.h"
#include "sakit.h"
#include "SenderThread.h"
#include "Socket.h"
#include "SocketDelegate.h"
#include "State.h"
#include "WorkerThread.h"

namespace sakit
{
	Socket::Socket(SocketDelegate* socketDelegate) : SocketBase()
	{
		this->socketDelegate = socketDelegate;
		this->sender = new SenderThread(this->socket);
	}

	Socket::~Socket()
	{
		this->sender->running = false;
		this->sender->join();
		delete this->sender;
		if (this->receiver != NULL)
		{
			this->receiver->running = false;
			this->receiver->join();
			delete this->receiver;
		}
	}

	bool Socket::isSending()
	{
		this->sender->mutex.lock();
		bool result = (this->sender->state == RUNNING);
		this->sender->mutex.unlock();
		return result;
	}

	bool Socket::isReceiving()
	{
		this->receiver->mutex.lock();
		bool result = (this->receiver->state == RUNNING);
		this->receiver->mutex.unlock();
		return result;
	}

	void Socket::update(float timeSinceLastFrame)
	{
		this->_updateSending();
		this->_updateReceiving();
	}

	void Socket::_updateSending()
	{
		this->sender->mutex.lock();
		State result = this->sender->result;
		if (this->sender->lastSent > 0)
		{
			int sent = this->sender->lastSent;
			this->sender->lastSent = 0;
			this->sender->mutex.unlock();
			this->socketDelegate->onSent(this, sent);
		}
		else
		{
			this->sender->mutex.unlock();
		}
		if (result == RUNNING || result == IDLE)
		{
			return;
		}
		this->sender->mutex.lock();
		this->sender->result = IDLE;
		this->sender->state = IDLE;
		this->sender->mutex.unlock();
		if (result == FINISHED)
		{
			this->socketDelegate->onSendFinished(this);
		}
		else if (result == FAILED)
		{
			this->socketDelegate->onSendFailed(this);
		}
	}

	int Socket::send(hstream* stream, int count)
	{
		return this->_send(stream, count);
	}

	int Socket::send(chstr data)
	{
		return this->_send(data);
	}

	bool Socket::sendAsync(hstream* stream, int count)
	{
		return this->_sendAsync(stream, count);
	}

	bool Socket::sendAsync(chstr data)
	{
		return this->_sendAsync(data);
	}

	bool Socket::_sendAsync(chstr data)
	{
		hstream stream;
		stream.write(data);
		stream.rewind();
		return this->_sendAsync(&stream, stream.size());
	}

	bool Socket::stopReceiveAsync()
	{
		this->receiver->mutex.lock();
		State receiverState = this->receiver->state;
		if (!this->_checkStopReceiveStatus(receiverState))
		{
			this->receiver->mutex.unlock();
			return false;
		}
		this->receiver->running = false;
		this->receiver->mutex.unlock();
		return true;
	}

	bool Socket::_checkSendStatus(State senderState)
	{
		if (senderState == RUNNING)
		{
			hlog::warn(sakit::logTag, "Cannot send, already sending!");
			return false;
		}
		return true;
	}

	bool Socket::_checkStartReceiveStatus(State receiverState)
	{
		if (receiverState == RUNNING)
		{
			hlog::warn(sakit::logTag, "Cannot start receiving, already receiving!");
			return false;
		}
		return true;
	}

	bool Socket::_checkStopReceiveStatus(State receiverState)
	{
		if (receiverState == IDLE)
		{
			hlog::warn(sakit::logTag, "Cannot stop receiving, not receiving!");
			return false;
		}
		return true;
	}

}
