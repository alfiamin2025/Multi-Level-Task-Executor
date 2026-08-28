/*
 * http_server.h
 * -------------
 * HTTP server API for the web dashboard.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

void StartHTTPServer(void);
void SendDashboardUpdate(const char* jsonData);
void StopHTTPServer(void);
void OpenDashboard(void);
void SendLogToDashboard(const char* message);
void RegisterCrashCallback(void (*callback)(void));

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SERVER_H */
