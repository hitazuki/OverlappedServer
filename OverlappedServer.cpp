﻿///////////////////////////////////////////////////////
// OverlappedServer.cpp文件

#include "Initsock.h"

#include <Mswsock.h>
#include <stdio.h>
#include <windows.h>

CInitSock theSock;

#define BUFFER_SIZE 1024
#define WSA_MAXIMUM_WAIT_EVENTS 6
#define FILE_NAME "qwe.txt"

DWORD WINAPI ServerThread(LPVOID lpParam);

typedef struct _SOCKET_OBJ
{
	SOCKET s;						// 套节字句柄
	int nOutstandingOps;			// 记录此套节字上的重叠I/O数量
	SOCKADDR_IN addrLocal;			// 本地地址
	SOCKADDR_IN addrRemote;			// 客户地址
	_SOCKET_OBJ *pNext;
	CRITICAL_SECTION s_cs;

	LPFN_ACCEPTEX lpfnAcceptEx;		// 扩展函数AcceptEx的指针（仅对监听套节字而言）
	LPFN_GETACCEPTEXSOCKADDRS lpfnGetAcceptExSockaddrs;
} SOCKET_OBJ, *PSOCKET_OBJ;

typedef struct _BUFFER_OBJ
{	
	OVERLAPPED ol;			// 重叠结构
	char *buff;				// send/recv/AcceptEx所使用的缓冲区
	int nLen;				// buff的长度
	PSOCKET_OBJ pSocket;	// 此I/O所属的套节字对象

	int nOperation;			// 提交的操作类型
#define OP_ACCEPT	1
#define OP_READ		2
#define OP_WRITE	3

	SOCKET sAccept;			// 用来保存AcceptEx接受的客户套节字（仅对监听套节字而言）
	_BUFFER_OBJ *pNext;
} BUFFER_OBJ, *PBUFFER_OBJ;

// 线程对象
typedef struct _THREAD_OBJ
{
	HANDLE events[WSA_MAXIMUM_WAIT_EVENTS];	// 记录当前线程要等待的事件对象的句柄
	int nBufferCount;						// 记录当前线程处理的IO的数量 <=  WSA_MAXIMUM_WAIT_EVENTS

	PBUFFER_OBJ pBufferHead;				// 当前线程处理的IO对象列表，pBufferHeader指向表头
	PBUFFER_OBJ pBufferTail;					// pBufferTail指向表尾

	CRITICAL_SECTION cs;					// 关键代码段变量，为的是同步对本结构的访问
	_THREAD_OBJ *pNext;						// 指向下一个THREAD_OBJ对象，为的是连成一个表

} THREAD_OBJ, *PTHREAD_OBJ;

// 线程,socket列表
PTHREAD_OBJ g_pThreadList;		// 指向线程对象列表表头
PSOCKET_OBJ g_pSocketList;		// 指向socket对象列表表头
CRITICAL_SECTION g_cs;			// 同步对此全局变量的访问

								// 状态信息
LONG g_nCurrentBuffers;		// 当前IO数量
LONG g_nFileSendSeq;        // 发送序列数

// 打印线程状态
void PrintThread()
{
	int count = 0;
	printf(" Threads status: ");

	::EnterCriticalSection(&g_cs);
	PTHREAD_OBJ pThread = g_pThreadList;
	while (pThread != NULL) 
	{
		count++;
		printf("th%d:%d ", count, pThread->nBufferCount);
		pThread = pThread->pNext;
	}
	::LeaveCriticalSection(&g_cs);

	printf("CurThreadNum:%d\n",count);
}

// 打印连接状态
void PrintSocket()
{
	int count = 0;
	printf(" Sockets status: ");

	::EnterCriticalSection(&g_cs);
	PSOCKET_OBJ pSocket = g_pSocketList;
	while (pSocket != NULL)
	{
		count++;
		printf("th%d:%s/%d ", count, inet_ntoa(pSocket->addrRemote.sin_addr), pSocket->addrRemote.sin_port);
		pSocket = pSocket->pNext;
	}
	::LeaveCriticalSection(&g_cs);

	printf("CurSocketNum:%d\n", count);
}

