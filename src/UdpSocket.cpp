/// @file
/// @version 1.0
/// 
/// @section LICENSE
/// 
/// This program is free software; you can redistribute it and/or modify it under
/// the terms of the BSD license: http://opensource.org/licenses/BSD-3-Clause

#include <hltypes/hlog.h>
#include <hltypes/hstring.h>
#include <hltypes/hstream.h>

#include "BroadcasterThread.h"
#include "PlatformSocket.h"
#include "sakit.h"
#include "sakitUtil.h"
#include "SenderThread.h"
#include "State.h"
#include "UdpReceiverThread.h"
#include "UdpSocket.h"
#include "UdpSocketDelegate.h"

namespace sakit
{
	UdpSocket::UdpSocket(UdpSocketDelegate* socketDelegate) : Socket(dynamic_cast<SocketDelegate*>(socketDelegate), BOUND),
		Binder(this->socket, dynamic_cast<BinderDelegate*>(socketDelegate))
	{
		this->socket->setConnectionLess(true);
		this->udpSocketDelegate = socketDelegate;
		this->receiver = this->udpReceiver = new UdpReceiverThread(this->socket, &this->timeout, &this->retryFrequency);
		this->broadcaster = new BroadcasterThread(this->socket);
		Binder::_integrate(&this->state, &this->mutexState, &this->localHost, &this->localPort);
		this->__register();
	}

	UdpSocket::~UdpSocket()
	{
		this->__unregister();
		this->broadcaster->join();
		delete this->broadcaster;
	}

	bool UdpSocket::hasDestination() const
	{
		return this->socket->isConnected();
	}

	void UdpSocket::_clear()
	{
		this->remoteHost = Host();
		this->remotePort = 0;
		this->localHost = Host();
		this->localPort = 0;
		this->multicastHosts.clear();
		this->socket->disconnect();
	}

	bool UdpSocket::setDestination(Host remoteHost, unsigned short remotePort)
	{
		hmutex::ScopeLock lock(&this->mutexState);
		if (!this->_canSetDestination(this->state))
		{
			return false;
		}
		this->state = CONNECTING; // just a precaution
		lock.release();
		// this is not a real connect on UDP, it just does its job of setting a proper remote host
		bool result = this->socket->connect(remoteHost, remotePort, this->localHost, this->localPort, this->timeout, this->retryFrequency);
		lock.acquire(&this->mutexState);
		if (result)
		{
			this->remoteHost = remoteHost;
			this->remotePort = remotePort;
		}
		else
		{
			this->remoteHost = Host();
			this->remotePort = 0;
		}
		this->state = BOUND;
		return result;
	}

	bool UdpSocket::joinMulticastGroup(Host interfaceHost, Host groupAddress)
	{
		hmutex::ScopeLock lock(&this->mutexState);
		if (!this->_canJoinMulticastGroup(this->state))
		{
			return false;
		}
		lock.release();
		bool result = this->socket->joinMulticastGroup(interfaceHost, groupAddress);
		if (result)
		{
			this->multicastHosts += std::pair<Host, Host>(interfaceHost, groupAddress);
		}
		return result;
	}

	bool UdpSocket::leaveMulticastGroup(Host interfaceHost, Host groupAddress)
	{
		std::pair<Host, Host> pair(interfaceHost, groupAddress);
		if (!this->multicastHosts.has(pair))
		{
			hlog::warnf(logTag, "Cannot leave multicast group, interface %s is not assigned to group %s!", interfaceHost.toString().cStr(), groupAddress.toString().cStr());
			return false;
		}
		hmutex::ScopeLock lock(&this->mutexState);
		if (!this->_canLeaveMulticastGroup(this->state))
		{
			return false;
		}
		lock.release();
		bool result = this->socket->leaveMulticastGroup(interfaceHost, groupAddress);
		if (result)
		{
			this->multicastHosts -= pair;
		}
		return result;
	}

