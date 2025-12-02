#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

#include "game_shared.h"

// 전역
static GameState g_state;
static int g_running = 1;

static CRITICAL_SECTION g_cs; // GameState 동기화용

unsigned __stdcall ClientThreadProc(void* arg);

void err_quit(const char* msg)
{
    int err = WSAGetLastError();
    printf("%s (err=%d)\n", msg, err);
    exit(1);
}

int SendGameState(SOCKET s)
{
    SV_GAME_STATE pkt;
    pkt.header.Type = 2; // SV_GAME_STATE
    pkt.header.Size = sizeof(SV_GAME_STATE) - sizeof(PACKET_HEADER);
    pkt.state = g_state;

    int toSend = sizeof(pkt);
    char* buf = (char*)&pkt;
    int sent = 0;

    while (sent < toSend) {
        int ret = send(s, buf + sent, toSend - sent, 0);
        if (ret <= 0) return 0;
        sent += ret;
    }
    return 1;
}

unsigned __stdcall ClientThreadProc(void* arg)
{
    SOCKET cs = (SOCKET)arg;
    printf("Client connected.\n");

    // ? Windows 논블로킹 설정
    u_long mode = 1;
    ioctlsocket(cs, FIONBIO, &mode);

    DWORD lastTick = GetTickCount();

    while (g_running) {

        //------------------------------------------------------------------
        // ? 1단계: 클라이언트 → 서버 입력 수신
        //------------------------------------------------------------------
        CL_PLACE_UNIT inPkt;
        int ret = recv(cs, (char*)&inPkt, sizeof(inPkt), 0);

        if (ret > 0 && inPkt.header.Type == 1) {
            EnterCriticalSection(&g_cs);

            int row = inPkt.row;
            int col = inPkt.col;
            int kind = inPkt.unitKind;

            printf("Place Unit Request: kind=%d row=%d col=%d\n", kind, row, col);

            int idx = row * 10 + col;
            if (idx >= 0 && idx < MAX_PLANTS) {
                g_state.plants[idx].kind = kind;
                g_state.plants[idx].hp = 100;
                g_state.plants[idx].atk = 10;
                g_state.plants[idx].tu = 1;
            }

            LeaveCriticalSection(&g_cs);
        }

        //------------------------------------------------------------------
        // ? 2단계: 게임 로직 갱신
        //------------------------------------------------------------------
        DWORD now = GetTickCount();
        float dt = (now - lastTick) / 1000.0f;

        if (dt >= 0.05f) {
            lastTick = now;
            EnterCriticalSection(&g_cs);
            UpdateGameState(&g_state, dt);
            LeaveCriticalSection(&g_cs);
        }

        //------------------------------------------------------------------
        // ? 3단계: 서버 → 클라 상태 전송
        //------------------------------------------------------------------
        EnterCriticalSection(&g_cs);
        if (!SendGameState(cs)) {
            LeaveCriticalSection(&g_cs);
            printf("Client disconnected.\n");
            break;
        }
        LeaveCriticalSection(&g_cs);

        Sleep(1);
    }

    closesocket(cs);
    return 0;
}



int main(void)
{
    InitializeCriticalSection(&g_cs);
    // 서버 시작
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }

    

    // socket()
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) err_quit("socket()");

    // bind()
    SOCKADDR_IN serveraddr;
    ZeroMemory(&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(9000);

    if (bind(listenSock, (SOCKADDR*)&serveraddr, sizeof(serveraddr)) == SOCKET_ERROR)
        err_quit("bind()");

    // listen()
    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR)
        err_quit("listen()");

    printf("Server listening on port 9000...\n");


    
    while (g_running) {
        SOCKADDR_IN clientaddr;
        int addrlen = sizeof(clientaddr);
        SOCKET clientSock = accept(listenSock, (SOCKADDR*)&clientaddr, &addrlen);
        if (clientSock == INVALID_SOCKET) {
            printf("accept() failed.\n");
            continue;
        }

        // 새로운 스레드가 생성도어 ClientTHreadProc 실행, 인자로 clinetsock 전달 
        HANDLE hThread = (HANDLE)_beginthreadex(
            NULL, 0, ClientThreadProc, (void*)clientSock, 0, NULL);
        CloseHandle(hThread);
    }

    closesocket(listenSock);
    WSACleanup();
    DeleteCriticalSection(&g_cs);
    return 0;
}