// 申请套节字对象和释放套节字对象的函数
PSOCKET_OBJ GetSocketObj(SOCKET s)
{
	PSOCKET_OBJ pSocket = (PSOCKET_OBJ)::GlobalAlloc(GPTR, sizeof(SOCKET_OBJ));
	if(pSocket != NULL)
	{
		pSocket->s = s;
	}
	::InitializeCriticalSection(&pSocket->s_cs);
	return pSocket;
}

// 将socket对象添加到列表中，由于accpet套接字的存在，与GetSocektObj分开写
void AddSocketObj(PSOCKET_OBJ pSocket)
{
	if (pSocket != NULL)
	{
		::EnterCriticalSection(&g_cs);
		pSocket->pNext = g_pSocketList;
		g_pSocketList = pSocket;
		::LeaveCriticalSection(&g_cs);
		printf("new socket\n");
	}
}

void FreeSocketObj(PSOCKET_OBJ pSocket)
{
	::EnterCriticalSection(&pSocket->s_cs);
	// 在线程对象列表中查找pThread所指的对象，如果找到就从中移除
	::EnterCriticalSection(&g_cs);
	PSOCKET_OBJ p = g_pSocketList;
	if (p == pSocket)		// 是第一个？
	{
		g_pSocketList = p->pNext;
	}
	else
	{
		while (p != NULL && p->pNext != pSocket)
		{
			p = p->pNext;
		}
		if (p != NULL)
		{
			// 此时，p是pThread的前一个，即“p->pNext == pThread”
			p->pNext = pSocket->pNext;
		}
	}
	::LeaveCriticalSection(&g_cs);

	// 释放资源
	if(pSocket->s != INVALID_SOCKET)
		::closesocket(pSocket->s);
	::LeaveCriticalSection(&pSocket->s_cs);
	::DeleteCriticalSection(&pSocket->s_cs);
	::GlobalFree(pSocket);
	printf("a socket has been closed\n");
	PrintSocket();
}

// 申请一个线程对象，初始化它的成员，并将它添加到线程对象列表中
PTHREAD_OBJ GetThreadObj()
{
	PTHREAD_OBJ pThread = (PTHREAD_OBJ)::GlobalAlloc(GPTR, sizeof(THREAD_OBJ));
	if (pThread != NULL)
	{
		::InitializeCriticalSection(&pThread->cs);
		// 创建一个事件对象，用于指示该线程的句柄数组需要重组
		pThread->events[0] = ::WSACreateEvent();

		// 将新申请的线程对象添加到列表中
		::EnterCriticalSection(&g_cs);
		pThread->pNext = g_pThreadList;
		g_pThreadList = pThread;
		::LeaveCriticalSection(&g_cs);
		printf("new thread\n");
		PrintThread();
	}
	return pThread;
}

// 释放一个线程对象，并将它从线程对象列表中移除
void FreeThreadObj(PTHREAD_OBJ pThread)
{
	// 在线程对象列表中查找pThread所指的对象，如果找到就从中移除
	::EnterCriticalSection(&g_cs);
	PTHREAD_OBJ p = g_pThreadList;
	if (p == pThread)		// 是第一个？
	{
		g_pThreadList = p->pNext;
	}
	else
	{
		while (p != NULL && p->pNext != pThread)
		{
			p = p->pNext;
		}
		if (p != NULL)
		{
			// 此时，p是pThread的前一个，即“p->pNext == pThread”
			p->pNext = pThread->pNext;
		}
	}
	::LeaveCriticalSection(&g_cs);

	// 释放资源
	::CloseHandle(pThread->events[0]);
	::DeleteCriticalSection(&pThread->cs);
	::GlobalFree(pThread);
	printf("a thread has been free\n");
	PrintThread();
}

// 重新建立线程对象的events数组
void RebuildArray(PTHREAD_OBJ pThread)
{
	::EnterCriticalSection(&pThread->cs);
	PBUFFER_OBJ pBuffer = pThread->pBufferHead;
	int i = 1;
	while (pBuffer != NULL)
	{
		pThread->events[i++] = pBuffer->ol.hEvent;
		pBuffer = pBuffer->pNext;
	}
	::LeaveCriticalSection(&pThread->cs);
}

/////////////////////////////////////////////////////////////////////

