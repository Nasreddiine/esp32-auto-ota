#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>

void ota_init(void);
void ota_check_update(void);
bool ota_is_update_available(void);
void ota_perform_update(void);
const char* ota_get_current_version(void);

#endif