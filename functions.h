#pragma once
/*
  provide server side functions in C
 */

void functions_init(struct template_state *tmpl);

typedef enum {
    AUTH_NONE= 0,
    AUTH_WEP = 1,
    AUTH_WPA = 2,
    AUTH_WPA2 = 3,
} apweb_enc_auth_t ;

const char *validate_ssid_password(const char *ssid,
                                   const char *password,
                                   apweb_enc_auth_t auth_mode,
                                   int channel);