// 向一个线程的IO列表中插入一个IO
BOOL InsertBufferObj(PTHREAD_OBJ pThread, PBUFFER_OBJ pBuffer)
{
	BOOL bRet = FALSE;
	::EnterCriticalSection(&pThread->cs);
	if (pThread->nBufferCount < WSA_MAXIMUM_WAIT_EVENTS - 1)
	{
		if (pThread->pBufferHead == NULL)
		{
			pThread->pBufferHead = pThread->pBufferTail = pBuffer;
		}
		else
		{
			pThread->pBufferTail->pNext = pBuffer;
			pThread->pBufferTail = pBuffer;
		}
		pThread->nBufferCount++;
		bRet = TRUE;
	}
	::LeaveCriticalSection(&pThread->cs);

	// 插入成功，说明成功创建了IO对象
	if (bRet)
	{
		::InterlockedIncrement(&g_nCurrentBuffers);
		printf(" New Buffer CurrentBuffers: %d \n", g_nCurrentBuffers);
	}
	return bRet;
}

// 将一个IO对象安排给空闲的线程处理
void AssignToFreeThread(PBUFFER_OBJ pBuffer)
{
	pBuffer->pNext = NULL;

	::EnterCriticalSection(&g_cs);
	PTHREAD_OBJ pThread = g_pThreadList;
	// 试图插入到现存线程
	while (pThread != NULL)
	{
		if (InsertBufferObj(pThread, pBuffer))
			break;
		pThread = pThread->pNext;
	}

	// 没有空闲线程，为这个套节字创建新的线程
	if (pThread == NULL)
	{
		pThread = GetThreadObj();
		InsertBufferObj(pThread, pBuffer);
		::CreateThread(NULL, 0, ServerThread, pThread, 0, NULL);
	}
	::LeaveCriticalSection(&g_cs);

	// 指示线程重建句柄数组
	::WSASetEvent(pThread->events[0]);
}

PBUFFER_OBJ GetBufferObj(PSOCKET_OBJ pSocket, ULONG nLen)
{
	PBUFFER_OBJ pBuffer = (PBUFFER_OBJ)::GlobalAlloc(GPTR, sizeof(BUFFER_OBJ));
	if(pBuffer != NULL)
	{
		pBuffer->buff = (char*)::GlobalAlloc(GPTR, nLen);
		pBuffer->ol.hEvent = ::WSACreateEvent();
		pBuffer->pSocket = pSocket;
		pBuffer->sAccept = INVALID_SOCKET;

		// 将新的BUFFER_OBJ分配给空闲线程
		AssignToFreeThread(pBuffer);
	}
	return pBuffer;
}

void FreeBufferObj(PTHREAD_OBJ pThread, PBUFFER_OBJ pBuffer)
{
	// 从列表中移除BUFFER_OBJ对象
	::EnterCriticalSection(&pThread->cs);
	PBUFFER_OBJ pTest = pThread->pBufferHead;
	BOOL bFind = FALSE;
	if(pTest == pBuffer)
	{
		if(pThread->pBufferHead == pThread->pBufferTail)
			pThread->pBufferHead = pThread->pBufferTail = NULL;
		else pThread->pBufferHead = pTest->pNext;
		pThread->nBufferCount--;
		bFind = TRUE;
	}
	else
	{
		while(pTest != NULL && pTest->pNext != pBuffer)
			pTest = pTest->pNext;
		if(pTest != NULL)
		{
			pTest->pNext = pBuffer->pNext;
			if(pTest->pNext == NULL)
				pThread->pBufferTail = pTest;
			pThread->nBufferCount--;
			bFind = TRUE;
		}
	}
	::LeaveCriticalSection(&pThread->cs);

	// 指示线程重建句柄数组
	::WSASetEvent(pThread->events[0]);

	// 释放它占用的内存空间
	if(bFind)
	{
		printf("nOutstandingOps:%d\n", pBuffer->pSocket->nOutstandingOps);
		::CloseHandle(pBuffer->ol.hEvent);
		::GlobalFree(pBuffer->buff);
		::GlobalFree(pBuffer);	
		::InterlockedDecrement(&g_nCurrentBuffers);
		printf(" Free Buffer CurrentBuffers: %d \n", g_nCurrentBuffers);
	}
}

