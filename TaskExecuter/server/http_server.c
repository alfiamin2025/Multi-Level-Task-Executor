/*
 * http_server.c
 * -------------
 * Lightweight HTTP server for the web dashboard.
 * Supports: File serving, SSE, Crash injection, Log clearing, State endpoint
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 8888
#define BUFFER_SIZE 8192
#define MAX_CLIENTS 10

/* Global state */
static SOCKET g_clientSockets[MAX_CLIENTS];
static int g_clientCount = 0;
static CRITICAL_SECTION g_clientLock;
static volatile LONG g_serverRunning = 1;
static char g_lastLogMessage[1024] = {0};

/* External function for crash injection (set by dispatcher) */
void (*g_crashCallback)(void) = NULL;

/* Forward declarations */
static DWORD WINAPI ClientHandler(LPVOID param);
static DWORD WINAPI ServerThread(LPVOID param);
static void BroadcastSSE(const char *data);
static void SendSSE(SOCKET client, const char *data);
static void HandleRequest(SOCKET client, const char *request);
static void SendFile(SOCKET client, const char* path);
static void SendResponse(SOCKET client, const char* status, const char* contentType, const char* body);

/* ================ Send File from web folder ================ */
static void SendFile(SOCKET client, const char* path) {
    char fullPath[512];

    /* Get executable directory */
    char exePath[512];
    GetModuleFileName(NULL, exePath, sizeof(exePath));
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *lastSlash = '\0';

    /* Go up TWO levels: from bin\Debug to project root */
    lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *lastSlash = '\0';
    lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *lastSlash = '\0';

    /* Build path */
    const char* filePath = path;
    if (filePath[0] == '/') filePath++;
    if (strlen(filePath) == 0) filePath = "dashboard.html";
    snprintf(fullPath, sizeof(fullPath), "%s\\web\\%s", exePath, filePath);

    /* Open file */
    HANDLE hFile = CreateFile(fullPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char response[512];
        snprintf(response, sizeof(response),
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n"
            "\r\n"
            "<html><body><h1>404 - File Not Found</h1>"
            "<p>Looking for: %s</p>"
            "<p>Make sure the 'web' folder exists with dashboard.html</p>"
            "</body></html>",
            fullPath);
        send(client, response, strlen(response), 0);
        return;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    const char* contentType = "text/html";
    const char* ext = strrchr(filePath, '.');
    if (ext) {
        if (strcmp(ext, ".css") == 0) contentType = "text/css";
        else if (strcmp(ext, ".js") == 0) contentType = "application/javascript";
        else if (strcmp(ext, ".json") == 0) contentType = "application/json";
        else if (strcmp(ext, ".png") == 0) contentType = "image/png";
        else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) contentType = "image/jpeg";
    }

    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        contentType, fileSize);
    send(client, header, strlen(header), 0);

    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        send(client, buffer, bytesRead, 0);
    }
    CloseHandle(hFile);
}

/* ================ Send HTTP Response ================ */
static void SendResponse(SOCKET client, const char* status, const char* contentType, const char* body) {
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, contentType, (int)strlen(body), body);
    send(client, response, strlen(response), 0);
}

/* ================ Send SSE Message ================ */
static void SendSSE(SOCKET client, const char* data) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "data: %s\n\n", data);
    send(client, buffer, strlen(buffer), 0);
}

/* ================ Broadcast to All Clients ================ */
static void BroadcastSSE(const char* data) {
    EnterCriticalSection(&g_clientLock);
    for (int i = 0; i < g_clientCount; i++) {
        if (g_clientSockets[i] != INVALID_SOCKET) {
            SendSSE(g_clientSockets[i], data);
        }
    }
    LeaveCriticalSection(&g_clientLock);
}

