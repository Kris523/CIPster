/*******************************************************************************
 * Copyright (c) 2009, Rockwell Automation, Inc.
 * All rights reserved.
 *
 ******************************************************************************/
#ifndef CIPSTER_NETWORKHANDLER_H_
#define CIPSTER_NETWORKHANDLER_H_


#include "typedefs.h"

/** @brief Start a TCP/UDP listening socket, accept connections, receive data
*    in select loop, call manageConnections periodically.
 *  @return status
 *          EIP_ERROR .. error
 */
EipStatus NetworkHandlerInitialize();

EipStatus NetworkHandlerProcessOnce();

EipStatus NetworkHandlerFinish();

#endif // CIPSTER_NETWORKHANDLER_H_