PBUFFER_OBJ FindBufferObj(PTHREAD_OBJ pThread, int nIndex)
{
	PBUFFER_OBJ pBuffer = pThread->pBufferHead;
	while(--nIndex)
	{
		if(pBuffer == NULL)
			return NULL;
		pBuffer = pBuffer->pNext;
	}
	return pBuffer;
}

BOOL PostAccept(PBUFFER_OBJ pBuffer)
{
	PSOCKET_OBJ pSocket = pBuffer->pSocket;
	if(pSocket->lpfnAcceptEx != NULL)
	{	
		// 设置I/O类型，增加套节字上的重叠I/O计数
		pBuffer->nOperation = OP_ACCEPT;
		::EnterCriticalSection(&pSocket->s_cs);
		pSocket->nOutstandingOps ++;
		::LeaveCriticalSection(&pSocket->s_cs);

		// 投递此重叠I/O  
		DWORD dwBytes;
		pBuffer->sAccept = 
			::WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
		BOOL b = pSocket->lpfnAcceptEx(pSocket->s, 
			pBuffer->sAccept,
			pBuffer->buff, 
			BUFFER_SIZE - ((sizeof(sockaddr_in) + 16) * 2),
			sizeof(sockaddr_in) + 16, 
			sizeof(sockaddr_in) + 16, 
			&dwBytes, 
			&pBuffer->ol);
		if(!b)
		{
			if(::WSAGetLastError() != WSA_IO_PENDING)
				return FALSE;
		}
		return TRUE;
	}
	return FALSE;
};

BOOL PostRecv(PBUFFER_OBJ pBuffer)
{	
	// 设置I/O类型，增加套节字上的重叠I/O计数
	pBuffer->nOperation = OP_READ;
	::EnterCriticalSection(&pBuffer->pSocket->s_cs);
	pBuffer->pSocket->nOutstandingOps ++;
	::LeaveCriticalSection(&pBuffer->pSocket->s_cs);

	// 投递此重叠I/O
	DWORD dwBytes;
	DWORD dwFlags = 0;
	WSABUF buf;
	buf.buf = pBuffer->buff;
	buf.len = pBuffer->nLen;
	if(::WSARecv(pBuffer->pSocket->s, &buf, 1, &dwBytes, &dwFlags, &pBuffer->ol, NULL) != NO_ERROR)
	{
		if(::WSAGetLastError() != WSA_IO_PENDING)
			return FALSE;
	}
	return TRUE;
}

BOOL PostSend(PBUFFER_OBJ pBuffer)
{
	// 设置I/O类型，增加套节字上的重叠I/O计数
	pBuffer->nOperation = OP_WRITE;
	::EnterCriticalSection(&pBuffer->pSocket->s_cs);
	pBuffer->pSocket->nOutstandingOps ++;
	::LeaveCriticalSection(&pBuffer->pSocket->s_cs);

	// 投递此重叠I/O
	DWORD dwBytes;
	DWORD dwFlags = 0;
	WSABUF buf;
	buf.buf = pBuffer->buff;
	buf.len = pBuffer->nLen;
	if(::WSASend(pBuffer->pSocket->s, 
			&buf, 1, &dwBytes, dwFlags, &pBuffer->ol, NULL) != NO_ERROR)
	{
		if(::WSAGetLastError() != WSA_IO_PENDING)
			return FALSE;
	}
	return TRUE;
}

BOOL SendFile(PSOCKET_OBJ pClient, char* fileName)
{
	FILE *fp = fopen(fileName, "rb");
	if (fp == NULL)
	{
		printf("File: %s Not Found!", fileName);
		return FALSE;
	}
	else
	{
		int file_block_length = 0;
		char buffer[BUFFER_SIZE];
		memset(buffer, 0, sizeof(buffer));
		while ((file_block_length = fread(buffer, sizeof(char), BUFFER_SIZE, fp)) > 0)
		{
			// 为发送数据创建一个BUFFER_OBJ对象，这个对象会在套节字出错或者关闭时释放
			PBUFFER_OBJ pSend = GetBufferObj(pClient, BUFFER_SIZE);
			if (pSend == NULL)
			{
				printf(" Too much connections! \n");
				FreeSocketObj(pClient);
				return FALSE;
			}
			::InterlockedIncrement(&g_nFileSendSeq);
			printf("g_nFileSendSeq: %d\nfile_block_length = %d", g_nFileSendSeq, file_block_length);
			//printf(" %s ", buffer);
			// 将数据复制到发送缓冲区
			pSend->nLen = file_block_length;
			memcpy(pSend->buff, buffer, file_block_length);

			// 投递此发送I/O（将数据回显给客户）
			if (!PostSend(pSend))
			{
				printf("Send File : %s Failed!", fileName);
				return FALSE;
			}
			memset(buffer, 0, sizeof(buffer));
		}
		fclose(fp);
		printf("File: %s Transfer Finished!", fileName);
		return TRUE;
	}
}