/* ================ Handle HTTP Request ================ */
static void HandleRequest(SOCKET client, const char* request) {
    char method[16], path[256], version[16];
    if (sscanf(request, "%15s %255s %15s", method, path, version) != 3) {
        SendResponse(client, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }

    char* query = strchr(path, '?');
    if (query) *query = '\0';

    /* ===== State Endpoint ===== */
    if (strcmp(path, "/state") == 0) {
        SendResponse(client, "200 OK", "application/json", "{\"status\":\"ok\"}");
        return;
    }

    /* ===== SSE Endpoint ===== */
    if (strcmp(path, "/events") == 0) {
        char response[256];
        snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n");
        send(client, response, strlen(response), 0);

        EnterCriticalSection(&g_clientLock);
        if (g_clientCount < MAX_CLIENTS) {
            g_clientSockets[g_clientCount++] = client;
        }
        LeaveCriticalSection(&g_clientLock);

        char buffer[128];
        while (g_serverRunning) {
            if (recv(client, buffer, sizeof(buffer), MSG_PEEK) <= 0) break;
            Sleep(100);
        }

        EnterCriticalSection(&g_clientLock);
        for (int i = 0; i < g_clientCount; i++) {
            if (g_clientSockets[i] == client) {
                g_clientSockets[i] = INVALID_SOCKET;
                break;
            }
        }
        LeaveCriticalSection(&g_clientLock);
        closesocket(client);
        return;
    }

    /* ===== Crash Injection ===== */
    if (strcmp(path, "/crash") == 0) {
        SendResponse(client, "200 OK", "text/plain", "Crash injected");
        /* Trigger the crash callback if registered */
        if (g_crashCallback) {
            g_crashCallback();
        }
        return;
    }

    /* ===== Clear Log ===== */
    if (strcmp(path, "/clear-log") == 0) {
        SendResponse(client, "200 OK", "text/plain", "Log cleared");
        /* Clear the log on server side */
        g_lastLogMessage[0] = '\0';
        /* Broadcast a clear log event */
        BroadcastSSE("{\"clearLog\":true}");
        return;
    }

    /* ===== Serve Files from web folder ===== */
    SendFile(client, path);
}

/* ================ Client Handler Thread ================ */
static DWORD WINAPI ClientHandler(LPVOID param) {
    SOCKET client = (SOCKET)(intptr_t)param;
    char buffer[BUFFER_SIZE];
    int bytes = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        HandleRequest(client, buffer);
    }
    closesocket(client);
    return 0;
}

/* ================ Server Thread ================ */
static DWORD WINAPI ServerThread(LPVOID param) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) { WSACleanup(); return 1; }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(server); WSACleanup(); return 1;
    }

    if (listen(server, 10) == SOCKET_ERROR) {
        closesocket(server); WSACleanup(); return 1;
    }

    printf("[HTTP Server] Listening on http://localhost:%d\n", PORT);
    printf("[HTTP Server] Open http://localhost:%d/dashboard.html\n", PORT);

    InitializeCriticalSection(&g_clientLock);

    while (g_serverRunning) {
        SOCKET client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET) break;
        HANDLE hThread = CreateThread(NULL, 0, ClientHandler, (LPVOID)(intptr_t)client, 0, NULL);
        if (hThread) CloseHandle(hThread);
    }

    closesocket(server);
    DeleteCriticalSection(&g_clientLock);
    WSACleanup();
    return 0;
}

/* ================ Public API ================ */

void StartHTTPServer(void) {
    HANDLE hThread = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    if (hThread) CloseHandle(hThread);
    Sleep(500);
}

void SendDashboardUpdate(const char* jsonData) {
    BroadcastSSE(jsonData);
}

void StopHTTPServer(void) {
    g_serverRunning = 0;
}

void OpenDashboard(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "start http://localhost:%d/dashboard.html", PORT);
    system(cmd);
}

void SendLogToDashboard(const char* message) {
    if (!message) return;
    strncpy(g_lastLogMessage, message, sizeof(g_lastLogMessage) - 1);

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", tm_info);

    char json[2048];
    snprintf(json, sizeof(json),
        "{\"log\":{\"time\":\"%s\",\"msg\":\"%s\"}}",
        timeStr, message);
    BroadcastSSE(json);
}

/* Register crash callback */
void RegisterCrashCallback(void (*callback)(void)) {
    g_crashCallback = callback;
}