	bool UdpSocket::setMulticastInterface(Host interfaceHost)
	{
		return this->socket->setMulticastInterface(interfaceHost);
	}

	bool UdpSocket::setMulticastTtl(int value)
	{
		return this->socket->setMulticastTtl(value);
	}

	bool UdpSocket::setMulticastLoopback(bool value)
	{
		return this->socket->setMulticastLoopback(value);
	}

	void UdpSocket::update(float timeDelta)
	{
		Binder::_update(timeDelta);
		Socket::update(timeDelta);
		hmutex::ScopeLock lock(&this->mutexState);
		hmutex::ScopeLock lockThread(&this->broadcaster->mutex);
		State result = this->broadcaster->result;
		if (result == RUNNING || result == IDLE)
		{
			return;
		}
		this->broadcaster->result = IDLE;
		this->state = (this->state == SENDING_RECEIVING ? RECEIVING : this->idleState);
		lockThread.release();
		lock.release();
		// delegate calls
		switch (result)
		{
		case FINISHED:	this->udpSocketDelegate->onBroadcastFinished(this);	break;
		case FAILED:	this->udpSocketDelegate->onBroadcastFailed(this);	break;
		default:															break;
		}
	}

	void UdpSocket::_updateReceiving()
	{
		harray<Host> remoteHosts;
		harray<unsigned short> remotePorts;
		harray<hstream*> streams;
		hmutex::ScopeLock lock(&this->mutexState);
		hmutex::ScopeLock lockThread(&this->receiver->mutex);
		if (this->udpReceiver->streams.size() > 0)
		{
			remoteHosts = this->udpReceiver->remoteHosts;
			remotePorts = this->udpReceiver->remotePorts;
			streams = this->udpReceiver->streams;
			this->udpReceiver->remoteHosts.clear();
			this->udpReceiver->remotePorts.clear();
			this->udpReceiver->streams.clear();
		}
		State result = this->receiver->result;
		if (result == RUNNING || result == IDLE)
		{
			lockThread.release();
			lock.release();
			for_iter (i, 0, streams.size())
			{
				this->udpSocketDelegate->onReceived(this, remoteHosts[i], remotePorts[i], streams[i]);
				delete streams[i];
			}
			return;
		}
		this->receiver->result = IDLE;
		this->state = (this->state == SENDING_RECEIVING ? SENDING : this->idleState);
		lockThread.release();
		lock.release();
		for_iter (i, 0, streams.size())
		{
			this->udpSocketDelegate->onReceived(this, remoteHosts[i], remotePorts[i], streams[i]);
			delete streams[i];
		}
		// delegate calls
		if (result == FINISHED)
		{
			this->socketDelegate->onReceiveFinished(this);
		}
	}

	int UdpSocket::receive(hstream* stream, Host& remoteHost, unsigned short& remotePort)
	{
		if (!this->_prepareReceive(stream))
		{
			return 0;
		}
		return this->_finishReceive(this->_receiveFromDirect(stream, remoteHost, remotePort));
	}
	
	hstr UdpSocket::receive(Host& remoteHost, unsigned short& remotePort)
	{
		hstream stream;
		int size = this->receive(&stream, remoteHost, remotePort);
		stream.rewind();
		char* p = new char[size + 1];
		stream.readRaw(p, size);
		p[size] = 0;
		hstr result = p;
		delete [] p;
		return result;
	}

	bool UdpSocket::startReceiveAsync(int maxPackages)
	{
		return this->_startReceiveAsync(maxPackages);
	}

	void UdpSocket::_activateConnection(Host remoteHost, unsigned short remotePort, Host localHost, unsigned short localPort)
	{
		SocketBase::_activateConnection(remoteHost, remotePort, localHost, localPort);
		this->socket->setRemoteAddress(remoteHost, remotePort);
	}