BOOL HandleIO(PTHREAD_OBJ pThread, PBUFFER_OBJ pBuffer)
{
	PSOCKET_OBJ pSocket = pBuffer->pSocket; // 从BUFFER_OBJ对象中提取SOCKET_OBJ对象指针，为的是方便引用
	::EnterCriticalSection(&pSocket->s_cs);
	pSocket->nOutstandingOps --;


	// 获取重叠操作结果
	DWORD dwTrans;
	DWORD dwFlags;
	BOOL bRet = ::WSAGetOverlappedResult(pSocket->s, &pBuffer->ol, &dwTrans, FALSE, &dwFlags);
	if(!bRet)
	{
		// 在此套节字上有错误发生，因此，关闭套节字，移除此缓冲区对象。
		// 如果没有其它抛出的I/O请求了，释放此缓冲区对象，否则，等待此套节字上的其它I/O也完成
		::EnterCriticalSection(&pSocket->s_cs);
		if(pSocket->s != INVALID_SOCKET)
		{
			::closesocket(pSocket->s);
			pSocket->s = INVALID_SOCKET;
		}
		::LeaveCriticalSection(&pSocket->s_cs);
		::LeaveCriticalSection(&pSocket->s_cs);

		if(pSocket->nOutstandingOps == 0)
			FreeSocketObj(pSocket);	
		
		FreeBufferObj(pThread, pBuffer);
		return FALSE;
	}

	// 没有错误发生，处理已完成的I/O
	switch(pBuffer->nOperation)
	{
	case OP_ACCEPT:	// 接收到一个新的连接，并接收到了对方发来的第一个封包
		{
			// 为新客户创建一个SOCKET_OBJ对象
			PSOCKET_OBJ pClient = GetSocketObj(pBuffer->sAccept);
			AddSocketObj(pClient);

			// 为发送数据创建一个BUFFER_OBJ对象，这个对象会在套节字出错或者关闭时释放
			PBUFFER_OBJ pSend = GetBufferObj(pClient, BUFFER_SIZE);	
			if(pSend == NULL)
			{
				printf(" Too much connections! \n");
				FreeSocketObj(pClient);
				::LeaveCriticalSection(&pSocket->s_cs);
				return FALSE;
			}
			RebuildArray(pThread);

			//读取socket四元组
			int nLocalLen, nRmoteLen;
			LPSOCKADDR pLocalAddr, pRemoteAddr;
			pSocket->lpfnGetAcceptExSockaddrs(
				pBuffer->buff,
				BUFFER_SIZE - ((sizeof(sockaddr_in) + 16) * 2),
				sizeof(sockaddr_in) + 16,
				sizeof(sockaddr_in) + 16,
				(SOCKADDR **)&pLocalAddr,
				&nLocalLen,
				(SOCKADDR **)&pRemoteAddr,
				&nRmoteLen);
			//pClient->addrLocal = (SOCKADDR_IN*)(pBuffer->buff + (BUFFER_SIZE - 2 * (sizeof(sockaddr_in) + 16)) + 10);
			//pClient->addrRemote = (sockaddr_in*)((pBuffer->buff + (BUFFER_SIZE - 2 * (sizeof(sockaddr_in) + 16)) + 10) + sizeof(sockaddr_in) + 10 + 2);
			memcpy(&pClient->addrLocal, pLocalAddr, nLocalLen);
			memcpy(&pClient->addrRemote, pRemoteAddr, nRmoteLen);
			PrintSocket();
			printf("your sock:%s %d\n", inet_ntoa(pClient->addrLocal.sin_addr), pClient->addrLocal.sin_port);
			printf("new client:%s %d\n", inet_ntoa(pClient->addrRemote.sin_addr), pClient->addrRemote.sin_port);
			printf("from %s/%d: %s\n", inet_ntoa(pClient->addrRemote.sin_addr), pClient->addrRemote.sin_port, pBuffer->buff);

			// 将数据复制到发送缓冲区
			pSend->nLen = dwTrans;
			memcpy(pSend->buff, pBuffer->buff, dwTrans);

			// 投递此发送I/O（将数据回显给客户）
			if(!PostSend(pSend))
			{
				// 万一出错的话，释放上面刚申请的两个对象
				FreeSocketObj(pClient);	
				FreeBufferObj(pThread, pSend);
				::LeaveCriticalSection(&pSocket->s_cs);
				return FALSE;
			}
			// 继续投递接受I/O
			PostAccept(pBuffer);
		}
		break;
	case OP_READ:	// 接收数据完成
		{
			if(dwTrans > 0)
			{
				// 创建一个缓冲区，以发送数据。这里就使用原来的缓冲区
				PBUFFER_OBJ pSend = pBuffer;
				pSend->nLen = dwTrans;
				pSend->buff[dwTrans] = '\0';
				printf("from %s/%d: %s\n", inet_ntoa(pSocket->addrRemote.sin_addr), pSocket->addrRemote.sin_port, pBuffer->buff);

				// 收到/recv进入文件传输阶段
				char *RecvCommand = "/recv:";
				if (strncmp(RecvCommand, pBuffer->buff, strlen(RecvCommand)) == 0)
				{
					if (!SendFile(pSocket, pBuffer->buff + 6))
					{
						char* ret_fail = "file not found";
						pSend->nLen = strlen(ret_fail);
						strcpy(pSend->buff, ret_fail);
						//pSend->buff[dwTrans] = '\0'
						PostSend(pSend);
					}
					else PostRecv(pSend);
				}
				// 投递发送I/O（将数据回显给客户）
				else PostSend(pSend);
			}
			else	// 套节字关闭
			{
	
				// 必须先关闭套节字，以便在此套节字上投递的其它I/O也返回
				::EnterCriticalSection(&pSocket->s_cs);
				if(pSocket->s != INVALID_SOCKET)
				{
					::closesocket(pSocket->s);
					pSocket->s = INVALID_SOCKET;
				}
				::LeaveCriticalSection(&pSocket->s_cs);
				::LeaveCriticalSection(&pSocket->s_cs);
				if(pSocket->nOutstandingOps == 0)
					FreeSocketObj(pSocket);		
				
				FreeBufferObj(pThread, pBuffer);
				return FALSE;
			}
		}
		break;
	case OP_WRITE:		// 发送数据完成
		{
			if(dwTrans > 0)
			{
				// 继续使用这个缓冲区投递接收数据的请求
				printf("send over\n");
				pBuffer->nLen = BUFFER_SIZE;
				if (pSocket->nOutstandingOps == 0)
				{
					PostRecv(pBuffer);
				}
				else FreeBufferObj(pThread, pBuffer);
			}
			else	// 套节字关闭
			{
				// 同样，要先关闭套节字
				::EnterCriticalSection(&pSocket->s_cs);
				if(pSocket->s != INVALID_SOCKET)
				{
					::closesocket(pSocket->s);
					pSocket->s = INVALID_SOCKET;
				}
				::LeaveCriticalSection(&pSocket->s_cs);
				::LeaveCriticalSection(&pSocket->s_cs);
				if(pSocket->nOutstandingOps == 0)
					FreeSocketObj(pSocket);	

				FreeBufferObj(pThread, pBuffer);
				return FALSE;
			}
		}
		break;
	}
	::LeaveCriticalSection(&pSocket->s_cs);
	return TRUE;
}

