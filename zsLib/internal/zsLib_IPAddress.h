/*
 *  Created by Robin Raymond.
 *  Copyright 2009-2013. Robin Raymond. All rights reserved.
 *
 * This file is part of zsLib.
 *
 * zsLib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (LGPL) as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * zsLib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with zsLib; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#pragma once

#ifndef ZSLIB_INTERNAL_IPADDRESS_H_2dc2c294826056b5f37f536cd66df194
#define ZSLIB_INTERNAL_IPADDRESS_H_2dc2c294826056b5f37f536cd66df194

#include <zsLib/types.h>
#include <zsLib/Exception.h>
#include <zsLib/String.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2def.h>
#include <ws2tcpip.h>
#else
#ifdef __QNX__
#include <sys/socket.h>
#endif // __QNX__
#include <netinet/in.h>
#endif //_WIN32

namespace zsLib
{
  namespace internal
  {
  }
}

#endif //ZSLIB_INTERNAL_IPADDRESS_H_2dc2c294826056b5f37f536cd66df194