	bool UdpSocket::broadcast(harray<NetworkAdapter> adapters, unsigned short remotePort, hstream* stream, int count)
	{
		if (!this->_checkSendParameters(stream, count))
		{
			return false;
		}
		hmutex::ScopeLock lock(&this->mutexState);
		if (!this->_canSend(this->state))
		{
			return false;
		}
		this->state = (this->state == RECEIVING ? SENDING_RECEIVING : SENDING);
		lock.release();
		bool result = this->socket->broadcast(adapters, remotePort, stream, count);
		lock.acquire(&this->mutexState);
		this->state = (this->state == SENDING_RECEIVING ? RECEIVING : this->idleState);
		return result;
	}

	bool UdpSocket::broadcast(unsigned short remotePort, hstream* stream, int count)
	{
		return this->broadcast(PlatformSocket::getNetworkAdapters(), remotePort, stream, count);
	}

	bool UdpSocket::broadcast(harray<NetworkAdapter> adapters, unsigned short remotePort, chstr data)
	{
		hstream stream;
		stream.write(data);
		stream.rewind();
		return this->broadcast(adapters, remotePort, &stream, (int)stream.size());
	}

	bool UdpSocket::broadcast(unsigned short remotePort, chstr data)
	{
		hstream stream;
		stream.write(data);
		stream.rewind();
		return this->broadcast(PlatformSocket::getNetworkAdapters(), remotePort, &stream, (int)stream.size());
	}

	bool UdpSocket::broadcastAsync(harray<NetworkAdapter> adapters, unsigned short remotePort, hstream* stream, int count)
	{
		if (!this->_checkSendParameters(stream, count))
		{
			return false;
		}
		hmutex::ScopeLock lock(&this->mutexState);
		hmutex::ScopeLock lockThread(&this->broadcaster->mutex);
		if (!this->_canSend(this->state))
		{
			return false;
		}
		this->state = (this->state == RECEIVING ? SENDING_RECEIVING : SENDING);
		this->broadcaster->result = RUNNING;
		this->broadcaster->stream->clear();
		this->broadcaster->stream->writeRaw(*stream, (int)hmin((int64_t)count, stream->size() - stream->position()));
		this->broadcaster->stream->rewind();
		this->broadcaster->adapters = adapters;
		this->broadcaster->remotePort = remotePort;
		this->broadcaster->start();
		return true;
	}

	bool UdpSocket::broadcastAsync(unsigned short remotePort, hstream* stream, int count)
	{
		return this->broadcastAsync(PlatformSocket::getNetworkAdapters(), remotePort, stream, count);
	}

	bool UdpSocket::broadcastAsync(harray<NetworkAdapter> adapters, unsigned short remotePort, chstr data)
	{
		hstream stream;
		stream.write(data);
		stream.rewind();
		return this->broadcastAsync(adapters, remotePort, &stream, (int)stream.size());
	}

	bool UdpSocket::broadcastAsync(unsigned short remotePort, chstr data)
	{
		hstream stream;
		stream.write(data);
		stream.rewind();
		return this->broadcastAsync(PlatformSocket::getNetworkAdapters(), remotePort, &stream, (int)stream.size());
	}

	bool UdpSocket::_canSetDestination(State state)
	{
		harray<State> allowed;
		allowed += BOUND;
		return _checkState(state, allowed, "set destination");
	}

	bool UdpSocket::_canJoinMulticastGroup(State state)
	{
		harray<State> allowed;
		allowed += BOUND;
		allowed += SENDING;
		allowed += RECEIVING;
		allowed += SENDING_RECEIVING;
		return _checkState(state, allowed, "join multicast group");
	}

	bool UdpSocket::_canLeaveMulticastGroup(State state)
	{
		harray<State> allowed;
		allowed += BOUND;
		allowed += SENDING;
		allowed += RECEIVING;
		allowed += SENDING_RECEIVING;
		return _checkState(state, allowed, "leave multicast group");
	}

}