DWORD WINAPI ServerThread(LPVOID lpParam)
{
	// 取得本线程对象的指针
	PTHREAD_OBJ pThread = (PTHREAD_OBJ)lpParam;
	while (TRUE)
	{
		//	等待网络事件
		int nIndex = ::WSAWaitForMultipleEvents(
			pThread->nBufferCount + 1, pThread->events, FALSE, WSA_INFINITE, FALSE);
		nIndex = nIndex - WSA_WAIT_EVENT_0;
		// 查看受信的事件对象
		for (int i = nIndex; i<pThread->nBufferCount + 1; i++)
		{
			nIndex = ::WSAWaitForMultipleEvents(1, &pThread->events[i], TRUE, 1000, FALSE);
			if (nIndex == WSA_WAIT_FAILED || nIndex == WSA_WAIT_TIMEOUT)
			{
				continue;
			}
			else
			{
				if (i == 0)				// events[0]受信，重建数组
				{
					RebuildArray(pThread);
					// 如果没有客户I/O要处理了，则本线程退出
					if (pThread->nBufferCount == 0)
					{
						FreeThreadObj(pThread);
						return 0;
					}
					::WSAResetEvent(pThread->events[0]);
				}
				else					// 处理网络事件
				{
					// 查找对应的IO对象指针，调用HandleIO处理网络事件
					PBUFFER_OBJ pBuffer = (PBUFFER_OBJ)FindBufferObj(pThread, i);
					if (pBuffer != NULL)
					{
						if (!HandleIO(pThread, pBuffer))
							RebuildArray(pThread);
					}
					else
						printf(" Unable to find socket object \n ");
				}
			}
		}
	}
	return 0;
}

