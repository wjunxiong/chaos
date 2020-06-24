#include "Event.h"
#include "Timer.h"
#include <stdio.h>


namespace chaos
{
	void Event::CancelEvent()
	{
		if (m_pCenter)
			m_pCenter->CancelEvent(this);
	}


	EventCentre::EventCentre() :
		m_pPoller(0),
		m_pTimer(0),
		m_running(false)
	{
	}


	EventCentre::~EventCentre()
	{
		if (m_pPoller)
			delete m_pPoller;

		if (!m_pTimer)
			delete m_pTimer;
	}


	int EventCentre::Init()
	{
		m_pPoller = Poller::AdapterNetDrive(this);
		if (!m_pPoller)
			return -1;

		m_pPoller->Init();

		m_pTimer = new Timer();
		if (!m_pTimer)
			return -1;

		return 0;
	}


	void EventCentre::EventLoop()
	{
		m_running = true;

		while (m_running)
		{
			if (0 != DispatchEvent())
				break;
		}
	}


	int EventCentre::DispatchEvent()
	{
		int ret = 0;

		if (0 != (ret = TimerDispatch()))
			return ret;

		if (0 != (ret = NetEventDispatch()))
			return ret;

		if (0 != (ret = SignalDispatch()))
			return ret;

		if(0 != (ret = ProcessActiveEvent()))
			return ret;

		return ret;
	}


	int EventCentre::ProcessActiveEvent()
	{
		for (auto it = m_activeEvs.begin(); it != m_activeEvs.end(); ++it)
		{
			if (!*it)
				continue;

			(*it)->Handle();
		}

		m_activeEvs.clear();

		return 0;
	}


	int EventCentre::RegisterEvent(Event* pEvent)
	{
		if (!pEvent)
			return -1;

		uint32 ev = pEvent->GetEv();

		const EventKey& evKey = pEvent->GetEvKey();

		if (ev & (EV_IOREAD | EV_IOWRITE | EV_IOEXCEPT)
			&& !(ev & ~(EV_IOREAD | EV_IOWRITE | EV_IOEXCEPT))
			&& m_pPoller)
		{
			//m_netEvs.insert(std::make_pair(pEvKey->fd, pEvent));
			m_pPoller->AddEvent(pEvent);
		}
		else if (ev & EV_TIMEOUT && !(ev & ~EV_TIMEOUT) && m_pTimer)
		{
			m_timerEvs.insert(std::make_pair(evKey.timerId, pEvent));
			m_pTimer->AddTimer((TimerEvent*)pEvent);
		}
		else if (ev & EV_SIGNAL && !(ev & ~EV_SIGNAL))
			m_signalEvs.insert(std::make_pair(evKey.signal, pEvent));
		else
			return -1;

		return 0;
	}


	int EventCentre::CancelEvent(Event* pEvent)
	{
		if (!pEvent)
			return -1;

		uint32 ev = pEvent->GetEv();

		const EventKey& evKey = pEvent->GetEvKey();

		if (ev & (EV_IOREAD | EV_IOWRITE | EV_IOEXCEPT) && m_pPoller)
		{
			//m_netEvs.erase(pEvKey->fd);
			m_pPoller->DelEvent(pEvent);
		}
		else if (ev & EV_TIMEOUT && m_pTimer)
		{
			m_timerEvs.erase(evKey.timerId);
			m_pTimer->DelTimer((TimerEvent*)pEvent);
		}
		else if (ev & EV_SIGNAL)
			m_signalEvs.erase(evKey.signal);
		else
			return -1;

		return 0;
	}


	int EventCentre::NetEventDispatch()
	{
		if (!m_pPoller)
			return -1;

		int ret = 0;
		if (0 != (ret = m_pPoller->Launch()))
			return ret;

		return ret;
	}


	int EventCentre::SignalDispatch()
	{
		return 0;
	}


	int EventCentre::TimerDispatch()
	{
		if (!m_pTimer)
			return -1;

		m_pTimer->DispatchTimer();

		return 0;
	}

}



