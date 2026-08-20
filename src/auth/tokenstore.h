#ifndef CORDLESS_AUTH_TOKENSTORE_H
#define CORDLESS_AUTH_TOKENSTORE_H

#include <stddef.h>
#include <stdbool.h>

// Persist the Discord user token under ux0:data/cordless/. XOR-obfuscated, not
// real security (homebrew has no secure element).

bool tokenstore_save(const char *token);

// Load the token into out (NUL-terminated). False if none stored.
bool tokenstore_load(char *out, size_t out_cap);

// Delete the stored token (logout).
void tokenstore_clear(void);

// Roster of known accounts (token + display info) for account switching.
// Same XOR obfuscation.
#define ACCOUNTS_MAX 8
typedef struct {
    char token[128];
    char name[64];
    char id[24];
    char avatar[40];
} account_t;

int  accounts_load(account_t *out, int max);
// Add or update (by id) an account.
bool accounts_add(const char *token, const char *name, const char *id, const char *avatar);
bool accounts_remove(const char *id);

#endif  // CORDLESS_AUTH_TOKENSTORE_H