void main()
{
	// 创建监听套节字，绑定到本地端口，进入监听模式
	int nPort = 4567;
	SOCKET sListen = 
		::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN si;
	si.sin_family = AF_INET;
	si.sin_port = ::ntohs(nPort);
	si.sin_addr.S_un.S_addr = INADDR_ANY;
	::bind(sListen, (sockaddr*)&si, sizeof(si));
	::listen(sListen, 200);

	// 为监听套节字创建一个SOCKET_OBJ对象
	PSOCKET_OBJ pListen = GetSocketObj(sListen);

	// 加载扩展函数AcceptEx
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	DWORD dwBytes;
	WSAIoctl(pListen->s, 
		SIO_GET_EXTENSION_FUNCTION_POINTER, 
		&GuidAcceptEx, 
		sizeof(GuidAcceptEx),
		&pListen->lpfnAcceptEx, 
		sizeof(pListen->lpfnAcceptEx), 
		&dwBytes, 
		NULL, 
		NULL);

	// 加载扩展函数GetAcceptExSockaddrs
	GUID GuidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
	::WSAIoctl(pListen->s,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidGetAcceptExSockaddrs,
		sizeof(GuidGetAcceptExSockaddrs),
		&pListen->lpfnGetAcceptExSockaddrs,
		sizeof(pListen->lpfnGetAcceptExSockaddrs),
		&dwBytes,
		NULL,
		NULL
	);

	::InitializeCriticalSection(&g_cs);
	// 在此可以投递多个接受I/O请求，GetBufferObj已创建处理请求的线程
	for(int i=0; i<5; i++)
	{
		PostAccept(GetBufferObj(pListen, BUFFER_SIZE));
	}

	while (true);
	::DeleteCriticalSection(&g_cs);

	/*while(TRUE)
	{
		int nIndex = 
			::WSAWaitForMultipleEvents(g_nBufferCount + 1, g_events, FALSE, WSA_INFINITE, FALSE);
		if(nIndex == WSA_WAIT_FAILED)
		{
			printf("WSAWaitForMultipleEvents() failed \n");
			break;
		}
		nIndex = nIndex - WSA_WAIT_EVENT_0;
		for(int i=0; i<=nIndex; i++)
		{
			int nRet = ::WSAWaitForMultipleEvents(1, &g_events[i], TRUE, 0, FALSE);
			if(nRet == WSA_WAIT_TIMEOUT)
				continue;
			else
			{
				::WSAResetEvent(g_events[i]);
				// 重新建立g_events数组
				if(i == 0)
				{
					RebuildArray();
					continue;
				}

				// 处理这个I/O
				PBUFFER_OBJ pBuffer = FindBufferObj(g_events[i]);
				if(pBuffer != NULL)
				{
					if(!HandleIO(pBuffer))
						RebuildArray();
					printf("g_nBufferCount:%d\n", g_nBufferCount);
				}
			}
		}
	}*/
}
