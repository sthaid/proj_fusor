#ifndef __UTIL_OWON_B35_H__
#define __UTIL_OWON_B35_H__

#define OWON_B35_VALUE_TYPE_DC_VOLT        1
#define OWON_B35_VALUE_TYPE_DC_MILLIVOLT   2
#define OWON_B35_VALUE_TYPE_DC_AMP         3
#define OWON_B35_VALUE_TYPE_DC_MILLIAMP    4
#define OWON_B35_VALUE_TYPE_DC_MICROAMP    5

#define OWON_B35_VALUE_TYPE_STR(x) \
    ((x) == OWON_B35_VALUE_TYPE_DC_VOLT      ? "VOLT"      : \
     (x) == OWON_B35_VALUE_TYPE_DC_MILLIVOLT ? "MILLIVOLT" : \
     (x) == OWON_B35_VALUE_TYPE_DC_AMP       ? "AMP"       : \
     (x) == OWON_B35_VALUE_TYPE_DC_MILLIAMP  ? "MILLIAMP"  : \
     (x) == OWON_B35_VALUE_TYPE_DC_MICROAMP  ? "MICROAMP"    \
                                             : "????")

double owon_b35_get_reading(char *meter_bluetooth_addr, int desired_value_type);

#endif
