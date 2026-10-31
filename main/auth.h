#pragma once
#include <stdbool.h>
#include <stddef.h>
void auth_init(void);
bool auth_login(const char *user, const char *pass, char *msg, size_t n);
void auth_logout(void);
int  auth_level(void);              /* 0 = not logged in, 1 = manager, 2 = admin (auto-logout after 2 min idle) */
const char *auth_user(void);
bool auth_passwd(const char *new_pass);
