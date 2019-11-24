/*
Copyright (c) 2019 Steven Haid

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

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

int32_t owon_b35_init(int32_t cnt, ...);   // id,addr,desired_value_type,name  ...
double owon_b35_get_value(int32_t id);

#endif
