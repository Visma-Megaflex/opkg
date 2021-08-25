/* vi: set expandtab sw=4 sts=4: */
/* opkg_openssl.h - the opkg package management system

    Copyright (C) 2001 University of Southern California
    Copyright (C) 2014 Paul Barker

    SPDX-License-Identifier: GPL-2.0-or-later

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2, or (at
    your option) any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.
*/

#ifndef OPKG_OPENSSL_H
#define OPKG_OPENSSL_H

#include <openssl/ssl.h>

#ifdef __cplusplus
extern "C" {
#endif

void openssl_init(void);
int opkg_verify_openssl_signature(const char *file, const char *sigfile);

#ifdef __cplusplus
}
#endif
#endif
