#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>

#include <setjmp.h>
#include <jpeglib.h>

#include "util_jpeg_decode.h"
#include "util_misc.h"

//
// documentationn: 
//   http://www.opensource.apple.com/source/tcl/tcl-87/tcl_ext/tkimg/tkimg/libjpeg/libjpeg.doc
//

// defines
//

#define MAX_JPEG_DECODE_CX 4

//
// typedefs
//

typedef struct {
    struct jpeg_decompress_struct cinfo; 
    bool                          initialized;
    struct jpeg_error_mgr         err_mgr;
    jmp_buf                       err_jmpbuf;
} jpeg_decode_cx_t;

//
// variables
//

static jpeg_decode_cx_t jpeg_decode_cx[MAX_JPEG_DECODE_CX];

static uint8_t ac_huff_tbl_0_bits[17] = { 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125,  };
static uint8_t ac_huff_tbl_0_huffval[256] = { 1, 2, 3, 0, 4, 17, 5, 18, 33, 49, 65, 6, 19, 81, 97, 7, 34, 113, 20, 50, 129, 145, 161, 8, 35, 66, 177, 193, 21, 82, 209, 240, 36, 51, 98, 114, 130, 9, 10, 22, 23, 24, 25, 26, 37, 38, 39, 40, 41, 42, 52, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  };
static uint8_t ac_huff_tbl_1_bits[17] = { 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119,  };
static uint8_t ac_huff_tbl_1_huffval[256] = { 0, 1, 2, 3, 17, 4, 5, 33, 49, 6, 18, 65, 81, 7, 97, 113, 19, 34, 50, 129, 8, 20, 66, 145, 161, 177, 193, 9, 35, 51, 82, 240, 21, 98, 114, 209, 10, 22, 36, 52, 225, 37, 241, 23, 24, 25, 26, 38, 39, 40, 41, 42, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 130, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 226, 227, 228, 229, 230, 231, 232, 233, 234, 242, 243, 244, 245, 246, 247, 248, 249, 250, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  };
static uint8_t dc_huff_tbl_0_bits[17] = { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,  };
static uint8_t dc_huff_tbl_0_huffval[256] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  };
static uint8_t dc_huff_tbl_1_bits[17] = { 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,  };
static uint8_t dc_huff_tbl_1_huffval[256] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  };

//
// prototypes
//

static void jpeg_decode_error_exit_override(j_common_ptr cinfo);
static void jpeg_decode_output_message_override(j_common_ptr cinfo);

// -----------------  JPEG DECODE  ---------------------------------------------------------

