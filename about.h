/*
Copyright (c) 2016 Steven Haid

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

static char about[] = "\
Fusor Data Acquisition - This program acquires, displays, and\n\
records camera and analog sensor values which are used in\n\
Farnsworth Fusor demonstration.\n\
\n\
Keyboard Controls:\n\
  Shift-Esc              : Quit\n\
  ?                      : Help\n\
  Ctrl-p, or Alt-p       : Capture Screenshot\n\
  Left, Right, Home, End : Summary Graph Time Select (*)\n\
  '-', '+'               : Summary Graph Time Scale\n\
  's', '1', '2'          : Select ADC Graph, and Modify Y Scale\n\
  'a', 'd', 'w', 'x'     : Camera Pan \n\
  'z', 'Z'               : Camera Zoom\n\
  'r'                    : Camera Pan/Zoom Reset \n\
  '3', '4'               : Change Neutron Pulse Height Threshold\n\
\n\
  (*) Use Ctl or Alt with Left/Right Arrow to increase response\n\
\n\
Program Options:\n\
  -h                           : help\n\
  -v                           : display version\n\
  -g <width>x<height>          : default = 1920x1000\n\
  -s <live-mode-data-server>   : default = rpi_data \n\
  -p <playback-mode-file-name> : select playback mode\n\
  -x                           : don't capture cam data in live mode\n\
  -t <secs>                    : generate test data file\n\
\n\
Author: Steven Haid      StevenHaid@gmail.com\n\
\n\
Source Code: https://github.com/sthaid/proj_fusor.git\n\
\n\
THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND,\n\
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF\n\
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.\n\
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY\n\
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,\n\
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE\n\
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n\
";
