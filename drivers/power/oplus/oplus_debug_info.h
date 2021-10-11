#ifndef __OPLUS_DEBUG_INFO__H
#define __OPLUS_DEBUG_INFO__H

#include "oppo_charger.h"

extern int oplus_chg_debug_info_init(void);
extern int oplus_chg_debug_chg_monitor(struct oppo_chg_chip *chip);
extern int oplus_chg_debug_set_cool_down_by_user(int is_cool_down);
extern int oplus_chg_debug_get_cooldown_current(int chg_current_by_tbatt, int chg_current_by_cooldown);
extern int oplus_chg_debug_set_soc_info(struct oppo_chg_chip *chip);

#endif