int32_t jpeg_decode(uint32_t cxid, uint32_t jpeg_decode_mode, uint8_t * jpeg, uint32_t jpeg_size, 
                    uint8_t ** out_buf, uint32_t * width, uint32_t * height)
{
    jpeg_decode_cx_t              * cx;
    struct jpeg_decompress_struct * cinfo; 
    JSAMPLE                         row[10000];
    JSAMPROW                        scanline[1] = { row };
    uint8_t                       * out = NULL;
    uint8_t                       * outp;
    uint32_t                        bytes_per_pixel;

    // preset returns to caller
    *out_buf = NULL;
    *width   = 0;
    *height  = 0;

    // verify cxid and jpeg_decode_mode args
    if (cxid >= MAX_JPEG_DECODE_CX) {
        ERROR("invalid cxid %d\n", cxid);
        return -1;
    }
    if (jpeg_decode_mode != JPEG_DECODE_MODE_GS &&
        jpeg_decode_mode != JPEG_DECODE_MODE_YUY2) 
    {
        ERROR("invalid jpeg_decode_mode %d\n", jpeg_decode_mode);
        return -1;
    }

    // init ptrs to cx and cinfo
    cx = &jpeg_decode_cx[cxid];
    cinfo = &cx->cinfo;

    // check if cx has not been initialized
    if (!cx->initialized) {
        // cx has not been initialized ...

        // use default error management, override the error_exit routine
        cinfo->err = jpeg_std_error(&cx->err_mgr);
        cinfo->err->error_exit = jpeg_decode_error_exit_override;
        cinfo->err->output_message = jpeg_decode_output_message_override;

        // setjmp, for use by the error exit override
        if (setjmp(cx->err_jmpbuf)) {
            free(out);
            return -1;
        }

        // initialize the jpeg decompress object
        jpeg_create_decompress(&cx->cinfo);

        // load default huffman tables;
        // Refer to "Abbreviated datastreams and multiple images" in libjpeg.doc
        cinfo->ac_huff_tbl_ptrs[0] = jpeg_alloc_huff_table((j_common_ptr) &cx->cinfo);
        memcpy(cinfo->ac_huff_tbl_ptrs[0]->bits, ac_huff_tbl_0_bits, 17);
        memcpy(cinfo->ac_huff_tbl_ptrs[0]->huffval, ac_huff_tbl_0_huffval, 256);
        cinfo->ac_huff_tbl_ptrs[1] = jpeg_alloc_huff_table((j_common_ptr) &cx->cinfo);
        memcpy(cinfo->ac_huff_tbl_ptrs[1]->bits, ac_huff_tbl_1_bits, 17);
        memcpy(cinfo->ac_huff_tbl_ptrs[1]->huffval, ac_huff_tbl_1_huffval, 256);
        cinfo->dc_huff_tbl_ptrs[0] = jpeg_alloc_huff_table((j_common_ptr) &cx->cinfo);
        memcpy(cinfo->dc_huff_tbl_ptrs[0]->bits, dc_huff_tbl_0_bits, 17);
        memcpy(cinfo->dc_huff_tbl_ptrs[0]->huffval, dc_huff_tbl_0_huffval, 256);
        cinfo->dc_huff_tbl_ptrs[1] = jpeg_alloc_huff_table((j_common_ptr) &cx->cinfo);
        memcpy(cinfo->dc_huff_tbl_ptrs[1]->bits, dc_huff_tbl_1_bits, 17);
        memcpy(cinfo->dc_huff_tbl_ptrs[1]->huffval, dc_huff_tbl_1_huffval, 256);

        // set the context initialized flag
        cx->initialized = true;
    } else {
        // cx has been initialized ...

        // setjmp, for use by the error exit override
        if (setjmp(cx->err_jmpbuf)) {
            free(out);
            return -1;
        }
    }

    // supply the jpeg buffer and size to the decoder
    jpeg_mem_src(&cx->cinfo, jpeg, jpeg_size);

    // read the jpeg header, require_image==true
    jpeg_read_header(&cx->cinfo, true);

    // map the jpeg_decode_mode input to the desired color space
    if (jpeg_decode_mode == JPEG_DECODE_MODE_GS) {
        cinfo->out_color_space = JCS_GRAYSCALE;
        bytes_per_pixel = 1;
    } else {
        cinfo->out_color_space = JCS_YCbCr;
        bytes_per_pixel = 2;
    }

    // initialize the decompression, this sets cinfo->output_width and cinfo->output_height
    jpeg_start_decompress(&cx->cinfo);

    // allocate memory for the output
    out = malloc(cinfo->output_width * cinfo->output_height * bytes_per_pixel);
    if (out == NULL) {
        ERROR("failed allocate memory for width=%d height=%d bytes_per_pixel=%d\n",
               cinfo->output_width, cinfo->output_height, bytes_per_pixel);
        return -1;
    }
    outp = out;

    // loop over scanlines
    while (cinfo->output_scanline < cinfo->output_height) {
        // read scanline
        jpeg_read_scanlines(&cx->cinfo, scanline, 1);

        // save the row data in the output buffer
        if (jpeg_decode_mode == JPEG_DECODE_MODE_GS) {
            memcpy(outp, row, cinfo->output_width);
            outp += cinfo->output_width;
        } else {
            int32_t i;
            uint8_t * r = row;
            for (i = 0; i < cinfo->output_width; i+=2) {
                outp[0] = r[0];     // y0
                outp[1] = r[1];     // v0   
                outp[2] = r[3];     // y1
                outp[3] = r[2];     // u0
                outp += 4;
                r += 6;
            }
        }
    }

    // complete
    jpeg_finish_decompress(&cx->cinfo);

    // return success
    *out_buf = out;
    *width   = cinfo->output_width;
    *height  = cinfo->output_height;
    return 0;
}

static void jpeg_decode_error_exit_override(j_common_ptr cinfo)
{
    jpeg_decode_cx_t * cx = (jpeg_decode_cx_t *)cinfo;
    longjmp(cx->err_jmpbuf, 1);
}

static void jpeg_decode_output_message_override(j_common_ptr cinfo)
{
    char buffer[JMSG_LENGTH_MAX];

    (*cinfo->err->format_message)(cinfo, buffer);
    ERROR("%s\n", buffer);
}

