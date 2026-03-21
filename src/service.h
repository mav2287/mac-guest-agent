#ifndef MGA_SERVICE_H
#define MGA_SERVICE_H

#define SERVICE_NAME    "com.macos.guest-agent"
#define BINARY_PATH     "/usr/local/bin/mac-guest-agent"
#define PLIST_PATH      "/Library/LaunchDaemons/com.macos.guest-agent.plist"
#define LOG_PATH        "/var/log/mac-guest-agent.log"
#define SHARE_PATH      "/usr/local/share/mac-guest-agent"

int service_install(void);
int service_uninstall(void);

#endif
