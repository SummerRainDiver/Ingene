﻿/*
	Asynchronous TCP/IP Socket 
	
	서버에 접속해서 ID를 설정하는 것 뿐인 프로그램
*/
#include "Server.h"

Server::Server(string ipAddress, unsigned short portNum) :
	work(new asio::io_context::work(context)),
	endpoint(asio::ip::address::from_string(ipAddress), portNum),
	gate(context, endpoint.protocol())
{
	waitingRooms.push_back(0); // 대기실에 밀어넣기
}

Server::~Server() 
{
	cout << "End\n";
}

void Server::Start()
{
	cout << "Start Server" << endl;
	cout << "Creating Threads" << endl;
	for (int i = 0; i < THREAD_SIZE; i++)
		threadGroup.create_thread(bind(&Server::WorkerThread, this));

	// thread 생성 시간 확보 
	this_thread::sleep_for(chrono::milliseconds(100));
	cout << "Threads Created" << endl;

	// listen 할 포트 개방
	context.post(bind(&Server::OpenGate, this));

	// 모든 스레드가 종료될 때까지 대기
	threadGroup.join_all();
}

void Server::WorkerThread()
{
	lock.lock();
	cout << "[" << boost::this_thread::get_id() << "]" << " Thread Start" << endl;
	lock.unlock();

	context.run();

	lock.lock();
	cout << "[" << boost::this_thread::get_id() << "]" << " Thread End" << endl;
	lock.unlock();
}

void Server::OpenGate()
{
	system::error_code err;
	gate.bind(endpoint, err);
	if (err)
	{
		cout << "bind failed: " << err.message() << endl;
		return;
	}

	gate.listen();
	cout << "Gate Opened" << endl;

	StartAccept();
	cout << "[" << boost::this_thread::get_id() << "]" << " Start Accepting" << endl;
}

void Server::StartAccept()
{
	Session* session = new Session();
	shared_ptr<asio::ip::tcp::socket> socket(new asio::ip::tcp::socket(context));
	session->socket = socket;

	// 클라이언트 접속을 받을 때 비동기로 처리하여 각각의 클라 접속을 독자적으로 처리할 수 있다
	// 비동기 방식은 주로 I/O 작업이나 네트워트 요청과 같이 시간이 오래 걸리는 작업에 Good
	gate.async_accept(*socket, session->endpoint, bind(&Server::OnAccept, this, _1, session));
}

void Server::OnAccept(const system::error_code& err, Session* session)
{
	if (err)
	{
		cout << "accept failed: " << err.message() << endl;
		return;
	}

	lock.lock();
	sessions.push_back(session);
	cout << "[" << boost::this_thread::get_id() << "]" << " Client Accepted" << endl;
	lock.unlock();

	context.post(bind(&Server::Receive, this, session));
	StartAccept();
}

void Server::Receive(Session* session)
{
	system::error_code err;
	size_t size = session->socket->read_some(asio::buffer(session->buf, sizeof(session->buf)), err);

	if (err)
	{
		// 수신에 실패하면 세션을 닫아버린다
		cout << "[" << boost::this_thread::get_id() << "] read failed: " << err.message() << endl;
		CloseSession(session);
		return;
	}

	if (size == 0)
	{
		// 0 일때, 통신을 종료하도록 한다 (세션을 닫는다)
		cout << "[" << boost::this_thread::get_id() << "] peer wants to end " << endl;
		CloseSession(session);
		return;
	}

	session->buf[size] = '\0';
	session->receiveBuffer = session->buf;
	PacketManager(session);
	cout << "[" << boost::this_thread::get_id() << "] " << session->receiveBuffer << endl;

	Receive(session);
}

void Server::PacketManager(Session* session)
{
	if (session->buf[0] == ':')
	{
		EIDState code = TranslatePacket(session->receiveBuffer);

		switch (code)
		{
		case EIDState::SET_ID:
			SetID(session);
			break;
		case EIDState::INVALID:
			session->sendBuffer = "유효하지 않은 명령어 입니다";
			session->socket->async_write_some(asio::buffer(session->sendBuffer), bind(&Server::OnSend, this, _1));
			// async_write_some : 스트림 소켓에 데이터를 비동기로 쓰는데 사용된다. 
			// 비동기 작업이 완료되기 전에 모든 데이터의 쓰기 작업이 완료 되어야 하는 경우 async_write 권장
			break;
		}
	}
	else  // 특수메세지가 아닌 경우
	{
		// id length가 0인 경우는 id를 아직 등록하지 않은 경우
		if (session->id.length() != 0)
		{
			string temp = "[" + session->id + "]:" + session->receiveBuffer;
		}
		else
		{
			session->sendBuffer = ":set ____을 통해 아이디를 먼저 등록하세요";
			session->socket->async_write_some(asio::buffer(session->sendBuffer), bind(&Server::OnSend, this, _1));
		}
	}
}

EIDState Server::TranslatePacket(string message)
{
	string temp = message.substr(0, sizeof(":set ") - 1);
	if (temp.compare(":set ") == 0)
	{
		return EIDState::SET_ID;
	}

	return EIDState::INVALID;
}

void Server::OnSend(const system::error_code& err)
{
	if (err)
	{
		cout << "[" << boost::this_thread::get_id() << "] async_write_some failed: " << err.message() << endl;
		return;
	}
}

void Server::SetID(Session* session)
{
	string temp = session->receiveBuffer.substr(sizeof(":set ") - 1, session->receiveBuffer.length());

	// 중복된 아이디인지 체크
	for (int i = 0; i < (int)sessions.size(); i++)
	{
		if (temp.compare(sessions[i]->id) == 0)
		{
			session->sendBuffer = "set falied: [" + temp + "]는 이미 사용중인 아이디 입니다";
			session->socket->async_write_some(asio::buffer(session->sendBuffer), bind(&Server::OnSend, this, _1));
			return;
		}
	}

	session->id = temp;
	session->sendBuffer = "set [" + temp + "] success!";
	session->socket->async_write_some(asio::buffer(session->sendBuffer), bind(&Server::OnSend, this, _1));
}

void Server::CloseSession(Session* session)
{
	// 세션을 삭제하기 전에 목록에서 지우고 닫는다.
	for (int i = 0; i < (int)sessions.size(); i++)
	{
		if (sessions[i]->socket == session->socket)
		{
			lock.lock();
			sessions.erase(sessions.begin() + i);
			lock.unlock();
			break;
		}
	}

	string temp = session->id;
	session->socket->close();
	delete session;
}