namespace chaos
{
	void Listener::Handle()
	{
		if (!m_pSocket)
			return;

		uint32 ev = GetCurEv();

		if (ev & EV_IOREAD)
		{
			do
			{
				//优化：这里考虑内存分配是在Accept调用中还是在此处分配
				Socket* pNewSock = m_pSocket->Accept();
				if (!pNewSock || 0 > pNewSock->GetFd() ||
					errno == EAGAIN ||
					pNewSock->GetFd() == INVALID_SOCKET)
				{
					delete pNewSock;
					return;
				}

				EventKey key;

				key.fd = pNewSock->GetFd();

				EventCentre* pCentre = GetCentre();
				if (!pCentre)
					return;

				Event* pNewEv = NULL;

#ifdef _WIN32
				pNewEv = new AsynConnecter(pCentre, pNewSock, EV_IOREAD | EV_IOWRITE, key);
#else
				pNewEv = new Connecter(pCentre, pNewSock, EV_IOREAD | EV_IOWRITE, key);
#endif
				if (!pNewEv)
				{
					delete pNewSock;
					return;
				}

				pCentre->RegisterEvent(pNewEv);

				SetCurEv(ev & (~EV_IOREAD));

			} while (m_pSocket->Block());
		}

	}
}


namespace chaos
{

	void Connecter::Handle()
	{
		if (!m_pSocket)
			return;

		uint32 ev = GetCurEv();

		if (ev & EV_IOREAD)
		{

			HandleRead();

			SetCurEv(ev & (~EV_IOREAD));
		}

		if (ev & EV_IOWRITE)
		{
			HandleWrite();
			SetCurEv(ev & (~EV_IOWRITE));
		}
	}



	int Connecter::HandleRead()
	{
		if (!m_pSocket || !m_pRBuffer)
			return -1;

		socket_unread_t unread = m_pSocket->GetUnreadByte();
		if (0 >= unread)
			return unread;

		while (0 < unread)
		{
			uint32 size = 0;

			char* buf = m_pRBuffer->GetWriteBuffer(&size);
			if (!buf)
				break;

			int read = m_pSocket->Recv(buf, size);

			if (0 >= read)
			{
				CancelEvent();
				break;
			}

			m_pRBuffer->MoveWriteBuffer(read);

			unread -= read;
		}

		return 0;
	}


	int Connecter::HandleWrite()
	{
		return m_pSocket && m_pWBuffer ?
			m_pWBuffer->WriteSocket(m_pSocket) :
			-1;
	}


#ifdef _WIN32
	void AsynConnecter::Handle()
	{
		if (!m_pSocket)
			return;

		uint32 ev = GetCurEv();

		if (ev & EV_IOREAD)
		{
			AsynRead();
			SetCurEv(ev & (~EV_IOREAD));
		}

		if (ev & EV_IOWRITE)
		{
			AsynWrite();
			SetCurEv(ev & (~EV_IOWRITE));
		}
	}


	int AsynConnecter::AsynRead()
	{
		if (!m_pOverlapped)
		{
			CancelEvent();
			return -1;
		}

		Socket* s = GetSocket();
		if (!s)
		{
			CancelEvent();
			return -1;
		}

		if (INVALID_IOCP_RET != m_pOverlapped->asynRet && 0 == m_pOverlapped->databuf.len)
		{
			printf("AsynRead close socket[%d]\n", s->GetFd());
			CancelEvent();
			return -1;
		}
		
		//收完数据后调整buffer的位置
		if(0 < m_pOverlapped->databuf.len)
			m_pRBuffer->MoveWriteBuffer(m_pOverlapped->databuf.len);

		uint32 size = 0;
		m_pOverlapped->databuf.buf = m_pRBuffer->GetWriteBuffer(&size);
		m_pOverlapped->databuf.len = size;

		m_pOverlapped->key.fd = s->GetFd();

		DWORD bytesRead = 0;
		DWORD flags = 0;

		int ret = WSARecv(m_pOverlapped->key.fd, &m_pOverlapped->databuf, 1, &bytesRead, &flags, &m_pOverlapped->overlapped, NULL);
		if (ret)
		{
			if(GetLastError() != WSA_IO_PENDING)
				CancelEvent();
		}

		return 0;
	}


	int AsynConnecter::AsynWrite()
	{
		return 0;
	}

#endif
}


namespace chaos
{
	void TimerEvent::Handle()
	{
		printf("test!\n");
	}
}
